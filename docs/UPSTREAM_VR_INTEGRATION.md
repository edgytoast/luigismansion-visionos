# VR integration test — upstream v1.1.4, build 65

Separate checkout based on release commit 850a2b9a7613678f27d29d47e2bb11c74d2e0557. VR behavior preserved through checkpoint 64. Current fallback executable and source tree were not replaced.

Preserved: first-person, miniature diorama and life-size; independent mode profiles; F1 VR controls, fixed menu placement and controller dot/slider drag; hand calibration, cuff closure, physical tools/holster settings; native vacuum startup locomotion; foyer downstairs continuity; scripted framing and speech handling. The small foyer fade and diorama dialogue zoom jitter remain known issues.

Integration: resolved 15 text-conflicting files; combined upstream interpolation/particle changes with independent stereo capture/replay. Rebased VR renderer patches after upstream patches as 0240–0245. Shared map hooks now call the upstream map/mirror adapter and keep the VR map-draw marker. Adopted the upstream non-overlapping host allocation window via our HLE address helpers. Kept VR configuration folders separate and disabled profile migration from the stock game. Stock release installation is disabled for the custom VR executable.

Validation so far: complete Windows executable build; 437 unique HLE registrations; VR camera/avatar tests pass; freshly compiled PAL module; synthetic glove/cuff rendering passes and normal exit in 61.0 seconds; native foyer retention/fade paths pass and normal exit in 47.6 seconds. Original 14-file frozen baseline remains unchanged. Mode INI snapshots were copied byte-for-byte from the Desktop fallback. This is not complete headset gameplay verification.

Upstream dependency unit tests were not built in the game-only build; the initial unrestricted CTest listing reported those as Not Run, not assertion failures. The three built VR/HLE tests all pass. New upstream mirrored-world and Silly options are unverified in VR.

Headset validation: both modes must initialize and submit eye images; then user should check F1 interactions/settings, gamepad and VR input, held tools, stairs, NPC speech and save menus. Only promote after acceptance; keep the fallback.

Headset startup checks completed on Steam Frame/SteamVR: first person submitted stereo frames and independent eye-1 replay; normal close; diorama submitted mode-0 stereo frames and was left open for user testing. The first PowerShell monitor could not read the live redirected log due to its file-sharing mode; direct Python reads confirmed the submissions, so that monitor timeout was not a runtime failure. Gameplay/F1 acceptance is pending.
