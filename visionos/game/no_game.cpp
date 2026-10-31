// Apple Vision Pro, built without a recompiled game module (no disc when the app was built): the
// whole VR pipeline still runs, so it can be proven before the disc arrives. The OpenXR session
// over the provider, the Metal eye images (IOSurfaces imported into Dawn, fences both ways), the
// panel quad that frames menus and the F1 menu's overlay quad all work as in the game; the picture
// is a notice instead of the mansion. Quit (the launcher) ends it. Written for
// luigismansion-visionos (Trevorbilt).
#include "lmvr_game_hooks.h"

#include "recomp/menu.h"
#include "recomp/hle.h"
#include "recomp/vr_host.h"

#include <aurora/aurora.h>
#include <aurora/event.h>
#include <imgui.h>

#include <chrono>
#include <exception>
#include <thread>

extern "C" int lmvr_visionos_no_game(void) {
    using namespace recomp;
    log("[visionos] no game module in this build: the VR pipeline runs with the F1 menu (no disc when it was built)");
    try {
    vrhost::attach();
    menu::init();
    if (!menu::isOpen()) menu::toggle();
    bool exiting = false;
    unsigned frames = 0;
    while (!exiting) {
        lmvr_visionos_frame_boundary();
        for (const AuroraEvent* ev = aurora_update(); ev && ev->type != AURORA_NONE; ++ev) {
            if (ev->type == AURORA_SDL_EVENT) menu::handleEvent(ev->sdl);
            if (ev->type == AURORA_EXIT) exiting = true;
        }
        if (lmvr_visionos_quit_requested() || menu::quitRequested()) break;
        if (vrhost::sessionHeld() || lmvr_visionos_game_held()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        vrhost::beginFrame();
        if (!aurora_begin_frame()) {
            vrhost::endFrame(false);
            continue;
        }
        // The notice, in the picture itself (the panel quad shows it below the menu's overlay).
        const ImVec2 size = ImGui::GetIO().DisplaySize;
        ImGui::SetNextWindowPos(ImVec2(size.x * 0.5f, size.y * 0.12f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
        ImGui::Begin("##lmvr-no-game", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs);
        ImGui::TextUnformatted("Luigi's Mansion VR for Apple Vision Pro");
        ImGui::TextUnformatted("This build has no game in it: it was built without a disc.");
        ImGui::Text("VR frames: %u", frames);
        ImGui::End();
        menu::draw();
        aurora_end_frame();
        bool submitted = false;
        if (vrhost::rendering()) {
            vrhost::copyEye(0);
            submitted = true;  // a menu screen is one texture on the panel quad, for both eyes
        }
        vrhost::endFrame(submitted);
        if (submitted && (++frames == 1 || frames % 600 == 0))
            log("[visionos] no-game VR frame %u submitted (panel %d, menu %d)", frames, int(vrhost::panelMode()),
                int(menu::isOpen()));
    }
    } catch (const std::exception& error) {
        // The launcher reports the failure (exit code 3) rather than the app vanishing.
        log("[visionos] no-game VR loop stopped: %s", error.what());
        menu::shutdown();
        return 3;
    }
    menu::shutdown();
    return 0;
}

// The disc the game module was recompiled from ("GLME01" USA, "GLMP01" Europe, "NONE" without a
// module): the launcher accepts only that release (Port.validate), since the game's code is built
// into the app from the player's own main.dol.
#include "recomp/ctx.h"
namespace recomp {
extern const GameModule kGameModule;
}
extern "C" const char* lmvr_visionos_built_game_id(void) {
    return recomp::kGameModule.gameId;
}
