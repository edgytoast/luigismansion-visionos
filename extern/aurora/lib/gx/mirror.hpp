#pragma once

// The scene mirror's hooks in the command processor (see aurora/mirror.h). FIFO thread.

#include "gx.hpp"

#include <string>

namespace aurora::gx::mirror {
// Cheap: whether draws are being recorded right now.
bool recording() noexcept;
void mark(u32 mark, f32 value) noexcept;
// GX_AURORA_BEGIN_OFFSCREEN / END_OFFSCREEN: draws into an offscreen pass aren't the scene.
void offscreen(bool begin) noexcept;
// GX_AURORA_DEBUG_GROUP_PUSH / POP (a test switch's labels): the label pushed, or null for a pop.
void debug_group(const std::string* label) noexcept;
// A draw as the command processor pushes it: `vertices` are vtxCount raw GX vertices laid out as
// `config` says; `indices` (host-endian, triangles) or none for `prim`'s own order. `merged`
// (visionos/patches/aurora/0106): the command processor merged it into the draw before it (no GX
// state changed between them), so the mirror can too.
void record(GXPrimitive prim, const ShaderConfig& config, u16 vtxCount, const u8* vertices, const u16* indices,
            u32 indexCount, bool merged = false) noexcept;
} // namespace aurora::gx::mirror
