#pragma once
// Skippable cutscenes and fast-forward (docs/ENHANCEMENTS.md, "Skippable cutscenes").
//
// Skipping does not jump over game code: the game keeps running every logic
// frame of the cutscene, only much faster than real time, hidden and muted,
// with the event script's "press A" waits answered automatically. The state
// after a skip is therefore the state after watching. The same fast-forward
// runs visibly while the developer hotkey (hold Tab) is down.
//
// Pieces: the guest frame loop asks onGuestFrame() whether to keep running
// without presenting (frame.cpp); discarded frames send no draws to Aurora
// (gx::g_discardDraws, hle_gx.cpp); the VI retrace timer runs faster
// (retraceSpeed, hle_vi.cpp); the pad filter masks START and supplies the A
// presses (hle_pad.cpp); audio is muted (audio_out.cpp); the prompt and the
// cover are drawn by drawOverlay (menu.cpp).
#include "recomp/ctx.h"

struct PADStatus;

namespace recomp::skip {

// Guest side, once per presented guest frame. True: the frame is not handed to
// the host; the guest continues straight into the next frame (fast-forward).
bool onGuestFrame(Ctx& c);
bool cinematicCamera(); // guest thread: script owns the camera; VR uses a panel
bool dialogueCamera(bool textShown = false); // guest thread: ordinary prompt encountered in the current event
bool choiceMenuActive(); // guest interpreter waits for a directional CHOICE
bool pauseMenuView(bool roomDrawn); // Start-requested pause, distinct from scene handovers
bool statusMenuView(); // native item/portrait-ghost inventory menu
bool gameBoyView(); // guest thread: full Game Boy Horror frame/search interface is open
// Guest side, inside PADRead: masks START while a cutscene can be skipped, and
// drives controller 1 while skipping.
void filterPad(PADStatus* pads, int count);

// Guest frames presented so far, shown or skipped (LM_INPUT_CLOCK=frames times --input by it).
u32 guestFrames();

// VI retrace rate multiplier (1 = console speed). Any thread.
double retraceSpeed();
// Audio gain multiplier 0..1 (muted while fast-forwarding, then faded back in). Any thread.
float audioGain();

// Test aids (coop_test.cpp): fast-forward while a scripted boss fight runs (LM_BOSS_FF).
void setTestFastForward(bool on);

// Host side, inside the Aurora frame: prompt, cover while skipping, Tab hotkey.
void drawOverlay();

}  // namespace recomp::skip
