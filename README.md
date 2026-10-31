# Luigi's Mansion VR - complete source snapshot, build 150

This is the complete authored source behind the companion playable build, including
Diorama, Life-size, experimental First person, F1 settings, HLE/recompiler/runtime,
checkpoint implementation, tests, build scripts and patched Aurora engine.
No retail game data, generated retail code, compiled game modules, personal saves,
Git metadata/history, private work directories or local compiler binaries are included.

Upstream: https://github.com/rob646464/luigis-mansion-recompiled
Base: v1.1.4, commit 850a2b9a7613678f27d29d47e2bb11c74d2e0557.
Aurora base: 50e5be30ec12cc72768bc564bc016b55e82be868, with this project's patches
already applied in extern/aurora. All patch files are included for reference.
See NOTICE.md, licenses, decomp/LICENSE, extern/aurora/LICENSE and
extern/openxr-sdk/LICENSE. The decomp/JSystem sources are CC0 implementations;
no code generated from a retail DOL is included. Retain third-party notices.

## Build on Windows
Install Visual Studio 2022 C++ Build Tools with a Windows SDK, compatible complete
LLVM (clang-cl, clang, lld-link and development headers; this build used LLVM 22),
CMake 3.25+ (presets require a compatible recent CMake), Ninja, Git and Python 3.
Put LLVM and Ninja on PATH. In PowerShell from this source folder:

    powershell -ExecutionPolicy Bypass -File tools/build.ps1 -Preset windows-clang -Configure -Target LuigisMansionRecomp
    cmake --build --preset windows-clang --target lmvr-camera-test lmvr-avatar-test lm-checkpoint-input-test
    ctest --test-dir build/windows-clang --output-on-failure
    cmake --install build/windows-clang --prefix out/runtime

CMake downloads the declared external build dependencies (SDL, Dawn, nod, etc.).
They are not game assets. This source ZIP is not an offline SDK/toolchain bundle.
Bundled OpenXR headers are used directly; no header download is needed.
No disc/DOL is needed to build the host application. First run asks the user to
supply their own supported PAL game copy, then compiles its module locally.

The root CMakeLists has only archive-specific adjustments: use vendored OpenXR
headers, and accept the already patched Aurora snapshot instead of running git
reset/patch commands. LMVR_PREPATCHED.txt verifies the patch set matches. The
runtime/game/VR source files match the working source byte for byte.
Do not initialize an Aurora submodule over the supplied patched engine tree.
If uploading to GitHub, it is a regular vendored source directory, not a submodule.

## Rebuild the native launcher
packaging/windows/vr-launcher.cpp is the complete Windows launcher source.
From a Visual Studio x64 developer prompt with clang-cl on PATH:

    clang-cl /std:c++20 /EHsc /O2 /MT packaging/windows/vr-launcher.cpp /Fe:out/runtime/LMVR-Launcher.exe /link /SUBSYSTEM:WINDOWS shell32.lib advapi32.lib user32.lib

Use the filename Luigi's Mansion VR.exe, place the installed runtime under bin,
and keep its llvm compiler directory and runtime DLLs. See RELEASE-README.md for
the shipped layout and user controls. The companion runtime ZIP already provides
all these files. No first-person launcher is required; select modes through F1.

## Development notes
Current scene decisions: docs/DIORAMA_PRESENTATION_HISTORY.md.
Older development-base/milestone notes are historical and may describe earlier
limitations; this README and build-info.json identify the current snapshot.
No source was committed or published by creating this archive.
