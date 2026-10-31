# Capture controls revision 41

Implemented: physical nozzle aiming and independent tool triggers; right VR stick supplies original directional counter-pull during capture, replacing left-stick-back automatic resistance. No physical-stroke HP gate. No automatic ghost-follow rotation. Tool mesh forward is calibrated independently of native stowing animation; the same transform supplies visible lip and suction origin/direction.

Pending: verified per-ghost maintenance-only 40-degree cone and 0.4-second grace, multi-target integration and native USA/PAL capture/reward comparison. These values are experimental, not enabled by this revision. Generic suction callbacks also handle loose objects, so they must not receive global aim widening. Preserve native acquisition, line of sight, reach, HP, AI, speed, damage, escape and reward rules. Never redirect acquisition toward an unattached target. No stamina, cooldowns or extra damage.

Validation: Windows build and vr-camera, vr-avatar, hle-registration-unique pass. Regression test combines glove contact and mesh-axis calibration with divergent runtime grip/aim orientations. Frozen baseline 14 files verify unchanged. No headset or full multi-ghost capture validation yet.

Revision 42 adds mirrored +/-90-degree local wrist roll to the shared glove/tool frame following headset feedback: real handshake pose appeared palm-down. Position and barrel-forward axes are unchanged; glove-local contact offsets rotate consistently. Automated palm-normal and tool-contact checks pass. Headset direction requires confirmation.

Revision 43 supersedes revision 42 wrist roll with 45 degrees. A held-only mirrored
90-degree turn around the measured contact maps the glove's curl cylinder axis
onto the unchanged barrel axis. Empty-hand finger tracking is unchanged. Regression
checks verify fixed contact and cylinder/barrel alignment. Native graphics appearance
and comfort remain headset checks; mathematical alignment is not visual validation.

Revision 44 reduces mirrored wrist roll to 15 degrees and separates local sleeve
direction from the held-only glove rotation. The sleeve uses the pre-grasp frame
orientation and final wrist position. Grasp and tool attachments remain as in 43.
Build and all three automated checks pass; appearance needs headset confirmation.

Revision 45 supersedes revision 44 wrist roll: no added roll, default outward yaw
zero, local short forearms off. Test profile updated after normal game close to
prevent saved settings restoring old defaults. Held-only grasp remains unchanged.
All three automated checks pass. No claim of measured physical-hand alignment.

Revision 46: user-requested 15-degree counterclockwise local wrist roll, same sign for both gloves, applied through shared calibrated glove/tool frame. Existing grasp and hidden sleeves retained. Three automated checks pass.

Revision 47: F1 independent left/right wrist roll sliders [-180,+180], defaults
left+15/right-15, live and saved. Appends fields after existing layout version,
so old configs retain these defaults. Both glove draw and tool contact paths use
per-hand values. Tests cover positive/negative/zero rotations and contact alignment.
Build and three automated checks pass; visual tuning is user-driven.

Revision 47: F1 independent left/right wrist roll sliders [-180,+180], defaults
left+15/right-15, live and saved. Appends fields after existing layout version,
so old configs retain these defaults. Both glove draw and tool contact paths use
per-hand values. Tests cover positive/negative/zero rotations and contact alignment.
Build and three automated checks pass; visual tuning is user-driven.

Revision 48: menu::isOpen routes F1 to mono panel submission and disables gameplay eye camera overrides for its duration. No persistent view-mode change. Build and three checks pass; headset menu fusion needs confirmation.

Revision 47: F1 independent left/right wrist roll sliders [-180,+180], defaults
left+15/right-15, live and saved. Appends fields after existing layout version,
so old configs retain these defaults. Both glove draw and tool contact paths use
per-hand values. Tests cover positive/negative/zero rotations and contact alignment.
Build and three automated checks pass; visual tuning is user-driven.

Revision 49 replaces revision 48 whole-view mono settings with a separate transparent
OpenXR quad over normal world projection. UI-only render target (Aurora 0228), third
swapchain and alpha-preserving transfer. Fixed panel 1.4m wide at 1.6m from recenter.
No eye-camera suppression. All acquired swapchains released and validated before
submission; no-render endFrame omits overlay. Three checks and Windows build pass;
headset visual comfort requires confirmation.

Revision 50 (built, not installed while user tests revision49): diorama/life-size
left stick routes to raw native gameplay axes outside first-person locomotion;
right stick routes to native C-stick; analog triggers use native L/R rather than
first-person ownership/holster checks. Existing F1 input suppression retained.
First-person mapping remains separate. Windows build and all three automated
checks pass; runtime controller/mode-switch validation pending. Current preview49
left running intentionally so the user can finish F1 stereo panel validation.

Revision 47: F1 independent left/right wrist roll sliders [-180,+180], defaults
left+15/right-15, live and saved. Appends fields after existing layout version,
so old configs retain these defaults. Both glove draw and tool contact paths use
per-hand values. Tests cover positive/negative/zero rotations and contact alignment.
Build and three automated checks pass; visual tuning is user-driven.

Revision 51 installs revision50 input routing and sets user-calibrated roll defaults
and reset values to +60.8/-60.8 degrees. Test profile preserved those angles.
Build, vr-camera, vr-avatar, hle-registration-unique pass. Frozen baseline verifies.
Diorama runtime controller/mode-switch test remains pending.

Revision 47: F1 independent left/right wrist roll sliders [-180,+180], defaults
left+15/right-15, live and saved. Appends fields after existing layout version,
so old configs retain these defaults. Both glove draw and tool contact paths use
per-hand values. Tests cover positive/negative/zero rotations and contact alignment.
Build and three automated checks pass; visual tuning is user-driven.

Revision 52:
- Per-mode VR profiles vr-diorama.ini, vr-life-size.ini, vr-first-person.ini. Legacy vr.ini migrates into its selected mode; switching saves/restores mode values. Mode changes clear transient first-person movement, capture and holster state. First-person gaze/hand overrides explicitly reject other modes.
- Native diorama/life-size sticks merge active axes without idle VR controllers erasing gamepad input. Pair with greater magnitude wins; neutral/noisy VR input preserves native axes. Buttons and analog triggers retain native mappings.
- Mirror hand height anchors to the visible head joint rather than elevated virtual eye height. Local gloves remain unchanged. Headset confirmation pending.
- Local wrist end caps use measured native white cuff rings and existing vertex attributes, double-sided and scoped outside reflections. Ring sourced before sleeve removal. No authored game asset changes.
Validation: build, vr-camera, vr-avatar, hle-registration-unique pass. Native isolated PAL fixture confirmed two wrist caps, normal exit, 25.4 seconds. Mirror and hardware input/mode persistence require user review. Frozen baseline 14 files verify unchanged.

Revision 53:
- Separate OpenXR thumbstick-click action; both pressed toggles F1 on rising edge. Controller aim ray hits the existing 1.6m/1.4m UI panel; trigger selects/drags, right stick scrolls. Same menu save path as keyboard F1. Gameplay/snap turn suppressed while menu owns input.
- Diorama analog triggers no longer depend on transient sRender. Fresh action samples remain available through non-renderable frames. First-person capture routing retains left-stick movement while right resistance stick is neutral; capture focus clears on vacuum release.
- Cuff geometry closes both end rings, double-sided. PAL fixture confirmed four rings before reflection test.
- Reflected held gloves preserve native holding orientation/finger articulation and equipment uses native hand-relative matrices; local tracked grasp unchanged. Mirror-only hand height default -0.25m, adjustable/saved per mode in F1. Native diagnostic maps input Y=-0.4 to mirror target Y122.22 with native head Y187.22, vs147.22 previously.
- Hose rear endpoint/tangent align to measured tool rear connector and barrel axis. Native hose simulation restored after draw.
Validation: Windows build and vr-camera, vr-avatar, hle-registration-unique pass. Pure checks include UI ray intersections, all cuff end caps, and explicit hose connector endpoint. Isolated PAL mirror/native held-tool fixture passed39.5s and normal exit. Final connector tangent uses the tested pure alignment math; headset appearance, shortcut/pointer and continuous trigger feel require review. Frozen 14-file base unchanged.


## Comfort and glove corrections, build 63

Game Boy Horror model/grab is off by default in first person, restored by F1 > VR > Enable grabbable Game Boy Horror; the appended per-mode setting migrates old files to disabled. Native button access remains. Reflections retain the authored device. Speech projection, glyphs and scissor rectangles use a headset-relative speech mapping, distinct from the stationary health HUD. Close-up scene speech can use this mapping too. Speech lift is limited to 8 cm for comfort. Oblique closed wrist-cut boundary loops are capped in addition to the previous axial cuff rings; the new regression and native PAL cuff fixture pass. Local glove only; mirror geometry unchanged. Suppress presentation fade when only an unused scripted-world flag changes in first person. Remaining foyer fade and actual cuff/text appearance require headset confirmation.
