#pragma once
// Game -> host calls for Apple Vision Pro, not exported from the framework. The base tree's
// visionOS patches (src/recomp/main.cpp) call these under LM_VR_VISIONOS; game_host.mm and
// visionos/game/no_game.cpp implement them.
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The top of every iteration of the game's main loop: drains this thread's autorelease pool
// (a plain pthread has no run loop, so Metal objects made on it would pile up).
void lmvr_visionos_frame_boundary(void);
// The launcher holds the game (a pause the window asked for).
bool lmvr_visionos_game_held(void);
// visionOS interrupted the app's audio (Siri, a call, an alarm): pause the sound only; frames
// must keep flowing to the compositor.
bool lmvr_visionos_audio_interrupted(void);
// The mansion in your room: the launcher opened a mixed space (the room shows around the
// diorama and its menus).
bool lmvr_visionos_room_enabled(void);
// The VR session reached FOCUSED for the first time: the game was seen, so from now on a closed
// space pauses (Resume) instead of quitting.
void lmvr_visionos_mark_session_focused(void);
// The launcher's Quit: the game leaves through its own exit path (it saves on the way).
bool lmvr_visionos_quit_requested(void);
// Built without a recompiled game module (no disc yet): the VR pipeline with the F1 menu and a
// notice instead of the game, until Quit. Returns the exit code.
int lmvr_visionos_no_game(void);

// --- The volume ("On your table", host/lmvr_window.cpp; the base's src/recomp/volume.cpp).
// The window mode is on for this run.
bool lmvr_window_enabled(void);
// Host thread, the top of a frame: waits for the window's tick (at most 100 ms) and numbers it.
void lmvr_window_begin_frame(void);
uint32_t lmvr_window_game_frame(void);
// A main 3D view was already drawn in this window frame (a cutscene fast-forwarding: several
// guest frames in one): the later ones aren't recorded.
bool lmvr_window_scene_seen(void);
// The main 3D view of this frame, as drawn.
typedef struct lmvr_window_scene {
    float volume_from_view[16];  // column-major: view space (game units) into the volume (metres)
    float tan_half_x, tan_half_y, focus;
    float scale;          // metres per game unit
    uint32_t set_serial;  // changes when the set is placed anew (and fades in)
    uint32_t fit_serial;  // changes at every fit, a resize's too
    bool placed;          // false: the set isn't placed yet
    bool cinematic;       // a cutscene's camera
    // A ghost capture: Luigi's and the ghost's places in the volume (metres), for the arrow that
    // shows which way to pull.
    bool capture;
    float capture_luigi[3], capture_ghost[3];
} lmvr_window_scene;
// Game thread: the main 3D view is drawn (its picture before the 2D is kept, for the panel's HUD).
void lmvr_window_scene_drawn(const lmvr_window_scene* scene);
// Game thread: the frame's 2D begins (the base's volume::beforeOverlay): the scene's picture is
// taken again, exactly as the 2D will be drawn over it.
void lmvr_window_overlay_begins(void);
// Host thread: the game's frame is finished, HUD and all. `flat_2d`: a full-screen 2D screen is up;
// `capture`: a ghost is being captured.
void lmvr_window_frame_done(bool flat_2d, bool capture);
// Host thread, before aurora_end_frame: the settings (the F1 overlay) are open. While they are,
// aurora renders the overlay alone too (aurora::gfx::set_overlay_capture), and each volume frame
// carries the last one in its `ui` surface, for the panel.
void lmvr_window_settings(bool open);
// The volume's size (metres) and the viewer's side (0 front, 1 right, 2 back, 3 left).
void lmvr_window_volume(float size[3], int* viewpoint);

#ifdef __cplusplus
}
#endif
