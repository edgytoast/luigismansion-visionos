#pragma once

#include "../internal.hpp"

#include <cstring>

namespace aurora::gx::fifo {

namespace detail {
extern uint8_t* sBufferData;
extern uint32_t sBufferSize;
extern uint32_t sBufferCapacity;
// A display-list (or capture) recording. Per thread, so one thread can record while another
// submits to the FIFO; a thread may instead share the recording another thread started
// (0235, join_recording: guest threads of an emulated console share its one CPU FIFO).
struct Recording {
  bool active = false;
  bool overflow = false; // a write did not fit: the recording is incomplete (GXEndDisplayList returns 0)
  uint8_t* buffer = nullptr;
  uint32_t size = 0;
  uint32_t pos = 0;
};
extern thread_local Recording tOwnRecording;
extern thread_local Recording* tJoinedRecording;
inline Recording& recording() noexcept {
  Recording* joined = tJoinedRecording;
  return joined != nullptr ? *joined : tOwnRecording;
}
} // namespace detail

enum class ProcessingMode : uint8_t {
  // Process FIFO synchronously at drain()
  Drain,
  // Process FIFO synchronously at publish() (useful for profiling)
  Inline,
  // Process FIFO asynchronously on a worker thread; drain() synchronizes
  Thread
};
ProcessingMode processing_mode() noexcept;

void init();
void shutdown();

void begin_frame() noexcept;
void end_frame() noexcept;

// Out-of-line slow path: grows internal buffer then appends data
void write_data_grow(const void* data, uint32_t length);

inline void write_data(const void* data, const uint32_t length) {
  auto& rec = detail::recording();
  if (!rec.active)
    LIKELY {
      if (length <= detail::sBufferCapacity - detail::sBufferSize)
        LIKELY {
          std::memcpy(detail::sBufferData + detail::sBufferSize, data, length);
          detail::sBufferSize += length;
          return;
        }
      write_data_grow(data, length);
    }
  else if (length <= rec.size - rec.pos) {
    std::memcpy(rec.buffer + rec.pos, data, length);
    rec.pos += length;
  } else {
    // 0235: nothing more is written once one write did not fit (a partial command would make the
    // rest of the list unparseable); end_display_list reports the overflow.
    rec.overflow = true;
    rec.pos = rec.size;
  }
}

inline void write_u8(const uint8_t val) {
  auto& rec = detail::recording();
  if (!rec.active)
    LIKELY {
      if (detail::sBufferSize < detail::sBufferCapacity)
        LIKELY {
          detail::sBufferData[detail::sBufferSize++] = val;
          return;
        }
      write_data_grow(&val, 1);
    }
  else if (rec.pos < rec.size) {
    rec.buffer[rec.pos++] = val;
  } else {
    rec.overflow = true;
  }
}

inline void write_u16(const uint16_t val) {
  const auto out = bswap(val);
  write_data(&out, sizeof(out));
}

inline void write_u32(const uint32_t val) {
  const auto out = bswap(val);
  write_data(&out, sizeof(out));
}

inline void write_u64(const uint64_t val) {
  const auto out = bswap(val);
  write_data(&out, sizeof(out));
}

inline void write_f32(const float val) {
  const auto out = bswap(val);
  write_data(&out, sizeof(out));
}

// Overwrites an unpublished u32 previously written at the given offset.
void patch_u32(uint32_t offset, uint32_t val);

// Marks a complete draw and publishes when the configured draw batch is full.
void finish_draw() noexcept;

// Makes commands written so far available to the FIFO processor.
void publish() noexcept;

using DrawDoneCallback = void (*)();
DrawDoneCallback set_draw_done_callback(DrawDoneCallback callback) noexcept;

// Display list recording
void begin_display_list(uint8_t* buf, uint32_t size);
uint32_t end_display_list();
bool in_display_list();
uint32_t display_list_pos();
// 0235: whether the recording being ended overflowed its buffer (valid until the next begin).
bool display_list_overflowed();
// 0235: the calling thread writes into the recording of the thread that last began one (shares it:
// both must never write at the same time), or (false) into its own again.
void join_recording(bool join);

// Appends recorded commands to the FIFO and publishes them (bypasses the
// calling thread's display-list redirection).
void replay(const uint8_t* data, uint32_t size);

// Ensure all buffered commands have been processed.
void drain();

// Stream offsets (debug: CPU/GPU race detection). stream_position() is the end of everything
// written so far (call on the writing thread); processed_position() how far the processor got.
uint64_t stream_position() noexcept;
uint64_t processed_position() noexcept;
uint64_t published_position() noexcept;

// Internal buffer inspection
const uint8_t* get_buffer_data();
uint32_t get_buffer_size();
void clear_buffer();

} // namespace aurora::gx::fifo
