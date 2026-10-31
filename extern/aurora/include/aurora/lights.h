#pragma once
// Extra dynamic lights for the enhanced lighting mode (include/aurora/enhanced.h).
//
// A game-side provider (e.g. ghosts emitting colored light) hands Aurora a set of point
// lights once per game frame. They are written into the GX FIFO as a command, so they stay in
// order with the frame's draws and are part of captured command streams (frame interpolation
// replays them, blended between game frames by `id`). They light world-geometry draws of that
// frame per pixel, added to the game's own lit color channels (so the game's materials and TEV
// decide how a surface responds), only while the enhanced lighting mode and its "extra lights"
// part are on; otherwise they are ignored.
#include <cstdint>

namespace aurora::gfx {
struct ExtraLight {
  float pos[3];      // VIEW space (GX eye space, same space as GXLoadLightObjImm positions)
  float color[3];    // linear RGB, may exceed 1
  float radius;      // world units; smooth falloff to 0 at radius
  float intensity;   // multiplier
  uint32_t flags;    // bit0: cast shadows, bit1: affects ghosts/translucent, bit2: specular
  uint32_t id;       // stable per-source id for interpolation pairing (0 = none)
  // 0187 (flag ExtraLightRoomBox): the light only reaches inside this box (its room), fading out
  // over a short margin past it, so a light without a shadow does not shine through walls into the
  // next room. VIEW space: box centre relative to `pos` (3), then the three half-axis vectors
  // (3 x 3: the box's world axes in view space, scaled by its half extents). Relative to `pos`
  // so it follows the light when frame interpolation blends the position.
  float box[12] = {};
};
static_assert(sizeof(ExtraLight) == 88);

constexpr uint32_t MaxExtraLights = 64; // 0023: was 16 (world lights: candles, fires, lamps)
constexpr uint32_t ExtraLightCastShadows = 1u << 0;
constexpr uint32_t ExtraLightTranslucent = 1u << 1;
constexpr uint32_t ExtraLightSpecular = 1u << 2;
// 0025: a flame: yellow-white near the source, red-orange toward the edge of its reach.
constexpr uint32_t ExtraLightFire = 1u << 3;
// 0187: limited to `box` (its room).
constexpr uint32_t ExtraLightRoomBox = 1u << 4;

// Call once per game frame (guest thread), before the frame's draws; applies to world-geometry
// draws of that frame. Lights beyond MaxExtraLights are dropped (pass the strongest first).
// count 0 clears the set.
void set_extra_lights(const ExtraLight* lights, uint32_t count);
} // namespace aurora::gfx
