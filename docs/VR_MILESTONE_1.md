# First VR milestone

Primary user test target: Steam Frame through SteamVR. Aim for other PC-connected
headsets through OpenXR; each runtime/headset combination needs testing. Standalone
headset execution is outside this Windows PC milestone.

## Required experience

- Select miniature diorama or life-size original third-person camera framing.
- Both modes render actual stereo geometry with tracked head pose.
- Diorama exposes scale and viewing distance; both support recentering.
- Preserve Xbox controls and PAL simulation speed; never tick gameplay per eye.
- Handle camera cuts/room changes comfortably and validate HUD readability.
- Keep the frozen PAL-fixed fallback and isolate VR settings, cards and caches.

## Current evidence (2026-10-20)

- `base-game/` contains 14 files copied from the working Desktop distribution,
  including the PAL-fixed executable, runtime DLLs and bundled LLVM compiler.
  No disc images or saves were copied. This is a frozen executable distribution,
  not an independent copy of the user's game assets.
- `tools/verify-base.ps1` checks SHA256 values in `docs/base-game-manifest.json`.
  All 14 files passed after copying. Do not build or package into this directory.
- `tools/launch-isolated.ps1 -Profile base` starts that fallback with its own
  profile. Default `-Profile vr` targets `out/vr/LuigisMansionRecomp.exe`, which
  has not been produced yet. Both set child-process APPDATA, LM_CARD_DIR and
  LM_CACHE_DIR. Their first launch needs disc setup; regular saves are not copied.
- Installed active OpenXR runtime registry entry points to SteamVR's
  `steamxr_win64.json`. This proves runtime registration, not headset availability.
- Aurora owns a Dawn/WebGPU device and window surface in
  `extern/aurora/lib/webgpu/gpu.cpp`. Its public webgpu interface currently
  exposes window surface creation, not external eye-image submission.
- `src/recomp/interp.cpp` records/replays game frames and restores GX state.
  Investigate replaying one simulation sample for two eye views. Texture copies,
  shadow passes, subviews and readbacks must remain consistent between eyes.

## Next technical gates

1. Probe OpenXR loader, extensions, HMD system and recommended stereo views.
2. Verify graphics interop against the project's pinned Dawn headers/library:
   native D3D12 device/queue access and external swapchain image handling.
   Select the runtime-required adapter before creating Aurora's device.
3. Add an opt-in session with explicit failure messages and intact flat-screen
   startup. Obtain predicted display time/poses once per submitted XR frame.
4. Render one real room twice with asymmetric eye projections and correct head
   transforms, without advancing simulation twice. Validate on the headset.
5. Implement both scale modes, recentering, UI and transition comfort, then test
   PAL door transitions and regression-check the regular flat-screen path.

Reference: Khronos OpenXR specification requires querying graphics requirements
and using the returned adapter LUID/minimum feature level for a D3D12 session:
https://registry.khronos.org/OpenXR/specs/1.1-khr/html/xrspec.html

No stereo rendering or headset session is implemented yet. Source inspection
does not prove that the existing Dawn binary exports the native interop needed.

## Capability probe and profile isolation

`tools/probe-openxr.py --loader <absolute-path-to-openxr_loader.dll>` performs
extension enumeration and attempts instance/system/stereo-view discovery without
creating a graphics session. The installed SteamVR loader enumerated extensions
successfully, including D3D12, D3D11 and Vulkan. Instance creation returned
`XR_ERROR_RUNTIME_FAILURE` (-2) both inside and outside the sandbox; no headset
system or stereo sizes were obtained. Do not infer a disconnected headset from
that error alone. Runtime logs included a missing VTE client session ID.

The pinned Dawn package headers provide `GetD3D12Device`,
`GetD3D12CommandQueue`, `RequestAdapterOptionsLUID`, and
`SharedTextureMemoryD3D12ResourceDescriptor`. Their presence supports investigating
native D3D12 interop, but does not yet prove binary exports, synchronization or
successful OpenXR swapchain import.

`LM_VR_BUILD` defaults ON in this development checkout and selects a distinct
`Luigi's Mansion VR` config/data/cache directory even when launching the executable
directly. `-DLM_VR_BUILD=OFF` retains the upstream PC profile name. The isolated
launcher additionally places cards/caches under this project's own profiles tree.

Both profile variants were compiled directly from `src/port/install.cpp` with a
small scratch harness. Config/data/cache paths and install.ini save/load passed
under scratch APPDATA for both PC and VR variants. This is not a full-game test.
The preserved Dawn DLL's export table also confirms native D3D12 device/queue,
adapter LUID and resource-descriptor exports. Actual swapchain import remains
untested. The first full configure failed on sandbox DNS downloading Dawn; a
network-enabled retry was started, with output in ignored `build-first-vr.log`.

## Verified headset session checkpoint

After the user connected the headset and started SteamVR, OpenXR instance and
HMD discovery passed. Both recommended stereo views were 2644 x 2644, samples 1.
The network-enabled full application build then completed successfully.

Added a reusable Windows OpenXR D3D12 session backend in `src/vr/` and a small
`lmvr-session-probe` target. Against the connected headset it successfully created
a session on the runtime-required adapter, created both eye swapchains (DXGI
format 29), ran 30 frames with valid tracked poses, and acquired/released all
60 eye images. The probe submitted zero composition layers; no game imagery was
displayed and no Dawn eye-image import was tested.

Added camera math in `src/recomp/vr_camera.*` for recenter-relative eye poses,
life-size and miniature scales, viewing distance and asymmetric GX projections.
Numerical checks cover clip planes, frustum edges, 64 mm stereo disparity at
two metres, rotated recentering and miniature placement. They passed in a direct
compiler run. CMake target: `lmvr-camera-test`; CTest name: `vr-camera`.
Game-unit scale remains configurable in the math and needs geometric calibration.
The camera target also passed its registered CTest (`vr-camera`, 1/1 passed).

The session backend and camera math are not yet connected to game presentation.
Next: integrate native graphics access/eye output with Aurora, apply eye transforms
to appropriate replay commands, and validate one real room on the headset.

## Verified Dawn/OpenXR image transfer

`src/vr/dawn_eye_output.*` implements a reusable eye-output bridge: a native
D3D12 intermediate with simultaneous-access/render-target flags is imported into
Dawn, receives a sampled game image, then is copied on Dawn's own native queue to
an acquired OpenXR eye image. The eye is returned to RENDER_TARGET state before
release. A CPU fence wait conservatively protects initial resource reuse; this
will need profiling before claiming comfortable headset performance.

The `lmvr-dawn-probe` executable selects the runtime's adapter, creates a Dawn
D3D12 device, runs this bridge for 20 eye copies over 10 tracked frames, and reads
back a central pixel from both headset eye resources. It verified the expected
test color within two byte values and reported zero Dawn validation errors.
It submitted no visible composition layers. This proves the image-transfer path,
not stereo geometry, game rendering, or headset comfort.

Two integration details were verified during the probe: Dawn needs its native
`allow_unsafe_apis` toggle (already used by Aurora) to expose texture import;
SteamVR returns typeless eye resources, which must be compared by compatible
DXGI format family rather than requiring exact equality with its chosen view
format. The full development application also rebuilt successfully after adding
the camera math sources.

Next concrete integration points:

- Before Aurora initialization, prepare OpenXR and select the required adapter
  LUID for its D3D12 device; request SharedTextureMemoryD3D12Resource.
- Attach the OpenXR session to Aurora's native device/queue, and use its completed
  `present_source()` as the bridge's source after render-worker synchronization.
- Preserve one selected interpolation sample for both eye replays. Apply eye
  transforms to the main scene's position/normal matrices and perspective
  projection; account for lights, shadow receivers, CPU vertices and subviews.
- Never call `frame::hostFrame()` once per eye. Submit both views at one predicted
  display time. Keep direct boot frames on the desktop until taped replay exists.

## Integrated preview and user feedback

The packaged `out/vr/LuigisMansionRecomp.exe` now connects the session and image
bridge to the game loop. Aurora selects the OpenXR-required D3D12 adapter through
patch 0224 and enables native texture import. The host selects one interpolation
sample, renders each eye from that same sample, then submits a projection layer.
The guest simulation is not advanced per eye. A VR tab selects miniature/life-size
views; miniature controls include scale, distance and vertical placement, persisted
in the isolated profile's vr.ini. F6 recenters. PAL modules were rebuilt from the
user's discs under the VR profile; ordinary game data/saves were not modified.

The first interactive preview reached real PAL gameplay. The user reported a small
room high and far away, and extremely close scripted scenes, supported by attached
headset screenshots. This does NOT satisfy the milestone's framing or comfort
requirements. The initial title ran around 90 submitted frames/sec; gameplay
fell much lower (around 13 at points), despite 30 game logic frames/sec. Rendering
and transfer timing instrumentation is now present to isolate that bottleneck.

Updated preview changes: title/file screens and script-controlled cutscenes retain
their original camera and appear on an OpenXR quad 1.6 metres ahead, 1.4 metres wide.
Cutscene state is sampled on the guest thread and stored with each tape. Gameplay
continues to use actual stereo geometry. Default miniature scale is now 0.10,
extra viewing distance 0.5 m, and height offset -0.20 m. Transitions/recentering
have a short fade-in. The first updated relaunch was prevented by an automatic
approval-review usage failure, so that version was not tested at that time.

After the user restarted SteamVR and asked to continue, the cutscene routing change
was built and packaged, then launched successfully (PAL, separate profile). SQLite
cache initialization now succeeds; the prior missing cache directory was created.
Visual feedback on this updated preview is pending. This remains experimental:
both modes, stereo fidelity, camera transitions, UI, door regression, flat-screen
regression and sustained headset performance still require verification.

While that interactive preview remains open, a follow-up build removes duplicate
game rendering for quad-panel screens (one panel texture serves both eyes) and
makes recentering preserve physical up using heading/position only. The upright
recenter math passed the rebuilt camera test. These follow-up changes are built
separately and have not yet replaced the running packaged preview. Its timing logs
confirm that game logic remains around 30 Hz while complex views can render much
slower; rendering synchronization is the larger measured cost than eye transfer.

## Stereo cinema revision

User feedback on the second preview: cutscenes should retain video-like framing
with stereo depth, door animations still zoomed excessively/mismatched stereo,
and gameplay remained too distant and low quality. A new preview preserves the
original cinematic projections, applies a restrained camera-space eye separation
with an off-axis convergence correction, and submits two coincident OpenXR quad
layers with LEFT/RIGHT visibility and independent images. Flat menu panels still
reuse one image. SubView-tagged event cameras (including door close-ups) now route
through cinema presentation even when the ordinary cutscene detector misses them.
The cinema projection test verifies unchanged framing at convergence and correct
near/far disparity ordering. It passes with the camera test suite.

The default miniature depth setback is now -0.25 m instead of +0.5 m (negative
offsets bring it closer). Automatic VR internal resolution now selects 3x 480p
instead of window resolution; explicit user resolution choices remain respected.
Upright recentering and single-render flat panels are included. This revision was
built, packaged after closing the old test process normally, and launched against
the existing isolated profile. Runtime logs confirm stereo cinema/world transitions
without reported OpenXR submission failures. Visual comfort, quality and the door
animation remain subject to the user's pending test feedback; no milestone
completion is claimed. The baseline's 14 files still pass their hash manifest.

## Pause, training, dialogue, and rendering-cost revision

User feedback: door/cinematic framing is now acceptable; the in-game Game Boy
Horror pause view remained oversized, training lacked depth, ordinary NPC dialogue
switched away from the surrounding room, and wall/frame pieces obstructed viewing.

The next interactive build captures the Game Boy Horror search/framed-screen state
using the existing co-op code's verified holder/screen fields and routes that UI to
a flat panel. A per-event dialogue latch distinguishes ordinary prompts from
cinematics. Training map 3 remains a tracked world, including during tutorial
events; a fallback selects its most-used scene camera even if the conservative
interpolator's eight-matrix threshold leaves mainView unset. Presentation regression
cases for training, NPC dialogue, pause, doors and title screens pass in the camera
test executable. Runtime checks of those exact cases are pending user feedback.

A significant rendering-cost mismatch was found: the VR presentation override
reported 90 fps, but GX accuracy selection checked the stored 30 fps setting and
therefore used the expensive per-draw early-Z split rather than the existing merged
replay path. It now uses frame::targetFps(). Sampled render-worker sync time dropped
from roughly 40+ ms to around 2 ms; sampled submissions varied around 45-90 fps.
This is improved, not proof of sustained headset performance. Console copy filters
are disabled on VR startup to reduce softness. The actual eye source logged in
this test is 3200x2400; further quality work must consider original texture detail,
screen-space copies and temporal stability rather than assuming the source remains
1280x960.

Room-node tracing matched local disc metadata and the guest renderer: the foyer
has a separate flag-0x80, three-batch group at the upper room boundary. A follow-up
build adds a reversible "Hide authored ceiling/frame pieces" VR option. It temporarily
sets only those static-room nodes' own part counts to zero during traversal and
restores them afterward; child traversal, actors, furniture, collision and assets
remain intact. Other walls are not generally removed. This cutaway build compiles
but is not yet installed over the running interactive preview or visually verified.

Independent format reference consulted for node layout (not copied implementation):
https://github.com/magcius/noclip.website/blob/main/src/LuigisMansion/bin.ts

### HUD alignment revision (headset feedback)
The preview now maps main-pass orthographic projections onto a common 1.4 by 1.05 m head-relative plane at 1.6 m, accounting for asymmetric eye frusta and physical eye offsets. Fixed game-frame GX matrix loads are retained for stereo transformation without interpolation. Continue/Quit frames without room geometry use the original framed panel. Dialogue portraits no longer select cinema for the whole room; dedicated closeups can select cinema in training too. All presentation changes begin a fade at zero gain, including cinema changes. The authored frame cutaway is included as an experimental toggle.
Build and vr-camera checks pass; frozen baseline verification passes all 14 files. Visual results for pause, dialogue, ghost health and transitions still require the next headset test. This is a candidate fix, not a completed validation.

Preview 5 feedback: Continue/Quit fixed; gameplay appeared zoomed out with GBH in lower right. Restricted HUD remapping to orthographic projections with recorded UI draw users and no recorded non-UI users, to avoid resizing full-screen EFB image passes. Build and camera test pass. Needs headset validation; pause image/scene sampling remains provisional.

Preview 6 feedback: room and Continue/Quit good; idle GBH follows head, NPC bubble text absent, ghost HP doubled, doors lack apparent stereo. Next candidate tags the whole VR HUD draw, allows font projections inside text groups to share the bubble mapping, and remaps associated scissor rectangles after interpolation. The idle GBH bezel is suppressed except while text/calls/search need it. Cinematic half-eye baseline increased from 0.75 to 3.2 game units. Build, vr-camera test, and 14-file baseline verification pass. All four visual changes await headset validation; room/camera options are unchanged.

Preview 7 feedback: substantial visual improvement; room and pause framing accepted, text readable. Remaining comfort requests: HUD should stay at recentered reference rather than follow head; speech bubbles sometimes too high; training GBH bezel remains during/between professor prompts.
Next candidate anchors the HUD centre at a fixed recentered reference 1.6 m away, 12 cm below eye level, with a 1.2 x 0.9 m layout. The existing orthographic layout uses a local affine approximation at that reference (centre and local scale tracked; this is not a fully perspective-correct rotated quad at the periphery). Tests verify asymmetric-eye agreement, stationary centre under yaw, recenter and hiding an anchor behind the viewer. Room transforms/options are unchanged. Training suppresses the bezel even when professor call mode remains latched; search retains it. Build, camera tests and 14-file baseline verification pass; headset comfort and training text/portrait continuity await validation.

Preview 8 feedback: room/HUD comfort accepted. Game Boy Horror map's 3D area/floor label misframed, and NPC introductions briefly select a panel. Next candidate detects actual map-renderer draws (opaque and translucent passes), keeps that menu in original framing on the menu panel, replaces the no-room-means-pause heuristic with a Start-requested pause latch, and guards phone/dialogue framed flags during events. Message-window draw evidence latches dialogue before its first ANYKEY prompt. Main-game scripted cameras remain tracked world unless a dedicated closeup is present; dedicated door closeups and separate opening scenes keep stereo cinema. Added US tags for the two existing PAL map hooks. Diagnostic view flags: 1 panel/menu requested, 2 map drawn, 4 room drawn, 8 cinematic event, 16 dialogue. Build, vr-camera tests and 14 baseline hashes pass. Actual map/Continue-Quit/NPC intro checks still need headset validation.

Preview 9 feedback: map should have stereo, not be flat; some animations/locked-door images too close or doubled. Key-carrying ghost's world depth is preferred but too near. Follow-up request explicitly asks weaker animation stereo, including Luigi approaching the mansion.
Next candidate gives mapScreen its own StereoPanel presentation before pause selection. Its map draw passes are tagged with their own camera; orthographic 3D map matrices receive a convergence-depth shear, while 2D menu labels retain their framing. Perspective cinema uses median captured model-view origin depth as convergence and bounds eye separation against the nearest recorded near clip/focal scale (max 0.06 NDC inter-eye difference); this is an estimate, not a guarantee about every unrecorded/inherited camera. Scripted tracked-world shots use at least 0.8 m camera setback in diorama and 0.8 m additional setback at life size. Animation depth defaults to 0.6, adjustable F1 > VR and appended compatibly to vr.ini; it scales eye separation in cinema and scripted worlds, not ordinary gameplay or map stereo. Added regression checks for bounded near/far disparity, game-unit invariance, orthographic stereo focus/depth and scripted setback in both view modes. Build/test/baseline verification pass. Map/door/intro visual results await headset testing.

Preview 10 restart feedback: 60% animation depth too weak; requested 75%. Mansion entrance room establishing shot obstructed by framing; Y-button floor-plan map still flat. Next candidate defaults animation depth to 0.75. Scripted camera setback now uses the median captured actor origin depth and moves only shots whose target would be nearer than 0.8 m; wide room shots retain normal placement (regression checked). Missing actor samples preserve normal camera placement. Y's framed GBH interface selects stereo even when map passes reuse a cached image and no map hook fires. Map orthographic geometry is identified by model/world categories in addition to own-camera scope, and its position-matrix projection links are captured for stereo shear. Map diagnostics now report near/ortho/focus/sample count. This cached-map detection is not yet proof the displayed map texture has independent depth; headset verification and render diagnostics are still required. Build, camera tests and all14 frozen baseline hashes pass.

Preview 11 feedback: request 85%; event-screen changes still bad stereo; GBH floor plan and search camera flat; room tour passes through framing/blackout geometry. Preview 12 defaults animation depth to 0.85 and restores authored projection/camera framing for non-dialogue cinematic shots (training remains tracked, ordinary text dialogue remains tracked). Shadow-pass matrices/projections remain authored. Menu stereo now uses captured geometry depth with a half-distance margin instead of blindly limiting against a much nearer unrelated camera clip; orthographic map retains its separate shear. Non-map animation baseline is capped at 3.2 units before the strength slider.
Added Aurora patch 0225: independent right-eye GX history slots 8..11, copies/generations 4..7, expanded EFB RAM history and byte-buffer generation keys. Temporary replay-mask slots4..7 stay reserved. Interp records each eye's end state/copy generation separately; simulation still advances only once per frame. CMake patch glob now detects newly added patch files; patch content is LF for git apply. Reviewed slot/index bounds and all generation masks; camera test and 14 baseline hashes pass. Previous preview11 executable checkpoint in out/checkpoints/preview11 for local rollback. Runtime/headset transition, map/search stereo, authored-tour framing and NPC-continuity checks remain pending; do not consider this milestone complete.

Preview 12 feedback: overall depth accepted as good enough; scene transitions/NPC lead-in remain perceived flat, GBH search camera still flat. Xbox B opens the GBH camera (Y opens floor plan). Requested NPC text slightly above Luigi, and reported upper foyer black while coins remain visible from bottom of stairs; visibility should respect what the game reveals.
Preview 13 raises message-window layout by default0.18m using the same offset for projection/text/borders/scissor, scoped to message group80044138; health/HUD anchors and depth unchanged. F1 VR Dialogue lift allows adjustment, compatible appended vr.ini field. Added default-on Conservative room visibility toggle: existing portal hook retains original traversal and closed-door check but uses the valid parent rectangle for open passages, avoiding original flat-camera room-scissor clipping. Actual geometry/depth still occludes; no global reveal, door/load/collision/asset edits. This is a candidate fix for missing upper foyer geometry, not confirmation about all black meshes. Build passes. Previous accepted preview12 executable checkpoint saved. Need headset speech height, foyer top floor, closed-door occlusion and performance checks. Flat NPC lead-ins/transitions and GBH B-camera stereo remain outstanding; milestone remains active.


## First-person foyer continuity, build 61

Headset feedback rejected the temporary camera-node flag bypass; removed. Runtime trace identified foyer room 2 downstairs and landing room 31: downstairs native model alpha decreases through pass 2 to zero, then disappears from the submitted room list after the camera transition completes. The room remains loaded.

The candidate remembers the lower foyer static model during its normal draw and validates its model type and node-table identity before drawing it with the original native opaque/translucent functions on the landing. It applies only in first person, map 2, room 31 after native transition state returns to zero. The cache clears outside the two foyer rooms; closed rooms, collision, assets, and diorama rendering are unchanged. Entering directly upstairs without a preceding downstairs draw is not covered yet. This is one verified room pair, not a general solution for every multi-floor room. Visual confirmation pending.

The isolated PAL retention fixture exercised the native landing after the camera fade and logged the supplementary downstairs draw; normal exit in 51.6 seconds. This confirms code-path execution, not headset appearance. Camera/avatar/HLE checks and 14 frozen baseline hashes pass.


## Foyer camera-fade continuity, build 62

User confirmed the lower foyer stays visible upstairs in build 61, but noted a brief fade while crossing floors. The candidate also replaces the native partial-alpha floor pass with native opaque/translucent draws while foyer transition state is 1 or 2. It is restricted to the cached, identity-validated lower/upper foyer models in first person; it replaces that fade submission rather than drawing duplicate fade geometry. Diorama and all other room/cinema fades are unchanged. Headset confirmation pending.


## Diorama event continuity, build 64

In diorama, main-game tapes containing static room geometry retain tracked-world presentation through cinematic/close-up lead-ins rather than switching to a stereo panel before dialogue latches. Pause/map interfaces and separate opening/demo scenes keep their panels. Diorama camera cuts and scripted-world flag changes no longer trigger an added host fade. The accepted close-shot setback remains active. Added presentation regressions cover NPC lead-in, pause precedence and separate demo framing; camera/avatar/HLE checks pass. Toad/save-menu headset sequence pending. First-person presentation and room-wall behavior unchanged.
