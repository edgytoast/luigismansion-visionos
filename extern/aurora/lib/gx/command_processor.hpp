#pragma once

#include "../internal.hpp"

#include <cstdint>

#include <dolphin/gx/GXAurora.h>

namespace aurora::gx::fifo {

struct ProcessResult {
  uint32_t bytesProcessed;
  bool drawDone;
};

// Process GX FIFO commands until the next draw done event or end of buffer
// streamBase: absolute stream position of data[0] (error reports, 0235).
ProcessResult process(const uint8_t* data, uint32_t size, uint64_t streamBase = 0) noexcept;
// 0235: called for each reported malformed command (on the processing thread).
void set_error_callback(AuroraFifoErrorCallback callback) noexcept;
void clear_draw_cache() noexcept;
// Forgets this frame's uploads of dynamically sized vertex arrays (see size_dynamic_arrays).
void reset_dynamic_arrays() noexcept;

} // namespace aurora::gx::fifo
