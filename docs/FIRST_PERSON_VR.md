# First-person VR development â€” 2026-10-20

The intended experience is Luigi's point of view, his body when looking down, his hat brim when looking up, and his own green sleeves and white gloves driven by tracked hands/controllers. Flashlight and keys belong on a waist belt; grip grabs them. Grip over the shoulder equips the Poltergust. Doors should use reach, grip and twist; keys should physically unlock them. Controller-free input is a later option if the runtime provides suitable hand tracking. Finish PC first-person before attempting standalone Quest 3 or Steam Frame builds.

## Implemented in the experimental build

- First-person camera anchored to Luigi's captured position, physical headset stereo and leaning. Default eye height 1.80 m and forward offset 0.20 m are adjustable in F1 > VR. The existing model remains drawn; the hat/body appearance still needs visual verification.
- View heading stays independent of Luigi's movement animation. Xbox movement and the left VR stick are remapped relative to headset heading. Right VR stick supplies 30-degree snap turns, with neutral rearming. F6 recenters.
- Menus and authored animations retain the existing flat/stereo cinema policy and 85% animation depth. First person does not apply miniature room-frame removal.
- OpenXR left/right grip and aim poses, squeeze, trigger and thumbstick actions. Suggested bindings for Touch, Index and Microsoft motion controllers; other devices depend on runtime bindings. Inactive/untracked hands do not drive arms. This is controller tracking, not optical hand tracking.
- An experimental draw-only adapter drives the gameplay Luigi's actual arm and glove skeleton using two-bone IK, controller wrist orientation and the retail animated fingers. Opaque/translucent draw calls restore their original joint/view matrices afterward. It does not change simulation or collision state. F1 > VR can disable tracked arms.
- Input-only grab intents for belt flashlight, belt key and shoulder Poltergust; nearest target selection, grip hysteresis, trigger use, wrist twist, tracking-loss release and inventory invalidation. Door targets are supported by the intent layer. These intents do not yet operate game objects.

## Validation completed

The Windows executable builds successfully. Both CTest suites pass: camera/world recovery, scale, IPD, lean, yaw, projection, recenter, UI policies and movement mapping; arm lengths/reach, bone direction reversals, glove orientation/roll, grab/release, tracking loss, target invalidation and door twist. Non-OpenXR stubs are retained.

The locally owned USA and PAL gameplay Luigi rigs have identical topology and inverse-bind matrices: 65 joints, 55 envelopes, combined SHA256 `bca8b00b93b71e3827754cb25de26fdccc437ccdd9b18fa438d665cff497c07e`. Arm chains are 16/17/18 with glove descendants through 31, and 33/34/35 through 48. The adapter only accepts the active player's model object with those counts. Different demo/replacement rigs are skipped.

All 14 frozen base-game files pass the original hash manifest. The accepted `out/vr` executable is retained. First-person output goes to `out/first-person`; project launcher profile is `profiles/first-person`, with separate settings, cards and cache.

OpenXR lifecycle probing failed at `xrCreateInstance` with result -2, both inside and outside the sandbox. No successful runtime or headset visual test was possible during this work. Compilation and math tests do not establish that the first-person view or rig looks correct in the game. No user testing was requested.

## Required next implementation

1. Validate first-person render capture and actual arm adapter activation when an OpenXR runtime is available; inspect head/face occlusion, hat brim, body orientation, wrist calibration, arm seams and room culling. Camera eye offset is provisional. The current full model can obstruct the camera when body animation turns its head.
2. Add first-person head/face masking that keeps the brim, and body-yaw behavior consistent with the headset. Support the required demo rigs or keep them in cinema. Arm shadows currently retain authored animation. Controller fingers retain retail animation, rather than tracking each finger.
3. Move hand updates closer to presentation; current pose deformation enters the game's captured/interpolated render stream, so it is not late-latched.
4. Connect actual inventory ownership and physical holsters to the grab state machine. Use runtime aim poses for flashlight cone, Poltergust nozzle, beam, suction and elemental effects, with the game's existing equipment/progression checks. Add hand/tool contact and haptics.
5. Locate live door/lock handles, bind reach/grip/twist/key insertion intents to existing guest interaction functions, preserve locked-room and scripted-event rules, and handle cancellation/tracking loss. Never call a door function solely because an input-only intent fired.
6. Add VR menu/confirm buttons, locomotion options and optional hand-tracking extension support. Xbox remains needed for menus, game actions and progression in this prototype.
7. Once PC first person is functional, profile reduced-resolution standalone Quest/Frame renderers. Neither standalone platform is implemented here.

The previously reported Game Boy Horror camera depth and flat NPC/event lead-ins are not claimed fixed by this work.

## Local launch and build

Run `powershell -ExecutionPolicy Bypass -File tools/launch-isolated.ps1 -Profile first-person`. This enables `LM_VR=1` and `LM_VR_FIRST_PERSON=1` only in the child process. A fresh isolated profile requires the user's own disc setup. Existing baseline/VR saves are not moved.

Build with `tools/build-local.ps1`, then install with `cmake --install build/windows-clang --prefix out/first-person`. Do not install this experiment into `out/vr` or `base-game`.

OpenXR input follows the [Khronos action-space specification](https://registry.khronos.org/OpenXR/specs/1.0/man/html/xrCreateActionSpace.html).


## Checkpoint 2 ï¿½ 2026-10-21: headset testing required

Additional implementation:

- VR confirm/back, pause, map and search button actions, with thumbstick menu navigation. Touch/Index defaults: right A confirms, right B goes back; left A/X opens the map; left B/Y opens search; left stick click pauses. Microsoft motion controllers have trackpad/menu bindings. Simple controllers receive basic select/pause/pose bindings. Actual Steam Frame bindings remain unverified.
- A render-to-guest button latch preserves short presses until PADRead and suppresses held buttons after focus loss or while the F1 overlay owns input.
- Brief optional haptics for snap turning and successful grab intents. Missing haptics cannot abort rendering.
- Live Poltergust ownership from the game's equipment flag, one-hand ownership of each tool, cancellation on player/map/room changes, release-before-regrab after cancellation, and a maximum reach while holding a door target. Keys stay unavailable until their actual room-specific inventory adapter is verified. These are still input intents, not working physical items.
- Corrected the avatar adapter's coordinate spaces. The game's joints are model-space. The adapter now recovers the model owner transform from untouched root joint/view matrices, transforms tracked targets into model space, validates that transform against a second joint, invokes the original skinning routine, updates the affected render matrices, and restores the original joint/view/normal buffers directly. This avoids rebuilding the character under a camera matrix that omitted its owner transform.
- Corrected anatomical handedness: the retail model faces +Z, whereas OpenXR faces -Z. The left controller drives joint chain 33/34/35, and the right drives 16/17/18. Both retain their own glove/finger descendants.

Validation:

- Full Windows executable build succeeded; both CTest suites passed, including affine owner-transform recovery, scaled/rotated model-space targets, short button presses, focus/overlay handoff, dual-hand competition, duplicate tool holsters, door reach cancellation and non-finite quaternion rejection.
- Automated actual-game synthetic pose runs succeeded on both PAL and USA and exited normally (PAL 26.4 s; USA 47.4 s). Logs show the pose adapter activated and model-space reconstruction residual 0.000000, followed by restoration of all three matrix buffers. Render dumps were captured locally and inspected. These runs used a normal flat camera, not an OpenXR headset.
- Reproduce locally with `python tools/test-vr-avatar.py PAL` or `USA`. This copies the existing local disc-module cache to an isolated work profile and never copies saves. The `LM_VR_AVATAR_TEST=1` switch is an automated test aid, not a user input mode.
- OpenXR probing still fails at instance creation (-2), including outside the sandbox. No live controller-action, haptics, stereo first-person, comfort or occlusion result is claimed.
- Frozen baseline files verified against the 14-file manifest. The accepted Preview13 diorama executable remains unchanged.

Next required headset checks:

1. First-person position, head turning/leaning and snap turning: check body/hat visibility, face occlusion, room completeness and comfort.
2. Raise and rotate each controller separately: check the correct sleeve/glove moves, wrists align, and arms do not cross or detach. Check tracking-loss recovery.
3. Check confirm/back, pause, map/search, menu navigation and feedback. The actual flashlight/Poltergust aim and physical door/key interaction remain disconnected.

Further camera/mesh calibration and attaching functional tools depend on these checks. This checkpoint deliberately retains the working diorama and frozen flat build as fallbacks. Standalone Quest/Steam Frame work remains deferred.


## Checkpoint 3 ï¿½ Steam Frame poses, face visibility and conversation anchor

User testing confirmed first-person position but reported no moving arms, nose/body occlusion, and external camera framing during parts of NPC introductions. Motion-controlled equipment remains deliberately unfinished.

Changes:

- Optionally enable the advertised `XR_VALVE_frame_controller_interaction` extension and suggest the native `/interaction_profiles/valve/frame_controller_valve` bindings. Frame defaults: right A confirm, right B back, left View pause, left D-pad Down map, left D-pad Up search; left stick movement and right stick snap turn. Other headset profiles retain their bindings. Reference: https://partner.steamgames.com/doc/steamhardware/steamframe/input .
- Accept active controller poses with valid position/orientation even when the runtime temporarily infers them without TRACKED bits. Inactive/invalid poses still stop driving hands. Log active interaction profiles and grip/aim flags so the remaining failure can be distinguished from avatar deformation.
- Hide only the gameplay model's face/nose/eye/ear materials during first-person draws. Verified model shape/material associations preserve hat, torso, sleeves and gloves. Material flags are restored immediately after each draw; cinema scenes retain the full face. Unknown replacement layouts are skipped. This does not yet guarantee every body/hat animation avoids eye occlusion.
- Snapshot Luigi's live actor position at guest frame capture; the regular view-update hook can be idle while events move him. Head-tracked NPC dialogue uses that current anchor, while existing authored cinema scenes remain in their stereo screen presentation. Scripted dialogue does not gain movement control.
- Face masking operates independently of the tracked-arm checkbox or controller-pose availability.

Validation: executable build and both CTest suites passed. The PAL synthetic avatar/face-mask run activated ten face-only material masks, reconstructed the owner transform with residual 0.000000 and exited normally. Frozen baseline manifest still passed. The running preview subsequently reported the native Steam Frame profile and active, valid grip and aim poses with flags 15 on both hands. Visible arm motion, occlusion comfort and NPC framing still require user confirmation.

The active test uses `profiles/first-person`, cloned from the user's previous experimental profile; the Downloads build, base game and accepted diorama executable are preserved. Tracking logs are in `profiles/first-person/logs/preview3-stdout.log` and `preview3-stderr.log`.


## Checkpoint 4 - Opaque draw hook and scripted Luigi

Found duplicate HLE registrations for fn_80059C54. The widescreen handler replaced the avatar handler for opaque draws, so earlier successful pose logs did not prove visible arms or face masking. There is now one handler combining both behaviors. A registration uniqueness regression check passes alongside the camera and avatar tests.

The current PAL synthetic smoke test applies the pose, hides eleven head/hat materials and restores joint/view/normal pools byte-for-byte. A render capture confirms the head is absent. The whole hat is temporarily hidden in first person to prevent clipping; a visible brim remains future work. Body and glove materials remain.

During scripted dialogue, matching scripted Luigi rigs can supply the current actor anchor instead of the hidden regular player. This needs headset verification in the professor introduction. Authored cinema shots retain screen presentation. Functional motion-controlled equipment is still pending.

Upstream synchronization is deferred at the user's request until first-person mode is finished. No upstream merge was performed.


## Checkpoint 5 - Shoulder recruitment and pickup-pose presentation

Headset feedback confirms visible hand tracking. Shoulder roots were fixed in model space. Both retail clavicles (15 and 32) now rotate toward extended/raised hands; shoulder displacement is limited to 6.5 cm and clavicle and arm segment lengths remain unchanged. Corresponding view/normal matrices and skinned envelopes are updated, then restored after the draw. Automated tests cover resting, overhead and far-reaching targets; headset comfort and sleeve deformation still need confirmation.

An interpreter ANYKEY wait no longer identifies an NPC conversation by itself: pickups also use that wait. Only rendered speech latches dialogue. During a non-dialogue authored event the avatar override yields, preserving Luigi's complete pickup pose in the existing stereo cinema presentation. Once control returns the normal first-person policy resumes. Pickup and NPC transitions still need a live check.

Confirmed goal: genuine 6DOF equipment and hands. Tracked position and orientation must drive the tool mesh and its gameplay origin/direction together. A PAD/C-stick approximation is not completion. This checkpoint still changes avatar presentation only; the equipment intent/holster code is not yet connected to tool effects, physical keys or door knobs. Planned adapters must validate inventory, scene ownership and tracking loss, and be tested for both light/ghost interaction and suction/object capture.


## Checkpoint 6 - Wrist handedness, sleeve bend plane and key-get signal

Headset testing rejected checkpoint 5 arm twisting and reported no pickup camera change. The right retail wrist's local Y axis points opposite the left wrist's axis; align its back-of-hand normal with the corrected sign. Upper and lower arm transforms now carry the elbow bend plane as well as the bone direction, instead of leaving sleeve roll unconstrained by that plane. Tests verify that the frame transform changes roll without moving either endpoint. Natural appearance remains a live test requirement.

The native key-get particle creation now explicitly requests pickup presentation. Particle resource names are recorded from both archive lookup overloads. During this state the stereo cinema policy overrides first-person dialogue selection and avatar masking yields, retaining the full pose. State is scoped to the current actor, records its pose controller after two frames, exits when that controller changes after the minimum interval, and has a 150-present safety timeout. This replaces reliance on script cutscene flags for pickups. Native key-get triggering and end timing still require verification; no claim of a successful live key pickup is made yet.

Genuine 6DOF equipment gameplay is still pending; this checkpoint does not connect the tools.


## Checkpoint 7 - Common body heading, tracked wrists and full pickup phases

Live checkpoint 6 feedback: inward glove angle remains, turning the game bends the arms away from controllers, and key camera only changes during put-away. Its log confirms the key-get effect arrives late in controller 2/34, motion 49. The native controller setter has separate pickup phases 0x21 and 0x22 (state 2). Pickup presentation now directly follows both phases, without a particle timer; the effect is only a diagnostic marker. Actor/scene validity and first-person mode gate the selection. The full model remains visible for these phases, and the first-person view returns when both phases have ended. Exact live timing still needs confirmation.

The skeleton now rotates about its actor into the same VR heading used for tracked hands, before arm IK. Bend hints use that same frame. Gloves independently map to the actual controller target position and orientation rather than a reach-clamped wrist. Sleeves can extend axially up to 1.6 times their original segment length; glove size stays independent of sleeve stretch. Extreme reaches can exceed the sleeve limit and still need visual validation.

Default glove calibration turns each hand outward 8 degrees. F1 > VR > Hand outward angle allows -20 to +30 degrees, saved in the isolated VR profile; it changes orientation without moving the wrist. Existing vr.ini files default to 8 degrees when the appended field is absent.

Regression checks cover arbitrary owner/game/VR yaw combinations, controller target positions, mirrored outward calibration, sleeve-axis stretch with unchanged thickness, and both pickup phases with negative NPC/door cases. All three CTest suites passed and the PAL offscreen avatar smoke exited normally. Frozen flat build is preserved. 6DOF tool effects remain unfinished.


## Checkpoint 8 - Floating gloves, head-facing body and controller fingers

User preference replaces connected green sleeves with floating white gloves. In first-person draws only, the shared green mesh's triangles are filtered by their sleeve skinning weights. Torso and leg triangles remain, including mixed packets. Triangle strips/fans/quads are expanded with winding retained; unknown/truncated display lists fail closed. The native packet field is a model-relative offset, so filtered lists use an offset back from the model to the HLE-owned buffer. Packet pointers/sizes are restored after drawing; no source assets are changed. Full arms remain in authored third-person pickup/cinema poses.

The body follows virtual heading plus horizontal headset yaw. Looking down does not pitch the body, and nearly vertical head directions retain the last stable horizontal yaw. Gloves remain in the controller reference frame independently of body-facing yaw. Connected-arm IK is removed from this rendering path.

Optional XR_EXT_hand_tracking reads each valid finger's joint bends. If advertised, XR_EXT_hand_tracking_data_source explicitly requests controller-inferred tracking, and XR_EXT_hand_joints_motion_range uses the controller-conforming range. Tracker creation/locating failure falls back to analog curls and cannot stop VR. Unsupported/invalid fingers independently fall back to trigger (index) and grip (other fingers). The thumb uses a partial grip curl in fallback. Logs report tracker availability and the five-bit valid-finger mask. No controller-free interaction is introduced.

Luigi's retail gloves have four digits: thumb and index/middle are mapped separately, with sensed ring/little curls averaged for the fourth visible digit. Fingers rebuild from the user's own bind matrices before curling, preserving bone lengths and avoiding doubled retail-animation curls. Controller sensing does not by itself connect inventory tools or doors; physical gameplay effects are still unfinished.

Validation: all three CTest suites passed, covering sensed/fallback digit independence, finger-chain lengths, open/closed curls, mixed torso/sleeve triangles, strip winding and malformed-list rejection. PAL offscreen smoke activated the filter (the sleeve-only packet becomes zero bytes), drove distinct synthetic trigger/grip poses, restored original matrices and exited normally. Live finger-sensing availability and headset appearance still require confirmation.

References: https://partner.steamgames.com/doc/steamhardware/steamframe/input and https://registry.khronos.org/OpenXR/specs/1.1/man/html/XR_EXT_hand_tracking.html .


## Checkpoint 9 - Detached cuff influences and a small eye setback

User confirms individual fingers work and pickup animations now switch to a stereo screen for the full animation and return smoothly to first person. Preserve that pickup policy. Requested correction: keep object sizes/perspective, pull camera back slightly, and remove cuffs stretching toward the body.

Retail glove cuffs use envelopes 68 and 72, each a 50/50 blend of wrist and forearm. The hidden forearm stayed at the body while the wrist moved, stretching the cuff. Rebase the forearm's bind transform into the same tracked glove frame before the native skinning call. This leaves finger sensing and glove dimensions unchanged. Remaining body mesh and original matrices are restored as before.

The first-person eye-forward default changes from 0.20 to 0.08 metres: a 12 cm setback. Eye height, object sizes, room scale and OpenXR projection remain unchanged. Controller wrist targets use the same updated eye reference, retaining physical hand alignment. Existing saved offsets remain user-controlled through F1 > VR > Eye forward offset; lower values move the eye back. No object-size reduction was implemented, following the user's clarification.

Validation: three CTest suites pass. New checks verify the mixed cuff stays rigid with the glove and the setback alters position only, preserving the view's linear scale. The PAL offscreen smoke exits normally; native cuff-matrix residuals are 0.000015 (left) and 0.000008 (right). User comfort and visible cuff detachment still need headset confirmation. Accepted base/diorama builds stay separate. Motion-controlled equipment effects remain unfinished.


## Checkpoint 10 - Input routing during events/choices and shadow lookup spaces

User accepts checkpoint 9 gloves and camera, plus the full pickup transition. New feedback: cannot move during training despite turning/button actions; gallery choice navigation fails; gallery professor and first ghost-room walls show odd shadows. No glove, camera scale/position, finger or pickup-policy changes in this checkpoint.

The VR input bridge returned early when presentation flags disallowed head-relative locomotion. This discarded the VR stick instead of simply changing its coordinate mapping. Event/dialogue views now receive raw stick axes; normal gameplay retains head-relative mapping. The guest remains responsible for actual movement locks. Stick input activity no longer depends on valid optical grip tracking. Explicit native CHOICE waits force raw UI axes and translate stick directions into D-pad bits for menus that consume digital directions. Choice routing does not apply to ordinary training movement.

Native shadow receivers have two coordinate spaces: TEXMTX values use model-space vertices; PT18/PT19 (slot >= 64) use already transformed view-space positions. VR previously multiplied both by inverse-eye, incorrectly making model-space lookups depend on head movement. Only post-transform/view-space receivers now undo the added eye transform. Shadow caster maps and model-space lookup matrices remain as authored. This corrects a concrete mapping error; the user's exact professor/wall visuals still need live verification. No blanket shadow disabling was introduced.

Validation: final build and three CTest suites pass. New regressions check raw menu/training axes, head-relative axes, dead zones and clamping, plus stable model-space shadows and cancellation of the added eye transform for view-space lookups under several rotations. PAL offscreen render/avatar smoke completed normally after the shadow/input-routing changes; the final CHOICE adapter still needs live menu verification. Base manifest remains checked separately.


## Checkpoint 11 - Head-relative training, root-facing torso, native trigger fallback and calls

User feedback: training movement must follow the HMD in every direction while the visible torso stays facing the HMD; right middle finger deformation; cannot use Poltergust; professor call portrait absent. Camera, glove size/cuffs and full pickup transitions remain unchanged.

Training/event gameplay no longer uses the raw-axis fallback. First-person world movement always uses the head-relative mapping. Only an explicit native CHOICE or panel/menu uses raw UI axes. Removed the stale presentation-derived movement-mode flag. The game retains its movement locks. Body visual alignment now measures the rendered root skin transform instead of relying on the actor-facing vector, which can disagree with walking animation. This targets a gaze-facing torso during sideways/backward motion; directional gait appearance still needs live checking.

Correction to checkpoint 8's interpretation: the optimized rig has four explicit digit chains plus a visible digit driven by blended envelopes. The prior four-channel mapping was incomplete. Explicit chains are thumb, index, middle and little; the intervening ring finger now has independently posed virtual-chain envelopes instead of inheriting the mean of its neighbors. All five sensed curl channels are retained. Right ring envelopes 70/71 and left 74/75/76 receive the independent ring pose after native skinning. This should prevent neighboring curl differences stretching the middle/ring region; exact right-middle appearance still needs verification.

Right OpenXR trigger now forwards native R/analog suction, left trigger native L/analog element input, only with owned Poltergust and an active first-person world, and not during CHOICE. This is an explicitly interim button fallback. It does not implement the still-pending 6DOF nozzle/beam/suction origin, grip equipment acquisition or physical doors/keys.

Professor calls in training can draw their bezel while actual text is shown; idle training still suppresses it. Call portrait partial viewports and clipping now follow the same comfortable HUD placement as the bezel, while their own 3D camera stays intact. Full-view resets remain unchanged. Restoring the actual call portrait and menu behavior requires live validation.

Validation: final build and all three CTest suites pass, including five independent sensed curl channels. PAL offscreen smoke uses deliberately different middle/ring/little curls on each hand, verifies cuff skinning remains rigid, and checks restored guest matrices/normal arrays. Native trigger gameplay, full torso stability and portrait placement require a headset pass. Frozen base files are checked separately.


## Checkpoint 12 - Attached equipment and interim headset aim

User accepts gloves and fingers. No changes to tracked hand calibration, finger articulation, camera setback, pickup presentation or locomotion in this checkpoint.

The nozzle/flashlight, backpack and Game Boy Horror are separate actor/model objects. First-person drawing now applies the torso's world yaw about Luigi's position to their model-view arrays, updates normal matrices, and restores all guest arrays after each draw. The native player root skin supplies authored facing, matching the torso correction. No disc model data changes. Authored pickup/cinematic presentation remains exempt.

A mutex-protected headset forward vector includes physical yaw/pitch and virtual snap turns. The flashlight hook temporarily overrides its native direction input before the original beam/ghost tests run, restoring the source vector after the call. The suction-record update applies look direction and rebuilds the native endpoint; its existing mapped hook is now enabled for PAL as well as USA. The nozzle render direction additionally rotates toward the same gaze around its attachment origin. This is interim headset aiming, not completed 6DOF tool control or equipment grabbing.

Validation: Windows build and three CTest suites pass. New checks cover headset pitch through snap turns and attachment stability in rotated camera coordinates. A fresh PAL synthetic avatar run rebuilt the game modules, exercised all three attached models (3/4/3 matrices), retained rigid gloves and restored guest arrays, then exited normally. All 14 frozen base-game files verified unchanged. Actual flashlight stun direction, suction interaction, nozzle pitch and visual pack alignment require the next SteamVR headset test.


## Checkpoint 13 - Tracked glove shadows, fit controls and secondary projections

User reports tracking and fingers work but glove placement feels closer to the face than real hands, then clarifies the offset is subjective. The tracking scale/reference math has not revealed a proven height error. No arbitrary position offset is enabled. Added optional F1 > VR hand height/reach adjustments (metres, default zero, reset button); negative height lowers the gloves, positive reach moves them farther forward in the upright recentered tracking frame. Rotation/finger articulation and camera remain unchanged. Both offsets persist at the end of vr.ini for backward-compatible loading.

The native shadow-caster hook previously did not run the scoped avatar pose. It now receives the same tracked glove/finger pose and equipment alignment. Head/hat hiding applies only to eye views, so Luigi keeps a complete head in his shadow. The existing scoped restoration still restores arrays/materials/display-list changes after drawing.

The uncovered first-room mirror was reported to have odd stereo. Found a concrete secondary-camera inconsistency in VR replay: untagged secondary perspective projections could be replaced while their non-main view matrices were left authored. World-mode replay now replaces only projections consumed by main-view geometry. Map and cinematic policies remain unchanged. This is a targeted projection correction, not a claim of a completed eye-correct reflected camera; the actual mirror needs headset verification.

Windows build and all three CTest suites pass. PAL synthetic validation checks glove poses and the shadow-caster path without SteamVR; live fit and mirror appearance remain pending. The first-person preview is left closed for the user's later headset test. Frozen base-game files verified separately.


## Checkpoint 14 - Complete reflected avatar and stable desktop preview

User confirmed checkpoint 13 glove placement feels better, shadows are good, and the uncovered mirror works. The reflected Luigi remains headless/armless because the local-eye visibility policy was applied to every avatar draw.

Scoped avatar drawing now recognizes reflected camera/model bases by normalized determinant before changing face materials, sleeve packets or joints. Those reflected draws retain the complete native avatar, including head, hat and authored arms. Local first-person drawing still hides face/arms and retains accepted tracked gloves. Native reflected arms are not yet driven by a new full-arm IK solution. Actual first-room mirror pass recognition requires live verification; reflection classification logs have been added.

The desktop previously presented both eye renders in sequence. New Aurora patch 0226 adds end-frame completion without desktop surface acquisition/presentation, while retaining GPU submission and readback callbacks. The left eye continues to present to the PC window; the right eye completes offscreen and is copied/submitted to OpenXR as before. Headset stereo and flat menus remain unchanged. This removes the alternating eye presentations without a third scene render.

Validation: Windows build and three CTest suites pass. New reflection tests cover ordinary rotations, single-axis reflections, two-axis flips, tiny/nonuniform scales and invalid bases. PAL avatar smoke run checks local-eye glove/face behavior and shadow poses. Desktop stability and native mirror pass recognition require the headset test. Frozen base-game files remain checked separately.


## Checkpoints 15/16 - Native mirror prepass identified from live PAL trace

User confirmed checkpoint 14 still drew the first-room mirror avatar without head/arms. No reflected-basis log was emitted. Checkpoint 15 was explicitly a diagnostic build: bounded native call-stack records separated normal actor draws from calls at PAL 8000CBD0/8000CBDC, before the main-view loop.

Inspection of the user's own USA/PAL discs verifies those calls are the native reflection prepass: fn_800116CC prepares the reflection viewport, actor renderer fn_80060E70 receives 0x10000/0x10100, then fn_80011750 copies the reflection region via fn_80025C98 before the regular scene renders. This camera has positive handedness, so the determinant fallback cannot identify it.

Checkpoint 16 scopes the existing USA/PAL actor-render hook using native bit 0x10000. During that scope the avatar override leaves Luigi and attached tools entirely native: full head/hat, complete authored arms, original materials and joint pools. The scope restores its prior state on every exit. Main-eye floating gloves, accepted fingers, shadow correction, pickup policy and stable left-eye desktop preview remain unchanged. Reflection arms use authored game poses; tracked full-arm IK remains pending. Removed checkpoint 15's call-stack tracing; a bounded reflection-prepass confirmation remains.

Windows build and all three CTest suites pass, including native reflection-pass routing versus normal/ghost passes. PAL synthetic smoke checks normal first-person gloves and shadow behavior and confirms the native reflection prepass retains the full avatar. The exact uncovered mirror appearance still needs a live checkpoint 16 check. Frozen base-game manifest checked separately.


## Checkpoint 17 - Short startle presentation and reflected VR heading

User confirmed checkpoint 16 restores the mirror's full model. New feedback: reflected Luigi does not follow joystick VR turning; the ceiling-light trap briefly locks movement, releases it before the falling light, and should show the small authored reaction on the stereo screen.

Reflections now preserve all native head/hat/sleeve geometry and authored hand poses while applying the same measured-root-to-VR-heading body alignment as the main avatar. Attached equipment receives heading alignment in the reflection too; reflected nozzle poses remain native instead of using floating tracked gloves. The completed pose is scoped/restored exactly as main/shadow draws. This adds turning without introducing full-arm tracked IK.

The native message-2 startle path on the user's disc requests controller state 2/substate 3 with reaction kind 5 (controller +0x330). Only that combination selects the new short-reaction stereo presentation; damage variants, ordinary movement, fear/panic modes, dialogue and pickups do not select it. Tape capture records reaction state, and first-person presentation yields to a stereo screen for its actual lifetime, returning when the native state ends. The full authored model draws during the reaction. No fixed camera timer, trap damage, simulation timing or input-lock changes are introduced. Existing pickup presentation is retained.

Windows build and three CTest suites pass, including exclusion checks for unrelated reaction states. PAL offscreen validation exercises native mirror heading with complete model geometry. Scripted native message-2 reactions entered controller 2/3 with motion 13 and selected the screen flag; after the reaction ended, controller 1/0 cleared that flag immediately. The probe exited normally. Exact falling-light framing and the reflected heading still require the live headset pass. Frozen base-game manifest checked separately.


## Checkpoint 18 - Optional short green forearms

User accepted checkpoint 17 mirror turning and native startle presentation, and requested short green forearms. Added a default-on F1 > VR "Short green forearms" checkbox, persisted at the end of vr.ini for backward-compatible loading. Turning it off restores the accepted floating-glove rendering. Tracking calibration, fingers, eye position, head-relative movement and equipment aiming are unchanged.

Uses the user's native green shirt/forearm geometry in shared shape 4. The packet filter now removes upper-arm/shoulder portions while retaining tracked forearm portions. Each native forearm joint already shares its glove's rigid skin frame; retained mixed forearm envelopes are also rebased to that frame after native skinning so no vertices can stretch back toward body-anchored upper arms. Per-hand tracking validity controls sleeve visibility. No shoulder attachment or full-arm IK is introduced. Sleeve endpoints use the native mesh cut; no new fade shader is included.

The rigid sleeves participate in the tracked shadow pass. Reflections retain the complete native head/arms with accepted VR heading alignment. Pickup and startle stereo-screen poses retain their full authored animation.

Validation: rebuilt the Windows executable and avatar test executable; all three CTest suites pass. Added a shared-mesh filter regression proving torso triangles remain identical, forearms survive the new mode and disappear in glove-only mode. PAL synthetic avatar smoke exited normally, logged five rigid forearm envelopes, and measured zero cuff-skin residual for both hands. Render inspected from the native-camera dump; headset fit/reach/appearance still requires the live pass. Frozen base-game files checked separately.


## Checkpoint 19 - Tracked full avatar in mirrors

User accepts optional short forearms and requests reflected movements/hands while retaining the complete head and arms. Added default-on F1 > VR "Tracked mirror avatar", with authored mirror poses as its fallback. First-person gloves, finger calibration, short sleeve rendering, camera, equipment gameplay, and existing pickup/startle policy remain unchanged.

Only the native reflection prepass uses the new full-arm solver. It takes the same left/right grip poses and five finger curls as the local gloves. Shoulder recruitment stays on the clavicle sphere and is capped; arm lengths extend at most 1.4x for reach. A consistent elbow bend plane controls sleeve roll. Extreme reflected wrist reaches clamp to the arm's reachable endpoint to keep anatomy connected; the local-eye gloves remain exact controller targets. The white cuffs remain rigid while the reflected green upper/forearms retain the connected IK skin. Lost tracking yields that hand to its native pose.

The live host publishes recentered headset pitch/roll with physical yaw removed, since the accepted body alignment already includes full horizontal headset and joystick heading. The mirror applies that tilt about the native neck pivot to head branch 51..58, preserving face/hat geometry and facial animation. Joint selection was verified from the user's own rig topology/inverse binds. The temporary private rig trace was removed. Native tracked equipment/holster effects remain a separate unfinished task; this change animates the reflected avatar only.

Validation: rebuilt main and avatar-test executables; three CTest suites pass. New tests cover near/overhead/extreme reflected reaches on both sides, shoulder displacement, bounded arm extension, and rigid head rotation about a fixed neck pivot. PAL synthetic native reflection run checks tracked wrist alignment and the full reflection path, alongside accepted glove/cuff and shadow checks. Headset elbow appearance, reflected finger gestures and head tilt still require the live check. Frozen base-game manifest checked separately.


## Checkpoint 20 - First hand-directed flashlight/Poltergust aiming build

User requested the next milestone after the tracked mirror avatar. Added default-on F1 > VR "Hand-directed flashlight and Poltergust". The right controller now determines the held tool's world-space emission point, aiming direction and roll, using the runtime aim pose when available and grip pose otherwise. It shares the accepted glove eye/reference anchor, virtual heading and hand fit offsets. Muzzle setback from grip is a provisional 0.12 m forward and still needs live handle-fit verification. Disabling the option or losing right grip tracking falls back to accepted headset aiming. Normal ownership, trigger mapping, native movement/control locks, menus, pickups and startle policy remain intact.

The flashlight hook temporarily overrides origin and direction inputs before the original native beam/ghost test, restoring the source vectors after the call. Disabled light updates are left alone. The suction-record hook updates the shared origin/axis and endpoint, translates the native vertical bound by the emission-height change, and updates the direction pointer used by caught/fired objects. The nozzle/flashlight mesh receives the matching rigid position/orientation correction, including reflected/shadow draws. Backpack and Game Boy Horror remain attached to the body. Avatar hands/fingers/short forearms are unchanged.

Controls remain native: point with the right controller; right trigger vacuums and left trigger uses the selected element/air behavior when available. This is the first motion-aiming milestone, not completed waist/shoulder equipment acquisition, independent two-hand tool inventory, physical keys or door knobs. Element VFX and particular captured-object behaviors still need live coverage.

Validation: Windows build and rebuilt avatar test suite pass all three CTest checks. New tests cover world-space controller aiming with yaw/pitch, glove-anchor fit, metre conversion, rigid tool orientation/roll and invalid inputs. A PAL diagnostic run with a moving/rotating simulated controller exercised the real native flashlight and suction hooks. Active flashlight output origin/direction residual and suction origin/direction/endpoint residual were zero; the native source ray continued to update independently. The isolated run exited normally. An earlier short probe only reached disabled intro flashlight frames; the final probe explicitly waited for an active beam. Actual hand/tool fit, ghost stun/capture, vertical aiming, tracking-loss fallback and element visuals require the headset test. Frozen base-game manifest checked separately.


## Checkpoint 21 - First waist-belt equipment build and live candle encounter

User accepted hand aiming and requested waist equipment with grip/hover selection, plus uninterrupted VR during the upstairs parlour ghost reveal after extinguishing the candles (after training).

Default-on F1 > VR > "Grip equipment from waist belt". Flashlight is on the left hip and the Poltergust nozzle on the right. Either hand can hover for a gold highlight and light haptic pulse, then hold grip to equip. Release grip to return the tool. A single native nozzle means one active tool at a time; a second grip cannot steal or duplicate it. The vacuum becomes available only after the native ownership bit is set. Holding the flashlight enables its native beam; holstering disables it. The held vacuum hand's trigger drives suction, and the opposite trigger retains native element/air use. Both runtime aim poses are now available, so tool emission and the model follow whichever hand holds it. The backpack stays on Luigi. Disabling belt mode restores checkpoint 20 trigger/right-hand aiming behavior.

The visible belt uses original, compact procedural 3D holster markers, projected separately into each eye. They share the hand reference space, follow horizontal head/body heading and head translation, and do not pitch with your gaze. They are a first reach/selection prototype, rendered as an overlay without scene depth occlusion; they are not finished asset-quality holsters and are not included in mirrors. The equipped tool retains its native model. The idle native flashlight moves to the left holster anchor. Physical keys, Game Boy Horror pickup, shoulder retrieval, hand-to-hand transfer and door knobs are not implemented here. Existing Game Boy Horror buttons remain available.

The identified candle/portrait reveal is event61. On first-person launch its archive is rebuilt in host memory from the user's mounted disc. Only camera/control ownership commands are blanked: MENUOFF, LUIGISTOP2, HLIGHTON, the initial Luigi STOP/WAIT0 pose, and the unique ima_demo.cmn camera path. The original timed speech, waits, portrait lighting, event flags (including final ghost-enable flag 91) and end commands remain at their original byte positions. No disc/install/save is rewritten. Its timeline still runs before enabling the ghosts, but movement and headset view remain active. This applies when the encounter archive is freshly loaded; a process already inside that event must restart/reload. Other story cinematics and accepted item-get/trap presentations remain authored.

Ordinary ghost-origin startle messages during free first-person gameplay are acknowledged without entering the native biku pose/control lock. Source validation uses the original actor slot or exact actor-position pointer, and native ghost type flag 8. Unknown sources, traps, damage messages and scripted story scenes continue through their original handlers.

Validation: Windows executable and avatar tests rebuilt. All three CTest checks pass. New cases cover belt reach after translation/90-degree yaw, inventory/hover exclusion, one-active-tool ownership, and encounter script fail-closed behavior. The production transformation was also run against both owned PAL and USA event61 scripts: byte length, timed messages/waits, lighting and all flag lines were preserved. An isolated PAL native tool smoke exited normally and retained zero flashlight/suction origin/direction residuals. This is not a headset verification of belt reach/visibility, live grip acquisition, or the modified encounter's playable progression. Those need the next live pass. Frozen base-game manifest still verifies all 14 files; out/vr remains untouched.


## Checkpoint 22 - Native handle attachment and holding-hand activation

User reports belt acquisition works, but tool placement does not look naturally held, and requests activation from the holding controller. Replaced the held mesh's muzzle-based placement with a rigid attachment from Luigi's original right wrist/palm frame to the selected tracked glove frame. This uses the same outward calibration, wrist position, eye anchor, virtual heading and fit offsets as the glove renderer. It works for either holding hand without changing accepted glove/finger calibration. The native tool's handle-to-muzzle relationship and model scale remain intact; the previously arbitrary 0.12 m muzzle placement remains only for legacy/non-gripped fallback. Native flashlight/suction origins and axes use the corresponding rigid attachment, with source rays captured before overrides. Exact retail rig validation protects alternate models.

Holding grip carries either tool; the holding hand's trigger now activates the flashlight as well as the vacuum. Releasing the trigger turns off the beam/suction while continuing to carry. The opposite hand's trigger still invokes an available Poltergust element. Pure trigger routing validates tracking, inventory ownership through the existing grab state, and clamps finite analog inputs. Belt-mode off retains the previous control scheme. Holster return, candle event61 live gameplay, short forearms and authored item-get/trap views remain unchanged.

Validation: main and avatar-test executables rebuilt; three CTest checks pass. Added both-hand trigger ownership/tracking-loss cases and native-wrist-to-calibrated-glove attachment checks preserving handle/muzzle distance and orientation under pitch and handed calibration. The isolated PAL native motion-tool smoke exited normally with zero flashlight/suction residuals; it uses the existing diagnostic controller path and does not verify live belt handle fit. Actual left/right grip appearance and activation require the headset pass. Frozen base game remains separately verified.

Next requested interactions: wrist-twist door handles and two-hand furniture shaking. Feasible through reach/grip/gesture adapters to native door-lock and shake logic; not implemented in checkpoint22. Fully hand-driven door/furniture physics is separate from triggering native interactions.


## Checkpoint 23 - Corrected held-tool forward and shared muzzle attachment

User confirms holding-hand triggers work, reports both held tools face backwards, and reports suction appears offset from the nozzle tip. Added a tool-only 180-degree yaw correction after the accepted hand calibration. Glove/finger tracking and trigger behavior are unchanged.

Replaced per-update wrist reconstruction with a per-tool native attachment captured from an untouched main opaque draw (never a shadow/reflection). The saved attachment includes grip-from-model, native emission point and emission axis in grip space. The visual model and gameplay ray now use the same current tracked grip transform and the same captured attachment. This removes guest-update versus model-draw animation drift. Attachments are separated for flashlight/vacuum, reset on player change and recalibrated when the native model changes. Model scale and native handle/muzzle separation remain intact. Legacy non-gripped/headset-aim fallback remains available. One initial draw can still use the legacy placement before calibration exists.

Validation: rebuilt Windows main and avatar-test targets; three CTest suites pass. New both-hand tests verify the tool frame reverses forward while retaining wrist position, and a muzzle transformed through the held model agrees with the physics-ray point under nontrivial native/model/controller rotations. Existing trigger ownership tests pass. An isolated PAL native smoke checks rendering/beam/suction hook regressions; diagnostic tool aiming bypasses live belt tracking, so actual orientation and suction-to-visible-tip alignment still require headset testing. Frozen base files remain separately verified.


## Checkpoint 24 - Restored camera, configurable hip/shoulder holsters, Game Boy grip

User rejected checkpoint23 held fit/orientation and requested the original camera and equipment layout. Restored the first-person eye-forward default from 0.08 m to its original 0.20 m, retaining 1.80 m eye height and unchanged room scale/projection. No vr.ini existed in the live profile at installation, so the new default applies; F1 still permits adjustment.

Default holsters: flashlight on right hip; Poltergust over right shoulder; Game Boy Horror on left hip (slightly forward to distinguish it when both hip devices are assigned to one side). Independent F1 > VR checkboxes select flashlight left hip, Poltergust left shoulder and Game Boy Horror left hip/right hip. Options append to the existing positional vr.ini format and preserve older configs. Changing a side cancels active grabs and requires grip release. The shoulder uses the existing backpack as its visual; hover haptics remain active. Hip slots retain prototype 3D markers.

Game Boy Horror availability follows native equipment bit8. Grip its hip slot to open the native map through the existing latched map action; no separate open button is required. Normal map/back controls still close it; this is a holster gesture opening the existing interface, not a physical tracked handheld display. Device grabs are excluded from tool aiming and flashlight/vacuum activation. Ownership and native menu/event locks remain in force. Keys and door/furniture gestures are still pending.

Removed the rejected fixed 180-degree prop flip. The source attachment now uses the native emission axis as forward instead of assuming that the wrist's finger axis matches the tool barrel. Held prop matrices are captured in grip space from the untouched authored draw and reused rigidly under the current tracked grip, avoiding later native animation offsets being added to the held prop. Emission and the rendered model share this grip space. Original native matrices are restored afterward. Glove/finger calibration and holding-controller triggers remain unchanged.

Validation: main and avatar-test executables rebuilt; all three CTest suites pass. Added default/swapped holster-side, shoulder reach, Game Boy target acquisition, and native-axis-to-controller-forward cases for both hands. The PAL native smoke now explicitly exercises the shared held-model/emission path under a moving synthetic controller (previous probes bypassed this live attachment path), exits normally, and reports zero native flashlight/suction output residuals. Native-camera dump inspected; headset fit, true tool-tip appearance, shoulder reach and Game Boy grip opening still require live validation. Frozen base files remain verified; out/vr untouched.


## Checkpoint 25 - Dual tools, wider grabs and motion-aimed GBH search prototype

User reports sideways tool placement, insufficient time to swap tools in combat, small grab zones and non-working GBH grip. Independent one-of-each ownership now allows a flashlight and vacuum in different hands. Each tool selects its own controller pose and trigger. Simultaneous flashlight/vacuum activation is routed separately into the native light/stun and suction systems; the opposite trigger only acts as an element trigger when that hand is empty, preventing the flashlight trigger also spraying an element. Duplicate ownership of the same tool is still blocked.

The game has one retyped equipment actor. Native flashlight/vacuum models are warmed and captured separately, with a small second S3D draw instance using owned matrix/cursor storage and the user's resident model. It is drawn through the existing native opaque/translucent paths with separate interpolation identity. Extra drawing is recursion guarded, and model pointers/attachments invalidate with the player/map equipment context. No second gameplay actor, inventory bit or new game asset is introduced. Full dual-tool shadow/reflection coverage and combat behavior remain live-test work.

Tool fit now reads the actual retail three-joint model's visible handle/lip region at runtime and aligns that rendered handle, rather than preserving the offset between animated Luigi and an equipment actor. The native attachment table uses a palm joint; the tracked palm distance positions the handle within the glove. Captured model matrices remain rigid under the selected hand. The lip point and native emission use the same grip-space attachment. Glove calibration, restored 0.20 m camera and model scale are unchanged. Visible fit still needs headset confirmation.

Flashlight and GBH hip acquisition radii are 0.30 m; shoulder vacuum radius is 0.34 m. Hover entry now pulses at 0.35 amplitude for 60 ms, with existing success feedback. Each hand excludes only a duplicate tool from hover/grab selection; holding the other tool does not remove the second slot's feedback. Side settings remain independent in F1.

GBH grip now requests native search camera (X in GameCube mapping) instead of the floor-plan map. A device-held session retains controller tracking through the native panel transition. The camera update hook retains native logic, then supplies the holding controller's world eye/look/up. The same hand's trigger sends inspect/confirm; releasing grip sends back/close. Equipment bit8 still gates the slot. Existing buttons remain available. This is a motion-aimed native search-interface prototype, NOT a finished tracked handheld screen displaying a separate live camera texture inside the surrounding VR room. That physical-display rendering remains unfinished.

Validation: main/avatar targets rebuilt; all three CTest suites pass, including simultaneous ownership/independent triggers, tracking loss, duplicate prevention and acquisition 25 cm outside the previous hip zone. An isolated PAL run logs capture of both native models and independent second-model draws, exercises held geometry/emission, reports zero native flashlight/suction output residuals and closes normally. Native-camera dumps inspected. Two camera probes did not reach the native search view through scripted input and were normally closed; the GBH camera hook, grip transition and inspection target accuracy are NOT claimed runtime-verified. Actual grip reliability, prop fit, dual stun/capture timing, GBH motion search and release-to-close require the headset pass. Frozen base/diorama outputs remain separate.

Checkpoint25 fit follow-up: the tool handle stays at the calibrated grip/palm point while the barrel uses the runtime aim orientation (grip orientation fallback when no aim pose exists). Aim-space translation is deliberately not used as the handle position. Tests verify aim pitch rotates the barrel without moving the grip centre. This restores the runtime-directed barrel behavior that worked before handle fitting was added.

In active holster mode VR trigger routing replaces native gamepad L/R values, preventing a SteamVR controller also exposed as a gamepad from activating the other weapon or an element. Turning holsters off restores the prior gamepad trigger fallback.


## Checkpoint 26 - Authored tool grip and visible-nozzle suction correction

User screenshot shows the tool body intersecting the glove. User confirms the mirror sheet fails with both left-stick counter-pull and a physical controller pull. Simultaneous tools, independent triggers and expanded grab zones were accepted; preserve those behaviors.

Replaced bounding-extremum handle fitting with the original right glove wrist/palm/up attachment frame and the same calibrated controller wrist used to draw the glove. This retains the authored handle offset instead of placing the outer tool shell at the palm. The tracked glove calibration, camera and tool sizes remain unchanged. The tool's physical barrel direction is derived from this authored attachment; headset fit/direction require checking.

Local inspection of both owned retail tool models confirms all draw packets load matrix 2. Lip extraction previously used parent matrix 1, which is a different animated transform. It now uses the actual rendered mesh matrix 2 for the shared visible-tip/gameplay emission attachment. Native suction records now reference owned VR ray-vector storage instead of overwriting borrowed native part vectors; the native builder refreshes those record references every update. Existing native tug rules remain in charge. This is a candidate correction for the sheet problem, not proof of successful sheet removal. Physical pull-to-tug input has not yet been implemented.

The requested hip-mounted 3D Game Boy Horror with a handheld live camera display is still unfinished. Its current native search interface remains the checkpoint25 prototype. This checkpoint does not claim that feature is complete.

Validation: main target rebuilt; all three CTest suites pass; isolated PAL synthetic dual-tool smoke passes and exits normally (57.5 seconds), with zero suction-ray/endpoint and flashlight-emission residuals. Frozen baseline verification passes for all14 files. Installed only to out/first-person. Headset grip fit and actual mirror-sheet removal remain unverified.


## Checkpoint 27 - Restore barrel aiming and route native effects to the held tip

Checkpoint26 user test: both tools face backward, suction effect is displaced, but the mirror sheet now pulls free. Treat sheet behavior as provisional until tools are correctly aligned. The prior Game Boy Horror search-screen prototype still does not satisfy the physical device request.

Restored runtime aim orientation for held barrels while retaining calibrated grip/palm placement. The narrow handle frame and mouth are computed from the actual mesh-2 transform; authored wrist axes no longer determine barrel orientation. A shared pure-math helper and composed transform tests verify mesh +Y becomes controller -Z aim and that the rendered mouth equals the gameplay emission point for both hands with differing grip/aim rotations. The checkpoint26 owned suction record storage is retained.

Added native nozzle position/direction getter adapters, limited to the current player's held vacuum. They return owned vectors at the same held mesh lip used for suction. This reaches consumers that previously kept reading original body-attached effect vectors even though the suction record had moved. Both tool meshes must be calibrated before getter redirection starts, so warmup uses untouched native sources. US getter functions are retained; PAL metadata was matched against the user's owned disc by unique exact12-byte signatures (US77408/77414 -> PAL7A290/7A29C). No copyrighted bytes are stored in the source.

Synthetic fixture activation now explicitly simulates its held flashlight trigger, mirroring live holster activation instead of depending on an incidental native enabled flag. The fixture must reach both model captures and the redirected native getters as well as the flashlight output check before it can pass. The first ungated-getter run did not complete calibration; a subsequent warmup-gated run captured both models but did not activate the native flashlight incidentally. Both runs were normally closed. These failures are not counted as successful end-to-end validation.

Game Boy Horror physical model/live-screen work remains unfinished. Located the owned retail model archive at model/gameboy.szp. The requested display needs its own camera render target inside a tracked 3D device while the main room continues in first person; reusing the current full-screen OpenXR panel does not meet that requirement. Do not describe the current screen as a completed handheld device.


Owned-vector validation follow-up: the HLE heap starts at 0x81900000, beyond retail MEM1. Beam and suction checks must accept the exact owned VR vectors in addition to retail vectors. They now do so through toolVectorValid; arbitrary host-window addresses remain rejected. Without this, source redirection made subsequent beam/suction updates skip their valid owned pointers. The final smoke criterion includes both model captures and the native source getter redirect, avoiding early success from an initial beam update before calibration.

Final checkpoint27 validation: main and avatar test targets build; all three CTest suites pass. The strengthened isolated PAL smoke reaches both native model captures, independent second-model drawing, redirected source getters and zero beam/suction output residuals, then closes normally. Real controller fit, visible suction particles and sheet removal require the headset check. Verified all14 frozen baseline files; installed only out/first-person.

## Checkpoint 28 - Tool handle clearance within the closed glove
User confirms sheet removal works in checkpoint27. Mirror screenshot shows handles embedded in palm geometry. Added a 0.055 m palm-normal clearance toward the curled fingers, applied in the calibrated glove frame to both tools. This changes only the shared attachment translation; controller aim orientation and all checkpoint27 suction/source routing are retained. F1 > VR > Tool palm clearance (m) permits live fit adjustment from 0 to 0.15 m, persisted as an appended optional vr.ini field; older settings retain the new default. Glove tracking and body/mirror IK remain unchanged. Tests check translation without barrel rotation or wrist tracking changes. Visible fit remains subject to headset confirmation. Physical GBH display remains unfinished.


## Checkpoint 29 - Measured closed-glove contact and separate handle/muzzle

User requests correct defaults rather than clearance tuning, with fingers around the handle behind the front and activation at the tip. Checkpoint27 sheet pulling is accepted; preserve its source routing.

Replaced wrist-to-palm-distance plus guessed normal clearance with a measured three-dimensional contact point per hand. The current user's resident Luigi bind matrices supply the wrist, palm and middle-finger chain. The same glove coordinate frame and finger-curl function used by rendering form a stable 80%-closed grip; the finger joint arc's circumcentre supplies the handle contact in glove space, including lateral position, depth and reach. Calibration is cached with the player/map equipment context. This does not reposition the tracked wrists, alter accepted glove yaw or rotate the barrels away from runtime aim. It does not follow trigger-dependent finger motion, avoiding tool wobble during use.

Each tool's source contact is measured on its full narrow cylindrical stem, behind the front shoulder by half the measured glove width plus the shaft radius, separately from its front lip. The mesh2 handle is mapped to the measured glove opening. The cached front lip continues to feed native flashlight, suction and native effect sources through the same held transform. The old palm-clearance slider is removed; its old trailing settings field is consumed for compatibility but ignored, so checkpoint28 values do not offset the new default. No user fitting step is required.

Validation includes contact-frame grip/aim separation, left/right lateral contact, finger-centre translation invariance and existing mouth/render alignment checks. Main and test targets build; all three CTest suites pass. Frozen baseline verification passes all14 files. Actual glove/handle contact still requires visual headset confirmation; this is not a claim that mesh-level finger collision or a fully adaptive cylindrical grasp solver is implemented. The physical Game Boy Horror/live screen remains unfinished.


Checkpoint29 screenshot follow-up before installation: the prior Y>=60 fitting window excluded most of the actual long stem, which spans approximately Y=0..69 in the owned retail meshes. That window remains suitable for locating the front lip, but not the handle. Stem fitting now profiles narrow-radius rings across its full length and places the contact behind the front shoulder with room for the glove, instead of using the near-bell neck. The two newly supplied mirror images still show checkpoint28; they are not a test of checkpoint29. Local measured glove contacts are left (2.610,-4.958,-15.257) and right (-2.317,-4.956,-15.238) in native units, providing both palm depth and forward/lateral grip placement. No native model vertex data is copied into source.

Final checkpoint29 native PAL smoke passes and closes normally (57.5 seconds), including both mesh captures and redirected native nozzle sources. All three CTest suites pass. Installed only out/first-person. Default visible handle fit and actual tip alignment remain subject to headset verification.



## Checkpoint 30 - Tool grasp, hooked-ghost follow and physical counter-pull

User accepts improved handle placement but reports an unnatural claw/pinch. While a flashlight or vacuum is held, all fingers now use a cylindrical grasp: thumb0.72, index0.78..0.88 following trigger, middle0.8 (same as the measured contact arc), ring0.82, little0.84. This prevents an unpressed trigger or resting thumb sensor from leaving the index/thumb spread around an otherwise closed grip. Releasing equipment restores the existing sensed finger tracking. Wrist tracking, anatomical contact and mesh handle/lip fitting remain as checkpoint29. No claim of full finger mesh collision or adaptive grasp IK.

The native capture-update hook fn80081BCC publishes a focus only for a living hooked ghost assigned to player0 while that player holds/activates the vacuum. Native stun/acquisition, health drain, capture strength, escape conditions and difficulty values remain unchanged. The render-side focus expires after180ms without refresh and is scoped to the equipment context and active grip/trigger. A tracking option gently changes tracking-rig yaw only, with an8-degree dead zone,45-degree/second cap, and bounded long-frame step. It retains the head yaw reference at capture start, so ordinary headset turning remains available; there is no forced pitch. A manual snap turn suspends follow for750ms. Follow stops when no capture focus is active; it does not snap back to an earlier heading.

Physical counter-pull is a separate option. At the start of an active hooked-ghost struggle it records the holding controller relative to the HMD. Subsequent horizontal hand displacement is converted into ordinary head-relative native stick input (4cm dead zone, full input at16cm). Direction is preserved, including wrong-way movement: there is no automatic opposite-direction correction or strength boost. Room-scale head translation is accounted for in the relative measurement, and virtual follow/snap rotation alone cannot create a hand stroke. Left VR stick input takes priority. Suction release, tracking loss, a menu/context change, or focus expiry resets the gesture. This prototype applies only to hooked ghosts; furniture/sheet pulling retains its native thumbstick control.

F1 > VR exposes Track hooked ghost during capture and Physical Poltergust counter-pull separately, both initially enabled for the requested test, persisted as optional trailing fields after the retired clearance field. Existing settings are compatible. Neutral flat/base builds do not publish or apply the VR capture focus. Frozen base and accepted diorama outputs remain untouched.

Validation: main/avatar targets build; all three CTest suites pass, including held-grasp index motion and stable wrapping, shortest-path yaw, dead zone/rate/long-frame bounds, physical dead zone/full retreat, wrong-way motion and head-relative conversion. An isolated native PAL rendering fixture is used for tool/source regressions. That flat synthetic fixture does not validate actual headset comfort, live ghost target detection, or controller tug effectiveness; these require the next real capture test.

Final checkpoint30 isolated PAL tool smoke passes and closes normally (56.5 seconds), reaching both tool captures and redirected native sources. Installed only out/first-person; actual capture follow/physical pull are unverified pending headset testing.


## Checkpoint 31/32 — capture split and interaction migration

Corrected captured-actor validation (ownership +0x950 belongs to its AI object, not the actor). Capture camera following is bounded horizontal turning, only while stick-back is held; manual snap pauses assistance. Native stick resistance and opposed physical controller strokes now have separate roles. The native capture wrapper gates HP drain without allowing the final HP tick to complete while blocked. Synthetic encounter attempts did not reach a valid hook, so actual capture health gating remains unverified.

Checkpoint 32 adds the default optional inventory toolbar, first-person authored-frame hiding, and tested world gesture primitives. Physical door/key/chest/furniture adapters and the physical Game Boy Horror display are unfinished and not enabled. See VR_WORLD_INTERACTIONS.md for exact status and combat design ideas. No balance modifiers were enabled.


## Checkpoint 33 — native ordinary-door opening prototype

Added an off-by-default F1 option for physical ordinary wooden doors. Knob contacts come from the native animator node matrices, not guessed world offsets. Empty-hand grip plus a wrist twist starts the existing in-room door opening animation without a button injection or cinema. Room links, native lock/busy/seal flags, model type and node counts are validated. Hover and completion have stereo markers and queued render-thread haptics. The occupied hand is excluded while the other hand remains usable. Disabling the option schedules native closing only for leaves this adapter opened, with map/table lifetime checks.

PAL and USA synthetic contact/twist tests completed native opening and recovery closing. All three CTest checks pass. Manual room-crossing tests did not complete a transition; no claim of fully manual doors yet. Keys, chests, furniture, physical Game Boy Horror and local-only hose suppression remain unfinished.

## Checkpoint 34

Extended the experimental wooden-door adapter with a second physical grip/twist to close adapter-opened leaves using the native reverse animation. PAL and USA native cycles passed (79.6 / 81.6 seconds, normal exits); software camera/avatar/registration tests passed. Manual doorway traversal remains unfinished after additional native collision/alignment experiments. Other requested world adapters remain in progress; see VR_WORLD_INTERACTIONS.md.

## Checkpoint 35 — headset toolbar visibility repair

The inventory toolbar and hover feedback were drawn into the desktop ImGui pass, while OpenXR copied the game-only render texture. This left invisible toolbar grab targets in the headset, with the native hip/shoulder models still visible. Aurora patch 0227 now composites game and per-eye ImGui into a separate native-resolution XR source on the render worker, including offscreen right-eye submissions. The desktop preview and game EFB remain separate. UI framebuffer scaling is restored after this pass.

Toolbar flashlight is now on the right, Game Boy Horror on the left. Slots are 22 cm apart with 10.5 cm reach, preserving non-overlap while widening activation. Camera/avatar/hook checks pass. Live headset visibility and gripping need confirmation.

## Checkpoint 36 — belt reach, order and stowed models

Moved the toolbar from 58 cm to 70 cm below the recentered head and from 38 cm to 23 cm forward. Slots from left to right are item/key, Game Boy Horror, flashlight, Poltergust; identities and ownership remain unchanged. Adjacent reach volumes remain non-overlapping. The item slot remains reserved and unavailable until the native key/item adapter is complete.

Replaced generic glyphs with colored equipment silhouettes: a lens/barrel flashlight, Poltergust pack/hose/nozzle, and the Game Boy Horror case/screen/buttons. Highlight and haptics remain tied to the same grab targets.

During active local first-person equipment mode, native stowed nozzle, backpack and Game Boy Horror mesh draws are omitted. Model calibration/update continues so later held and dual-tool rendering still works. Held tools remain visible. Explicit reflection passes, reflected camera bases, shadow passes and authored pickup/startle/cinema presentation retain native draws. No equipment ownership or saved progression is changed. Headset reach and mirror visibility need user confirmation.

## Checkpoint 37 — three-slot belt and shoulder vacuum

Raised the default belt by 20 cm from checkpoint 36, to 50 cm below the tracked head, with 23 cm front reach. Only item, Game Boy Horror and flashlight remain on the belt (left to right); Poltergust uses a 34 cm shoulder zone. F1 > VR's existing left-shoulder option applies in toolbar mode too. Added persisted F1 belt height/front-distance controls and reset. The item slot remains reserved until its native adapter is ready.

Removed the toolbar's stacked marker boxes. Each equipment silhouette and its highlighted card now share exactly one projected center, tied to its grab contact.

Reflection equipment uses native model poses plus the same body-heading correction as Luigi. Controller aiming/contact offsets and dual-tool clones are excluded from reflection passes. With equipment mode enabled, reflected arms/fingers retain the native animation instead of tracked wrist IK, so native held tools and hands share an authored pose. First-person held tool motion is unchanged. Mirrors show the game's native active tool, not a second VR-only clone when both tools are held. Native mirror fit and shoulder/belt reach await headset confirmation. Camera/avatar/hook checks pass.

## Checkpoint 38 — model holsters and independent ghost pulls

Removed the toolbar, belt markers, reserved key box and toolbar controls. Existing ini layout fields remain readable for compatibility but toolbar mode is retired. New defaults are flashlight left hip, Game Boy Horror right hip and Poltergust right shoulder. F1 > VR retains a side checkbox for each tool. An ini schema marker migrates old side defaults once while preserving subsequently saved choices.

Native stowed equipment meshes are visible again. Flashlight uses its cached model at the matching hip mount, including when the native nozzle has switched to vacuum. Game Boy Horror's mesh center follows the selected hip. Mirrors retain the checkpoint 37 native arm/equipment pose. First-person tracked hand/tool aiming remains active while held.

Replaced the single shared ghost pull state with sixteen independent capture tracks. Alternating native capture callbacks no longer erase each other's stroke or repeatedly change camera focus. The focus stays on a fresh primary ghost until it releases/expires. Each ghost retains its own observed escape direction, stroke distance and 160 ms native-tick permission window. A 1.5 cm opposing stroke at over 6 cm/s inside a 60-degree opposition cone opens that ghost's window; wrong-way/sideways travel, stale capture, tracking loss or a long frame gap deny it. Holding still expires the window. Direction changes greater than 60 degrees reset that ghost's accumulated stroke/window.

Only the hand holding and activating Poltergust contributes controller movement, relative to tracked head translation. Stick-back is separate resistance/camera assistance, not a second prerequisite for damage permission. Native damage cadence, native ghost behavior and final-HP handling remain in place. A queued pulse in the Poltergust hand confirms actual native HP drain, rate limited to 90 ms. Multiple ghosts moving in different directions require different opposing strokes; they do not receive blanket damage from one permitted target.

Software regressions cover interleaved two/three-ghost updates, stable focus, independent directions, wrong-way motion, stationary timeout, direction changes, release, stale tracks, long frame gaps and reset, plus the new default/swapped holster sides. Native multi-ghost gameplay still needs headset confirmation.

Checkpoint 38 handle calibration now projects the authored closed-hand contact onto the actual mesh shaft, with finite/range/proximity checks and the earlier bounded fallback for incompatible poses. PAL diagnostic measurements were Y=26.32 for both tools, compared with previous guessed contacts of Y=61.87 (vacuum) and Y=37.29 (flashlight). This contact is independent of the front lip used for light/suction. The projection math is regression tested under translated, rotated and scaled matrices; native synthetic smoke checks emission alignment, not headset comfort. Hose deformation and complete manual doorway traversal remain unfinished.

## Checkpoint 39 - native hose and doorway traversal

The live Poltergust hose is rendered through a separate J3D model managed by Luigi +0x7D8, manager +0x18, with fifteen rope points at +0x4 (stride 0x1C) and its model at +0x5C. Earlier S3D-only traces did not reach it. A draw scope retargets the authored curve between the body-aligned pack connector and held nozzle attachment, rebuilds the original native rib/skin matrices using fn_8008A504, then restores rope points, pose matrices, envelope matrices and envelope flags. Full reflection and cinematic/pickup presentations retain the native hose. The native simulation never consumes modified points. Curve generation uses endpoint tangents, bounded slack sag and arc-length resampling, with finite/range checks and native fallback. Controller tracking/attachment loss also falls back to native drawing.

PAL native synthetic-tool smoke exercised the adapter and all fifteen ribs, exiting normally in 106.8 seconds. Software tests check exact endpoints, even spacing, reflected curves, invalid/degenerate input and excessive reach. This proves the adapter runs; hose appearance, comfort, left/right reach and reflection continuity still need headset testing.

Physical doorknobs now have a narrow native collision adapter and a player/hand-clearance close tracker; see VR_DOOR_ADAPTER.md. Keep the F1 experimental door option off until intentionally testing supported unlocked wooden doors. Locked/key doors retain the flat stereo presentation.

The final door fixtures passed 42 -> 41 -> 42 plus native closing on both owned-disc versions (USA 30.5 s, PAL 30.4 s, normal exits), after adding a validated threshold ground contact. The first one-way test had missed a fall through the floor gap; that was caught and corrected by the return fixture. Software camera/avatar/hook-registration checks pass. Headset checks are now needed for hose appearance and supported-doorknob reach/auto-close comfort. No headset preview was launched.

## Checkpoint 40 - grip space and separate local/reflected arms

Removed capture-follow rig rotation and its F1 control. The legacy ini field is consumed but ignored and saved as zero; native stick resistance and per-ghost motion damage remain independent.

Held tool contacts now use the calibrated controller grip orientation shared with gloves. Their native authored rotation relative to the original holding hand is retained, while measured shaft contact and emission lip remain separate. Runtime aim-space rotation no longer rotates the model away from the glove.

First-person short green sleeves use a detached 16 cm forearm transform pointing back along the tracked wrist. Direct lower-arm joints and mixed skin envelopes use the same transform; white cuff envelopes remain pinned to the glove. The tracked wrist is not clamped to the shoulder. Mirrors retain connected clavicle/arm IK, including while holsters are enabled. Reflected held tools and the hose use the reflected wrist contact after IK, and the second held tool can appear in the reflection. Stowed Game Boy Horror follows its selected hip in reflection passes too. Native cinematic/pickup presentations remain separate.

Software tests cover grip/aim orientation separation, authored rotation preservation, detached wrist/end alignment, and prior camera/avatar behavior. The isolated PAL synthetic mirror-pose fixture reached connected IK and held-tool draw scopes, exiting normally in 74.6 seconds. This exercises the pose adapter; actual reflected appearance, GBH stow fit and controller comfort still require headset review.
