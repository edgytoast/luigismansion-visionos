#pragma once

// Apple Vision Pro: the mansion "On your table", in a volume in the shared space (visionOS gives an
// app no head or controller pose outside a Full Space, so the game plays flat, with a gamepad).
// The scene mirror (extern/aurora/include/aurora/mirror.h) records the main 3D view's draws; this
// module places them in the volume as a world-locked set, sized to the room (volume.cpp has the
// rules), and hands each frame's placement to the port's window host (lmvr_game_hooks.h). Off
// unless the port's window mode is on (LM_VOLUME=1, set by the host before the game starts).

#include "recomp/ctx.h"

namespace recomp::volume {

bool enabled();

// Host thread, the top of the main loop's frame: waits for the window's next update (at most
// 100 ms), which paces the game.
void hostBeginFrame();
// Host thread: the game's frame is finished, HUD and all (before the F1 overlay).
void hostFrameDone();
// Host thread, before aurora_end_frame: whether the settings (the F1 overlay) are open. While they
// are, aurora renders the overlay alone too, and the window host puts it on the volume's panel.
void beforeEndFrame(bool settingsOpen);
// The volume's settings, in memory only (main.cpp at start, menu.cpp's init after it loads them
// again): the resolution scale at most 2 (the window's own by default), VR's tested renderer settings
// (no enhanced lighting, no copy filters, no VSync, no pause on focus loss). Saving writes the
// player's own values for them (port::setSaveFilter).
void applySettingsOverrides();
// "Restore all defaults" on the table (menu.cpp): the player's values taken at start are dropped
// and the save filter with them, so the reset writes the real defaults; applySettingsOverrides then
// takes those as the player's own and overrides them again in memory.
void forgetPlayersSettings();
// The resolution scale the volume renders at for a setting (0: the window's own; above 2: 2).
int resolutionScale(int setting);

// Guest thread, around the main 3D view (enhanced_lighting.cpp's fn_8000BB98 with r3 == 0).
void sceneBegin();
void sceneEnd();

// Guest thread: the frame's 2D is about to begin (the HUD, a message window, the room's name). The
// panel's picture of the scene is taken again here, after what the game does to its picture
// between the main 3D view and its 2D (a copy to a 16-bit texture and back): the panel's HUD is
// then exactly what the 2D changed, faint parts of dark text boxes too.
void beforeOverlay();

// The map has settled (its main view drawn kSettleFrames times in a row: what the mirror waits for
// before it records). The volume's wider culling (widescreen.cpp) waits for it too: a map's first
// frames load its models.
bool settled();

// Guest thread (hle_pad.cpp): the stick, turned from the volume's front (or the side the viewer
// moved to) into the game camera's.
void applyMovement(s8& x, s8& y);

// gameplay_hooks.cpp: a ghost is being captured this frame (the Poltergust's capture update).
void noteCapture(u32 ghost);

// hle_gx.cpp: the scene's perspective projection as emitted (m[0][0], m[1][1]).
void notePerspective(float m00, float m11);

// Guest thread: marks for the scene mirror around a part of the main view (aurora/mirror.h:
// MarkGhostPass round the ghost layer's pass, unused_content.cpp; MarkWorld round a room's static
// model, tags.cpp), only while the volume is on. Nested scopes of one mark send it once.
class MirrorScope {
public:
    MirrorScope(u32 mark, bool on);
    ~MirrorScope();
    MirrorScope(const MirrorScope&) = delete;
    MirrorScope& operator=(const MirrorScope&) = delete;

private:
    u32 mark_;
    bool on_;
};

}  // namespace recomp::volume
