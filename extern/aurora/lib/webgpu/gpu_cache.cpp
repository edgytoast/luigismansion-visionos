#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

#include "../internal.hpp"
#include "gpu.hpp"
#include "../io.hpp"
#include "../sqlite_utils.hpp"

#include <sqlite3.h>
#include <fmt/format.h>
#if defined(AURORA_CACHE_USE_ZSTD)
#include <zstd.h>
#endif
#define XXH_STATIC_LINKING_ONLY
#include <xxhash.h>

namespace aurora::webgpu {
static Module Log("aurora::gpu::cache");

static sqlite3* db;
static sqlite3_stmt* load_stmt;
static sqlite3_stmt* store_stmt;
static bool cache_broken;
static std::mutex cache_mutex;
static uint64_t db_generation = 1; // 0227: under cache_mutex
static std::vector<XXH128_hash_t> cache_keys_used;
#if defined(AURORA_CACHE_USE_ZSTD)
static std::vector<uint8_t> compress_buffer;
#endif

// 0226: this launch's session number (aurora_session table), entries at open, statistics (cache_stats).
static int64_t cache_session = 0;
static uint64_t entries_at_open = 0;

static std::atomic<uint32_t> stat_hits{0}, stat_misses{0}, stat_stores{0};
static std::atomic<uint64_t> stat_load_ns{0}, stat_store_ns{0};
static int64_t stat_now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
struct StatTimer {
  std::atomic<uint64_t>& total;
  int64_t t0 = stat_now_ns();
  ~StatTimer() { total.fetch_add(static_cast<uint64_t>(stat_now_ns() - t0), std::memory_order_relaxed); }
};

CacheStats cache_stats() {
  return CacheStats{
      .hits = stat_hits.load(std::memory_order_relaxed),
      .misses = stat_misses.load(std::memory_order_relaxed),
      .stores = stat_stores.load(std::memory_order_relaxed),
      .loadNs = stat_load_ns.load(std::memory_order_relaxed),
      .storeNs = stat_store_ns.load(std::memory_order_relaxed),
      .broken = cache_broken,
      .entriesAtOpen = static_cast<uint32_t>(entries_at_open),
      .session = static_cast<uint32_t>(cache_session),
  };
}

// 3: per-entry checksum (XXH3 of the uncompressed value), verified on every load (patch 0017).
constexpr int CACHE_SCHEMA = 3;
// % of rows pruned to trigger a full VACUUM
constexpr uint64_t VacuumPrunePercentThreshold = 25;
// 0226: an entry is pruned once it went unused for this many sessions (launches) in a row, not at
// the first session that did not need it: switching graphics settings (Enhanced lighting on/off,
// MSAA, accuracy toggles) used to delete every blob of the other settings at once, so switching
// back compiled all of them again; and blobs first read after the prune ran (pipelines created
// later in a session) were deleted at every launch and compiled again at the next.
constexpr int64_t PruneUnusedSessions = 5;
static size_t keys_marked_used = 0;  // cache_keys_used[0, n) already written to last_used

static std::filesystem::path cache_path() {
  return io::fs_path_from_string(g_config.cachePath) / "dawn_cache.db";
}

void (*g_cacheOpenHook)() = nullptr;
bool g_cacheVerifyOnOpen = true;

// Set when SQLite reports a damaged file; acted on once no transaction is open.
static bool reset_pending = false;

static void close_db() {
  ++db_generation; // 0227: per-thread readers reopen
  if (load_stmt != nullptr) {
    sqlite3_finalize(load_stmt);
    load_stmt = nullptr;
  }
  if (store_stmt != nullptr) {
    sqlite3_finalize(store_stmt);
    store_stmt = nullptr;
  }
  if (db != nullptr) {
    sqlite3_close(db);
    db = nullptr;
  }
}

static void init_abort() {
  cache_broken = true;
  close_db();
}

static void delete_db_files() {
  for (const char* suffix : {"", "-wal", "-shm", "-journal"}) {
    auto path = cache_path();
    path += suffix;
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }
}

void cache_delete_files() { delete_db_files(); }

static bool is_corruption(int ret) {
  const int primary = ret & 0xFF;
  return primary == SQLITE_CORRUPT || primary == SQLITE_NOTADB;
}

// A damaged file found while running: drop it, the next cache access recreates it.
static void reset_corrupt_db() {
  Log.warn("Dawn cache database is damaged; deleting and recreating it");
  close_db();
  delete_db_files();
  cache_keys_used.clear();
}

static int check(int ret) {
  if (ret != SQLITE_OK) {
    Log.error("SQLite operation failed: {}", sqlite3_errmsg(db));
  }

  return ret;
}

static bool ensure_schema_up_to_date() {
  sqlite::Transaction tx(db, Log, true);
  if (!tx) {
    Log.error("Failed to open schema check transaction", sqlite3_errmsg(db));
    return false;
  }

  auto ret = sqlite::exec(db, "CREATE TABLE IF NOT EXISTS aurora_schema(value INTEGER);");
  if (ret != SQLITE_OK) {
    Log.error("Failed to create schema table: {}", sqlite3_errmsg(db));
    return false;
  }

  bool match = false;
  auto cmd = fmt::format("SELECT * FROM aurora_schema WHERE value = {}", CACHE_SCHEMA);
  ret = sqlite::exec(db, cmd.c_str(), [&match](int, char**, char**) { match = true; }, nullptr);
  if (ret != SQLITE_OK) {
    Log.error("Failed to check schema table: {}", sqlite3_errmsg(db));
    return false;
  }

  if (match) {
    // 0226: caches written before the session counter get the last_used column (rows start at 0:
    // the next prune keeps whatever that session reads and ages out the rest).
    bool hasLastUsed = false;
    ret = sqlite::exec(db, "PRAGMA table_info(cache);", [&hasLastUsed](int argc, char** argv, char**) {
      if (argc > 1 && argv[1] != nullptr && std::strcmp(argv[1], "last_used") == 0) {
        hasLastUsed = true;
      }
    });
    if (ret != SQLITE_OK) {
      Log.error("Failed to read the cache table layout: {}", sqlite3_errmsg(db));
      return false;
    }
    if (!hasLastUsed &&
        sqlite::exec(db, "ALTER TABLE cache ADD COLUMN last_used INTEGER NOT NULL DEFAULT 0;") != SQLITE_OK) {
      Log.error("Failed to add the last_used column: {}", sqlite3_errmsg(db));
      return false;
    }
    tx.commit();
    return true;
  }

  cmd = fmt::format(
      R"(DROP TABLE IF EXISTS cache;
CREATE TABLE cache (
  key BLOB PRIMARY KEY NOT NULL,
  value BLOB NOT NULL,
  size INTEGER NOT NULL,
  compressed INTEGER NOT NULL,
  checksum INTEGER NOT NULL,
  last_used INTEGER NOT NULL DEFAULT 0
);
DELETE FROM aurora_schema;
INSERT INTO aurora_schema VALUES ({});)",
      CACHE_SCHEMA);
  ret = sqlite::exec(db, cmd.c_str());
  if (ret != SQLITE_OK) {
    Log.error("Failed to update schema: {}", sqlite3_errmsg(db));
    return false;
  }

  tx.commit();
  return true;
}

static bool cache_init_core() {
  Log.debug("SQLite version {}", sqlite3_libversion());

  std::string file = io::fs_path_to_string(cache_path());
  Log.debug("Using dawn cache at {}", file);
  auto ret = sqlite3_open(file.c_str(), &db);
  if (ret != SQLITE_OK) {
    Log.error("Failed to open database: {}", sqlite3_errmsg(db));
    return false;
  }

  // WAL mode + NORMAL = no need for disk syncs, consistent but not durable is fine.
  ret = sqlite::exec(db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;");
  if (ret != SQLITE_OK) {
    Log.error("Failed to set pragmas: {}", sqlite3_errmsg(db));
    return false;
  }

  // Structural check of every page (a truncated or overwritten file fails here instead of
  // somewhere in the middle of loading; the per-entry checksums catch damaged values).
  // 0231: no whole-file check (it read every page of a 60-500 MB file before the first pipeline);
  // entries are verified by their checksums as they are read, and an SQLite corruption error on a
  // lookup or store resets the file. AURORA_CACHE_QUICK_CHECK=1 runs the check anyway (testing).
  static const bool sQuickCheck = std::getenv("AURORA_CACHE_QUICK_CHECK") != nullptr;
  if (g_cacheVerifyOnOpen) {
    Log.info("The previous session did not exit cleanly: Dawn cache entries are verified as they are read");
  }
  bool integrityOk = !sQuickCheck;
  std::string integrityResult;
  ret = !sQuickCheck ? SQLITE_OK
                     : sqlite::exec(db, "PRAGMA quick_check;", [&integrityOk, &integrityResult](int argc, char** argv, char**) {
    if (argc > 0 && argv[0] != nullptr && std::strcmp(argv[0], "ok") == 0) {
      integrityOk = true;
    } else if (argc > 0 && argv[0] != nullptr && integrityResult.empty()) {
      integrityResult = argv[0];
    }
  });
  if (ret != SQLITE_OK || !integrityOk) {
    Log.error("Dawn cache integrity check failed: {}",
              ret != SQLITE_OK ? sqlite3_errmsg(db) : integrityResult.c_str());
    return false;
  }

  if (!ensure_schema_up_to_date()) {
    Log.error("Failed to validate schema");
    return false;
  }

  // 0226: count launches (sessions) for the age-based prune.
  {
    sqlite::Transaction tx(db, Log, true);
    if (!tx) {
      return false;
    }
    int64_t last = 0;
    ret = sqlite::exec(db,
                       "CREATE TABLE IF NOT EXISTS aurora_session(value INTEGER);"
                       "SELECT value FROM aurora_session LIMIT 1;",
                       [&last](int argc, char** argv, char**) {
                         if (argc > 0 && argv[0] != nullptr) {
                           last = std::strtoll(argv[0], nullptr, 10);
                         }
                       });
    if (ret != SQLITE_OK) {
      Log.error("Failed to read the cache session counter: {}", sqlite3_errmsg(db));
      return false;
    }
    cache_session = last + 1;
    const auto cmd = fmt::format("DELETE FROM aurora_session; INSERT INTO aurora_session VALUES ({});", cache_session);
    if (sqlite::exec(db, cmd.c_str()) != SQLITE_OK) {
      Log.error("Failed to write the cache session counter: {}", sqlite3_errmsg(db));
      return false;
    }
    tx.commit();
  }
  keys_marked_used = 0;

  ret = sqlite3_prepare_v3(db, "SELECT value, size, compressed, checksum FROM cache WHERE key = ?", -1,
                           SQLITE_PREPARE_PERSISTENT, &load_stmt, nullptr);
  if (ret != SQLITE_OK) {
    Log.error("Failed to prepare statement: {}", sqlite3_errmsg(db));
    return false;
  }

  ret = sqlite3_prepare_v3(db,
                           "REPLACE INTO cache (key, value, size, compressed, checksum, last_used) "
                           "VALUES (?, ?, ?, ?, ?, ?)",
                           -1, SQLITE_PREPARE_PERSISTENT, &store_stmt, nullptr);
  if (ret != SQLITE_OK) {
    Log.error("Failed to prepare statement: {}", sqlite3_errmsg(db));
    return false;
  }

  return true;
}

static std::optional<uint64_t> select_uint64(const char* sql) {
  sqlite3_stmt* stmt = nullptr;
  auto ret = sqlite3_prepare_v3(db, sql, -1, 0, &stmt, nullptr);
  if (ret != SQLITE_OK) {
    Log.error("Failed to prepare statement '{}': {}", sql, sqlite3_errmsg(db));
    return std::nullopt;
  }

  std::optional<uint64_t> result;
  ret = sqlite3_step(stmt);
  if (ret == SQLITE_ROW) {
    result = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
  } else if (ret != SQLITE_DONE) {
    Log.error("Failed to execute statement '{}': {}", sql, sqlite3_errmsg(db));
  }

  sqlite3_finalize(stmt);
  return result;
}

static bool fill_used_key_table(const std::vector<XXH128_hash_t>& usedKeys) {
  auto ret = sqlite::exec(db,
                          "CREATE TEMP TABLE IF NOT EXISTS cache_keys_used ("
                          "key BLOB PRIMARY KEY NOT NULL"
                          ");"
                          "DELETE FROM cache_keys_used;");
  if (ret != SQLITE_OK) {
    Log.error("Failed to prepare dawn cache prune key table: {}", sqlite3_errmsg(db));
    return false;
  }

  sqlite3_stmt* stmt = nullptr;
  ret = sqlite3_prepare_v3(db, "INSERT OR IGNORE INTO cache_keys_used (key) VALUES (?)", -1, 0, &stmt, nullptr);
  if (ret != SQLITE_OK) {
    Log.error("Failed to prepare dawn cache prune key insert: {}", sqlite3_errmsg(db));
    return false;
  }

  for (const auto& keyHash : usedKeys) {
    ret = sqlite3_bind_blob(stmt, 1, &keyHash, sizeof(keyHash), SQLITE_TRANSIENT);
    if (ret != SQLITE_OK) {
      Log.error("Failed to bind dawn cache prune key: {}", sqlite3_errmsg(db));
      sqlite3_finalize(stmt);
      return false;
    }

    ret = sqlite3_step(stmt);
    if (ret != SQLITE_DONE) {
      Log.error("Failed to insert dawn cache prune key: {}", sqlite3_errmsg(db));
      sqlite3_finalize(stmt);
      return false;
    }

    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
  }

  sqlite3_finalize(stmt);
  return true;
}

// 0226: the first start after a reboot reads the blobs from disk in random order, one at a time
// (cache_mutex): 244 MB took 6.5 s from an NVMe SSD with Enhanced lighting. A sequential read of
// the whole file on a background thread puts it in the OS file cache first (0.2-0.5 s from an SSD,
// a few seconds from a hard disk); the lookups then find it there.
static void prefetch_file(const std::filesystem::path& path) {
  std::thread([path] {
    const int64_t t0 = stat_now_ns();
    std::ifstream in(path, std::ios::binary);
    std::vector<char> buf(size_t{4} << 20);
    uint64_t total = 0;
    while (in.read(buf.data(), static_cast<std::streamsize>(buf.size())) || in.gcount() > 0) {
      total += static_cast<uint64_t>(in.gcount());
    }
    Log.info("Dawn shader cache prefetched ({} MB in {:.0f} ms)", total >> 20, (stat_now_ns() - t0) / 1e6);
  }).detach();
}

static bool cache_init() {
  if (cache_broken) {
    return false;
  }

  if (reset_pending) {
    reset_pending = false;
    reset_corrupt_db();
  }

  if (db) {
    return true;
  }

  static bool hookCalled = false;
  if (!hookCalled) {
    hookCalled = true;
    if (g_cacheOpenHook != nullptr) {
      g_cacheOpenHook(); // may delete the cache files after a crash at startup
    }
  }

  if (!cache_init_core()) {
    // Not a database / damaged / wrong layout: recreate it rather than running without a cache.
    Log.warn("Dawn cache database is unusable; deleting and recreating it");
    close_db();
    delete_db_files();
    if (!cache_init_core()) {
      Log.error("SQLite DB init failed");
      init_abort();
      return false;
    }
  }

  entries_at_open = select_uint64("SELECT COUNT(*) FROM cache").value_or(0);
  Log.info("Dawn shader cache {} opened (session {}, {} entries)", io::fs_path_to_string(cache_path()), cache_session,
           entries_at_open);
  if (entries_at_open > 0) { // 0231: also after an unclean exit (no whole-file check any more)
    prefetch_file(cache_path());
  }

  return true;
}

// Reads, decompresses and verifies one entry with the given connection and lookup statement.
enum class ReadResult { Miss, Hit, Damaged, Corrupt };
static ReadResult read_verified_entry(sqlite3* conn, sqlite3_stmt* stmt, const XXH128_hash_t& keyHash,
                                      std::vector<uint8_t>& out) {
  sqlite3_bind_blob(stmt, 1, &keyHash, sizeof(keyHash), SQLITE_TRANSIENT);
  const auto ret = sqlite3_step(stmt);
  if (ret == SQLITE_DONE) {
    sqlite3_reset(stmt);
    return ReadResult::Miss;
  }
  if (ret != SQLITE_ROW) {
    Log.error("Looking up cache key failed: {}", sqlite3_errmsg(conn));
    sqlite3_reset(stmt);
    return is_corruption(ret) ? ReadResult::Corrupt : ReadResult::Miss;
  }

  const auto* foundPtr = static_cast<const uint8_t*>(sqlite3_column_blob(stmt, 0));
  const auto storedSize = sqlite3_column_bytes(stmt, 0);
  const auto size = sqlite3_column_int64(stmt, 1);
  const bool compressed = sqlite3_column_int(stmt, 2) != 0;
  const auto checksum = static_cast<uint64_t>(sqlite3_column_int64(stmt, 3));
  const char* problem = nullptr;
  if (size <= 0 || size > (int64_t{1} << 30) || (storedSize > 0 && foundPtr == nullptr)) {
    problem = "bad size";
  } else if (compressed) {
#if defined(AURORA_CACHE_USE_ZSTD)
    out.resize(static_cast<size_t>(size));
    const auto zstdRet = ZSTD_decompress(out.data(), out.size(), foundPtr, static_cast<size_t>(storedSize));
    if (ZSTD_isError(zstdRet) || zstdRet != out.size()) {
      problem = "decompression failed";
    }
#else
    problem = "zstd-compressed but zstd support is disabled";
#endif
  } else if (storedSize != size) {
    problem = "size mismatch";
  } else {
    out.assign(foundPtr, foundPtr + storedSize);
  }
  if (problem == nullptr && XXH3_64bits(out.data(), out.size()) != checksum) {
    problem = "checksum mismatch";
  }
  sqlite3_reset(stmt);
  if (problem != nullptr) {
    Log.warn("Dawn cache entry damaged ({}); deleting it", problem);
    out.clear();
    return ReadResult::Damaged;
  }
  return ReadResult::Hit;
}

// Deletes a damaged entry (main connection, cache_mutex held), so Dawn sees a miss and rebuilds it.
static void delete_entry(const XXH128_hash_t& keyHash) {
  if (db == nullptr) {
    return;
  }
  sqlite3_stmt* del = nullptr;
  if (sqlite3_prepare_v3(db, "DELETE FROM cache WHERE key = ?", -1, 0, &del, nullptr) == SQLITE_OK) {
    sqlite3_bind_blob(del, 1, &keyHash, sizeof(keyHash), SQLITE_TRANSIENT);
    sqlite3_step(del);
  }
  sqlite3_finalize(del);
}

// 0227: lookups run on a read connection of their own thread (WAL readers do not block each
// other): with one connection behind cache_mutex, the pipeline threads spent most of their lookup
// time waiting for each other. Writes, deletions and resets stay on the main connection under the
// mutex; a reader whose generation is out of date (main database closed or reset) reopens.
namespace {
struct Reader {
  sqlite3* conn = nullptr;
  sqlite3_stmt* stmt = nullptr;
  uint64_t generation = 0;
  bool failed = false; // could not be opened for this generation: use the main connection
  void close() {
    if (stmt != nullptr) {
      sqlite3_finalize(stmt);
      stmt = nullptr;
    }
    if (conn != nullptr) {
      sqlite3_close(conn);
      conn = nullptr;
    }
  }
  ~Reader() { close(); }
};
thread_local Reader t_reader;
// Dawn first asks for the size (value == nullptr), then reads: the entry verified on the size
// query is kept per thread for the read.
thread_local XXH128_hash_t t_verified_key{};
thread_local std::vector<uint8_t> t_verified_value;
thread_local bool t_verified_valid = false;
} // namespace

static bool open_reader(uint64_t generation) {
  t_reader.close();
  t_reader.generation = generation;
  t_reader.failed = true;
  const std::string file = io::fs_path_to_string(cache_path());
  if (sqlite3_open_v2(file.c_str(), &t_reader.conn, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, nullptr) !=
      SQLITE_OK) {
    t_reader.close();
    return false;
  }
  sqlite3_busy_timeout(t_reader.conn, 2000);
  if (sqlite3_prepare_v3(t_reader.conn, "SELECT value, size, compressed, checksum FROM cache WHERE key = ?", -1,
                         SQLITE_PREPARE_PERSISTENT, &t_reader.stmt, nullptr) != SQLITE_OK) {
    t_reader.close();
    return false;
  }
  t_reader.failed = false;
  return true;
}

static void note_hit(const XXH128_hash_t& keyHash) {
  {
    std::lock_guard lock(cache_mutex);
    cache_keys_used.push_back(keyHash);
  }
  stat_hits.fetch_add(1, std::memory_order_relaxed);
}

size_t load_from_cache(void const* key, size_t keySize, void* value, size_t valueSize, void*) {
  const StatTimer timer{stat_load_ns};
  const auto keyHash = XXH128(key, keySize, 0);
  if (value != nullptr && t_verified_valid && XXH128_isEqual(t_verified_key, keyHash)) {
    t_verified_valid = false;
    if (valueSize != t_verified_value.size()) {
      return 0;
    }
    std::memcpy(value, t_verified_value.data(), valueSize);
    note_hit(keyHash);
    return valueSize;
  }

  uint64_t generation;
  {
    std::lock_guard lock(cache_mutex);
    if (!cache_init()) {
      return 0;
    }
    generation = db_generation;
  }
  if (t_reader.generation != generation) {
    open_reader(generation);
  }

  std::vector<uint8_t> entry;
  ReadResult result;
  if (!t_reader.failed) {
    result = read_verified_entry(t_reader.conn, t_reader.stmt, keyHash, entry);
  } else {
    std::lock_guard lock(cache_mutex);
    if (!cache_init()) {
      return 0;
    }
    result = read_verified_entry(db, load_stmt, keyHash, entry);
  }
  if (result == ReadResult::Damaged || result == ReadResult::Corrupt) {
    std::lock_guard lock(cache_mutex);
    if (result == ReadResult::Damaged) {
      delete_entry(keyHash);
    } else {
      t_reader.close();
      t_reader.generation = 0;
      reset_corrupt_db();
    }
  }
  if (result != ReadResult::Hit) {
    stat_misses.fetch_add(1, std::memory_order_relaxed);
    return 0;
  }
  if (value == nullptr) {
    t_verified_key = keyHash;
    t_verified_value = std::move(entry);
    t_verified_valid = true;
    return t_verified_value.size();
  }
  if (valueSize != entry.size()) {
    return entry.size();
  }
  std::memcpy(value, entry.data(), valueSize);
  note_hit(keyHash);
  return valueSize;
}

void store_to_cache(void const* key, size_t keySize, void const* value, size_t valueSize, void*) {
  const StatTimer timer{stat_store_ns};
  std::lock_guard lock(cache_mutex);

  if (!cache_init()) {
    return;
  }

  sqlite::Transaction tx(db, Log, true);
  if (!tx) {
    Log.error("Failed to open store transaction");
    return;
  }

  const void* storedValue = value;
  sqlite3_uint64 storedValueSize = valueSize;
  int compressed = 0;
#if defined(AURORA_CACHE_USE_ZSTD)
  const auto bound = ZSTD_compressBound(valueSize);
  if (ZSTD_isError(bound)) {
    Log.error("Failed to calculate ZSTD_compressBound: {}", ZSTD_getErrorName(bound));
    return;
  }

  if (compress_buffer.size() < bound) {
    compress_buffer.resize(bound);
  }

  const auto compressRet = ZSTD_compress(compress_buffer.data(), compress_buffer.size(), value, valueSize, 0);
  if (ZSTD_isError(compressRet)) {
    Log.error("ZSTD compression error: {}", ZSTD_getErrorName(compressRet));
    return;
  }

  if (compressRet < valueSize) {
    storedValue = compress_buffer.data();
    storedValueSize = compressRet;
    compressed = 1;
  }
#endif

  const auto keyHash = XXH128(key, keySize, 0);
  check(sqlite3_bind_blob64(store_stmt, 1, &keyHash, sizeof(keyHash), SQLITE_TRANSIENT));
  check(
      sqlite3_bind_blob64(store_stmt, 2, storedValue, storedValueSize, compressed ? SQLITE_STATIC : SQLITE_TRANSIENT));
  check(sqlite3_bind_int64(store_stmt, 3, static_cast<sqlite3_int64>(valueSize)));
  check(sqlite3_bind_int(store_stmt, 4, compressed));
  check(sqlite3_bind_int64(store_stmt, 5, static_cast<sqlite3_int64>(XXH3_64bits(value, valueSize))));
  check(sqlite3_bind_int64(store_stmt, 6, cache_session));

  const auto ret = sqlite3_step(store_stmt);
  if (ret != SQLITE_DONE) {
    // Error or something
    Log.error("Failed to insert row: {}", sqlite3_errmsg(db));
    sqlite3_reset(store_stmt);
    if (is_corruption(ret)) {
      reset_pending = true;
    }
    return;
  }

  check(sqlite3_reset(store_stmt));
  check(sqlite3_bind_null(store_stmt, 2));
  check(sqlite3_bind_null(store_stmt, 4));

  tx.commit();
  cache_keys_used.push_back(keyHash);
  stat_stores.fetch_add(1, std::memory_order_relaxed);
}

void cache_prune() {
  std::lock_guard lock(cache_mutex);
  if (!cache_init()) {
    return;
  }
  if (cache_keys_used.empty()) {
    Log.warn("Skipping Dawn cache prune because no cache keys were used");
    return;
  }

  uint64_t totalRows = 0;
  sqlite3_int64 deletedRows = 0;
  {
    sqlite::Transaction tx(db, Log, true);
    if (!tx) {
      Log.error("Failed to open dawn cache prune transaction");
      return;
    }

    if (!fill_used_key_table(cache_keys_used)) {
      return;
    }

    const auto totalRowsResult = select_uint64("SELECT COUNT(*) FROM cache");
    if (!totalRowsResult) {
      return;
    }
    totalRows = *totalRowsResult;

    // 0226: mark what this session used so far, then drop only rows unused for several sessions.
    const auto mark = fmt::format("UPDATE cache SET last_used = {} WHERE key IN (SELECT key FROM cache_keys_used)",
                                  cache_session);
    if (sqlite::exec(db, mark.c_str()) != SQLITE_OK) {
      Log.error("Failed to mark used dawn cache rows: {}", sqlite3_errmsg(db));
      return;
    }
    const auto prune =
        fmt::format("DELETE FROM cache WHERE last_used <= {}", cache_session - PruneUnusedSessions);
    const auto ret = sqlite::exec(db, prune.c_str());
    if (ret != SQLITE_OK) {
      Log.error("Failed to prune dawn cache rows: {}", sqlite3_errmsg(db));
      return;
    }
    deletedRows = sqlite3_changes64(db);
    keys_marked_used = cache_keys_used.size();

    tx.commit();
  }

  if (deletedRows == 0) {
    Log.debug("Dawn cache prune completed; no stale entries found");
    return;
  }

  Log.info("Pruned {} Dawn cache entries unused for {} sessions", deletedRows, PruneUnusedSessions);

  // VACUUM if we removed at least 25% of the rows
  if (totalRows != 0 && static_cast<uint64_t>(deletedRows) * 100ull >= totalRows * VacuumPrunePercentThreshold) {
    if (const auto ret = sqlite::exec(db, "VACUUM;"); ret != SQLITE_OK) {
      Log.warn("Failed to vacuum dawn cache after pruning: {}", sqlite3_errmsg(db));
      return;
    }
  }

  if (const auto ret = sqlite::exec(db, "PRAGMA wal_checkpoint(TRUNCATE);"); ret != SQLITE_OK) {
    Log.warn("Failed to checkpoint dawn cache WAL: {}", sqlite3_errmsg(db));
  }
}

// 0226: entries read or written after the prune (or in a session without one) are marked used at
// shutdown, so the age-based prune never drops something a session needed.
static void mark_late_used_keys() {
  if (db == nullptr || cache_broken || keys_marked_used >= cache_keys_used.size()) {
    return;
  }
  sqlite::Transaction tx(db, Log, true);
  if (!tx) {
    return;
  }
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v3(db, "UPDATE cache SET last_used = ? WHERE key = ?", -1, 0, &stmt, nullptr) != SQLITE_OK) {
    Log.warn("Failed to prepare dawn cache usage update: {}", sqlite3_errmsg(db));
    return;
  }
  for (size_t i = keys_marked_used; i < cache_keys_used.size(); ++i) {
    sqlite3_bind_int64(stmt, 1, cache_session);
    sqlite3_bind_blob(stmt, 2, &cache_keys_used[i], sizeof(XXH128_hash_t), SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_reset(stmt);
  }
  sqlite3_finalize(stmt);
  tx.commit();
  keys_marked_used = cache_keys_used.size();
}

void cache_shutdown() {
  {
    std::lock_guard lock(cache_mutex);
    mark_late_used_keys();
    ++db_generation; // 0227: readers of other threads reopen (or stay closed)
    t_reader.close();
  }
#if defined(AURORA_CACHE_USE_ZSTD)
  compress_buffer.clear();
#endif
  cache_keys_used.clear();
  if (load_stmt != nullptr) {
    check(sqlite3_finalize(load_stmt));
    load_stmt = nullptr;
  }
  if (store_stmt != nullptr) {
    check(sqlite3_finalize(store_stmt));
    store_stmt = nullptr;
  }
  if (db != nullptr) {
    check(sqlite3_close(db));
    db = nullptr;
  }
}

} // namespace aurora::webgpu
