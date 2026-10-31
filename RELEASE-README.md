# Luigi's Mansion VR - build 150

Default: 3D Diorama. Windows x64 PC VR preview.
Based on Luigi's Mansion Recompiled v1.1.4:
https://github.com/rob646464/luigis-mansion-recompiled
Upstream commit: 850a2b9a7613678f27d29d47e2bb11c74d2e0557.
Credit to the upstream recompilation authors, encounter/Aurora, lm-decomp,
Dusklight/JSystem, Khronos OpenXR and other third-party contributors.
Retain the included licenses and notices when redistributing.

## Setup
1. Extract the entire ZIP to a writable local folder. Keep all files together.
2. Install/start SteamVR, connect the headset, and set SteamVR as the active
   OpenXR runtime in SteamVR settings.
3. Launch **Luigi's Mansion VR.exe**. On first use, Browse/Install asks for your
   own European/PAL Luigi's Mansion disc image (GLMP01). Select its real location;
   it does not have to be named specially or placed in GameFiles.
4. Allow the bundled compiler and shader preparation to finish. Game code/assets
   are generated/read locally from your own game, never included in this ZIP.
5. Face forward and press F6, or release a right-stick click, to recenter Diorama.

Flat-screen play: launch the same EXE with **--flat** as its command-line/Steam
launch option. Flat mode does not need SteamVR. No first-person launcher is shipped;
Life-size and experimental First person remain available under F1 > VR.
This is not a Quest APK or a supported native standalone Steam Frame build.
Steam Frame standalone through Proton remains experimental; PCVR streaming works.
SteamVR itself is installed separately and is not bundled.

## Features
- Tracked stereo miniature Diorama, adjustable scene scale and distance.
- Stereo story/boss/item/door scenes, room tours and scene-specific visibility fixes.
- Comfortable menus/dialogue, adjustable HUD, separate settings for each VR mode.
- Upstream v1.1.4 enhancements and Silly options; map element-source markers.
- Four shareable room checkpoints, including progress and current element supply.
- Stable desktop preview and controller-accessible F1 settings.
- Experimental First person with tracked Luigi gloves, flashlight/Poltergust grips
  and per-tool side settings. It is unfinished and not the recommended default.

## Diorama controls
Original controller gameplay; motion-controller sticks/buttons also work.
Xbox defaults (remappable in F1 > Controls):
- Left stick: move. Right stick: original C-stick aim.
- A: interact/confirm. X: flashlight/original B/cancel.
- B: Game Boy Horror search camera. Y: map.
- Right trigger: Poltergust. Left trigger: original L action.
- Shoulder buttons: original Z/element action. Menu/View: pause.
- Right-stick click/release: recenter (unless checkpoint shortcuts are enabled).
- F6 always recenters VR. Both sticks clicked together: open/close F1.

Motion controllers: left stick moves, right stick aims, right trigger vacuums,
right A/B confirm/back. Steam Frame left D-pad down opens map and up opens search;
View opens pause. Touch/Index-style left face buttons supply map/search, with
left-stick click for pause. Runtime bindings can vary. Vive wand bindings are
not complete; other headset/controller combinations need testing.

## F1 menu
F1 or both stick clicks opens settings. Use the controller pointer dot and trigger
to click; hold trigger to drag sliders. Controller navigation and desktop mouse
are available. VR options select modes, recenter, scene scale/depth and HUD layout.
Controls contains right-stick vertical inversion. The footer shows build/mode.

## Experimental First person
Left stick moves relative to head direction; right stick turns. Grip near left hip
for flashlight, over right shoulder for Poltergust (sides configurable in F1).
Hold grip to carry, release to holster; holding hand's trigger activates that tool.
Both tools can be held. Aim physically. Ghost capture, tool alignment, mirror pose,
hose and manual object/door interaction are still experimental. Game Boy Horror
physical grabbing is disabled by default; normal map/search buttons remain.

## Share checkpoints for bug reports
F1 room checkpoint controls select one of four slots. Enable optional shortcuts:
left-stick click captures selected slot; right-stick click loads selected slot.
Disabling only removes those shortcuts. Both sticks still open F1; F6 recenters.
Files live in **statesaves** beside the launcher: PAL-1.bin to PAL-4.bin (or USA).
Capture before triggering the problem. Share the slot, build number, mode, relevant
F1 settings and reproduction steps. Recipient closes the game, backs up an occupied
slot, copies the file into statesaves, restarts, then loads it through F1.
Matching game region is required; matching VR builds are recommended.
These are room/progress checkpoints, not exact-moment snapshots of conversations,
animations or live ghosts. Normal saves remain separate; saving normally after a
checkpoint load stores the restored progress. No personal checkpoints are included.

## Contents / troubleshooting
Includes runtime EXE/DLLs, application OpenXR loader, compiler/linker, source-free
pipeline descriptions, VR settings, credits and licenses. No ROM, Nintendo assets,
extracted retail executable, generated game module, memory cards or source code.
A separate source ZIP accompanies this release.
Personal setup, saves/cache/logs are created under profile after launch.
For problems, include profile/logs/launcher-errors.log and launcher-output.log.
Do not send ROMs, assets, generated game modules or your entire profile in reports.
