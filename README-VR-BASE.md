# Luigi's Mansion VR development base

This is a local development checkout of
https://github.com/rob646464/luigis-mansion-pc-port at
`db02360a39860dba673446ad964fc5569916e13e`, on branch `vr-base`.
The tested PAL memory fix is applied as an uncommitted, reviewable diff.
The upstream remote is retained. Nothing has been pushed upstream.

## Working baseline

- Keep PAL support and the port's existing content available.
- The HLE heap now lives at `0x81900000..0x81A00000`, outside the game's MEM1
  arena. The game receives its full 24 MiB arena. See `docs/RECOMPILER.md`.
- On the reported PAL door load, the old reservation caused a resource
  allocation to fail and the decompressor to overwrite the clock in low memory.
  The user confirmed normal game speed with the fix.
- The remaining persistent stop was resolved in the user's test by disabling
  F1 > Enhancements > General > **Pause when window loses focus**. This is a
  local user setting, not a change to the source default. A short loading hitch
  can still occur during the transition.
- The fixed flat-screen executable is installed in the user's existing Desktop
  game folder; its original executable was backed up there.

An OpenXR session backend and camera math are now implemented. A connected-headset
probe passed session creation, tracking and eye-image acquisition. They are not yet
connected to the game's renderer, so no game stereo imagery or motion controls are
available yet. See `docs/VR_MILESTONE_1.md` for current evidence.

## Build on this PC

Visual Studio 2022 C++ Build Tools, CMake and Ninja are installed on this PC.
The local `.tools/llvm` folder contains clang 22.1.3, lld-link and matching
compiler headers. It is ignored by Git. LLVM's license is included with it.
Aurora is checked out at the upstream-pinned submodule revision; CMake applies
the project's existing Aurora patches during configuration.

From PowerShell in this folder:

```powershell
powershell -ExecutionPolicy Bypass -File tools/setup-openxr-headers.ps1
powershell -ExecutionPolicy Bypass -File tools/build-local.ps1 -Configure
```

This builds `build/windows-clang/LuigisMansionRecomp.exe`. CMake downloads the
declared third-party build dependencies if they are not already present.
For a packaged development build with its runtime libraries and bundled compiler:

```powershell
cmake --install build/windows-clang --prefix out/vr
```

Run `tools/launch-isolated.ps1 -Profile vr`. On first launch it can ask
for the user's discs and build the game modules. Do not commit disc images,
extracted game data, generated game modules, saves or build output.

The local toolchain is a convenience for this machine, not part of Git history.
Other machines need a complete compatible LLVM installation with development
headers; the end-user port's minimal bundled compiler alone lacks those headers.

## Start VR work

Read `docs/VR_BASE.md` first. Preserve this flat-screen behavior while adding
an optional VR path. Choose the headset/runtime and the intended camera mode
before implementing rendering and input.

Primary target is Steam Frame through SteamVR; both diorama and life-size
third-person modes are planned. `LM_VR_BUILD` uses a separate profile name by
default. The frozen `base-game/` copy remains the PAL-fixed fallback; use
`tools/verify-base.ps1` to check it and `tools/launch-isolated.ps1 -Profile base`
to launch it with its own profile. It needs disc setup independently.

## First-person experiment

See docs/FIRST_PERSON_VR.md for implemented behavior, validation and remaining interaction work. The experimental executable is installed separately in out/first-person; the accepted out/vr build remains available.
