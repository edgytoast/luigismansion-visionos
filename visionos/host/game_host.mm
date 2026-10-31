// The real host behind the lmvr_visionos_* bridge: Luigi's Mansion VR (the recompiled game with
// its VR mod) on Apple Vision Pro. Compiled INTO LuigisMansionVisionGame.framework, with
// -fobjc-arc. Written for luigismansion-visionos (Trevorbilt), from the avp-game-porting scaffold
// and Wind Waker VR's host.
//
//   - SwiftUI owns main(). The game's main() is lmvr_game_main (the base's src/recomp/main.cpp
//     under LM_VR_VISIONOS) and runs on a detached pthread with a 16 MB stack once the
//     CompositorLayer exists. Its guest threads get deep stacks of their own (host_thread.h).
//   - SDL is told main already happened (SDL_SetMainReady) and runs its offscreen video driver
//     (aurora's visionOS patch): the game thread never touches UIKit.
//   - The game's VR host (src/recomp/vr_host.cpp) makes its OpenXR session over the vendored
//     provider: LM_VR=1, Diorama by default (LM_VR_VIEW_MODE).
//   - Files: settings, memory cards and the VR mode profiles in Documents/Luigi's Mansion VR
//     (XDG_CONFIG_HOME / XDG_DATA_HOME), shader caches in Library/Caches (XDG_CACHE_HOME), the
//     program's own files (pipelines.bin) in the app bundle (LM_EXE_DIR). Nothing is written
//     outside the app's container.
//   - Quit = the game's own exit path (the main loop's `exiting`), so it saves on the way out.
//   - Test hooks from the environment (SIMCTL_CHILD_LMVR_*): LMVR_ARGS (extra game options, or
//     KEY=VALUE environment settings such as LM_MENU=vr), LMVR_VIEW_MODE (diorama | life-size |
//     first-person).

#include "lmvr_visionos.h"
#include "lmvr_game_hooks.h"

#include "vr/visionos/xr_visionos.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_init.h>
#define SDL_MAIN_HANDLED 1
#include <SDL3/SDL_main.h>

#import <Foundation/Foundation.h>

#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>

#include <atomic>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

extern "C" int lmvr_game_main(int argc, char* argv[]);  // src/recomp/main.cpp (LM_VR_VISIONOS)
extern "C" void* objc_autoreleasePoolPush(void);
extern "C" void objc_autoreleasePoolPop(void* pool);

namespace {

constexpr size_t kGameThreadStackSize = 16 * 1024 * 1024;

std::mutex g_mutex;
std::string g_assetPath;
std::string g_lastError;
std::atomic_bool g_started{false};
std::atomic_bool g_running{false};
std::atomic_int g_exitCode{0};
std::atomic_bool g_roomBehindMenus{false};
std::atomic_bool g_paused{false};
std::atomic_bool g_audioInterrupted{false};
std::atomic_bool g_quitRequested{false};
std::atomic_bool g_worldRecentered{false};
std::atomic_bool g_sessionFocusedOnce{false};

void SetError(std::string message) {
    NSLog(@"[LMVR] %s", message.c_str());
    std::lock_guard lock(g_mutex);
    g_lastError = std::move(message);
}

std::string Directory(NSSearchPathDirectory which) {
    NSArray<NSString*>* paths = NSSearchPathForDirectoriesInDomains(which, NSUserDomainMask, YES);
    return paths.count > 0 ? std::string(paths.firstObject.fileSystemRepresentation) : std::string();
}

void PrepareEnvironment() {
    const std::string documents = Directory(NSDocumentDirectory);
    const std::string caches = Directory(NSCachesDirectory);
    // Settings, saves and the VR profiles: Documents/Luigi's Mansion VR (Files shows it).
    setenv("XDG_CONFIG_HOME", documents.c_str(), 1);
    setenv("XDG_DATA_HOME", documents.c_str(), 1);
    // Shader and pipeline caches: rebuilt if visionOS clears them.
    setenv("XDG_CACHE_HOME", caches.c_str(), 1);
    // The program's own files (pipelines.bin, the shipped precache list): the app's resources.
    setenv("LM_EXE_DIR", [NSBundle mainBundle].resourcePath.fileSystemRepresentation, 1);
    // No modal dialogs on errors: they go to the log, and the launcher shows the exit code.
    setenv("LM_BACKGROUND", "1", 1);
    if (!lmvr_window_enabled()) {
        setenv("LM_VR", "1", 1);
        const char* mode = std::getenv("LMVR_VIEW_MODE");
        setenv("LM_VR_VIEW_MODE", mode != nullptr ? mode : "diorama", 1);
    } else if (std::getenv("LMVR_TEST_PLAIN_FLAT") != nullptr) {
        // Test runs: LMVR_TEST_PLAIN_FLAT=1 plays the game flat with none of the volume's hooks
        // (nothing shows; for comparing the game's own flat behaviour).
    } else {
        // On your table: the game plays flat (its own camera) and its main 3D view is placed in
        // the volume (src/recomp/volume.cpp). The scene mirror keeps what is nearer than the
        // window's glass (there is none: the set is in the volume, not behind a pane) and the whole
        // room, behind and beside the camera's view too (a table is seen from all round).
        setenv("LM_VOLUME", "1", 1);
        setenv("AURORA_MIRROR_NEAR", "raw", 0);
        setenv("AURORA_MIRROR_CLIP_MARGIN", "4", 0);
        setenv("AURORA_MIRROR_CLIP", "none", 0);
    }
    NSLog(@"[LMVR] data in %s, caches in %s", documents.c_str(), caches.c_str());
}

// The game's own output (printf/fprintf): to Library/Logs/game.log (the previous run's kept as
// game.previous.log) unless a console is attached (a Simulator run with --console-pty). Headset
// problems are diagnosed from it (`xcrun devicectl device copy from --domain-type
// appDataContainer --domain-identifier <bundle id> --source Library/Logs/game.log`).
void RedirectGameOutput() {
    if (isatty(STDOUT_FILENO)) {
        return;
    }
    NSString* library = NSSearchPathForDirectoriesInDomains(NSLibraryDirectory, NSUserDomainMask, YES).firstObject;
    if (library == nil) {
        return;
    }
    NSString* logs = [library stringByAppendingPathComponent:@"Logs"];
    [[NSFileManager defaultManager] createDirectoryAtPath:logs withIntermediateDirectories:YES attributes:nil error:nil];
    NSString* log = [logs stringByAppendingPathComponent:@"game.log"];
    NSString* previous = [logs stringByAppendingPathComponent:@"game.previous.log"];
    [[NSFileManager defaultManager] removeItemAtPath:previous error:nil];
    [[NSFileManager defaultManager] moveItemAtPath:log toPath:previous error:nil];
    if (std::freopen(log.fileSystemRepresentation, "w", stdout) != nullptr) {
        dup2(fileno(stdout), STDERR_FILENO);
        std::setvbuf(stdout, nullptr, _IOLBF, 0);
        std::setvbuf(stderr, nullptr, _IONBF, 0);
        NSLog(@"[LMVR] game output in %s", log.fileSystemRepresentation);
    }
}

void* GameThreadMain(void*) {
    pthread_setname_np("Luigi's Mansion VR game");
    void* pool = objc_autoreleasePoolPush();
    RedirectGameOutput();
    std::string asset;
    {
        std::lock_guard lock(g_mutex);
        asset = g_assetPath;
    }
    PrepareEnvironment();
    std::vector<std::string> args = {"LuigisMansionVR"};
    if (!asset.empty()) {
        args.push_back("--disc");
        args.push_back(asset);
    }
    if (const char* extra = std::getenv("LMVR_ARGS"); extra != nullptr) {
        std::istringstream tokens(extra);
        for (std::string token; tokens >> token;) {
            const size_t eq = token.find('=');
            if (token.rfind("--", 0) != 0 && eq != std::string::npos && eq > 0) {
                setenv(token.substr(0, eq).c_str(), token.substr(eq + 1).c_str(), 1);
                NSLog(@"[LMVR] option %s", token.c_str());
            } else {
                args.push_back(token);
            }
        }
    }
    std::vector<char*> argv;
    for (std::string& arg : args) {
        argv.push_back(arg.data());
    }
    argv.push_back(nullptr);
    NSLog(@"[LMVR] game starting (%s) with %s", lmvr_window_enabled() ? "the volume" : "VR",
          asset.empty() ? "no disc" : asset.c_str());
    const int code = lmvr_game_main(static_cast<int>(args.size()), argv.data());
    objc_autoreleasePoolPop(pool);
    g_exitCode.store(code);
    g_running.store(false);
    NSLog(@"[LMVR] game returned %d", code);
    if (code != 0) {
        SetError(code == 1 && asset.empty() ? "No Luigi's Mansion disc was found."
                                           : "The game stopped (exit code " + std::to_string(code) +
                                                 "). The log has the details.");
    }
    return nullptr;
}

}  // namespace

extern "C" {

void lmvr_visionos_set_asset_path(const char* path) {
    std::lock_guard lock(g_mutex);
    g_assetPath = path != nullptr ? path : "";
}

void lmvr_visionos_set_layer_renderer(void* layer_renderer) {
    // The provider retains it; its session starts (or, after a Crown pause, resumes) with it.
    xr_visionos_set_layer_renderer(layer_renderer);
}

void lmvr_visionos_spatial_event(uint64_t event_id, int phase, int chirality, bool has_ray, float origin_x,
                                 float origin_y, float origin_z, float direction_x, float direction_y,
                                 float direction_z, bool has_pose, float pose_x, float pose_y, float pose_z) {
    xr_visionos_spatial_event(event_id, phase, chirality, has_ray, origin_x, origin_y, origin_z, direction_x,
                              direction_y, direction_z, has_pose, pose_x, pose_y, pose_z);
}

bool lmvr_visionos_layer_invalidated(void) {
    return xr_visionos_layer_invalidated();
}

bool lmvr_visionos_can_resume(void) {
    // The game holds while its space is closed (vrhost::sessionHeld) and takes the next layer,
    // once it was seen: a space closed before its first focused frame quits instead.
    return g_running.load() && g_sessionFocusedOnce.load();
}

void lmvr_visionos_world_recentered(void) {
    g_worldRecentered.store(true);
}

void lmvr_visionos_set_audio_interrupted(bool interrupted) {
    g_audioInterrupted.store(interrupted);
}

bool lmvr_visionos_start_game(void) {
    bool expected = false;
    if (!g_started.compare_exchange_strong(expected, true)) {
        SetError("The game already ran in this process; relaunch the app to play again.");
        return false;
    }
    SDL_SetMainReady();
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, kGameThreadStackSize);
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    g_running.store(true);
    const int result = pthread_create(&thread, &attributes, &GameThreadMain, nullptr);
    pthread_attr_destroy(&attributes);
    if (result != 0) {
        g_running.store(false);
        SetError("Could not start the game thread (error " + std::to_string(result) + ").");
        return false;
    }
    return true;
}

bool lmvr_visionos_game_running(void) {
    return g_running.load();
}

int lmvr_visionos_exit_code(void) {
    return g_exitCode.load();
}

void lmvr_visionos_request_quit(void) {
    // The game's main loop leaves at its next iteration (held or not) and shuts down as on a
    // closed window: settings and memory cards are written, then the process ends (exit 0).
    g_quitRequested.store(true);
    g_paused.store(false);
    g_audioInterrupted.store(false);
    if (SDL_WasInit(SDL_INIT_EVENTS) != 0) {
        SDL_Event event{};
        event.type = SDL_EVENT_QUIT;
        SDL_PushEvent(&event);
    }
}

const char* lmvr_visionos_last_error(void) {
    static thread_local std::string copy;
    {
        std::lock_guard lock(g_mutex);
        copy = g_lastError;
    }
    if (copy.empty()) {
        if (const char* provider = xr_visionos_last_error(); provider != nullptr) {
            copy = provider;
        }
    }
    return copy.c_str();
}

void lmvr_visionos_set_room_behind_menus(bool enabled) {
    g_roomBehindMenus.store(enabled);
}

void lmvr_visionos_set_safety_boundary(bool enabled) {
    xr_visionos_set_safety_boundary(enabled);
}

void lmvr_visionos_set_paused(bool paused) {
    g_paused.store(paused);
}

// --- Window mode ("On your table", the volume): host/lmvr_window.cpp.

// --- Game -> host (lmvr_game_hooks.h; not exported).

void lmvr_visionos_frame_boundary(void) {
    static void* pool = nullptr;
    if (pool != nullptr) {
        objc_autoreleasePoolPop(pool);
    }
    pool = objc_autoreleasePoolPush();
}

bool lmvr_visionos_game_held(void) {
    return !g_quitRequested.load() && g_paused.load();
}

bool lmvr_visionos_room_enabled(void) {
    return g_roomBehindMenus.load();
}

bool lmvr_visionos_audio_interrupted(void) {
    return !g_quitRequested.load() && g_audioInterrupted.load();
}

void lmvr_visionos_mark_session_focused(void) {
    if (!g_sessionFocusedOnce.exchange(true)) {
        NSLog(@"[LMVR] the session is focused: a closed space now pauses the game");
    }
}

bool lmvr_visionos_quit_requested(void) {
    return g_quitRequested.load();
}

}  // extern "C"
