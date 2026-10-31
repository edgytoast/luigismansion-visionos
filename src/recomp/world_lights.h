#pragma once
// World lights (F1 > Visuals > Enhanced lighting > World lights; docs/RENDERING.md,
// "Enhanced lighting > World lights"): candles, fireplaces, lamps, chandeliers, element sprays,
// fire ghosts, sparkles... become coloured extra lights, like the ghost lights.
#include "recomp/ctx.h"
#include <vector>
#include <utility>

namespace recomp::worldlights {

// The main view fn_8000BB98(0) is about to be drawn (enhanced_lighting.cpp): build this frame's
// world lights and submit them (recomp::submitExtraLights, after the ghost lights).
void onMainView(Ctx& c);
// Temporary draw mask for the legacy cold-breath particle group.
class HiddenPlayerParticles {
    std::vector<std::pair<u32,u8>> saved_;
public:
    HiddenPlayerParticles();
    ~HiddenPlayerParticles();
    HiddenPlayerParticles(const HiddenPlayerParticles&)=delete;
    HiddenPlayerParticles& operator=(const HiddenPlayerParticles&)=delete;
};

// A resource the game loaded by path (JKRArchive::getResource(path), unused_assets.cpp's hook):
// the map's jmp tables (objinfo, roominfo, furnitureinfo) are kept for lamps and room lights.
void noteResource(const char* path, u32 data);

}  // namespace recomp::worldlights
