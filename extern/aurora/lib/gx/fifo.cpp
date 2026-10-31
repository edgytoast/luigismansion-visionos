#include "fifo.hpp"

#include "../thread.hpp"
#include "../activity.hpp"
#include "command_processor.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <chrono>
#include <thread>

#include <tracy/Tracy.hpp>

namespace aurora::gx::fifo {
namespace detail {
uint8_t* sBufferData = nullptr;
uint32_t sBufferSize = 0;
uint32_t sBufferCapacity = 0;
thread_local Recording tOwnRecording;
thread_local Recording* tJoinedRecording = nullptr;
} // namespace detail

namespace {
constexpr Module Log{"aurora::gx::fifo"};
// AURORA_FIFO_INLINE=1 (debug): process the FIFO on the thread that publishes it, so every draw
// reads guest memory (arrays, matrices, textures) at the moment it is issued.
const ProcessingMode kProcessingMode = [] {
  const char* e = std::getenv("AURORA_FIFO_INLINE");
  return e != nullptr && e[0] == '1' ? ProcessingMode::Inline : ProcessingMode::Thread;
}();
constexpr uint32_t kDrawBatchSize = 1;

bool sFrameActive = false;
uint32_t sPendingDraws = 0;
std::atomic<uint64_t> sPublished{0};
std::atomic<uint64_t> sProcessed{0};
uint64_t sStreamBase = 0;
std::mutex sBufferMutex;
std::atomic<uint32_t> sWorkerWake{0};
thread::Thread sWorkerThread;
std::atomic<DrawDoneCallback> sDrawDoneCallback{nullptr};

void dispatch_draw_done() noexcept {
  if (const auto callback = sDrawDoneCallback.load(std::memory_order_acquire); callback != nullptr) {
    callback();
  }
}

void wake_worker() noexcept {
  sWorkerWake.fetch_add(1, std::memory_order_release);
  sWorkerWake.notify_all();
}

void process_to(uint64_t target, std::memory_order order) noexcept {
  uint64_t processed = sProcessed.load(std::memory_order_relaxed);
  while (processed < target) {
    ProcessResult result{};
    {
      std::lock_guard lock{sBufferMutex};
      AURORA_ASSERT(processed >= sStreamBase && target <= sStreamBase + detail::sBufferSize,
                    "FIFO processing range [{}, {}) is outside buffered range [{}, {})", processed, target, sStreamBase,
                    sStreamBase + detail::sBufferSize);
      const auto start = static_cast<uint32_t>(processed - sStreamBase);
      const auto size = static_cast<uint32_t>(target - processed);
      result = process(detail::sBufferData + start, size, processed);
    }
    AURORA_ASSERT(result.bytesProcessed > 0 && result.bytesProcessed <= target - processed,
                  "FIFO processor made invalid progress: processed {} of {} remaining bytes", result.bytesProcessed,
                  target - processed);
    if (result.drawDone) {
      dispatch_draw_done();
    }
    processed += result.bytesProcessed;
    sProcessed.store(processed, order);
    sProcessed.notify_all();
  }
}

// AURORA_FIFO_DELAY_US=<n> (debug): the worker sleeps n microseconds before each batch it
// processes, so it lags the publishing thread the way a busy GPU would (provokes CPU/GPU races).
const uint32_t kWorkerDelayUs = [] {
  const char* e = std::getenv("AURORA_FIFO_DELAY_US");
  return e != nullptr ? static_cast<uint32_t>(std::strtoul(e, nullptr, 10)) : 0u;
}();

void worker_main(std::stop_token token) noexcept {
  std::stop_callback wakeOnStop{token, wake_worker};
  while (true) {
    const uint32_t event = sWorkerWake.load(std::memory_order_acquire);
    const uint64_t processed = sProcessed.load(std::memory_order_relaxed);
    const uint64_t published = sPublished.load(std::memory_order_acquire);
    if (published != processed) {
      if (kWorkerDelayUs != 0) {
        std::this_thread::sleep_for(std::chrono::microseconds(kWorkerDelayUs));
      }
      const activity::Scope busy{activity::Thread::Fifo, "processing GX commands"};
      process_to(published, std::memory_order_release);
      thread::tick();
      continue;
    }

    if (token.stop_requested()) {
      break;
    }
    sWorkerWake.wait(event, std::memory_order_acquire);
  }
}

void start_worker() {
  if (kProcessingMode != ProcessingMode::Thread || sWorkerThread.joinable()) {
    return;
  }
  sWorkerThread = thread::Thread{{
                                     .name = "Aurora FIFO processor",
                                     .affinity = thread::Affinity::SharedCache,
                                 },
                                 worker_main};
}

void stop_worker() {
  if (!sWorkerThread.joinable()) {
    return;
  }
  sWorkerThread.request_stop();
  sWorkerThread.join();
}
} // namespace

ProcessingMode processing_mode() noexcept { return kProcessingMode; }

void init() {
  stop_worker();

  constexpr uint32_t initialCapacity = 64 * 1024;
  free(detail::sBufferData);
  detail::sBufferData = static_cast<uint8_t*>(malloc(initialCapacity));
  AURORA_ASSERT(detail::sBufferData != nullptr, "fifo::init: failed to allocate {} bytes", initialCapacity);
  detail::sBufferSize = 0;
  detail::sBufferCapacity = initialCapacity;
  detail::recording() = {};

  sFrameActive = false;
  sPendingDraws = 0;
  sStreamBase = 0;
  sPublished.store(0, std::memory_order_relaxed);
  sProcessed.store(0, std::memory_order_relaxed);
  sWorkerWake.store(0, std::memory_order_relaxed);

  start_worker();
}

void shutdown() { stop_worker(); }

void begin_frame() noexcept { sFrameActive = true; }

void end_frame() noexcept {
  sFrameActive = false;
  clear_draw_cache(); // command_processor
}

void write_data_grow(const void* data, uint32_t length) {
  const uint64_t needed64 = static_cast<uint64_t>(detail::sBufferSize) + length;
  AURORA_ASSERT(needed64 <= std::numeric_limits<uint32_t>::max(), "fifo::write_data: buffer size overflow");
  const auto needed = static_cast<uint32_t>(needed64);
  const auto doubledCapacity = static_cast<uint64_t>(detail::sBufferCapacity) * 2;
  const auto newCapacity = static_cast<uint32_t>(
      std::min<uint64_t>(std::max(doubledCapacity, needed64), std::numeric_limits<uint32_t>::max()));
  const auto grow = [newCapacity] {
    auto* resized = static_cast<uint8_t*>(realloc(detail::sBufferData, newCapacity));
    AURORA_ASSERT(resized != nullptr, "fifo::write_data: failed to allocate {} bytes", newCapacity);
    detail::sBufferData = resized;
  };
  if (sWorkerThread.joinable()) {
    std::lock_guard lock{sBufferMutex};
    grow();
  } else {
    grow();
  }
  std::memcpy(detail::sBufferData + detail::sBufferSize, data, length);
  detail::sBufferSize = needed;
  detail::sBufferCapacity = newCapacity;
}

void publish() noexcept {
  if (!sFrameActive || kProcessingMode == ProcessingMode::Drain || detail::recording().active) {
    return;
  }

  const uint64_t target = sStreamBase + detail::sBufferSize;
  if (target > sPublished.load(std::memory_order_relaxed)) {
    sPendingDraws = 0;
    sPublished.store(target, std::memory_order_release);
    if (kProcessingMode == ProcessingMode::Thread) {
      wake_worker();
    } else {
      process_to(target, std::memory_order_relaxed);
    }
  }
}

DrawDoneCallback set_draw_done_callback(DrawDoneCallback callback) noexcept {
  return sDrawDoneCallback.exchange(callback, std::memory_order_acq_rel);
}

void finish_draw() noexcept {
  if (!sFrameActive || kProcessingMode == ProcessingMode::Drain || detail::recording().active) {
    return;
  }
  if (++sPendingDraws >= kDrawBatchSize) {
    publish();
  }
}

void patch_u32(uint32_t offset, uint32_t val) {
  AURORA_ASSERT(!detail::recording().active && offset <= detail::sBufferSize &&
                    sizeof(uint32_t) <= detail::sBufferSize - offset,
                "fifo::patch_u32: invalid patch offset {} (buffer size {})", offset, detail::sBufferSize);
  AURORA_ASSERT(sStreamBase + offset >= sPublished.load(std::memory_order_relaxed),
                "fifo::patch_u32: offset {} is below the published watermark", offset);
  const auto out = bswap(val);
  std::memcpy(detail::sBufferData + offset, &out, sizeof(out));
}

namespace {
std::atomic<detail::Recording*> sLastRecording{nullptr}; // 0235: what join_recording joins
bool sLastOverflow = false;
} // namespace

void begin_display_list(uint8_t* buf, uint32_t size) {
  auto& rec = detail::recording();
  rec.active = true;
  rec.overflow = false;
  rec.buffer = buf;
  rec.size = size;
  rec.pos = 0;
  if (detail::tJoinedRecording == nullptr) {
    sLastRecording.store(&rec, std::memory_order_release);
  }
}

uint32_t end_display_list() {
  auto& rec = detail::recording();
  rec.active = false;
  const uint32_t bytesWritten = rec.pos;
  const uint32_t padded = (bytesWritten + 31) & ~31u;
  while (rec.pos < padded && rec.pos < rec.size) {
    rec.buffer[rec.pos++] = 0;
  }
  sLastOverflow = rec.overflow;
  rec.buffer = nullptr;
  rec.size = 0;
  rec.pos = 0;
  rec.overflow = false;
  return padded;
}

bool display_list_overflowed() { return sLastOverflow; }

void join_recording(bool join) {
  if (!join) {
    detail::tJoinedRecording = nullptr;
    return;
  }
  auto* last = sLastRecording.load(std::memory_order_acquire);
  detail::tJoinedRecording = last == &detail::tOwnRecording ? nullptr : last;
}

bool in_display_list() { return detail::recording().active; }

uint32_t display_list_pos() { return detail::recording().pos; }

void replay(const uint8_t* data, uint32_t size) {
  AURORA_ASSERT(!detail::recording().active, "fifo::replay: called while recording");
  if (size <= detail::sBufferCapacity - detail::sBufferSize) {
    std::memcpy(detail::sBufferData + detail::sBufferSize, data, size);
    detail::sBufferSize += size;
  } else {
    write_data_grow(data, size);
  }
  publish();
}

void drain() {
  // A thread that is recording has nothing of its own in the FIFO.
  if (detail::recording().active || detail::sBufferSize == 0) {
    return;
  }

  ZoneScoped;
  const uint64_t target = sStreamBase + detail::sBufferSize;

  switch (kProcessingMode) {
  case ProcessingMode::Drain:
  case ProcessingMode::Inline:
    sPublished.store(target, std::memory_order_relaxed);
    process_to(target, std::memory_order_relaxed);
    break;
  case ProcessingMode::Thread: {
    sPublished.store(target, std::memory_order_release);
    wake_worker();
    const activity::Scope busy{activity::Thread::Host, "waiting for the GX FIFO thread (drain)"};

    uint64_t processed = sProcessed.load(std::memory_order_acquire);
    if (processed < target) {
      do {
        sProcessed.wait(processed, std::memory_order_acquire);
        processed = sProcessed.load(std::memory_order_acquire);
      } while (processed < target);
    }
    break;
  }
  }

  {
    std::lock_guard lock{sBufferMutex};
    sStreamBase = target;
    detail::sBufferSize = 0;
  }
  sPendingDraws = 0;
}

uint64_t stream_position() noexcept { return sStreamBase + detail::sBufferSize; }
uint64_t processed_position() noexcept { return sProcessed.load(std::memory_order_acquire); }
uint64_t published_position() noexcept { return sPublished.load(std::memory_order_acquire); }

const uint8_t* get_buffer_data() { return detail::sBufferData; }
uint32_t get_buffer_size() { return detail::sBufferSize; }

void clear_buffer() {
  const uint64_t processed = sProcessed.load(std::memory_order_acquire);
  AURORA_ASSERT(sPublished.load(std::memory_order_acquire) == processed,
                "fifo::clear_buffer: published commands are still pending");
  std::lock_guard lock{sBufferMutex};
  sStreamBase = processed;
  detail::sBufferSize = 0;
  sPendingDraws = 0;
}

} // namespace aurora::gx::fifo
