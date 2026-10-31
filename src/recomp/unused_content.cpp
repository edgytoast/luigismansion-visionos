// Unused content still present in the retail code, re-enabled as F1 > Enhancements options.
// Both are hooks (tools/recomp/hooks.txt) that run the original function and only steer which
// of the game's own code paths it takes. Findings and addresses: docs/UNUSED_CONTENT.md.
#include "recomp/processing_visibility.h"
#include "recomp/enhanced_lighting.h"
#include "recomp/vr_avatar_guest.h"
#include "port/settings.h"
#include "recomp/hle.h"
#include "recomp/interp.h"
#include "recomp/runtime.h"
#include "recomp/version.h"
#include "recomp/vr_host.h"
#include "recomp/volume.h"

#include <aurora/mirror.h>

#include <dolphin/pad.h>

#include <bit>
#include <cstdio>
#include <cstdlib>

namespace recomp {
u16 padButtons();  // hle_pad.cpp: port 1 buttons as last handed to the game
void unusedAssetsFrame(Ctx& c);  // unused_assets.cpp: per-frame work (nozzles)
void testStateFrame(Ctx& c);     // test_state.cpp: LM_TEST_STATE presets
void unusedModelsFrame();        // unused_models.cpp: green Toad

namespace {

bool envFlag(const char* name) {
    const char* v = std::getenv(name);
    return v && *v && *v != '0';
}

// Game Boy Horror HUD screen mode (lbl_804D846C). Retail only ever sets 4 (normal) and 3
// (E. Gadd's call, fn_8003EA78/fn_8003EA84). Mode 2 is the leftover live camera: the HUD
// update reports the screen rectangle to the renderer (fn_80010EBC), which then renders the
// second view (first-person camera fn_80021DDC) into it; static noise covers the screen
// while it is off-screen.
#define kGbhScreenMode VA(0x804D846C)
constexpr u32 kGbhModeCamera = 2;
constexpr u32 kGbhModeNormal = 4;
constexpr u32 kGbhModeClock = 1;  // cut E3 clock + radar

bool sGbhCamOn = envFlag("LM_GBH_CAM");  // test aid: start with the camera on
u16 sPrevButtons = 0;
bool sGbhClockOn = envFlag("LM_GBH_CLOCK");  // test aid: start with the clock on

}  // namespace

// Keep the VR HUD on its ordinary money/bill/gold display. This call only
// queues the selected-treasure notification after the pickup was credited.
// Flat-screen play retains the original notification.
HLE(fn_8005724C) {
    if(vrhost::enabled())return;
    callOriginal(c,"fn_8005724C");
}

// Game Boy Horror HUD update (once per game frame while the HUD is up).
HLE(fn_8003CFC0) {
    static const bool sForce = envFlag("LM_GBH_CAM");
    const bool enabled = sForce || port::settings().enhancements.gbhCamera;
    const u16 buttons = padButtons();
    if (!enabled) {
        sGbhCamOn = false;
    } else if ((buttons & PAD_BUTTON_UP) && !(sPrevButtons & PAD_BUTTON_UP)) {
        sGbhCamOn = !sGbhCamOn;
        if (sGbhCamOn) sGbhClockOn = false;
        log("[unused] Game Boy Horror camera %s", sGbhCamOn ? "on" : "off");
    }
    // Cut E3 clock/radar screen (GBH mode 1): D-pad Left; exclusive with the camera.
    if (!port::settings().enhancements.gbhClock && !envFlag("LM_GBH_CLOCK")) {
        sGbhClockOn = false;
    } else if ((buttons & PAD_BUTTON_LEFT) && !(sPrevButtons & PAD_BUTTON_LEFT)) {
        sGbhClockOn = !sGbhClockOn;
        if (sGbhClockOn) sGbhCamOn = false;
        log("[unused] Game Boy Horror clock %s", sGbhClockOn ? "on" : "off");
    }
    setPatch(3, sGbhClockOn);
    // Unused ghost tracker: D-pad Down cycles off -> far mode -> close mode (patch switches 1/2).
    static u32 sTracker = 0;
    if (!port::settings().enhancements.gbhTracker) {
        sTracker = 0;
    } else if ((buttons & PAD_BUTTON_DOWN) && !(sPrevButtons & PAD_BUTTON_DOWN)) {
        sTracker = (sTracker + 1) % 3;
        log("[unused] Game Boy Horror ghost tracker: %s", sTracker == 0 ? "off" : sTracker == 1 ? "far mode" : "close mode");
    }
    setPatch(1, sTracker == 1);
    setPatch(2, sTracker == 2);
    sPrevButtons = buttons;
    static const bool sShowHud = [] {
        const char* v = std::getenv("LM_GBH_CAM");
        return v && *v == '2';
    }();
    if (sShowHud) {  // test aid (LM_GBH_CAM=2): GBH HUD up before Luigi has the Game Boy Horror
        static const u32 player = guestSymbol("fn_800AD39C");
        const Ctx saved = c;
        const u32 p = guestCall(c, player, 0);
        c = saved;
        if (p >= 0x80000000u && p < 0x81800000u) wr8(p + 0x55, 1);
    }
    // Screen mode: camera (2) / clock (1) / normal (4); E. Gadd's call screen (3) is left alone.
    const u32 mode = rd32(kGbhScreenMode);
    if (mode == kGbhModeNormal || mode == kGbhModeCamera || mode == kGbhModeClock) {
        const u32 want = sGbhCamOn ? kGbhModeCamera : sGbhClockOn ? kGbhModeClock : kGbhModeNormal;
        if (mode != want) wr32(kGbhScreenMode, want);
    }
    callOriginal(c, "fn_8003CFC0");
    testStateFrame(c);
    unusedAssetsFrame(c);
    unusedModelsFrame();
    // Test aid: LM_TELEPORT=x,y,z,frames moves Luigi (actor slot 0, position +0x44) there once,
    // `frames` HUD updates into gameplay (used to reach the 2F doors for screenshots).
    static const char* sTp = std::getenv("LM_TELEPORT");
    if (sTp && *sTp) {
        static int sFrames = 0;
        float x = 0, y = 0, z = 0;
        int at = 300;
        std::sscanf(sTp, "%f,%f,%f,%d", &x, &y, &z, &at);
        if (++sFrames == at) {
            const u32 actor = rd32(VA(0x803D48A0));
            if (actor >= 0x80000000u && actor < 0x81800000u) {
                wr32(actor + 0x44, std::bit_cast<u32>(x));
                wr32(actor + 0x48, std::bit_cast<u32>(y));
                wr32(actor + 0x4C, std::bit_cast<u32>(z));
                log("[unused] teleported Luigi to %.0f,%.0f,%.0f", x, y, z);
            }
        }
    }
}

// Actor draw pass. r3 selects the pass: (r3 & 0x30) == 0x10 draws normal actors into the
// scene, 0x30 draws ghosts (type flag 8) into the separate ghost layer that is later added
// back at half resolution weighted by the ghost's alpha (fn_8000C2D0, fn_8005F280). Pass 0
// (used by the never-shipped Game Boy Horror view) draws every actor, ghosts included,
// straight into the scene with their ghost lighting and alpha: the early, more solid look.
// The ghost pass is also where enhanced lighting tags draws as ghosts (enhanced_lighting.h).
HLE(fn_80060E70) {
    processing::MachineDrawScope machineVisibility(c.r[3]);
    vravatar::ReflectionScope reflection(c.r[3]);
    static const bool sForce = envFlag("LM_EARLY_GHOSTS");
    const u32 pass = c.r[3] & 0x30;
    struct GhostScope {
        bool prev = enh::ghostPass();
        explicit GhostScope(bool on) { enh::setGhostPass(on); }
        ~GhostScope() { enh::setGhostPass(prev); }
    } ghostScope(pass == 0x30);
    struct CharScope {
        bool prev = enh::characterPass();
        explicit CharScope(bool on) { enh::setCharacterPass(on); }
        ~CharScope() { enh::setCharacterPass(prev); }
    } charScope(pass != 0x30);
    // Apple Vision Pro, On your table: the scene mirror adds the ghosts over the scene as this layer is.
    volume::MirrorScope ghostLayer(aurora::mirror::MarkGhostPass, pass == 0x30);
    if (!sForce) return  // dev-only experiment (LM_EARLY_GHOSTS=1); the menu option is the beta lighting below
        callOriginal(c, "fn_80060E70");
    if (pass == 0x10) {
        c.r[3] &= ~0x30u;  // normal actors + ghosts, drawn directly
    } else if (pass == 0x30) {
        c.r[3] = (c.r[3] & ~0x30u) | 0x20;  // no case for 0x20: the ghost layer stays empty
    }
    callOriginal(c, "fn_80060E70");
}

// Early ghost lighting: the known cheat for the pre-release ghost look patches the channel
// setup call at 0x80058290 in fn_800581E8 (the ghost lighting function): light mask 0x80
// (light 7) -> 0x40 (light 6) and attenuation function 0 -> 2. fn_8005A5D4 takes the
// GXSetChanCtrl-style arguments (r7 = light mask, r9 = attenuation), so adjust them only
// for that call site (the European fn_800581E8 is identical; VA() maps the call site).
HLE(fn_8005A5D4) {
    if (c.lr == VA(0x80058290) + 4 && port::settings().enhancements.earlyGhostLighting) {
        c.r[7] = 0x40;
        c.r[9] = 2;
    }
    callOriginal(c, "fn_8005A5D4");
}

}  // namespace recomp
