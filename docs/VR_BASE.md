# VR handoff

## Preserve the tested baseline

Branch `vr-base` starts from upstream commit
`db02360a39860dba673446ad964fc5569916e13e`. The PAL memory patch is present in
the working tree. Inspect `git diff` before starting new work and keep the
baseline fix separate from VR changes.

The PAL failure was reproduced with the user and an isolated automated test.
A `0x4E280` resource allocation failed with the original 1 MiB HLE reservation.
The guest decompressor at PAL `0x8000713C` wrote through a zero destination,
corrupting the bus clock. Moving the HLE reservation into the existing host
memory window preserved the clock and normal speed. The separate focus-pause
setting was disabled for the user's successful final check. No full-game
playthrough or VR testing has been completed.

## First decisions

Confirm the headset and intended PC runtime, then choose the first experience:
third-person stereoscopic viewing, a first-person camera, or a room-scale mode.
These have different camera, input, interaction and comfort requirements.

## Suggested investigation order

1. Build and run the unchanged flat-screen PAL baseline in a scratch profile.
2. Trace camera/projection setup, frame ownership and Aurora presentation.
3. Investigate OpenXR integration and graphics-device/swapchain interoperation
   with the existing Dawn renderer before choosing an implementation approach.
4. Add an optional headset session and stereo rendering without changing the
   game's simulation rate. Keep the flat-screen launch path working.
5. Add tracked camera behavior and a readable HUD/dialogue presentation.
6. Add controller bindings and comfort options for the chosen camera mode.
7. Recheck door scenes, mirror warps, the Game Boy Horror, cutscenes and bosses
   on both US and PAL bases.

## Source entry points

- `src/recomp/main.cpp`: application loop, setup, renderer initialization.
- `src/recomp/frame.cpp`: guest/host frame ownership and presentation.
- `src/recomp/pacing.cpp`: presentation pacing.
- `src/recomp/interp.cpp`: recorded frames, interpolation and replay.
- `src/recomp/hle_gx.cpp`: translated GX calls and projection/view state.
- `src/recomp/widescreen.cpp`: existing camera, viewport and HUD adjustments.
- `src/recomp/controls.cpp`: keyboard/gamepad mapping and input handling.
- `src/port/settings.h`, `src/recomp/menu.cpp`: persisted options and F1 UI.
- `extern/aurora/lib/webgpu`, `extern/aurora/lib/gfx`: graphics device,
  render worker, render targets and presentation.

These are investigation starting points, not a claim that VR can be added by
changing only the camera matrix. Stereo visibility, screen-space effects,
reflections, HUD layout and cutscenes need their own validation.

## Test data and saves

Use the user's own disc images outside this repository. Automated runs should
use scratch APPDATA and LM_CARD_DIR locations, LM_OFFSCREEN=1 and scripted
input. Do not modify the user's normal settings or saves during automated tests.
Keep game-derived data and build output ignored by Git.
