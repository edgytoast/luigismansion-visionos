#include "../activity.hpp"
#include "pipeline_cache.hpp"

#include "clear.hpp"
#include "enhanced.hpp"
#include "frame_packet.hpp"
#include "resources.hpp"
#include "hash.hpp"
#include "../gx/pipeline.hpp"
#include "../io.hpp"
#ifdef AURORA_ENABLE_RMLUI
#include "../rmlui/pipeline.hpp"
#endif
#include "../sqlite_utils.hpp"
#include "../webgpu/gpu.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <ranges>
#include <variant>
#include <thread>

#include <SDL3/SDL_iostream.h>
#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <fmt/format.h>
#include <tracy/Tracy.hpp>
#if defined(AURORA_CACHE_USE_ZSTD)
#include <zstd.h>
#endif

#include <aurora/pipeline_cache.h>
#include <dolphin/gx/GXAurora.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace aurora::gfx {
static Module Log("aurora::gfx::pipeline_cache");

// Precache bookkeeping (include/aurora/pipeline_cache.h).
static std::atomic<uint32_t> g_precacheTotal{0};
static std::atomic<uint32_t> g_precacheDone{0};
static std::atomic<uint32_t> g_demandCreated{0};
static std::atomic<uint32_t> g_stalledDraws{0};
static std::atomic<uint64_t> g_stallNs{0};
static std::atomic<uint64_t> g_maxStallNs{0};
static std::atomic<uint32_t> g_seeded{0};
static std::atomic_bool g_pipelineLog{false};
static std::atomic_bool g_waitForPipelines{true};
static std::atomic<uint32_t> g_droppedDraws{0};
// 0221: draws skipped for a missing pipeline (any pass): counted per second for the log, and the
// first skip of each pipeline is logged with what is known about its config (the MISS detail of
// on-demand pipelines, kept for that).
static std::atomic<uint32_t> g_skipWindow{0}, g_skipWindowCasters{0};
static std::mutex g_skipMutex;
static absl::flat_hash_set<PipelineRef> g_skipWarned;
static absl::flat_hash_map<PipelineRef, std::string> g_missDetails; // under g_pipelineMutex
static uint32_t g_requestedThreads = 0;
static uint32_t g_workerThreadCount = 0;
// 0227: extra workers (up to all hardware threads but one, at most 16) that only take work while the
// application boosts creation, e.g. while the game is held for a batch that loads from the cache
// (CPU bound, nothing else runs). g_activeWorkers of the g_pipelineThreads take work.
static std::atomic<uint32_t> g_activeWorkers{0};
static uint32_t g_boostWorkers = 0;
static std::condition_variable g_boostCv;
static std::optional<uint32_t> g_rebuildAccuracy; // GX accuracy flags the known set was last built for
// 0217: enhanced-lighting variant bits (enhanced::precache_bits) the known set was last built for,
// and the pipelines a rebuild queued that are not built yet (the application holds the game
// while they compile: aurora_pipeline_cache_get_stats rebuildPending).
static std::optional<uint32_t> g_rebuildEnhBits;
static std::atomic<uint32_t> g_rebuildPending{0};
static std::atomic<uint32_t> g_rebuildCount{0};    // 0226: rebuilds so far (a new batch for the hold)
static std::atomic<bool> g_prewarmPending{false};  // 0226: enhanced::start_prewarm still running
static thread_local bool g_inRebuild = false; // set while rebuild_pipeline_cache queues (its thread)

uint64_t now_ns() noexcept {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

// 0226: GX pipeline creation time by stage (aurora_pipeline_cache_get_stats).
static std::atomic<uint64_t> g_createNs[static_cast<size_t>(CreateStage::Count)]{};
void add_create_time(CreateStage stage, uint64_t ns) noexcept {
  g_createNs[static_cast<size_t>(stage)].fetch_add(ns, std::memory_order_relaxed);
}
static std::atomic<uint32_t> g_moduleReuse{0};
void note_module_reuse() noexcept { g_moduleReuse.fetch_add(1, std::memory_order_relaxed); }

// 2: every row is validated on load (stored hash = xxh3 of the config bytes seeded by type,
// i.e. a per-entry checksum, plus field ranges) and pipeline_bad records configs whose creation
// failed or crashed (patch 0017). Older caches are dropped.
constexpr int PipelineCacheSchema = 2;
constexpr const char* InitialPipelineCacheName = "initial_pipeline_cache.db";
constexpr const char* SdlVfsName = "aurora_pipeline_cache_sdl_vfs";

using NewPipelineCallback = std::function<CompiledPipeline()>;

struct PendingPipeline {
  PipelineRef hash;
  NewPipelineCallback create;
  bool precache = false; // queued by background precaching (rebuild_pipeline_cache)
  bool rebuild = false;  // 0217: queued by a rebuild (counted in g_rebuildPending)
};

enum class CacheWriteOp : uint8_t {
  Upsert,   // remember a config (pipeline_cache row)
  MarkBad,  // record a failed/crashed config (pipeline_bad row; Invalid also drops its pipeline_cache row)
  ClearBad, // a quarantined config was created fine after all
};

struct PipelineCacheWrite {
  ShaderType type;
  PipelineRef hash;
  uint32_t configVersion;
  ByteBuffer config;
  uint32_t firstFrameUsed = UINT32_MAX;
  CacheWriteOp op = CacheWriteOp::Upsert;
  uint32_t badReason = 0;
  uint32_t badCrashes = 0;
};

// ---- Robustness (patch 0017): bad configs, crash journal ------------------------------------
// A cached config that fails validation is skipped and deleted; one whose pipeline creation
// fails (WGSL compile / Dawn validation error) is recorded as Invalid and never built again; one
// that was being created when the process died (crash journal below) is a CrashSuspect: it is not
// precached any more, may still be built on demand (which lifts the quarantine when it works) and
// is refused after a second crash. Keyed by the config's cache key (xxh3 of the config, seeded by
// its shader type), the same value as pipeline_cache.hash.
enum class BadReason : uint32_t {
  Invalid = 1,
  CrashSuspect = 2,
};
struct BadPipeline {
  ShaderType type;
  BadReason reason;
  uint32_t crashes;
};
static std::mutex g_badMutex;
static absl::flat_hash_map<HashType, BadPipeline> g_badPipelines;
// Crash suspects found by the journal at startup, recorded once the database is open.
static std::vector<std::pair<ShaderType, HashType>> g_journalSuspects;
static bool g_safeMode = false; // repeated startup crashes: ignore the shipped precache list
// 0231: how the previous session ended (0 clean, 1 closed or killed, 2 crashed) and whether the
// caches were reset after repeated startup crashes (aurora_pipeline_cache_get_stats).
static uint32_t g_prevExit = 0;
static bool g_cachesReset = false;
static void mark_pipeline_bad(ShaderType type, HashType cacheKey, BadReason reason, const char* why);
static void journal_begin(ShaderType type, HashType cacheKey);
static void journal_end();
static void journal_update_phase();
static void journal_close();
void check_crash_journal();

using SavedPipelineConfig = std::variant<gx::PipelineConfig, clear::PipelineConfig
#ifdef AURORA_ENABLE_RMLUI
                                         ,
                                         rmlui::PipelineConfig
#endif
                                         >;

struct KnownPipeline {
  ShaderType type;
  SavedPipelineConfig config;
  uint32_t firstFrameUsed;
};

struct SdlVfsSqliteFile {
  sqlite3_file base;
  SDL_IOStream* io = nullptr;
};

static std::mutex g_pipelineMutex;
static bool g_hasPipelineThread = false;
static size_t g_pipelinesPerFrame = 0;
// For synchronous pipeline fallback (OpenGL)
#ifdef NDEBUG
constexpr size_t BuildPipelinesPerFrame = 5;
#else
constexpr size_t BuildPipelinesPerFrame = 1;
#endif
static std::vector<std::thread> g_pipelineThreads;
static std::atomic_bool g_pipelineThreadEnd = false;
static std::condition_variable g_pipelineQueueCv;
static std::condition_variable g_pipelineReadyCv;
static absl::flat_hash_map<PipelineRef, CompiledPipeline> g_pipelines;
static absl::flat_hash_map<HashType, KnownPipeline> g_knownPipelines;
static std::optional<uint64_t> g_pipelineLayoutKey;
static std::deque<PendingPipeline> g_pipelineQueue;
static std::deque<PendingPipeline> g_backgroundPipelineQueue;
static absl::flat_hash_set<PipelineRef> g_pendingPipelines;
static std::atomic_bool g_gpuCachePrunePending = false;
static bool g_gpuCachePruneArmed = false; // prune once the first full rebuild has been queued

static sqlite3* g_pipelineCacheDb = nullptr;
static sqlite3_stmt* g_pipelineCacheLoadStmt = nullptr;
static sqlite3_stmt* g_pipelineCacheUpsertStmt = nullptr;
static bool g_pipelineCacheBroken = false;
static std::thread g_pipelineCacheWriterThread;
static std::condition_variable g_pipelineCacheWriterCv;
static std::mutex g_pipelineCacheWriterMutex;
static std::deque<PipelineCacheWrite> g_pipelineCacheWriteQueue;
static bool g_pipelineCacheWriterStop = false;
static int g_sdlVfsRegisterResult = SQLITE_ERROR;

static SdlVfsSqliteFile* sdl_vfs_file(sqlite3_file* file) { return reinterpret_cast<SdlVfsSqliteFile*>(file); }

static sqlite3_vfs* default_vfs(sqlite3_vfs* vfs) { return static_cast<sqlite3_vfs*>(vfs->pAppData); }

static int sdl_vfs_close(sqlite3_file* file) {
  auto* vfsFile = sdl_vfs_file(file);
  if (vfsFile->io != nullptr) {
    SDL_CloseIO(vfsFile->io);
    vfsFile->io = nullptr;
  }
  return SQLITE_OK;
}

static int sdl_vfs_read(sqlite3_file* file, void* buffer, int amount, sqlite3_int64 offset) {
  auto* vfsFile = sdl_vfs_file(file);
  if (vfsFile->io == nullptr || offset < 0 || amount < 0) {
    return SQLITE_IOERR_READ;
  }
  if (SDL_SeekIO(vfsFile->io, offset, SDL_IO_SEEK_SET) < 0) {
    return SQLITE_IOERR_SEEK;
  }

  auto* dst = static_cast<uint8_t*>(buffer);
  int total = 0;
  while (total < amount) {
    const size_t read = SDL_ReadIO(vfsFile->io, dst + total, static_cast<size_t>(amount - total));
    if (read == 0) {
      if (SDL_GetIOStatus(vfsFile->io) == SDL_IO_STATUS_EOF) {
        std::memset(dst + total, 0, static_cast<size_t>(amount - total));
        return SQLITE_IOERR_SHORT_READ;
      }
      return SQLITE_IOERR_READ;
    }
    if (read > static_cast<size_t>(std::numeric_limits<int>::max() - total)) {
      return SQLITE_IOERR_READ;
    }
    total += static_cast<int>(read);
  }
  return SQLITE_OK;
}

static int sdl_vfs_write(sqlite3_file*, const void*, int, sqlite3_int64) { return SQLITE_READONLY; }

static int sdl_vfs_truncate(sqlite3_file*, sqlite3_int64) { return SQLITE_READONLY; }

static int sdl_vfs_sync(sqlite3_file*, int) { return SQLITE_OK; }

static int sdl_vfs_file_size(sqlite3_file* file, sqlite3_int64* size) {
  auto* vfsFile = sdl_vfs_file(file);
  if (vfsFile->io == nullptr || size == nullptr) {
    return SQLITE_IOERR_FSTAT;
  }

  const auto ioSize = SDL_GetIOSize(vfsFile->io);
  if (ioSize < 0) {
    return SQLITE_IOERR_FSTAT;
  }
  *size = static_cast<sqlite3_int64>(ioSize);
  return SQLITE_OK;
}

static int sdl_vfs_lock(sqlite3_file*, int) { return SQLITE_OK; }

static int sdl_vfs_unlock(sqlite3_file*, int) { return SQLITE_OK; }

static int sdl_vfs_check_reserved_lock(sqlite3_file*, int* reserved) {
  if (reserved != nullptr) {
    *reserved = 0;
  }
  return SQLITE_OK;
}

static int sdl_vfs_file_control(sqlite3_file*, int op, void* arg) {
  switch (op) {
  case SQLITE_FCNTL_LOCKSTATE:
    *static_cast<int*>(arg) = SQLITE_LOCK_NONE;
    return SQLITE_OK;
  case SQLITE_FCNTL_HAS_MOVED:
    *static_cast<int*>(arg) = 0;
    return SQLITE_OK;
  case SQLITE_FCNTL_SIZE_HINT:
    return SQLITE_OK;
  default:
    return SQLITE_NOTFOUND;
  }
}

static int sdl_vfs_sector_size(sqlite3_file*) { return 4096; }

static int sdl_vfs_device_characteristics(sqlite3_file*) {
  return SQLITE_IOCAP_IMMUTABLE | SQLITE_IOCAP_UNDELETABLE_WHEN_OPEN;
}

static constexpr sqlite3_io_methods SdlVfsIoMethods{
    .iVersion = 1,
    .xClose = sdl_vfs_close,
    .xRead = sdl_vfs_read,
    .xWrite = sdl_vfs_write,
    .xTruncate = sdl_vfs_truncate,
    .xSync = sdl_vfs_sync,
    .xFileSize = sdl_vfs_file_size,
    .xLock = sdl_vfs_lock,
    .xUnlock = sdl_vfs_unlock,
    .xCheckReservedLock = sdl_vfs_check_reserved_lock,
    .xFileControl = sdl_vfs_file_control,
    .xSectorSize = sdl_vfs_sector_size,
    .xDeviceCharacteristics = sdl_vfs_device_characteristics,
};

static int sdl_vfs_open(sqlite3_vfs*, sqlite3_filename name, sqlite3_file* file, int flags, int* outFlags) {
  auto* vfsFile = sdl_vfs_file(file);
  vfsFile->base.pMethods = nullptr;
  vfsFile->io = nullptr;

  if (name == nullptr || (flags & SQLITE_OPEN_READWRITE) != 0 || (flags & SQLITE_OPEN_READONLY) == 0) {
    return SQLITE_CANTOPEN;
  }

  vfsFile->io = SDL_IOFromFile(name, "rb");
  if (vfsFile->io == nullptr) {
    return SQLITE_CANTOPEN;
  }

  vfsFile->base.pMethods = &SdlVfsIoMethods;
  if (outFlags != nullptr) {
    *outFlags = SQLITE_OPEN_READONLY;
  }
  return SQLITE_OK;
}

static int sdl_vfs_delete(sqlite3_vfs*, const char*, int) { return SQLITE_READONLY; }

static int sdl_vfs_access(sqlite3_vfs*, const char* name, int flags, int* result) {
  if (result == nullptr) {
    return SQLITE_IOERR_ACCESS;
  }
  if (name == nullptr || flags == SQLITE_ACCESS_READWRITE) {
    *result = 0;
    return SQLITE_OK;
  }

  auto* io = SDL_IOFromFile(name, "rb");
  *result = io != nullptr ? 1 : 0;
  if (io != nullptr) {
    SDL_CloseIO(io);
  }
  return SQLITE_OK;
}

static int sdl_vfs_full_pathname(sqlite3_vfs*, const char* name, int outSize, char* out) {
  if (name == nullptr || out == nullptr || outSize <= 0) {
    return SQLITE_CANTOPEN;
  }
  sqlite3_snprintf(outSize, out, "%s", name);
  return SQLITE_OK;
}

static void* sdl_vfs_dl_open(sqlite3_vfs*, const char*) { return nullptr; }

static void sdl_vfs_dl_error(sqlite3_vfs*, int bytes, char* message) {
  if (message != nullptr && bytes > 0) {
    sqlite3_snprintf(bytes, message, "%s", "Dynamic loading is unsupported");
  }
}

static void (*sdl_vfs_dl_sym(sqlite3_vfs*, void*, const char*))(void) { return nullptr; }

static void sdl_vfs_dl_close(sqlite3_vfs*, void*) {}

static int sdl_vfs_randomness(sqlite3_vfs* vfs, int bytes, char* out) {
  if (auto* base = default_vfs(vfs); base != nullptr && base->xRandomness != nullptr) {
    return base->xRandomness(base, bytes, out);
  }
  if (out != nullptr && bytes > 0) {
    std::memset(out, 0, static_cast<size_t>(bytes));
  }
  return bytes;
}

static int sdl_vfs_sleep(sqlite3_vfs* vfs, int microseconds) {
  if (auto* base = default_vfs(vfs); base != nullptr && base->xSleep != nullptr) {
    return base->xSleep(base, microseconds);
  }
  return microseconds;
}

static int sdl_vfs_current_time(sqlite3_vfs* vfs, double* time) {
  if (auto* base = default_vfs(vfs); base != nullptr && base->xCurrentTime != nullptr) {
    return base->xCurrentTime(base, time);
  }
  if (time != nullptr) {
    *time = 2440587.5;
  }
  return SQLITE_OK;
}

static int sdl_vfs_get_last_error(sqlite3_vfs*, int, char*) { return SQLITE_OK; }

static bool register_sdl_vfs() {
  static std::once_flag registerOnce;
  std::call_once(registerOnce, [] {
    auto* baseVfs = sqlite3_vfs_find(nullptr);
    static sqlite3_vfs sdlVfs{
        .iVersion = 1,
        .szOsFile = static_cast<int>(sizeof(SdlVfsSqliteFile)),
        .mxPathname = 4096,
        .pNext = nullptr,
        .zName = SdlVfsName,
        .pAppData = baseVfs,
        .xOpen = sdl_vfs_open,
        .xDelete = sdl_vfs_delete,
        .xAccess = sdl_vfs_access,
        .xFullPathname = sdl_vfs_full_pathname,
        .xDlOpen = sdl_vfs_dl_open,
        .xDlError = sdl_vfs_dl_error,
        .xDlSym = sdl_vfs_dl_sym,
        .xDlClose = sdl_vfs_dl_close,
        .xRandomness = sdl_vfs_randomness,
        .xSleep = sdl_vfs_sleep,
        .xCurrentTime = sdl_vfs_current_time,
        .xGetLastError = sdl_vfs_get_last_error,
    };
    g_sdlVfsRegisterResult = sqlite3_vfs_register(&sdlVfs, 0);
  });
  return g_sdlVfsRegisterResult == SQLITE_OK;
}

#if defined(__cpp_lib_atomic_ref)
static std::atomic_ref queuedPipelines{detail::resources().stats.queuedPipelines};
static std::atomic_ref createdPipelines{detail::resources().stats.createdPipelines};
#else
struct AtomicStatRef {
  uint32_t& ref;
  uint32_t operator++() { return __atomic_add_fetch(&ref, 1, __ATOMIC_RELAXED); }
  uint32_t operator--() { return __atomic_sub_fetch(&ref, 1, __ATOMIC_RELAXED); }
  uint32_t operator++(int) { return __atomic_fetch_add(&ref, 1, __ATOMIC_RELAXED); }
  uint32_t operator--(int) { return __atomic_fetch_sub(&ref, 1, __ATOMIC_RELAXED); }
  uint32_t operator+=(uint32_t val) { return __atomic_add_fetch(&ref, val, __ATOMIC_RELAXED); }
  uint32_t operator=(uint32_t val) {
    __atomic_store_n(&ref, val, __ATOMIC_RELAXED);
    return val;
  }
};
static AtomicStatRef queuedPipelines{detail::resources().stats.queuedPipelines};
static AtomicStatRef createdPipelines{detail::resources().stats.createdPipelines};
#endif

template <typename PipelineConfig>
static PipelineCacheWrite make_pipeline_cache_write(ShaderType type, PipelineRef hash, const PipelineConfig& config,
                                                    uint32_t firstFrameUsed) {
  static_assert(std::has_unique_object_representations_v<PipelineConfig>);

  PipelineCacheWrite write{
      .type = type,
      .hash = hash,
      .configVersion = config.version,
      .config = ByteBuffer(sizeof(config)),
      .firstFrameUsed = firstFrameUsed,
  };
  std::memcpy(write.config.data(), &config, sizeof(config));
  return write;
}

static void enqueue_pipeline_cache_write(PipelineCacheWrite write) {
  if (g_pipelineCacheBroken || g_pipelineCacheDb == nullptr) {
    return;
  }

  {
    std::lock_guard lock{g_pipelineCacheWriterMutex};
    g_pipelineCacheWriteQueue.emplace_back(std::move(write));
  }
  g_pipelineCacheWriterCv.notify_one();
}

template <typename Queue>
static auto find_pending_pipeline(Queue& queue, PipelineRef hash) {
  return std::find_if(queue.begin(), queue.end(), [=](const PendingPipeline& pending) { return pending.hash == hash; });
}

enum class PipelinePriority {
  Background, // cache warmup
  Normal,     // async skip draw
  Blocking,   // block until compiled
};

static void promote_pending_pipeline(PipelineRef hash, PipelinePriority priority) {
  if (priority == PipelinePriority::Background) {
    return;
  }

  auto backgroundIt = find_pending_pipeline(g_backgroundPipelineQueue, hash);
  if (backgroundIt == g_backgroundPipelineQueue.end()) {
    return;
  }
  if (priority == PipelinePriority::Blocking) {
    g_pipelineQueue.emplace_front(std::move(*backgroundIt));
  } else {
    g_pipelineQueue.emplace_back(std::move(*backgroundIt));
  }
  g_backgroundPipelineQueue.erase(backgroundIt);
}

static std::optional<PendingPipeline> take_pending_pipeline(PipelineRef hash) {
  auto priorityIt = find_pending_pipeline(g_pipelineQueue, hash);
  if (priorityIt != g_pipelineQueue.end()) {
    PendingPipeline pending = std::move(*priorityIt);
    g_pipelineQueue.erase(priorityIt);
    g_pendingPipelines.erase(hash);
    return pending;
  }

  auto backgroundIt = find_pending_pipeline(g_backgroundPipelineQueue, hash);
  if (backgroundIt != g_backgroundPipelineQueue.end()) {
    PendingPipeline pending = std::move(*backgroundIt);
    g_backgroundPipelineQueue.erase(backgroundIt);
    g_pendingPipelines.erase(hash);
    return pending;
  }

  return std::nullopt;
}

static void notify_pipeline_ready(bool queued, uint32_t pipelineCount) {
  createdPipelines += pipelineCount;
  if (queued && --queuedPipelines == 0 && g_gpuCachePrunePending.exchange(false, std::memory_order_acq_rel)) {
    // Prune GPU cache entries after fully loading the pipeline cache.
    webgpu::cache_prune();
  }
  g_pipelineReadyCv.notify_all();
}

// 0217: known configs are kept in their canonical form (gx::canonicalize_pipeline_config), so lists
// and caches recorded before it collapse to one entry per shader and an export is canonical.
template <typename Config>
static Config canonical_known(const Config& config) {
  if constexpr (std::is_same_v<Config, gx::PipelineConfig>) {
    auto c = config;
    gx::canonicalize_pipeline_config(c);
    return c;
  } else {
    return config;
  }
}

template <typename Config>
static bool remember_pipeline_config(ShaderType type, const Config& config, uint32_t firstFrameUsed, bool persist) {
  const auto cacheKey = xxh3_hash(config, static_cast<HashType>(type));
  bool changed = false;
  bool isNew = false;
  {
    std::lock_guard lock{g_pipelineMutex};
    auto [it, inserted] = g_knownPipelines.try_emplace(cacheKey, KnownPipeline{type, config, firstFrameUsed});
    changed = inserted || firstFrameUsed < it->second.firstFrameUsed;
    isNew = inserted;
    it->second.firstFrameUsed = std::min(it->second.firstFrameUsed, firstFrameUsed);
  }
  if (persist && changed) {
    enqueue_pipeline_cache_write(make_pipeline_cache_write(type, cacheKey, config, firstFrameUsed));
  }
  return isNew;
}

static CompiledPipeline create_pending(PendingPipeline& pending);

// 0217: what a MISS log line says about the pipeline (set by resolve_pipeline on this thread).
static thread_local const gx::PipelineConfig* tl_missConfig = nullptr;
static thread_local const RenderTargetLayout* tl_missLayout = nullptr;
static thread_local HashType tl_missCacheKey = 0;
static std::string miss_detail() {
  if (tl_missConfig == nullptr || tl_missLayout == nullptr) {
    return {};
  }
  const auto& c = *tl_missConfig;
  const auto& l = *tl_missLayout;
  const auto& sc = c.shaderConfig;
  return fmt::format(" config {:016x} layout {:016x} ({} color {} depth {} x{}) enh{} dO{} lD{} efb{} dith{} eb{} "
                     "blend {} {}/{} op {} cUaUzU {}{}{} zc{} zf{} cull {} dA {}",
                     tl_missCacheKey, l.key, l.colorAttachmentCount,
                     l.colorAttachmentCount ? static_cast<uint32_t>(l.colorAttachments[0].format) : 0u,
                     static_cast<uint32_t>(l.depthStencilFormat), l.sampleCount, int(sc.enhClass), int(sc.depthOnly),
                     int(sc.lightDepth), int(sc.efbFmt), int(sc.dither), int(sc.efbBlend), int(c.blendMode),
                     int(c.blendFacSrc), int(c.blendFacDst), int(c.blendOp), int(c.colorUpdate), int(c.alphaUpdate),
                     int(c.depthUpdate), int(c.depthCompare), int(c.depthFunc), int(c.cullMode),
                     c.dstAlpha == UINT32_MAX ? -1 : int(c.dstAlpha));
}

static PipelineRef find_pipeline_impl(PipelineRef runtimeKey, NewPipelineCallback&& cb,
                                      PipelinePriority priority = PipelinePriority::Normal) {
  ZoneScoped;

  const bool blocking = priority == PipelinePriority::Blocking;
  bool notifyWorker = false;
  bool pipelineReady = false;
  bool createdPipeline = false;
  uint32_t pipelineCount = 0;
  bool queued = false;
  {
    std::scoped_lock guard{g_pipelineMutex};
    auto pipelineIt = g_pipelines.find(runtimeKey);
    if (pipelineIt != g_pipelines.end()) {
      pipelineReady = true;
    } else if (g_pendingPipelines.contains(runtimeKey)) {
      if (blocking && !g_hasPipelineThread) {
        auto pending = take_pending_pipeline(runtimeKey);
        if (pending) {
          auto result = create_pending(*pending);
          pipelineCount = result.pipeline_count();
          g_pipelines.try_emplace(runtimeKey, std::move(result));
          pipelineReady = true;
          g_pipelinesPerFrame += std::max(1u, pipelineCount);
          createdPipeline = true;
          queued = true;
        }
      } else {
        // 0221: a draw needs a pipeline that is still compiling (precached or not): keep its config
        // for a skipped-draw warning.
        if (priority != PipelinePriority::Background && g_missDetails.size() < 4096 && !g_missDetails.contains(runtimeKey)) {
          g_missDetails.emplace(runtimeKey, miss_detail());
        }
        promote_pending_pipeline(runtimeKey, priority);
        notifyWorker = priority != PipelinePriority::Background;
      }
    } else if (!g_hasPipelineThread && (blocking || g_pipelinesPerFrame < BuildPipelinesPerFrame)) {
      if (priority != PipelinePriority::Background) {
        ++g_demandCreated;
      }
      auto result = cb();
      pipelineCount = result.pipeline_count();
      g_pipelines.try_emplace(runtimeKey, std::move(result));
      pipelineReady = true;
      g_pipelinesPerFrame += std::max(1u, pipelineCount);
      createdPipeline = true;
    } else {
      PendingPipeline pending{
          .hash = runtimeKey,
          .create = std::move(cb),
          .precache = priority == PipelinePriority::Background,
          .rebuild = priority == PipelinePriority::Background && g_inRebuild,
      };
      if (priority == PipelinePriority::Background) {
        ++g_precacheTotal;
        if (pending.rebuild) {
          ++g_rebuildPending;
        }
      } else {
        // A draw needs a pipeline nothing precached: a gap in the precache list.
        ++g_demandCreated;
        std::string detail = miss_detail();
        if (g_pipelineLog.load(std::memory_order_relaxed)) {
          Log.info("pipeline {:016x}: MISS (not precached) frame {}{}", runtimeKey, current_frame(), detail);
        }
        if (g_missDetails.size() < 4096) {
          g_missDetails.insert_or_assign(runtimeKey, std::move(detail)); // 0221: for a skipped-draw warning
        }
      }
      switch (priority) {
      case PipelinePriority::Background:
        g_backgroundPipelineQueue.emplace_back(std::move(pending));
        break;
      case PipelinePriority::Normal:
        g_pipelineQueue.emplace_back(std::move(pending));
        break;
      case PipelinePriority::Blocking:
        g_pipelineQueue.emplace_front(std::move(pending));
        break;
      }
      g_pendingPipelines.insert(runtimeKey);
      ++queuedPipelines;
      notifyWorker = true;
    }
  }

  if (createdPipeline) {
    notify_pipeline_ready(queued, pipelineCount);
  }

  if (notifyWorker) {
    g_pipelineQueueCv.notify_one();
  }

  if (blocking && !pipelineReady) {
    std::unique_lock lock{g_pipelineMutex};
    g_pipelineReadyCv.wait(lock, [=] { return g_pipelines.contains(runtimeKey) || g_pipelineThreadEnd; });
  }
  return runtimeKey;
}

// ---- Config validation (patch 0017) --------------------------------------------------------
// Configs read back from pipeline_cache.db or a precache list are checked before they reach the
// shader generator, whose switches abort (FATAL) on values it does not know and which indexes
// fixed-size arrays with stage/texcoord/texmap ids. A corrupt or stale entry is skipped instead.
// The checks mirror lib/gx/shader.cpp, shader_info.cpp and gx.cpp (build_pipeline); anything
// they miss is still caught by the crash journal.
namespace {
template <typename T>
bool raw_bool_ok(const T& value) {
  static_assert(sizeof(T) == 1);
  uint8_t raw;
  std::memcpy(&raw, &value, 1);
  return raw <= 1;
}

bool tev_op_ok(const gx::TevOp& op) {
  const auto o = static_cast<uint32_t>(op.op);
  if (!(o == GX_TEV_ADD || o == GX_TEV_SUB || (o >= GX_TEV_COMP_R8_GT && o <= GX_TEV_COMP_RGB8_EQ))) {
    return false;
  }
  return static_cast<uint32_t>(op.bias) < GX_MAX_TEVBIAS && static_cast<uint32_t>(op.scale) < GX_MAX_TEVSCALE &&
         static_cast<uint32_t>(op.outReg) < GX_MAX_TEVREG && raw_bool_ok(op.clamp) && op._p1 == 0 && op._p2 == 0 &&
         op._p3 == 0;
}

bool tex_mtx_ok(uint32_t m) { return m >= GX_TEXMTX0 && m <= GX_IDENTITY && (m - GX_TEXMTX0) % 3 == 0; }
bool pt_tex_mtx_ok(uint32_t m) {
  return m == GX_PTIDENTITY || (m >= GX_PTTEXMTX0 && m <= GX_PTTEXMTX19 && (m - GX_PTTEXMTX0) % 3 == 0);
}

bool uses_tex_arg(const gx::TevStage& stage) {
  const auto& c = stage.colorPass;
  const auto& a = stage.alphaPass;
  const auto tc = [](GXTevColorArg arg) { return arg == GX_CC_TEXC || arg == GX_CC_TEXA; };
  return tc(c.a) || tc(c.b) || tc(c.c) || tc(c.d) || a.a == GX_CA_TEXA || a.b == GX_CA_TEXA || a.c == GX_CA_TEXA ||
         a.d == GX_CA_TEXA;
}

// Returns nullptr when the config is safe to hand to the shader generator, else the reason.
const char* validate_config(const gx::PipelineConfig& c) {
  using namespace aurora::gx;
  if (c.version != GXPipelineConfigVersion) {
    return "config version";
  }
  if (static_cast<uint32_t>(c.depthFunc) > GX_ALWAYS || static_cast<uint32_t>(c.cullMode) > GX_CULL_ALL ||
      static_cast<uint32_t>(c.blendMode) >= GX_MAX_BLENDMODE ||
      static_cast<uint32_t>(c.blendFacSrc) > GX_BL_INVDSTALPHA ||
      static_cast<uint32_t>(c.blendFacDst) > GX_BL_INVDSTALPHA || static_cast<uint32_t>(c.blendOp) > GX_LO_SET) {
    return "depth/cull/blend state";
  }
  if (!raw_bool_ok(c.depthCompare) || !raw_bool_ok(c.depthUpdate) || !raw_bool_ok(c.alphaUpdate) ||
      !raw_bool_ok(c.colorUpdate)) {
    return "update flags";
  }
  const auto& s = c.shaderConfig;
  if (s.logicOut > 2 || s.pad0 != 0) {
    return "logic op source";
  }
  switch (s.fogType) {
  case GX_FOG_NONE:
  case GX_FOG_PERSP_LIN:
  case GX_FOG_PERSP_EXP:
  case GX_FOG_PERSP_EXP2:
  case GX_FOG_PERSP_REVEXP:
  case GX_FOG_PERSP_REVEXP2:
  case GX_FOG_ORTHO_LIN:
  case GX_FOG_ORTHO_EXP:
  case GX_FOG_ORTHO_EXP2:
  case GX_FOG_ORTHO_REVEXP:
  case GX_FOG_ORTHO_REVEXP2:
    break;
  default:
    return "fog type";
  }
  if (s.zTexOp > GX_ZT_REPLACE || s.zTexFmt > 2 || s.efbFmt > 2) {
    return "z texture / EFB format";
  }
  if (s.tevStageCount < 1 || s.tevStageCount > MaxTevStages) {
    return "TEV stage count";
  }
  if (s.numIndStages > MaxIndStages) {
    return "indirect stage count";
  }
  for (const auto& swap : s.tevSwapTable) {
    if (swap.red > GX_CH_ALPHA || swap.green > GX_CH_ALPHA || swap.blue > GX_CH_ALPHA || swap.alpha > GX_CH_ALPHA) {
      return "TEV swap table";
    }
  }
  for (uint32_t attr = 0; attr < s.attrs.size(); ++attr) {
    const auto& a = s.attrs[attr];
    if (a.attrType > GX_INDEX16 || !raw_bool_ok(a.le) || !raw_bool_ok(a.nbt3)) {
      return "vertex attribute";
    }
    const bool fetched = attr == GX_VA_POS || attr == GX_VA_NRM || attr == GX_VA_CLR0 || attr == GX_VA_CLR1 ||
                         (attr >= GX_VA_TEX0 && attr <= GX_VA_TEX7);
    if (a.attrType != GX_NONE && fetched && a.compType > GX_RGBA8) {
      return "vertex component type";
    }
  }
  if (s.attrs[GX_VA_POS].attrType == GX_NONE) {
    return "no position attribute";
  }
  const auto& nrm = s.attrs[GX_VA_NRM];
  const bool hasNbt = nrm.attrType != GX_NONE && nrm.cnt == 9 && nrm.compType <= GX_F32;
  for (const auto& cc : s.colorChannels) {
    if (cc.matSrc > GX_SRC_VTX || cc.ambSrc > GX_SRC_VTX || cc.diffFn > GX_DF_CLAMP || cc.attnFn > GX_AF_NONE ||
        !raw_bool_ok(cc.lightingEnabled) || cc._p1 != 0 || cc._p2 != 0 || cc._p3 != 0) {
      return "color channel";
    }
  }
  uint32_t sampledCoords = 0;
  for (uint32_t i = 0; i < s.tevStageCount; ++i) {
    const auto& st = s.tevStages[i];
    const auto& cp = st.colorPass;
    const auto& ap = st.alphaPass;
    if (cp.a > GX_CC_ZERO || cp.b > GX_CC_ZERO || cp.c > GX_CC_ZERO || cp.d > GX_CC_ZERO || ap.a > GX_CA_ZERO ||
        ap.b > GX_CA_ZERO || ap.c > GX_CA_ZERO || ap.d > GX_CA_ZERO) {
      return "TEV argument";
    }
    if (!tev_op_ok(st.colorOp) || !tev_op_ok(st.alphaOp)) {
      return "TEV operation";
    }
    const auto kc = static_cast<uint32_t>(st.kcSel);
    const auto ka = static_cast<uint32_t>(st.kaSel);
    if (!(kc <= GX_TEV_KCSEL_1_8 || (kc >= GX_TEV_KCSEL_K0 && kc <= GX_TEV_KCSEL_K3_A)) ||
        !(ka <= GX_TEV_KASEL_1_8 || (ka >= GX_TEV_KASEL_K0_R && ka <= GX_TEV_KASEL_K3_A))) {
      return "TEV konst selection";
    }
    if ((st.texCoordId != GX_TEXCOORD_NULL && st.texCoordId >= MaxTexCoord) ||
        (st.texMapId != GX_TEXMAP_NULL && st.texMapId >= MaxTextures) ||
        (st.channelId != GX_COLOR_NULL && st.channelId > GX_ALPHA_BUMPN)) {
      return "TEV order";
    }
    if (st.tevSwapRas >= GX_MAX_TEVSWAP || st.tevSwapTex >= GX_MAX_TEVSWAP) {
      return "TEV swap selection";
    }
    const auto mtx = static_cast<uint32_t>(st.indTexMtxId);
    if (st.indTexStage >= MaxIndStages || st.indTexFormat >= GX_MAX_ITFORMAT || st.indTexBiasSel >= GX_MAX_ITBIAS ||
        st.indTexAlphaSel > GX_ITBA_U || !(mtx <= GX_ITM_2 || (mtx >= GX_ITM_S0 && mtx <= GX_ITM_S2) ||
                                            (mtx >= GX_ITM_T0 && mtx <= GX_ITM_T2)) ||
        st.indTexWrapS >= GX_MAX_ITWRAP || st.indTexWrapT >= GX_MAX_ITWRAP || !raw_bool_ok(st.indTexUseOrigLOD) ||
        !raw_bool_ok(st.indTexAddPrev) || st._p1 != 0 || st._p2 != 0) {
      return "TEV indirect state";
    }
    // Texcoords the shader reads (as lib/gx/shader_info.cpp derives them).
    if (st.texMapId != GX_TEXMAP_NULL && uses_tex_arg(st)) {
      if (st.texCoordId == GX_TEXCOORD_NULL) {
        return "texture without texcoord";
      }
      sampledCoords |= 1u << st.texCoordId;
    }
    if (st.indTexStage < s.numIndStages) {
      const bool usesTevTexCoord = st.indTexMtxId != GX_ITM_OFF || st.indTexWrapS != GX_ITW_OFF ||
                                   st.indTexWrapT != GX_ITW_OFF || st.indTexAddPrev;
      if (usesTevTexCoord && st.texCoordId != GX_TEXCOORD_NULL) {
        sampledCoords |= 1u << st.texCoordId;
      }
      if (st.indTexMtxId != GX_ITM_OFF || st.channelId == GX_ALPHA_BUMP || st.channelId == GX_ALPHA_BUMPN) {
        const auto& ind = s.indStages[st.indTexStage];
        if (ind.texCoordId >= MaxTexCoord || ind.texMapId >= MaxTextures || ind.scaleS >= GX_MAX_ITSCALE ||
            ind.scaleT >= GX_MAX_ITSCALE) {
          return "indirect stage";
        }
        sampledCoords |= 1u << ind.texCoordId;
      }
    }
  }
  if (s.zTexOp != 0) {
    const auto& last = s.tevStages[s.tevStageCount - 1];
    if (last.texCoordId == GX_TEXCOORD_NULL || last.texMapId == GX_TEXMAP_NULL) {
      return "z texture without texture";
    }
    sampledCoords |= 1u << last.texCoordId;
  }
  // Texgens of texcoords the shader can read (emboss adds its source coordinate).
  for (uint32_t pass = 0; pass < 2; ++pass) {
    for (uint32_t i = 0; i < MaxTexCoord; ++i) {
      if ((sampledCoords & (1u << i)) == 0) {
        continue;
      }
      const auto& tcg = s.tcgs[i];
      if (tcg.type > GX_TG_SRTG || tcg.src > GX_MAX_TEXGENSRC || !tex_mtx_ok(tcg.mtx) ||
          !pt_tex_mtx_ok(tcg.postMtx) || !raw_bool_ok(tcg.normalize) || tcg._p2 != 0 || tcg._p3 != 0) {
        return "texgen";
      }
      if (tcg.type >= GX_TG_BUMP0 && tcg.type <= GX_TG_BUMP7) {
        if (tcg.embossSrc >= MaxTexCoord || !hasNbt) {
          return "emboss texgen";
        }
        sampledCoords |= 1u << tcg.embossSrc;
      } else if (tcg.type != GX_TG_SRTG) {
        if (tcg.src >= GX_TG_TEXCOORD0 && tcg.src <= GX_TG_TEXCOORD6) {
          return "texgen source";
        }
        if (tcg.src >= GX_TG_TEX0 && tcg.src <= GX_TG_TEX7 &&
            s.attrs[GX_VA_TEX0 + (tcg.src - GX_TG_TEX0)].attrType == GX_NONE) {
          return "texgen source attribute";
        }
        if ((tcg.src == GX_TG_BINRM || tcg.src == GX_TG_TANGENT) && !hasNbt) {
          return "texgen source attribute";
        }
      }
    }
  }
  const auto& ac = s.alphaCompare;
  if (static_cast<uint32_t>(ac.comp0) > GX_ALWAYS || static_cast<uint32_t>(ac.comp1) > GX_ALWAYS ||
      static_cast<uint32_t>(ac.op) >= GX_MAX_ALPHAOP || ac.ref0 > 255 || ac.ref1 > 255) {
    return "alpha compare";
  }
  return nullptr;
}

const char* validate_config(const clear::PipelineConfig& c) {
  if (c.version != clear::ClearPipelineConfigVersion) {
    return "config version";
  }
  if (!raw_bool_ok(c.clearColor) || !raw_bool_ok(c.clearAlpha) || !raw_bool_ok(c.clearDepth) || c._pad != 0) {
    return "clear flags";
  }
  return nullptr;
}

#ifdef AURORA_ENABLE_RMLUI
const char* validate_config(const rmlui::PipelineConfig& c) {
  return c.version == rmlui::RmlPipelineConfigVersion ? nullptr : "config version";
}
#endif
} // namespace

// Validates a config read back from disk: the stored key must be the xxh3 of the bytes (the
// per-entry checksum), then the field ranges.
template <typename Config>
static const char* validate_cached_config(ShaderType type, const Config& config, HashType storedKey) {
  if (xxh3_hash(config, static_cast<HashType>(type)) != storedKey) {
    return "checksum mismatch";
  }
  return validate_config(config);
}

static bool is_quarantined(HashType cacheKey) {
  std::lock_guard lock{g_badMutex};
  // 0231: a config suspected once (in flight when a run crashed) is still precached: it is built
  // like any other (guarded_create lifts the suspicion when it builds fine). Skipping it only
  // moved its compile into play, as a hitch.
  const auto it = g_badPipelines.find(cacheKey);
  return it != g_badPipelines.end() && (it->second.reason == BadReason::Invalid || it->second.crashes >= 2);
}

#if defined(WEBGPU_DAWN)
static wgpu::ErrorType pop_error_scope(std::string& message) {
  wgpu::ErrorType result = wgpu::ErrorType::NoError;
  const auto future = webgpu::g_device.PopErrorScope(
      wgpu::CallbackMode::WaitAnyOnly,
      [&](wgpu::PopErrorScopeStatus status, wgpu::ErrorType type, wgpu::StringView msg) {
        if (status == wgpu::PopErrorScopeStatus::Success && type != wgpu::ErrorType::NoError) {
          result = type;
          message.assign(msg.data, msg.length);
        }
      });
  webgpu::g_instance.WaitAny(future, UINT64_MAX);
  return result;
}
#endif

void push_shader_error_scope() {
#if defined(WEBGPU_DAWN)
  webgpu::g_device.PushErrorScope(wgpu::ErrorFilter::Validation);
#endif
}

bool pop_shader_error_scope(const char* what) {
#if defined(WEBGPU_DAWN)
  std::string message;
  if (pop_error_scope(message) != wgpu::ErrorType::NoError) {
    Log.error("{} failed to compile: {}", what, message);
    return false;
  }
#endif
  return true;
}

// 0233: pipelines being created right now, with their start time, so an application waiting for
// them (a startup hold) can say which one stopped making progress (aurora_pipeline_cache_describe_stuck).
namespace {
constexpr int InflightSlots = 64;
struct InflightSlot {
  std::atomic<uint64_t> key{0};
  std::atomic<uint32_t> type{0};
  std::atomic<uint64_t> startNs{0}; // 0: free
};
InflightSlot g_inflight[InflightSlots];
std::atomic<uint32_t> g_inflightNext{0};
thread_local int t_inflightSlot = -1;

struct InflightScope {
  InflightScope(ShaderType type, HashType key) {
    if (t_inflightSlot < 0) {
      t_inflightSlot = static_cast<int>(g_inflightNext.fetch_add(1) % InflightSlots);
    }
    auto& slot = g_inflight[t_inflightSlot];
    slot.key.store(static_cast<uint64_t>(key), std::memory_order_relaxed);
    slot.type.store(underlying(type), std::memory_order_relaxed);
    slot.startNs.store(now_ns(), std::memory_order_release);
  }
  ~InflightScope() { g_inflight[t_inflightSlot].startNs.store(0, std::memory_order_release); }
};
} // namespace

// Every pipeline is created through here: refuses configs recorded as bad, marks the config in
// the crash journal while it is being built, and turns a Dawn error (WGSL compile error, device
// validation error) into a failed pipeline (draws using it are skipped) instead of a fatal
// uncaptured error. Error scopes are per thread in Dawn, so worker threads do not see each
// other's errors.
static CompiledPipeline guarded_create(ShaderType type, HashType cacheKey, const NewPipelineCallback& create) {
  bool wasSuspect = false;
  {
    std::lock_guard lock{g_badMutex};
    if (const auto it = g_badPipelines.find(cacheKey); it != g_badPipelines.end()) {
      if (it->second.reason == BadReason::Invalid || it->second.crashes >= 2) {
        return {};
      }
      wasSuspect = true;
    }
  }
  journal_begin(type, cacheKey);
  const InflightScope inflight{type, cacheKey};
  // 0233 test aid AURORA_TEST_STUCK_PIPELINE=<n>[,<s>]: the n-th creation of this run takes s seconds
  // longer (default 120), like a driver stuck in one shader (the startup hold's stall report).
  static const auto sStuck = [] {
    std::pair<uint32_t, uint32_t> r{0, 120};
    if (const char* v = std::getenv("AURORA_TEST_STUCK_PIPELINE")) {
      char* end = nullptr;
      r.first = static_cast<uint32_t>(std::strtoul(v, &end, 10));
      if (end != nullptr && *end == ',') {
        r.second = static_cast<uint32_t>(std::strtoul(end + 1, nullptr, 10));
      }
    }
    return r;
  }();
  if (sStuck.first != 0) {
    static std::atomic<uint32_t> sCreations{0};
    if (sCreations.fetch_add(1) + 1 == sStuck.first) {
      Log.warn("AURORA_TEST_STUCK_PIPELINE: pipeline config {:016x} held for {} s", cacheKey, sStuck.second);
      std::this_thread::sleep_for(std::chrono::seconds(sStuck.second));
    }
  }
#if defined(WEBGPU_DAWN)
  webgpu::g_device.PushErrorScope(wgpu::ErrorFilter::OutOfMemory);
  webgpu::g_device.PushErrorScope(wgpu::ErrorFilter::Internal);
  webgpu::g_device.PushErrorScope(wgpu::ErrorFilter::Validation);
#endif
  auto result = create();
#if defined(WEBGPU_DAWN)
  std::string validationMsg, internalMsg, oomMsg;
  const auto validation = pop_error_scope(validationMsg);
  const auto internal = pop_error_scope(internalMsg);
  const auto oom = pop_error_scope(oomMsg);
#endif
  journal_end();
#if defined(WEBGPU_DAWN)
  if (validation != wgpu::ErrorType::NoError) {
    Log.error("pipeline config {:016x} (type {}): creation failed, recorded as bad: {}", cacheKey, underlying(type),
              validationMsg);
    mark_pipeline_bad(type, cacheKey, BadReason::Invalid, "creation failed");
    return {};
  }
  if (internal != wgpu::ErrorType::NoError || oom != wgpu::ErrorType::NoError) {
    // Possibly transient (driver/memory): fail this attempt without recording the config.
    Log.error("pipeline config {:016x} (type {}): creation failed: {}", cacheKey, underlying(type),
              internal != wgpu::ErrorType::NoError ? internalMsg : oomMsg);
    return {};
  }
#endif
  if (!result.main) {
    // The generated shader did not compile (pop_shader_error_scope logged why).
    mark_pipeline_bad(type, cacheKey, BadReason::Invalid, "shader compilation failed");
    return {};
  }
  if (wasSuspect) {
    {
      std::lock_guard lock{g_badMutex};
      g_badPipelines.erase(cacheKey);
    }
    Log.info("pipeline config {:016x}: built fine after a crash, quarantine lifted", cacheKey);
    PipelineCacheWrite write{.type = type, .hash = cacheKey, .configVersion = 0, .op = CacheWriteOp::ClearBad};
    enqueue_pipeline_cache_write(std::move(write));
  }
  return result;
}

static PipelineRef resolve_pipeline(ShaderType type, const gx::PipelineConfig& config, const RenderTargetLayout& layout,
                                    PipelinePriority priority) {
  const auto cacheKey = xxh3_hash(config, static_cast<HashType>(type));
  const auto runtimeKey = xxh3_hash(layout.key, cacheKey);
  tl_missConfig = &config;
  tl_missLayout = &layout;
  tl_missCacheKey = cacheKey;
  const PipelineRef ref = find_pipeline_impl(
      runtimeKey,
      [type, cacheKey, config, layout] {
        return guarded_create(type, cacheKey, [&] { return create_pipeline(config, layout); });
      },
      priority);
  // 0221: miss_detail() now runs for every draw that meets a missing pipeline: no pointers to this
  // call's arguments may outlive it (other find_pipeline overloads use find_pipeline_impl too).
  tl_missConfig = nullptr;
  tl_missLayout = nullptr;
  return ref;
}

static PipelineRef resolve_pipeline(ShaderType type, const clear::PipelineConfig& config,
                                    const RenderTargetLayout& layout, PipelinePriority priority) {
  const auto cacheKey = xxh3_hash(config, static_cast<HashType>(type));
  const auto runtimeKey = xxh3_hash(layout.key, cacheKey);
  return find_pipeline_impl(
      runtimeKey,
      [type, cacheKey, config, layout] {
        return guarded_create(type, cacheKey,
                              [&] { return CompiledPipeline{.main = create_pipeline(config, layout)}; });
      },
      priority);
}

#ifdef AURORA_ENABLE_RMLUI
static PipelineRef resolve_pipeline(ShaderType type, const rmlui::PipelineConfig& config, const RenderTargetLayout&,
                                    PipelinePriority priority) {
  const auto cacheKey = xxh3_hash(config, static_cast<HashType>(type));
  return find_pipeline_impl(
      cacheKey,
      [type, cacheKey, config] {
        return guarded_create(type, cacheKey, [&] { return CompiledPipeline{.main = rmlui::create_pipeline(config)}; });
      },
      priority);
}
#endif

static void pipeline_cache_abort() {
  g_pipelineCacheBroken = true;
  if (g_pipelineCacheLoadStmt != nullptr) {
    sqlite3_finalize(g_pipelineCacheLoadStmt);
    g_pipelineCacheLoadStmt = nullptr;
  }
  if (g_pipelineCacheUpsertStmt != nullptr) {
    sqlite3_finalize(g_pipelineCacheUpsertStmt);
    g_pipelineCacheUpsertStmt = nullptr;
  }
  if (g_pipelineCacheDb != nullptr) {
    sqlite3_close(g_pipelineCacheDb);
    g_pipelineCacheDb = nullptr;
  }
}

static bool write_pipeline_cache_record(const PipelineCacheWrite& write);

static std::string pipeline_cache_seed_path() {
  if (g_config.resourcesPath == nullptr || g_config.resourcesPath[0] == '\0') {
    return InitialPipelineCacheName;
  }

  std::string path{g_config.resourcesPath};
  if (path.back() != '/' && path.back() != '\\') {
    path += '/';
  }
  path += InitialPipelineCacheName;
  return path;
}

static sqlite3* open_pipeline_cache_seed_db(const std::string& path) {
  if (!register_sdl_vfs()) {
    Log.warn("Failed to register SDL pipeline cache seed VFS");
    return nullptr;
  }

  sqlite3* seedDb = nullptr;
  const auto ret = sqlite3_open_v2(path.c_str(), &seedDb, SQLITE_OPEN_READONLY | SQLITE_OPEN_PRIVATECACHE, SdlVfsName);
  if (ret != SQLITE_OK) {
    if (seedDb != nullptr) {
      sqlite3_close(seedDb);
    }
    Log.info("No bundled initial pipeline cache found at '{}'", path);
    return nullptr;
  }

  bool schemaMatch = false;
  const auto schemaQuery = fmt::format("SELECT 1 FROM aurora_schema WHERE value = {}", PipelineCacheSchema);
  const auto schemaRet =
      sqlite::exec(seedDb, schemaQuery.c_str(), [&schemaMatch](int, char**, char**) { schemaMatch = true; });
  if (schemaRet != SQLITE_OK) {
    Log.warn("Failed to read bundled pipeline cache schema from '{}': {}", path, sqlite3_errmsg(seedDb));
    sqlite3_close(seedDb);
    return nullptr;
  }
  if (!schemaMatch) {
    Log.warn("Bundled pipeline cache '{}' does not use schema version {}", path, PipelineCacheSchema);
    sqlite3_close(seedDb);
    return nullptr;
  }

  return seedDb;
}

static void seed_pipeline_cache() {
  if (g_pipelineCacheBroken || g_pipelineCacheDb == nullptr || g_pipelineCacheUpsertStmt == nullptr) {
    return;
  }

  const auto seedPath = pipeline_cache_seed_path();
  sqlite3* seedDb = open_pipeline_cache_seed_db(seedPath);
  if (seedDb == nullptr) {
    return;
  }

  sqlite3_stmt* seedStmt = nullptr;
  auto closeSeed = [&] {
    if (seedStmt != nullptr) {
      sqlite3_finalize(seedStmt);
      seedStmt = nullptr;
    }
    sqlite3_close(seedDb);
  };

  auto ret = sqlite3_prepare_v3(seedDb,
                                "SELECT type, hash, config_version, config_size, config, first_frame_used "
                                "FROM pipeline_cache",
                                -1, 0, &seedStmt, nullptr);
  if (ret != SQLITE_OK) {
    Log.warn("Failed to read bundled pipeline cache rows from '{}': {}", seedPath, sqlite3_errmsg(seedDb));
    closeSeed();
    return;
  }

  bool writeFailed = false;
  bool readFailed = false;
  uint32_t mergedRows = 0;
  uint32_t skippedRows = 0;
  {
    sqlite::Transaction tx(g_pipelineCacheDb, Log, true);
    if (!tx) {
      Log.error("Failed to begin pipeline cache seed transaction");
      closeSeed();
      pipeline_cache_abort();
      return;
    }

    while ((ret = sqlite3_step(seedStmt)) == SQLITE_ROW) {
      const auto typeValue = sqlite3_column_int(seedStmt, 0);
      const auto hashValue = sqlite3_column_int64(seedStmt, 1);
      const auto configVersionValue = sqlite3_column_int(seedStmt, 2);
      const auto configSizeValue = sqlite3_column_int(seedStmt, 3);
      const auto* configBlob = static_cast<const uint8_t*>(sqlite3_column_blob(seedStmt, 4));
      const auto configBlobSize = sqlite3_column_bytes(seedStmt, 4);
      const auto firstFrameUsedValue = sqlite3_column_int64(seedStmt, 5);
      constexpr auto MaxShaderTypeValue = std::numeric_limits<std::underlying_type_t<ShaderType>>::max();
      if (typeValue < 0 || typeValue > MaxShaderTypeValue || configVersionValue < 0 || configSizeValue < 0 ||
          configSizeValue != configBlobSize || (configBlobSize > 0 && configBlob == nullptr) ||
          firstFrameUsedValue < 0 || firstFrameUsedValue > std::numeric_limits<uint32_t>::max()) {
        ++skippedRows;
        continue;
      }

      PipelineCacheWrite write{
          .type = static_cast<ShaderType>(typeValue),
          .hash = static_cast<PipelineRef>(hashValue),
          .configVersion = static_cast<uint32_t>(configVersionValue),
          .config = ByteBuffer(static_cast<size_t>(configBlobSize)),
          .firstFrameUsed = static_cast<uint32_t>(firstFrameUsedValue),
      };
      if (configBlobSize > 0) {
        std::memcpy(write.config.data(), configBlob, static_cast<size_t>(configBlobSize));
      }

      if (!write_pipeline_cache_record(write)) {
        writeFailed = true;
        break;
      }
      ++mergedRows;
    }

    if (!writeFailed && ret != SQLITE_DONE) {
      Log.warn("Failed while reading bundled pipeline cache rows from '{}': {}", seedPath, sqlite3_errmsg(seedDb));
      readFailed = true;
    }

    if (!writeFailed && !readFailed) {
      tx.commit();
    }
  }

  closeSeed();

  if (writeFailed) {
    pipeline_cache_abort();
    return;
  }

  if (!readFailed) {
    Log.info("Seeded pipeline cache from '{}' ({} rows merged, {} rows skipped)", seedPath, mergedRows, skippedRows);
  }
}

static std::filesystem::path pipeline_cache_db_path() {
  return io::fs_path_from_string(g_config.cachePath) / "pipeline_cache.db";
}

// Deletes an SQLite database with its WAL/SHM side files (corrupt or crash-suspect cache).
static void delete_sqlite_files(const std::filesystem::path& path) {
  for (const char* suffix : {"", "-wal", "-shm", "-journal"}) {
    auto p = path;
    p += suffix;
    std::error_code ec;
    std::filesystem::remove(p, ec);
  }
}

static bool open_pipeline_cache_db_once();

static bool prepare_pipeline_cache_db() {
  if (g_pipelineCacheBroken) {
    return false;
  }
  if (g_pipelineCacheDb != nullptr) {
    return true;
  }

  if (!open_pipeline_cache_db_once()) {
    // Unreadable (not a database, corrupt pages, truncated): start over with an empty cache
    // instead of running without one (or crashing on it).
    Log.warn("Pipeline cache database '{}' is unusable; deleting and recreating it",
             io::fs_path_to_string(pipeline_cache_db_path()));
    pipeline_cache_abort();
    delete_sqlite_files(pipeline_cache_db_path());
    g_pipelineCacheBroken = false;
    if (!open_pipeline_cache_db_once()) {
      pipeline_cache_abort();
      return false;
    }
  }

  // Configs recorded as bad by earlier runs, then crash suspects found by the journal.
  {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v3(g_pipelineCacheDb, "SELECT type, hash, reason, crashes FROM pipeline_bad", -1, 0, &stmt,
                           nullptr) == SQLITE_OK) {
      std::lock_guard lock{g_badMutex};
      while (sqlite3_step(stmt) == SQLITE_ROW) {
        const auto type = sqlite3_column_int(stmt, 0);
        const auto reason = sqlite3_column_int(stmt, 2);
        if (type < 0 || type > 255 || (reason != 1 && reason != 2)) {
          continue;
        }
        g_badPipelines[static_cast<HashType>(sqlite3_column_int64(stmt, 1))] =
            BadPipeline{static_cast<ShaderType>(type), static_cast<BadReason>(reason),
                        static_cast<uint32_t>(std::max(0, sqlite3_column_int(stmt, 3)))};
      }
    }
    sqlite3_finalize(stmt);
  }
  for (const auto& [type, key] : g_journalSuspects) {
    mark_pipeline_bad(type, key, BadReason::CrashSuspect, "the previous run ended while it was being created");
  }
  g_journalSuspects.clear();

  seed_pipeline_cache();
  if (g_pipelineCacheBroken) {
    return false;
  }

  return true;
}

static bool open_pipeline_cache_db_once() {
  const auto path = io::fs_path_to_string(pipeline_cache_db_path());
  auto ret = sqlite3_open(path.c_str(), &g_pipelineCacheDb);
  if (ret != SQLITE_OK) {
    Log.error("Failed to open pipeline cache database: {}", sqlite3_errmsg(g_pipelineCacheDb));
    return false;
  }

  ret = sqlite::exec(g_pipelineCacheDb, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;");
  if (ret != SQLITE_OK) {
    Log.error("Failed to set pipeline cache pragmas: {}", sqlite3_errmsg(g_pipelineCacheDb));
    return false;
  }

  // The file is small (a few MB at most): check every page so a damaged file is replaced now
  // rather than failing half way through loading.
  bool integrityOk = !webgpu::g_cacheVerifyOnOpen; // 0226: only after an unclean exit
  std::string integrityResult;
  ret = !webgpu::g_cacheVerifyOnOpen ? SQLITE_OK
                                     : sqlite::exec(g_pipelineCacheDb, "PRAGMA quick_check;", [&integrityOk, &integrityResult](int argc, char** argv, char**) {
    if (argc > 0 && argv[0] != nullptr && std::strcmp(argv[0], "ok") == 0) {
      integrityOk = true;
    } else if (argc > 0 && argv[0] != nullptr && integrityResult.empty()) {
      integrityResult = argv[0];
    }
  });
  if (ret != SQLITE_OK || !integrityOk) {
    Log.error("Pipeline cache integrity check failed: {}",
              ret != SQLITE_OK ? sqlite3_errmsg(g_pipelineCacheDb) : integrityResult.c_str());
    return false;
  }

  bool schemaFailed = false;
  {
    sqlite::Transaction tx(g_pipelineCacheDb, Log, true);
    if (!tx) {
      Log.error("Failed to begin pipeline cache schema transaction");
      schemaFailed = true;
    } else {
      ret = sqlite::exec(g_pipelineCacheDb, "CREATE TABLE IF NOT EXISTS aurora_schema(value INTEGER);");
      if (ret != SQLITE_OK) {
        Log.error("Failed to create pipeline cache schema table: {}", sqlite3_errmsg(g_pipelineCacheDb));
        schemaFailed = true;
      }
    }

    bool schemaMatch = false;
    if (!schemaFailed) {
      const auto schemaQuery = fmt::format("SELECT 1 FROM aurora_schema WHERE value = {}", PipelineCacheSchema);
      ret = sqlite::exec(g_pipelineCacheDb, schemaQuery.c_str(),
                         [&schemaMatch](int, char**, char**) { schemaMatch = true; });
      if (ret != SQLITE_OK) {
        Log.error("Failed to read pipeline cache schema version: {}", sqlite3_errmsg(g_pipelineCacheDb));
        schemaFailed = true;
      }
    }

    if (!schemaFailed && !schemaMatch) {
      const auto schemaSql = fmt::format(
          R"(DROP TABLE IF EXISTS pipeline_cache;
DROP TABLE IF EXISTS pipeline_bad;
CREATE TABLE pipeline_cache (
  type INTEGER NOT NULL,
  hash INTEGER NOT NULL,
  config_version INTEGER NOT NULL,
  config_size INTEGER NOT NULL,
  config BLOB NOT NULL,
  first_frame_used INTEGER NOT NULL,
  PRIMARY KEY (type, hash)
);
CREATE INDEX pipeline_cache_load_order_idx
  ON pipeline_cache(type, config_version, first_frame_used);
DELETE FROM aurora_schema;
INSERT INTO aurora_schema VALUES ({});)",
          PipelineCacheSchema);
      ret = sqlite::exec(g_pipelineCacheDb, schemaSql.c_str());
      if (ret != SQLITE_OK) {
        Log.error("Failed to initialize pipeline cache schema: {}", sqlite3_errmsg(g_pipelineCacheDb));
        schemaFailed = true;
      }
    }

    if (!schemaFailed) {
      ret = sqlite::exec(g_pipelineCacheDb, R"(CREATE TABLE IF NOT EXISTS pipeline_bad (
  type INTEGER NOT NULL,
  hash INTEGER NOT NULL,
  reason INTEGER NOT NULL,
  crashes INTEGER NOT NULL,
  PRIMARY KEY (type, hash)
);)");
      if (ret != SQLITE_OK) {
        Log.error("Failed to create pipeline_bad table: {}", sqlite3_errmsg(g_pipelineCacheDb));
        schemaFailed = true;
      }
    }

    if (!schemaFailed) {
      tx.commit();
    }
  }

  if (schemaFailed) {
    pipeline_cache_abort();
    return false;
  }

  ret = sqlite3_prepare_v3(g_pipelineCacheDb,
                           "SELECT hash, config, first_frame_used FROM pipeline_cache "
                           "WHERE type = ? AND config_version = ? "
                           "ORDER BY first_frame_used ASC, rowid ASC",
                           -1, SQLITE_PREPARE_PERSISTENT, &g_pipelineCacheLoadStmt, nullptr);
  if (ret != SQLITE_OK) {
    Log.error("Failed to prepare pipeline cache load statement: {}", sqlite3_errmsg(g_pipelineCacheDb));
    pipeline_cache_abort();
    return false;
  }

  ret = sqlite3_prepare_v3(
      g_pipelineCacheDb,
      "INSERT INTO pipeline_cache (type, hash, config_version, config_size, config, first_frame_used) "
      "VALUES (?, ?, ?, ?, ?, ?) "
      "ON CONFLICT(type, hash) DO UPDATE SET "
      "config_version = excluded.config_version, "
      "config_size = excluded.config_size, "
      "config = excluded.config, "
      "first_frame_used = MIN(pipeline_cache.first_frame_used, excluded.first_frame_used)",
      -1, SQLITE_PREPARE_PERSISTENT, &g_pipelineCacheUpsertStmt, nullptr);
  if (ret != SQLITE_OK) {
    Log.error("Failed to prepare pipeline cache upsert statement: {}", sqlite3_errmsg(g_pipelineCacheDb));
    pipeline_cache_abort();
    return false;
  }

  return true;
}

static void prune_old_pipeline_cache_versions() {
  if (!prepare_pipeline_cache_db()) {
    return;
  }

  const auto clearDelete = fmt::format("DELETE FROM pipeline_cache WHERE type = {} AND config_version < {}",
                                       underlying(ShaderType::Clear), clear::ClearPipelineConfigVersion);
  auto ret = sqlite::exec(g_pipelineCacheDb, clearDelete.c_str());
  if (ret != SQLITE_OK) {
    Log.error("Failed to prune clear pipeline cache rows: {}", sqlite3_errmsg(g_pipelineCacheDb));
    pipeline_cache_abort();
    return;
  }

  const auto gxDelete = fmt::format("DELETE FROM pipeline_cache WHERE type = {} AND config_version < {}",
                                    underlying(ShaderType::GX), gx::GXPipelineConfigVersion);
  ret = sqlite::exec(g_pipelineCacheDb, gxDelete.c_str());
  if (ret != SQLITE_OK) {
    Log.error("Failed to prune GX pipeline cache rows: {}", sqlite3_errmsg(g_pipelineCacheDb));
    pipeline_cache_abort();
    return;
  }

#ifdef AURORA_ENABLE_RMLUI
  const auto rmlDelete = fmt::format("DELETE FROM pipeline_cache WHERE type = {} AND config_version < {}",
                                     underlying(ShaderType::Rml), rmlui::RmlPipelineConfigVersion);
  ret = sqlite::exec(g_pipelineCacheDb, rmlDelete.c_str());
  if (ret != SQLITE_OK) {
    Log.error("Failed to prune RmlUi pipeline cache rows: {}", sqlite3_errmsg(g_pipelineCacheDb));
    pipeline_cache_abort();
  }
#endif
}

static bool write_bad_record(const PipelineCacheWrite& write) {
  const auto hash = static_cast<int64_t>(write.hash);
  std::string sql;
  if (write.op == CacheWriteOp::ClearBad) {
    sql = fmt::format("DELETE FROM pipeline_bad WHERE type = {} AND hash = {};", underlying(write.type), hash);
  } else {
    sql = fmt::format("INSERT INTO pipeline_bad (type, hash, reason, crashes) VALUES ({0}, {1}, {2}, {3}) "
                      "ON CONFLICT(type, hash) DO UPDATE SET reason = excluded.reason, crashes = excluded.crashes;",
                      underlying(write.type), hash, write.badReason, write.badCrashes);
    if (write.badReason == underlying(BadReason::Invalid)) {
      sql += fmt::format("DELETE FROM pipeline_cache WHERE type = {} AND hash = {};", underlying(write.type), hash);
    }
  }
  if (sqlite::exec(g_pipelineCacheDb, sql.c_str()) != SQLITE_OK) {
    Log.error("Failed to record bad pipeline: {}", sqlite3_errmsg(g_pipelineCacheDb));
    return false;
  }
  return true;
}

static bool write_pipeline_cache_record(const PipelineCacheWrite& write) {
  if (write.op != CacheWriteOp::Upsert) {
    return write_bad_record(write);
  }
  const auto fail = [&]() {
    sqlite3_reset(g_pipelineCacheUpsertStmt);
    sqlite3_clear_bindings(g_pipelineCacheUpsertStmt);
    return false;
  };

  auto ret = sqlite3_bind_int(g_pipelineCacheUpsertStmt, 1, underlying(write.type));
  if (ret != SQLITE_OK) {
    Log.error("Failed to bind pipeline cache type: {}", sqlite3_errmsg(g_pipelineCacheDb));
    return fail();
  }
  ret = sqlite3_bind_int64(g_pipelineCacheUpsertStmt, 2, static_cast<sqlite3_int64>(write.hash));
  if (ret != SQLITE_OK) {
    Log.error("Failed to bind pipeline cache hash: {}", sqlite3_errmsg(g_pipelineCacheDb));
    return fail();
  }
  ret = sqlite3_bind_int(g_pipelineCacheUpsertStmt, 3, static_cast<int>(write.configVersion));
  if (ret != SQLITE_OK) {
    Log.error("Failed to bind pipeline cache config version: {}", sqlite3_errmsg(g_pipelineCacheDb));
    return fail();
  }
  ret = sqlite3_bind_int(g_pipelineCacheUpsertStmt, 4, static_cast<int>(write.config.size()));
  if (ret != SQLITE_OK) {
    Log.error("Failed to bind pipeline cache config size: {}", sqlite3_errmsg(g_pipelineCacheDb));
    return fail();
  }
  ret = sqlite3_bind_blob64(g_pipelineCacheUpsertStmt, 5, write.config.data(), write.config.size(), SQLITE_TRANSIENT);
  if (ret != SQLITE_OK) {
    Log.error("Failed to bind pipeline cache config blob: {}", sqlite3_errmsg(g_pipelineCacheDb));
    return fail();
  }
  ret = sqlite3_bind_int64(g_pipelineCacheUpsertStmt, 6, static_cast<sqlite3_int64>(write.firstFrameUsed));
  if (ret != SQLITE_OK) {
    Log.error("Failed to bind pipeline cache first-frame-used: {}", sqlite3_errmsg(g_pipelineCacheDb));
    return fail();
  }

  ret = sqlite3_step(g_pipelineCacheUpsertStmt);
  if (ret != SQLITE_DONE) {
    Log.error("Failed to upsert pipeline cache row: {}", sqlite3_errmsg(g_pipelineCacheDb));
    return fail();
  }

  sqlite3_reset(g_pipelineCacheUpsertStmt);
  sqlite3_clear_bindings(g_pipelineCacheUpsertStmt);
  return true;
}

static void pipeline_cache_writer() {
#ifdef TRACY_ENABLE
  tracy::SetThreadName("Pipeline cache writer thread");
#endif

  while (true) {
    std::deque<PipelineCacheWrite> batch;
    {
      std::unique_lock lock{g_pipelineCacheWriterMutex};
      g_pipelineCacheWriterCv.wait(lock,
                                   [] { return g_pipelineCacheWriterStop || !g_pipelineCacheWriteQueue.empty(); });
      if (g_pipelineCacheWriterStop && g_pipelineCacheWriteQueue.empty()) {
        return;
      }
      batch.swap(g_pipelineCacheWriteQueue);
    }

    bool writeFailed = false;
    {
      sqlite::Transaction tx(g_pipelineCacheDb, Log, true);
      if (!tx) {
        Log.error("Failed to begin pipeline cache write transaction");
        writeFailed = true;
      } else {
        for (const auto& write : batch) {
          if (!write_pipeline_cache_record(write)) {
            writeFailed = true;
            break;
          }
        }

        if (!writeFailed) {
          tx.commit();
        }
      }
    }

    if (writeFailed) {
      pipeline_cache_abort();
      return;
    }
  }
}

static CompiledPipeline create_pending(PendingPipeline& pending) {
  const uint64_t start = now_ns();
  auto result = pending.create();
  if (g_pipelineLog.load(std::memory_order_relaxed)) {
    Log.info("pipeline {:016x}: created in {:.2f} ms ({}) frame {}", pending.hash, (now_ns() - start) / 1e6,
             pending.precache ? "precache" : "demand", current_frame());
  }
  if (pending.precache) {
    ++g_precacheDone;
  }
  if (pending.rebuild) {
    --g_rebuildPending;
  }
  return result;
}

static void pipeline_worker(uint32_t index) {
#ifdef TRACY_ENABLE
  tracy::SetThreadName("Pipeline compilation thread");
#endif

  while (g_hasPipelineThread || g_pipelinesPerFrame < BuildPipelinesPerFrame) {
    PendingPipeline pending;
    {
      std::unique_lock lock{g_pipelineMutex};
      if (g_hasPipelineThread) {
        // 0227: a boost worker sleeps while not boosted. If it was woken for work after the boost
        // ended, it passes the wake-up on to a worker that is active.
        if (index >= g_activeWorkers.load(std::memory_order_relaxed)) {
          g_pipelineQueueCv.notify_one();
          g_boostCv.wait(lock, [index] {
            return index < g_activeWorkers.load(std::memory_order_relaxed) || g_pipelineThreadEnd;
          });
        }
        // Several workers share the queues: always re-check under the lock.
        g_pipelineQueueCv.wait(lock, [index] {
          return !g_pipelineQueue.empty() || !g_backgroundPipelineQueue.empty() || g_pipelineThreadEnd ||
                 index >= g_activeWorkers.load(std::memory_order_relaxed);
        });
        if (!g_pipelineThreadEnd && index >= g_activeWorkers.load(std::memory_order_relaxed)) {
          continue;
        }
      } else if (g_pipelineQueue.empty() && g_backgroundPipelineQueue.empty()) {
        return;
      }
      if (g_pipelineThreadEnd) {
        break;
      }
      auto& source = !g_pipelineQueue.empty() ? g_pipelineQueue : g_backgroundPipelineQueue;
      pending = std::move(source.front());
      source.pop_front();
    }
    auto result = create_pending(pending);
    const auto pipelineCount = result.pipeline_count();
    {
      std::lock_guard lock{g_pipelineMutex};
      g_pipelines.try_emplace(pending.hash, std::move(result));
      g_pendingPipelines.erase(pending.hash);
    }
    if (!g_hasPipelineThread) {
      g_pipelinesPerFrame += std::max(1u, pipelineCount);
    }
    notify_pipeline_ready(true, pipelineCount);
  }
}

// A draw is about to be encoded but its pipeline is still queued or compiling: move it to the
// front of the queue and wait for it. Skipping the draw instead (Aurora's original behaviour)
// made objects vanish for a few frames whenever a new shader was first needed.
static bool wait_for_pending_pipeline(PipelineRef ref, CompiledPipeline& out) {
  const uint64_t start = now_ns();
  const activity::Scope busy{activity::Thread::Fifo, "waiting for a pipeline to be created"};
  std::unique_lock lock{g_pipelineMutex};
  if (!g_pendingPipelines.contains(ref)) {
    return false;
  }
  if (!g_hasPipelineThread) {
    auto pending = take_pending_pipeline(ref);
    if (!pending) {
      return false;
    }
    lock.unlock();
    auto result = create_pending(*pending);
    const auto pipelineCount = result.pipeline_count();
    lock.lock();
    g_pipelines.try_emplace(ref, std::move(result));
    lock.unlock();
    notify_pipeline_ready(true, pipelineCount);
    lock.lock();
  } else {
    // Still queued (not yet picked up by a worker): compile it next.
    if (auto it = find_pending_pipeline(g_pipelineQueue, ref); it != g_pipelineQueue.end()) {
      auto pending = std::move(*it);
      g_pipelineQueue.erase(it);
      g_pipelineQueue.emplace_front(std::move(pending));
    } else if (auto bit = find_pending_pipeline(g_backgroundPipelineQueue, ref);
               bit != g_backgroundPipelineQueue.end()) {
      auto pending = std::move(*bit);
      g_backgroundPipelineQueue.erase(bit);
      g_pipelineQueue.emplace_front(std::move(pending));
    }
    g_pipelineQueueCv.notify_one();
    g_pipelineReadyCv.wait(lock, [=] { return g_pipelines.contains(ref) || g_pipelineThreadEnd; });
  }
  const auto it = g_pipelines.find(ref);
  const bool ok = it != g_pipelines.end() && it->second.main;
  if (ok) {
    out = it->second;
  }
  lock.unlock();
  const uint64_t waited = now_ns() - start;
  ++g_stalledDraws;
  g_stallNs += waited;
  uint64_t prevMax = g_maxStallNs.load(std::memory_order_relaxed);
  while (waited > prevMax && !g_maxStallNs.compare_exchange_weak(prevMax, waited)) {
  }
  if (g_pipelineLog.load(std::memory_order_relaxed)) {
    Log.info("pipeline {:016x}: draw waited {:.2f} ms frame {}", ref, waited / 1e6, current_frame());
  }
  return ok;
}

template <typename PipelineConfig>
static size_t load_pipeline_cache_entries(ShaderType type, uint32_t configVersion) {
  if (!prepare_pipeline_cache_db()) {
    return 0;
  }

  auto ret = sqlite3_bind_int(g_pipelineCacheLoadStmt, 1, underlying(type));
  if (ret != SQLITE_OK) {
    Log.error("Failed to bind pipeline cache load type: {}", sqlite3_errmsg(g_pipelineCacheDb));
    pipeline_cache_abort();
    return 0;
  }
  ret = sqlite3_bind_int(g_pipelineCacheLoadStmt, 2, static_cast<int>(configVersion));
  if (ret != SQLITE_OK) {
    Log.error("Failed to bind pipeline cache load config version: {}", sqlite3_errmsg(g_pipelineCacheDb));
    pipeline_cache_abort();
    return 0;
  }

  size_t acceptedRows = 0;
  std::vector<int64_t> invalidRows;
  while ((ret = sqlite3_step(g_pipelineCacheLoadStmt)) == SQLITE_ROW) {
    const auto storedKey = static_cast<HashType>(sqlite3_column_int64(g_pipelineCacheLoadStmt, 0));
    const auto* configBlob = static_cast<const uint8_t*>(sqlite3_column_blob(g_pipelineCacheLoadStmt, 1));
    const auto configSize = sqlite3_column_bytes(g_pipelineCacheLoadStmt, 1);
    const auto firstFrameRaw = sqlite3_column_int64(g_pipelineCacheLoadStmt, 2);
    const char* reason = nullptr;
    PipelineConfig config{};
    if (configSize != static_cast<int>(sizeof(PipelineConfig)) || configBlob == nullptr) {
      reason = "size mismatch";
    } else if (firstFrameRaw < 0 || firstFrameRaw > std::numeric_limits<uint32_t>::max()) {
      reason = "first frame out of range";
    } else {
      std::memcpy(&config, configBlob, sizeof(config));
      reason = validate_cached_config(type, config, storedKey);
    }
    if (reason == nullptr) {
      std::lock_guard lock{g_badMutex};
      if (const auto it = g_badPipelines.find(storedKey); it != g_badPipelines.end() &&
                                                         it->second.reason == BadReason::Invalid) {
        reason = "recorded as bad";
      }
    }
    if (reason != nullptr) {
      if (invalidRows.size() < 16) {
        Log.warn("pipeline cache: skipping invalid entry {:016x} (type {}): {}", storedKey, underlying(type), reason);
      }
      invalidRows.push_back(static_cast<int64_t>(storedKey));
      continue;
    }

    remember_pipeline_config(type, canonical_known(config), static_cast<uint32_t>(firstFrameRaw), false);
    ++acceptedRows;
  }

  const bool readFailed = ret != SQLITE_DONE;
  if (readFailed) {
    Log.error("Failed to read pipeline cache rows: {}", sqlite3_errmsg(g_pipelineCacheDb));
  }

  sqlite3_reset(g_pipelineCacheLoadStmt);
  sqlite3_clear_bindings(g_pipelineCacheLoadStmt);

  if (!invalidRows.empty() && !readFailed) {
    Log.warn("pipeline cache: deleted {} invalid entries of type {}", invalidRows.size(), underlying(type));
    sqlite::Transaction tx(g_pipelineCacheDb, Log, true);
    bool ok = static_cast<bool>(tx);
    for (const auto hash : invalidRows) {
      if (!ok) {
        break;
      }
      const auto sql =
          fmt::format("DELETE FROM pipeline_cache WHERE type = {} AND hash = {};", underlying(type), hash);
      ok = sqlite::exec(g_pipelineCacheDb, sql.c_str()) == SQLITE_OK;
    }
    if (ok) {
      tx.commit();
    }
  }
  if (readFailed) {
    // Damaged pages: drop the file so the next launch starts clean; this run keeps what it read.
    Log.warn("pipeline cache: database damaged, it will be recreated");
    pipeline_cache_abort();
    delete_sqlite_files(pipeline_cache_db_path());
  }
  return acceptedRows;
}

static size_t load_pipeline_cache() {
  if (!prepare_pipeline_cache_db()) {
    return 0;
  }
  prune_old_pipeline_cache_versions();
  if (g_pipelineCacheBroken) {
    return 0;
  }

  size_t acceptedRows = 0;
#ifdef AURORA_ENABLE_RMLUI
  acceptedRows += load_pipeline_cache_entries<rmlui::PipelineConfig>(ShaderType::Rml, rmlui::RmlPipelineConfigVersion);
#endif
  acceptedRows +=
      load_pipeline_cache_entries<clear::PipelineConfig>(ShaderType::Clear, clear::ClearPipelineConfigVersion);
  acceptedRows += load_pipeline_cache_entries<gx::PipelineConfig>(ShaderType::GX, gx::GXPipelineConfigVersion);
  return acceptedRows;
}

static void start_pipeline_cache_writer() {
  if (!prepare_pipeline_cache_db()) {
    return;
  }

  g_pipelineCacheWriterStop = false;
  g_pipelineCacheWriterThread = std::thread(pipeline_cache_writer);
}

static void stop_pipeline_cache_writer() {
  if (g_pipelineCacheWriterThread.joinable()) {
    {
      std::lock_guard lock{g_pipelineCacheWriterMutex};
      g_pipelineCacheWriterStop = true;
    }
    g_pipelineCacheWriterCv.notify_one();
    g_pipelineCacheWriterThread.join();
  } else {
    g_pipelineCacheWriterStop = false;
  }

  g_pipelineCacheWriteQueue.clear();
}

PipelineRef find_pipeline(const clear::PipelineConfig& config, const RenderTargetLayout& layout) {
  remember_pipeline_config(ShaderType::Clear, config, current_frame(), true);
  return resolve_pipeline(ShaderType::Clear, config, layout, PipelinePriority::Normal);
}

static void queue_gx_variants(const gx::PipelineConfig& recorded, uint32_t accuracy, uint32_t enhBits,
                              absl::flat_hash_set<HashType>* seen);

PipelineRef find_pipeline(const gx::PipelineConfig& drawConfig, const RenderTargetLayout& layout) {
  // 0217: pipelines are keyed (and built) by the canonical config.
  gx::PipelineConfig config = drawConfig;
  gx::canonicalize_pipeline_config(config);
  const bool isNew = remember_pipeline_config(ShaderType::GX, config, current_frame(), true);
  const auto ref = resolve_pipeline(ShaderType::GX, config, layout, PipelinePriority::Normal);
  if (isNew) {
    // A config the precache did not know: queue the variants it leads to under the current
    // settings now (compiled while this one is), instead of one more miss for each later.
    queue_gx_variants(config, gx::g_accuracyFlags.load(std::memory_order_relaxed), enhanced::precache_bits(), nullptr);
  }
  return ref;
}

#ifdef AURORA_ENABLE_RMLUI
PipelineRef find_pipeline(const rmlui::PipelineConfig& config) {
  remember_pipeline_config(ShaderType::Rml, config, 0, true);
  return resolve_pipeline(ShaderType::Rml, config, {}, PipelinePriority::Blocking);
}
#endif

// The accuracy flags' variant of a recorded config (lib/gx/gx.cpp populate_pipeline_config): known
// configs (and the shipped precache list) were recorded with some set of flags; build the variant
// the current flags produce so turning a flag on or off does not miss the precache.
static gx::PipelineConfig adjust_accuracy(const gx::PipelineConfig& config, uint32_t accuracy) {
  auto adjusted = config;
  adjusted.shaderConfig.intTev = (accuracy & GX_AURORA_ACCURACY_INT_TEV) != 0;
  if ((accuracy & GX_AURORA_ACCURACY_EFB_DEPTH) == 0) {
    adjusted.shaderConfig.efbFmt = 0;
    adjusted.shaderConfig.dither = false;
    adjusted.shaderConfig.efbBlend = false;
  } else if ((accuracy & GX_AURORA_ACCURACY_EFB_BLEND) != 0 && adjusted.shaderConfig.efbFmt == 1 &&
             adjusted.shaderConfig.efbBlend) {
    // 0176: a blended RGBA6 draw is built without the in-shader quantization (as
    // populate_pipeline_config does), so lists recorded without 6-bit blending cover it.
    const bool replace =
        (adjusted.blendMode == GX_BM_BLEND && adjusted.blendFacSrc == GX_BL_ONE &&
         adjusted.blendFacDst == GX_BL_ZERO) ||
        (adjusted.blendMode == GX_BM_LOGIC && adjusted.blendOp == GX_LO_COPY);
    if (!replace) {
      adjusted.shaderConfig.efbFmt = 0;
      adjusted.shaderConfig.dither = false;
      adjusted.shaderConfig.efbBlend = false;
    }
  }
  return adjusted;
}

// 0217: queue every pipeline a recorded config leads to under the given settings: its accuracy
// variant (canonical), the enhanced-lighting variants (enhanced::precache_variants: shader
// classes, glow, depth pre-pass, shadow casters, beam) and the early-Z split pair, each for the
// render targets it is drawn into (scene + offscreen; 0214: depth-only / glow targets). Built
// from the recorded configs at every rebuild, so any combination of settings is covered by one
// list recorded with any settings. `seen` (optional) skips configs already queued this rebuild.
static void queue_gx_variants(const gx::PipelineConfig& recorded, uint32_t accuracy, uint32_t enhBits,
                              absl::flat_hash_set<HashType>* seen) {
  static thread_local RenderTargetLayout scene, offscreen;
  static thread_local uint64_t sceneKey = UINT64_MAX;
  if (const auto s = scene_render_target_layout(); s.key != sceneKey) {
    scene = s;
    offscreen = s;
    offscreen.colorAttachmentCount = 1;
    offscreen.sampleCount = 1;
    detail::finalize_render_target_layout(offscreen);
    sceneKey = s.key;
  }
  gx::PipelineConfig base = adjust_accuracy(recorded, accuracy);
  gx::canonicalize_pipeline_config(base);
  static const RenderTargetLayout* const sExtra = [] {
    // The enhanced passes' own targets (enhanced::precache_layouts of a depth / glow shaped config).
    static RenderTargetLayout l[3];
    gx::PipelineConfig d{};
    d.shaderConfig.depthOnly = 1;
    bool sceneToo = true;
    enhanced::precache_layouts(d, &l[0], sceneToo);
    gx::PipelineConfig g{};
    g.colorUpdate = true;
    g.dstAlpha = UINT32_MAX;
    g.blendMode = GX_BM_BLEND;
    g.blendFacSrc = GX_BL_SRCALPHA;
    g.blendFacDst = GX_BL_ONE;
    enhanced::precache_layouts(g, &l[1], sceneToo);
    l[2] = enhanced::depth_normal_layout(); // 0224
    return l;
  }();
  const auto queue = [&](const gx::PipelineConfig& in, uint8_t targets) {
    gx::PipelineConfig v = in;
    gx::canonicalize_pipeline_config(v);
    const auto key = xxh3_hash(v, static_cast<HashType>(ShaderType::GX));
    if (seen != nullptr && !seen->insert(key ^ (uint64_t(targets) << 56)).second) {
      return;
    }
    // Bad or crash-suspect configs are not precached (a suspect may still be built when a draw
    // needs it, see guarded_create).
    if (is_quarantined(key)) {
      return;
    }
    if (targets & enhanced::PrecacheDepthTarget) {
      resolve_pipeline(ShaderType::GX, v, sExtra[0], PipelinePriority::Background);
    }
    if (targets & enhanced::PrecacheDepthNormalTarget) {
      resolve_pipeline(ShaderType::GX, v, sExtra[2], PipelinePriority::Background);
    }
    if (targets & enhanced::PrecacheGlowTarget) {
      resolve_pipeline(ShaderType::GX, v, sExtra[1], PipelinePriority::Background);
    }
    if (targets & enhanced::PrecacheScene) {
      resolve_pipeline(ShaderType::GX, v, scene, PipelinePriority::Background);
    }
    if ((targets & enhanced::PrecacheOffscreen) && offscreen.key != scene.key) {
      resolve_pipeline(ShaderType::GX, v, offscreen, PipelinePriority::Background);
    }
  };
  enhanced::precache_variants(base, enhBits, [&](const gx::PipelineConfig& v, uint8_t targets) {
    queue(v, targets);
    if ((accuracy & GX_AURORA_ACCURACY_EARLY_Z) != 0 && (targets & enhanced::PrecacheScene) &&
        !v.shaderConfig.depthOnly && !v.shaderConfig.lightDepth) {
      gx::PipelineConfig depthOnly, color;
      if (gx::early_z_split_variants(v, depthOnly, color)) {
        queue(depthOnly, enhanced::PrecacheScene | enhanced::PrecacheOffscreen);
        queue(color, enhanced::PrecacheScene | enhanced::PrecacheOffscreen);
      }
    }
  });
}

void rebuild_pipeline_cache() {
  ZoneScoped;
  const auto scene = scene_render_target_layout();
  auto offscreen = scene;
  offscreen.colorAttachmentCount = 1;
  offscreen.sampleCount = 1;
  detail::finalize_render_target_layout(offscreen);
  g_pipelineLayoutKey = scene.key;

  std::vector<KnownPipeline> known;
  {
    std::lock_guard lock{g_pipelineMutex};
    known.reserve(g_knownPipelines.size());
    for (const auto& pipeline : g_knownPipelines | std::views::values) {
      known.push_back(pipeline);
    }
  }
  std::ranges::sort(known, {}, &KnownPipeline::firstFrameUsed);
  // The GX accuracy flags (GXSetAuroraAccuracy) and the enhanced-lighting parts that select
  // variants are baked into each config's shader; every recorded config is expanded into the
  // variants the current settings produce (0217: queue_gx_variants).
  const uint32_t accuracy = gx::g_accuracyFlags.load(std::memory_order_relaxed);
  const uint32_t enhBits = enhanced::precache_bits();
  g_rebuildAccuracy = accuracy;
  g_rebuildEnhBits = enhBits;
  absl::flat_hash_set<HashType> seen;
  seen.reserve(known.size() * 4);
  const uint32_t before = g_precacheTotal.load();
  g_inRebuild = true;
  for (const auto& pipeline : known) {
    std::visit(
        [&](const auto& config) {
          if constexpr (std::is_same_v<std::decay_t<decltype(config)>, gx::PipelineConfig>) {
            queue_gx_variants(config, accuracy, enhBits, &seen);
          } else {
            if (is_quarantined(xxh3_hash(config, static_cast<HashType>(pipeline.type)))) {
              return;
            }
            resolve_pipeline(pipeline.type, config, scene, PipelinePriority::Background);
            if (offscreen.key != scene.key) {
              resolve_pipeline(pipeline.type, config, offscreen, PipelinePriority::Background);
            }
          }
        },
        pipeline.config);
  }
  g_inRebuild = false;
  // 0223: enhanced lighting's own passes (not GX pipelines) are built alongside; the startup hold
  // (rebuildPending) waits for them too.
  ++g_rebuildPending;
  g_prewarmPending = true;
  if (!enhanced::start_prewarm([] {
        g_prewarmPending = false;
        --g_rebuildPending;
      })) {
    g_prewarmPending = false;
    --g_rebuildPending;
  }
  ++g_rebuildCount;
  Log.info("Pipeline precache: {} known configs -> {} configs for accuracy {:#x} enhanced {:#x}, {} new pipelines queued",
           known.size(), seen.size(), accuracy, enhBits, g_precacheTotal.load() - before);
  if (g_gpuCachePruneArmed) {
    // Prune unused Dawn cache entries once everything queued so far has been built.
    g_gpuCachePruneArmed = false;
    g_gpuCachePrunePending = true;
  }
}

void initialize_pipeline_cache() {
  // Before either cache database is opened (normally already done by the Dawn cache's first use).
  check_crash_journal();
  g_pipelineCacheBroken = false;
  g_pipelineCacheWriterStop = false;
  g_pipelineThreadEnd = false;
  g_gpuCachePrunePending = false;

  if (webgpu::g_backendType == wgpu::BackendType::WebGPU) {
    g_hasPipelineThread = false;
    g_workerThreadCount = 0;
  } else {
    g_hasPipelineThread = true;
    uint32_t threads = g_requestedThreads;
    if (threads == 0) {
      threads = std::clamp(std::thread::hardware_concurrency() / 2, 1u, 8u);
    }
    g_workerThreadCount = threads;
    // 0227: boost workers up to all hardware threads but one, at most 16 (they sleep unless
    // boosted). Cached pipelines stop scaling at about 12-16 threads (Dawn's device lock, taken by
    // every creation call): 10076 cached pipelines took 2.1 s with 6 threads, 1.8 with 8, 1.7 with 12,
    // 1.4-1.6 with 16 and 1.7-1.9 with 23 (i9-12900KS).
    g_boostWorkers =
        std::max(threads, std::min(std::max(std::thread::hardware_concurrency(), 2u) - 1, 16u));
    g_activeWorkers = threads;
    for (uint32_t i = 0; i < g_boostWorkers; ++i) {
      g_pipelineThreads.emplace_back(pipeline_worker, i);
    }
  }

  const size_t loadedCount = load_pipeline_cache();
  // The known set is built on the first frame (begin_pipeline_frame), once the application has
  // added its precache list (aurora_pipeline_cache_seed) and set the GX accuracy flags.
  g_pipelineLayoutKey.reset();
  g_rebuildAccuracy.reset();
  g_rebuildEnhBits.reset();
  if (!g_pipelineCacheBroken && loadedCount > 0) {
    g_gpuCachePruneArmed = true;
  }

  if (!g_pipelineCacheBroken) {
    start_pipeline_cache_writer();
  }
}

void shutdown_pipeline_cache() {
  enhanced::stop_prewarm(); // 0223
  if (g_hasPipelineThread) {
    {
      std::lock_guard lock{g_pipelineMutex};
      g_pipelineThreadEnd = true;
    }
    g_pipelineQueueCv.notify_all();
    g_pipelineReadyCv.notify_all();
    g_boostCv.notify_all();
    for (auto& thread : g_pipelineThreads) {
      thread.join();
    }
    g_pipelineThreads.clear();
  }
  g_hasPipelineThread = false;

  stop_pipeline_cache_writer();
  pipeline_cache_abort();
  g_pipelineCacheBroken = false;
  g_pipelinesPerFrame = 0;
  g_gpuCachePrunePending = false;
  g_gpuCachePruneArmed = false;
  g_rebuildAccuracy.reset();
  g_rebuildEnhBits.reset();
  g_rebuildPending = 0;
  g_pipelines.clear();
  gx::clear_shader_module_cache(); // 0227
  g_knownPipelines.clear();
  g_pipelineLayoutKey.reset();
  g_pipelineQueue.clear();
  g_backgroundPipelineQueue.clear();
  g_pendingPipelines.clear();

  queuedPipelines = 0;
  createdPipelines = 0;
  {
    std::lock_guard lock{g_badMutex};
    g_badPipelines.clear();
  }
  journal_close(); // clean exit
}

static void log_skipped_draws(); // 0221

void begin_pipeline_frame() {
  log_skipped_draws();
  if (!g_hasPipelineThread) {
    g_pipelinesPerFrame = 0;
  }
  journal_update_phase();
  if (g_pipelineLayoutKey != scene_render_target_layout().key ||
      g_rebuildAccuracy != gx::g_accuracyFlags.load(std::memory_order_relaxed) ||
      g_rebuildEnhBits != enhanced::precache_bits()) {
    rebuild_pipeline_cache();
  }
}

void end_pipeline_frame() {
  if (!g_hasPipelineThread) {
    pipeline_worker(0);
  }
}

// 0028: the pipeline if it is built, without waiting (optional passes skip draws instead).
bool try_get_pipeline(PipelineRef ref, CompiledPipeline& pipeline) {
  std::lock_guard guard{g_pipelineMutex};
  const auto it = g_pipelines.find(ref);
  if (it == g_pipelines.end() || !it->second.main) {
    return false;
  }
  pipeline = it->second;
  return true;
}

// 0221: a draw skipped for a missing pipeline: counted (per-second log line in
// begin_pipeline_frame) and, the first time for this pipeline, a warning with its config.
static void note_skipped_draw(PipelineRef ref, const char* what, const char* why) {
  ++g_droppedDraws;
  ++g_skipWindow;
  if (std::strcmp(what, "shadow caster") == 0) {
    ++g_skipWindowCasters;
  }
  {
    std::lock_guard lock{g_skipMutex};
    if (g_skipWarned.size() >= 4096 || !g_skipWarned.insert(ref).second) {
      return;
    }
  }
  std::string detail;
  bool pending = false;
  {
    std::lock_guard guard{g_pipelineMutex};
    if (const auto it = g_missDetails.find(ref); it != g_missDetails.end()) {
      detail = it->second;
    }
    pending = g_pendingPipelines.contains(ref);
  }
  Log.warn("{} skipped: pipeline {:016x} {} (frame {}, {}){}", what, ref, why, current_frame(),
           pending ? "still queued" : "not queued", detail.empty() ? std::string(" config: precache list entry") : detail);
}

static void log_skipped_draws() {
  using Clock = std::chrono::steady_clock;
  static Clock::time_point sWindow = Clock::now();
  const auto now = Clock::now();
  const double secs = std::chrono::duration<double>(now - sWindow).count();
  if (secs < 1.0) {
    return;
  }
  sWindow = now;
  const uint32_t n = g_skipWindow.exchange(0, std::memory_order_relaxed);
  const uint32_t casters = g_skipWindowCasters.exchange(0, std::memory_order_relaxed);
  if (n != 0) {
    Log.warn("{} draws skipped for a missing pipeline in the last {:.1f} s ({} shadow casters)", n, secs, casters);
  }
}

bool get_pipeline(PipelineRef ref, CompiledPipeline& pipeline, const char* what) {
  bool failed = false;
  {
    std::lock_guard guard{g_pipelineMutex};
    const auto it = g_pipelines.find(ref);
    if (it != g_pipelines.end()) {
      if (it->second.main) {
        pipeline = it->second;
        return true;
      }
      failed = true;
    }
  }
  if (failed) {
    note_skipped_draw(ref, what, "creation failed"); // (takes g_pipelineMutex itself)
    return false;
  }
  // Not built yet: never drop the draw, wait for it.
  if (!g_waitForPipelines.load(std::memory_order_relaxed)) {
    note_skipped_draw(ref, what, "not built yet (waiting off)");
    return false;
  }
  if (!wait_for_pending_pipeline(ref, pipeline)) {
    note_skipped_draw(ref, what, "not built and not queued, or creation failed");
    return false;
  }
  return true;
}

// ---- Bad configs and the crash journal (patch 0017) ------------------------------------------

static void mark_pipeline_bad(ShaderType type, HashType cacheKey, BadReason reason, const char* why) {
  uint32_t crashes = 0;
  {
    std::lock_guard lock{g_badMutex};
    auto [it, inserted] = g_badPipelines.try_emplace(cacheKey, BadPipeline{type, reason, 0});
    auto& entry = it->second;
    if (reason == BadReason::CrashSuspect) {
      if (!inserted && entry.reason == BadReason::Invalid) {
        return; // already refused for good
      }
      entry.crashes += 1;
    }
    entry.type = type;
    entry.reason = reason;
    crashes = entry.crashes;
  }
  Log.warn("pipeline config {:016x} (type {}) recorded as {}: {}{}", cacheKey, underlying(type),
           reason == BadReason::Invalid ? "bad" : "crash suspect", why,
           reason == BadReason::CrashSuspect && crashes >= 2 ? " (repeatedly; it will not be built any more)" : "");
  PipelineCacheWrite write{
      .type = type,
      .hash = cacheKey,
      .configVersion = 0,
      .op = CacheWriteOp::MarkBad,
      .badReason = underlying(reason),
      .badCrashes = crashes,
  };
  enqueue_pipeline_cache_write(std::move(write));
}

// The journal is a small memory-mapped file next to the caches. Each thread that creates a
// pipeline writes the config's key into its slot before and clears it after; the session phase
// says whether the process is starting up (precaching), running, or exited cleanly. Stores to a
// shared file mapping survive a crash of the process (the OS writes the pages back), so the next
// launch knows exactly which configs were being built when it died, and whether it died during
// startup. That breaks crash-on-startup loops a bad cache entry would otherwise cause:
//   - configs in an active slot become crash suspects (not precached any more; see above);
//   - a startup crash with no config in flight (e.g. a damaged Dawn cache blob) deletes both
//     caches; a second consecutive startup crash also ignores the shipped precache list.
namespace {
constexpr uint32_t JournalMagic = 0x4E4A5541; // "AUJN"
constexpr uint32_t JournalVersion = 2; // 2: crashed flag (0231)
constexpr uint32_t JournalSlots = 64;
constexpr size_t JournalSize = 4096;
enum JournalPhase : uint32_t {
  PhaseClean = 0,   // exited through shutdown_pipeline_cache
  PhaseStartup = 1, // from launch until the startup precache finished
  PhaseRunning = 2,
};
struct JournalSlot {
  std::atomic<uint64_t> key;
  std::atomic<uint32_t> type;
  std::atomic<uint32_t> active;
};
struct JournalData {
  uint32_t magic;
  uint32_t version;
  std::atomic<uint32_t> phase;
  uint32_t startupCrashes; // consecutive launches that crashed during startup
  // 0231: set by aurora_pipeline_cache_note_crash (the application's crash handler). A session
  // that ended uncleanly without it was closed or killed (task manager, a debugger, the OS at
  // shutdown, a test harness) or lost power: no crash suspects, nothing deleted.
  std::atomic<uint32_t> crashed;
  JournalSlot slots[JournalSlots];
};
static_assert(sizeof(JournalData) <= JournalSize);
static_assert(std::atomic<uint64_t>::is_always_lock_free && std::atomic<uint32_t>::is_always_lock_free);

JournalData* g_journal = nullptr;
#ifdef _WIN32
HANDLE g_journalFile = INVALID_HANDLE_VALUE;
HANDLE g_journalMapping = nullptr;
#else
int g_journalFd = -1;
#endif
std::atomic<uint32_t> g_journalNextSlot{0};
thread_local int t_journalSlot = -1;
uint32_t g_journalFrames = 0;
std::once_flag g_journalOnce;

void* journal_map(const std::filesystem::path& path) {
#ifdef _WIN32
  g_journalFile = CreateFileW(path.wstring().c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (g_journalFile == INVALID_HANDLE_VALUE) {
    return nullptr;
  }
  g_journalMapping = CreateFileMappingW(g_journalFile, nullptr, PAGE_READWRITE, 0, JournalSize, nullptr);
  if (g_journalMapping == nullptr) {
    CloseHandle(g_journalFile);
    g_journalFile = INVALID_HANDLE_VALUE;
    return nullptr;
  }
  void* view = MapViewOfFile(g_journalMapping, FILE_MAP_ALL_ACCESS, 0, 0, JournalSize);
  if (view == nullptr) {
    CloseHandle(g_journalMapping);
    CloseHandle(g_journalFile);
    g_journalMapping = nullptr;
    g_journalFile = INVALID_HANDLE_VALUE;
  }
  return view;
#else
  g_journalFd = open(path.c_str(), O_RDWR | O_CREAT, 0644);
  if (g_journalFd < 0) {
    return nullptr;
  }
  if (ftruncate(g_journalFd, JournalSize) != 0) {
    close(g_journalFd);
    g_journalFd = -1;
    return nullptr;
  }
  void* view = mmap(nullptr, JournalSize, PROT_READ | PROT_WRITE, MAP_SHARED, g_journalFd, 0);
  if (view == MAP_FAILED) {
    close(g_journalFd);
    g_journalFd = -1;
    return nullptr;
  }
  return view;
#endif
}

void reset_caches_after_crash(const char* why) {
  Log.warn("{}: deleting the pipeline cache and the Dawn shader cache (they are rebuilt)", why);
  delete_sqlite_files(pipeline_cache_db_path());
  webgpu::cache_delete_files();
}

void check_crash_journal_once() {
  if (g_config.cachePath == nullptr) {
    return;
  }
  const auto path = io::fs_path_from_string(g_config.cachePath) / "pipeline_cache.journal";
  auto* data = static_cast<JournalData*>(journal_map(path));
  if (data == nullptr) {
    Log.warn("Could not open the pipeline crash journal '{}'; crash recovery disabled", io::fs_path_to_string(path));
    return;
  }
  g_journal = data;
  if (data->magic != JournalMagic || data->version != JournalVersion) {
    std::memset(static_cast<void*>(data), 0, JournalSize);
    data->magic = JournalMagic;
    data->version = JournalVersion;
  }

  const uint32_t prevPhase = data->phase.load();
  const bool unclean = prevPhase != PhaseClean;
  const bool crashed = unclean && data->crashed.load() != 0;
  data->crashed.store(0);
  // 0226: verify pipeline_cache.db on open only after a session that did not exit cleanly
  // (0231: dawn_cache.db is verified per entry as it is read instead).
  webgpu::g_cacheVerifyOnOpen = unclean;
  g_prevExit = !unclean ? 0u : crashed ? 2u : 1u;
  // 0231: configs that were being built are crash suspects only when the run really crashed
  // (once each: one config can be in flight for several render target layouts at once).
  absl::flat_hash_set<HashType> inFlight;
  for (auto& slot : data->slots) {
    if (crashed && slot.active.load() != 0) {
      const auto type = slot.type.load();
      const auto key = static_cast<HashType>(slot.key.load());
      if (type <= 255 && inFlight.insert(key).second) {
        g_journalSuspects.emplace_back(static_cast<ShaderType>(type), key);
      }
    }
    slot.active.store(0);
  }
  const auto suspects = static_cast<uint32_t>(inFlight.size());
  if (!unclean) {
    data->startupCrashes = 0;
  } else if (!crashed) {
    // 0231: closed or killed, not crashed. This used to count as a startup crash: with no
    // pipeline in flight (e.g. killed in the first seconds of play) both caches were deleted and
    // the next start compiled everything again (over 140 s with Enhanced lighting); killed while
    // compiling, the configs in flight were refused as crash suspects. Everything written before
    // is kept: Dawn stores each compiled shader and pipeline as it is made.
    Log.warn("The previous run ended {} without a crash report (closed or killed): the shader caches are kept; "
             "only shaders it had not finished are compiled",
             prevPhase == PhaseStartup ? "during startup" : "while running");
  } else if (prevPhase == PhaseRunning) {
    // A crash after startup (only its in-flight configs are suspect).
    data->startupCrashes = 0;
    Log.warn("The previous run crashed{}", suspects > 0 ? fmt::format(" while creating {} pipeline(s)", suspects)
                                                        : std::string{});
  } else {
    data->startupCrashes = std::min(data->startupCrashes + 1, 100u);
    Log.warn("The previous run crashed during startup ({} time(s) in a row, {} pipeline(s) in flight)",
             data->startupCrashes, suspects);
    // 0231: the caches are only reset to break a crash loop (third crash in a row at startup);
    // damaged entries are found by their checksums as they are read.
    if (data->startupCrashes >= 3) {
      reset_caches_after_crash("Repeated startup crashes");
      g_cachesReset = true;
      Log.warn("Repeated startup crashes: ignoring the shipped pipeline precache list for this run");
      g_safeMode = true;
    }
  }
  data->phase.store(PhaseStartup);
  g_journalFrames = 0;
}
} // namespace

void check_crash_journal() { std::call_once(g_journalOnce, check_crash_journal_once); }

static void journal_begin(ShaderType type, HashType cacheKey) {
  if (g_journal == nullptr) {
    return;
  }
  if (t_journalSlot < 0) {
    t_journalSlot = static_cast<int>(g_journalNextSlot.fetch_add(1) % JournalSlots);
  }
  auto& slot = g_journal->slots[t_journalSlot];
  slot.key.store(static_cast<uint64_t>(cacheKey), std::memory_order_relaxed);
  slot.type.store(underlying(type), std::memory_order_relaxed);
  slot.active.store(1, std::memory_order_release);
}

static void journal_end() {
  if (g_journal == nullptr || t_journalSlot < 0) {
    return;
  }
  g_journal->slots[t_journalSlot].active.store(0, std::memory_order_release);
}

// Startup ends once the first precache pass has finished (and at least a few seconds of frames
// were shown): a crash after that is not a startup loop.
static void journal_update_phase() {
  if (g_journal == nullptr || g_journal->phase.load(std::memory_order_relaxed) != PhaseStartup) {
    return;
  }
  if (++g_journalFrames < 30) { // 0231: was 300 (a kill in the first seconds of play counted as startup)
    return;
  }
  bool idle;
  {
    std::lock_guard lock{g_pipelineMutex};
    idle = g_pendingPipelines.empty();
  }
  if (idle && g_precacheDone.load() >= g_precacheTotal.load()) {
    g_journal->startupCrashes = 0;
    g_journal->phase.store(PhaseRunning);
    Log.info("Pipeline startup finished; crash journal now in running phase");
  }
}

static void journal_close() {
  if (g_journal == nullptr) {
    return;
  }
  for (auto& slot : g_journal->slots) {
    slot.active.store(0);
  }
  g_journal->phase.store(PhaseClean);
  // The view stays mapped until the process exits (late pipeline creations on other threads may
  // still touch their slot); only flush it.
#ifdef _WIN32
  FlushViewOfFile(g_journal, JournalSize);
#else
  msync(g_journal, JournalSize, MS_ASYNC);
#endif
}

// The Dawn cache opens lazily (possibly while the device is created, before
// initialize_pipeline_cache): run the journal check before it opens.
static const bool g_journalHookInstalled = [] {
  webgpu::g_cacheOpenHook = &check_crash_journal;
  return true;
}();

// ---- Precache list files (include/aurora/pipeline_cache.h) ----------------------------------

namespace {
constexpr uint32_t PrecacheMagic = 0x43505541; // "AUPC"
// Format 2 (patch 0017) adds a CRC-32 of the raw payload to the header and one per row; format 1
// lists are still read (rows validated by field ranges only).
constexpr uint32_t PrecacheFormat = 2;
constexpr uint32_t PrecacheFlagZstd = 1;
constexpr size_t PrecacheHeaderSize = 4 + 4 + 4 + 4 + 8;

uint32_t crc32(const uint8_t* data, size_t size) {
  static const auto table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) {
        c = (c & 1) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      }
      t[i] = c;
    }
    return t;
  }();
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < size; ++i) {
    crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFu;
}

template <typename T>
bool read_le(const uint8_t*& p, const uint8_t* end, T& out) {
  if (static_cast<size_t>(end - p) < sizeof(T)) {
    return false;
  }
  std::memcpy(&out, p, sizeof(T)); // little-endian hosts only (x86/ARM)
  p += sizeof(T);
  return true;
}

template <typename T>
void write_le(std::vector<uint8_t>& buf, T value) {
  const auto* b = reinterpret_cast<const uint8_t*>(&value);
  buf.insert(buf.end(), b, b + sizeof(T));
}

std::vector<uint8_t> read_file(const char* path) {
  std::vector<uint8_t> data;
  SDL_IOStream* io = SDL_IOFromFile(path, "rb");
  if (io == nullptr) {
    return data;
  }
  const Sint64 size = SDL_GetIOSize(io);
  if (size > 0) {
    data.resize(static_cast<size_t>(size));
    if (SDL_ReadIO(io, data.data(), data.size()) != data.size()) {
      data.clear();
    }
  }
  SDL_CloseIO(io);
  return data;
}

// nullptr = accepted; "version" = for another Aurora version (silently skipped); else the reason.
template <typename Config>
const char* seed_row(ShaderType type, uint32_t version, const uint8_t* bytes, uint32_t size, uint32_t firstFrame) {
  if (size != sizeof(Config) || version != Config{}.version) {
    return "version";
  }
  Config config;
  std::memcpy(&config, bytes, sizeof(config));
  if (const char* reason = validate_config(config)) {
    return reason;
  }
  if (const auto key = xxh3_hash(config, static_cast<HashType>(type)); is_quarantined(key)) {
    std::lock_guard lock{g_badMutex};
    if (g_badPipelines[key].reason == BadReason::Invalid) {
      return "recorded as bad";
    }
  }
  remember_pipeline_config(type, canonical_known(config), firstFrame, false);
  return nullptr;
}
} // namespace

} // namespace aurora::gfx

using namespace aurora::gfx;

uint32_t aurora_pipeline_cache_seed(const char* path) {
  if (g_safeMode) {
    Log.warn("Pipeline precache list '{}' ignored after repeated startup crashes", path);
    return 0;
  }
  const auto file = read_file(path);
  if (file.size() < PrecacheHeaderSize) {
    Log.info("No pipeline precache list at '{}'", path);
    return 0;
  }
  const uint8_t* p = file.data();
  const uint8_t* end = p + file.size();
  uint32_t magic = 0, format = 0, flags = 0, count = 0, payloadCrc = 0, reserved = 0;
  uint64_t rawSize = 0;
  read_le(p, end, magic);
  read_le(p, end, format);
  read_le(p, end, flags);
  read_le(p, end, count);
  read_le(p, end, rawSize);
  if (magic != PrecacheMagic || (format != 1 && format != 2) || rawSize > (64ull << 20)) {
    Log.warn("Pipeline precache list '{}' has an unknown format", path);
    return 0;
  }
  if (format >= 2 && (!read_le(p, end, payloadCrc) || !read_le(p, end, reserved))) {
    Log.warn("Pipeline precache list '{}' is truncated", path);
    return 0;
  }
  std::vector<uint8_t> payload;
  if ((flags & PrecacheFlagZstd) != 0) {
#if defined(AURORA_CACHE_USE_ZSTD)
    payload.resize(static_cast<size_t>(rawSize));
    const size_t ret = ZSTD_decompress(payload.data(), payload.size(), p, static_cast<size_t>(end - p));
    if (ZSTD_isError(ret) || ret != rawSize) {
      Log.warn("Pipeline precache list '{}' failed to decompress", path);
      return 0;
    }
#else
    Log.warn("Pipeline precache list '{}' is compressed but zstd support is disabled", path);
    return 0;
#endif
  } else {
    payload.assign(p, end);
  }
  if (format >= 2 && crc32(payload.data(), payload.size()) != payloadCrc) {
    // Keep going: every row carries its own CRC, damaged rows are skipped below.
    Log.warn("Pipeline precache list '{}' fails its checksum; checking each row", path);
  }
  p = payload.data();
  end = p + payload.size();
  uint32_t accepted = 0, otherVersion = 0, rejected = 0;
  for (uint32_t i = 0; i < count; ++i) {
    uint8_t type = 0;
    uint32_t version = 0, size = 0, firstFrame = 0, rowCrc = 0;
    if (!read_le(p, end, type) || !read_le(p, end, version) || !read_le(p, end, size) ||
        !read_le(p, end, firstFrame) || (format >= 2 && !read_le(p, end, rowCrc)) ||
        static_cast<size_t>(end - p) < size) {
      Log.warn("Pipeline precache list '{}' is truncated", path);
      break;
    }
    const char* reason = "unknown type";
    if (format >= 2 && crc32(p, size) != rowCrc) {
      reason = "checksum mismatch";
    } else {
      switch (static_cast<ShaderType>(type)) {
      case ShaderType::Clear:
        reason = seed_row<clear::PipelineConfig>(ShaderType::Clear, version, p, size, firstFrame);
        break;
      case ShaderType::GX:
        reason = seed_row<aurora::gx::PipelineConfig>(ShaderType::GX, version, p, size, firstFrame);
        break;
      default:
        break;
      }
    }
    p += size;
    if (reason == nullptr) {
      ++accepted;
    } else if (std::strcmp(reason, "version") == 0) {
      ++otherVersion;
    } else {
      if (rejected < 16) {
        Log.warn("Pipeline precache list '{}': row {} rejected: {}", path, i, reason);
      }
      ++rejected;
    }
  }
  g_seeded += accepted;
  Log.info("Pipeline precache list '{}': {} configs ({} for another Aurora version skipped, {} invalid)", path,
           accepted, otherVersion, rejected);
  return accepted;
}

bool aurora_pipeline_cache_export(const char* path) {
  std::vector<uint8_t> payload;
  uint32_t count = 0;
  {
    std::lock_guard lock{g_pipelineMutex};
    for (const auto& known : g_knownPipelines | std::views::values) {
      std::visit(
          [&]<typename Config>(const Config& config) {
            if constexpr (!std::is_same_v<Config, clear::PipelineConfig> &&
                          !std::is_same_v<Config, aurora::gx::PipelineConfig>) {
              return;
            } else {
              write_le<uint8_t>(payload, static_cast<uint8_t>(known.type));
              write_le<uint32_t>(payload, config.version);
              write_le<uint32_t>(payload, sizeof(Config));
              write_le<uint32_t>(payload, known.firstFrameUsed);
              const auto* b = reinterpret_cast<const uint8_t*>(&config);
              write_le<uint32_t>(payload, crc32(b, sizeof(Config)));
              payload.insert(payload.end(), b, b + sizeof(Config));
              ++count;
            }
          },
          known.config);
    }
  }
  std::vector<uint8_t> file;
  write_le<uint32_t>(file, PrecacheMagic);
  write_le<uint32_t>(file, PrecacheFormat);
  write_le<uint32_t>(file, 0);
  write_le<uint32_t>(file, count);
  write_le<uint64_t>(file, payload.size());
  write_le<uint32_t>(file, crc32(payload.data(), payload.size()));
  write_le<uint32_t>(file, 0);
  file.insert(file.end(), payload.begin(), payload.end());
  SDL_IOStream* io = SDL_IOFromFile(path, "wb");
  if (io == nullptr) {
    return false;
  }
  const bool ok = SDL_WriteIO(io, file.data(), file.size()) == file.size();
  SDL_CloseIO(io);
  Log.info("Exported {} pipeline configs to '{}'", count, path);
  return ok;
}

void aurora_pipeline_cache_get_stats(AuroraPipelineStats* out) {
  if (out == nullptr) {
    return;
  }
  *out = {};
  out->precacheTotal = g_precacheTotal.load(std::memory_order_relaxed);
  out->precacheDone = g_precacheDone.load(std::memory_order_relaxed);
  {
    std::lock_guard lock{g_pipelineMutex};
    out->pending = static_cast<uint32_t>(g_pendingPipelines.size());
  }
  out->demandCreated = g_demandCreated.load(std::memory_order_relaxed);
  out->stalledDraws = g_stalledDraws.load(std::memory_order_relaxed);
  out->stallNs = g_stallNs.load(std::memory_order_relaxed);
  out->maxStallNs = g_maxStallNs.load(std::memory_order_relaxed);
  out->workerThreads = aurora::gfx::g_activeWorkers.load(std::memory_order_relaxed);
  out->seeded = g_seeded.load(std::memory_order_relaxed);
  out->droppedDraws = g_droppedDraws.load(std::memory_order_relaxed);
  out->rebuildPending = g_rebuildPending.load(std::memory_order_relaxed);
  // 0226
  const auto cache = aurora::webgpu::cache_stats();
  out->blobHits = cache.hits;
  out->blobMisses = cache.misses;
  out->blobStores = cache.stores;
  out->blobLoadNs = cache.loadNs;
  out->blobStoreNs = cache.storeNs;
  out->blobCacheBroken = cache.broken ? 1 : 0;
  out->blobEntriesAtOpen = cache.entriesAtOpen;
  out->blobSession = cache.session;
  out->rebuildCount = aurora::gfx::g_rebuildCount.load(std::memory_order_relaxed);
  out->prewarmPending = aurora::gfx::g_prewarmPending.load(std::memory_order_relaxed) ? 1 : 0;
  out->modulesReused = aurora::gfx::g_moduleReuse.load(std::memory_order_relaxed);
  out->prevExit = aurora::gfx::g_prevExit;
  out->cachesReset = aurora::gfx::g_cachesReset ? 1 : 0;
  out->wgslNs = aurora::gfx::g_createNs[static_cast<size_t>(aurora::gfx::CreateStage::Wgsl)].load(std::memory_order_relaxed);
  out->moduleNs = aurora::gfx::g_createNs[static_cast<size_t>(aurora::gfx::CreateStage::Module)].load(std::memory_order_relaxed);
  out->pipelineNs = aurora::gfx::g_createNs[static_cast<size_t>(aurora::gfx::CreateStage::Pipeline)].load(std::memory_order_relaxed);
}

void aurora_pipeline_cache_set_log(bool enabled) { g_pipelineLog = enabled; }

void aurora_pipeline_cache_set_wait(bool enabled) { g_waitForPipelines = enabled; }

void aurora_pipeline_cache_set_threads(uint32_t threads) { g_requestedThreads = std::min(threads, 32u); }

void aurora_pipeline_cache_note_crash(void) {
  // 0231: called from a crash handler: one store into the mapped journal (the OS writes the page
  // back after the process dies).
  if (auto* journal = aurora::gfx::g_journal) {
    journal->crashed.store(1);
  }
}

uint32_t aurora_pipeline_cache_describe_stuck(uint64_t minNs, char* buf, size_t bufSize) {
  using namespace aurora::gfx;
  const uint64_t now = now_ns();
  uint32_t count = 0;
  std::string text;
  for (auto& slot : g_inflight) {
    const uint64_t start = slot.startNs.load(std::memory_order_acquire);
    if (start == 0 || start > now || now - start < minNs) {
      continue;
    }
    ++count;
    if (!text.empty()) {
      text += ", ";
    }
    text += fmt::format("config {:016x} (type {}) for {:.0f} s", slot.key.load(std::memory_order_relaxed),
                        slot.type.load(std::memory_order_relaxed), (now - start) / 1e9);
  }
  if (buf != nullptr && bufSize > 0) {
    const size_t n = std::min(text.size(), bufSize - 1);
    std::memcpy(buf, text.data(), n);
    buf[n] = '\0';
  }
  return count;
}

void aurora_pipeline_cache_set_boost(bool enabled) {
  using namespace aurora::gfx;
  if (!g_hasPipelineThread) {
    return;
  }
  const uint32_t want = enabled ? g_boostWorkers : g_workerThreadCount;
  {
    std::lock_guard lock{g_pipelineMutex};
    if (g_activeWorkers.load(std::memory_order_relaxed) == want) {
      return;
    }
    g_activeWorkers = want;
  }
  g_boostCv.notify_all();
  g_pipelineQueueCv.notify_all();
}
