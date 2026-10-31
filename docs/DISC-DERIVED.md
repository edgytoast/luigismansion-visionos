# What's made from your disc, and why it's never here

Luigi's Mansion VR runs the game's own code, recompiled from your disc. That code, and everything
else made from your disc, is built on your Mac and stays there. None of it is in this repository,
none of it is ever committed, and there's no prebuilt app to download.

## Made from your disc

| What | Where it stays | What it is |
| --- | --- | --- |
| `main.dol` | `.scratch/disc/main.dol` | The game's executable, copied out of your disc image by `visionos/scripts/extract-dol.py`. |
| The recompiled game | `.scratch/recomp-GLME01/` (`func_table.cpp`, `funcs.h`, `funcs_000.cpp` to `funcs_031.cpp`) | About 11,000 of the game's functions turned into C++ by `lmrecompile`. The first line of each file says it was generated from your `main.dol` and must not be committed. |
| The game framework | `.scratch/build-visionos-*/LuigisMansionVisionGame.framework` | The recompiled game compiled for visionOS, with aurora, SDL and the VR mod. |
| The app | `.scratch/xcode/Products/` | `LuigisMansionVision.app`, with the framework inside. It's for your headset only: don't share it. |
| Your disc image, on the headset | The app's Documents folder | The game's art, sound and data, read straight from your image while you play. |
| Shaders, on the headset | The app's Library/Caches | Built from the game's draws on the first start. |
| Saves and settings, on the headset | Documents › Luigi's Mansion VR | Your memory cards, the game's settings and the VR settings. |

The app also bundles `data/pipelines.bin`, aurora's list of GPU pipeline states to prepare ahead of
time. It's part of Luigi's Mansion Recompiled's own repository (compressed render-state keys, no game
code, art or sound), not something made from your disc.

The other build products (the recompiler for your Mac in `.scratch/host`, the CMake builds in
`.scratch/build-*` and Dawn in `.scratch/dawn`) are made from this repository's source and what
the build downloads.

## How it stays out

- Everything above lives under `.scratch/` (or on the headset), and `.gitignore` ignores `.scratch/`
  along with every disc and game-file format (`*.iso`, `*.gcm`, `*.rvz`, `*.ciso`, `*.wbfs`,
  `*.dol`, `*.rel`, `*.arc`, `*.szp`) and any `recomp_gen/` folder.
- The recompilation's own `.gitignore` keeps out disc images, extracted game folders, `main.dol`
  and its build folders too.
- Before each release, every file in every new commit is checked for disc and game-file formats,
  the recompiler's output, any run of bytes from the game's disc or `main.dol`, build products, and
  large binaries.

If you fork this repository, keep it that way: never commit `.scratch/`, a disc image, a `main.dol`
or anything generated from one, and never publish a built app.
