# Notice

What's in this repository, who made each part, and the terms it comes with.

| Part | Where | By | Terms |
| --- | --- | --- | --- |
| The visionOS port: the app, the launcher, the host that runs the game, the build scripts | `visionos/` | [Trevorbilt](https://trevorbilt.com) | MIT ([`visionos/LICENSE`](visionos/LICENSE)) |
| The visionOS OpenXR provider | `visionos/openxr-provider/` | [iChris4](https://github.com/iChris4/Wiicompiled_VR) (WiiCompiled VR), changed by Trevorbilt (listed in its README) | GPL-3.0-or-later ([`LICENSE`](visionos/openxr-provider/LICENSE)) |
| SMAA | `visionos/openxr-provider/src/smaa/` | Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando Navarro and Diego Gutierrez | MIT ([`LICENSE.txt`](visionos/openxr-provider/src/smaa/LICENSE.txt)) |
| TrevorbiltKit, the launcher shared by Trevorbilt's ports | `visionos/TrevorbiltKit/` | Trevorbilt | MIT; its fonts (Space Mono, Roboto) SIL OFL 1.1; the Trevorbilt name, badge and app icons stay Trevorbilt's ([`NOTICE.md`](visionos/TrevorbiltKit/Sources/TrevorbiltKit/Resources/Brand/NOTICE.md)) |
| Luigi's Mansion Recompiled, v1.1.4 and its history | the rest of the tree (`src/`, `tools/`, `include/`, `docs/`, `CMakeLists.txt` and so on) | [rob646464](https://github.com/rob646464/luigis-mansion-recompiled) | Shared publicly as source on GitHub, without a licence file |
| Luigi's Mansion VR, build 150 | the commit "LMVR build 150 by u/sonarix": the VR code in `src/vr/`, `src/recomp/` and `tools/`, aurora vendored with its patches, `packaging/` | [u/sonarix](https://www.reddit.com/user/sonarix) | Shared publicly as source, without a licence file, in [u/sonarix's release on r/virtualreality](https://www.reddit.com/r/virtualreality/comments/1xg3kvj/), where he says he doesn't plan to keep developing it |
| Trevorbilt's changes to the two above and to aurora | the commit "visionOS: the port's changes to the recompilation, the VR mod and aurora" | Trevorbilt | MIT, like the rest of his work here |
| lm-decomp | `decomp/` | [SomeJakeGuy and contributors](https://github.com/SomeJakeGuy/lm-decomp) | CC0 1.0 ([`decomp/LICENSE`](decomp/LICENSE)) |
| JSystem and OS code from Dusklight | `libs/JSystem/`, `include/global.h`, `include/helpers/`, `src/port/os/OSThread.cpp`, `OSMutex.cpp`, `OSContext.cpp` | [TwilitRealm](https://github.com/TwilitRealm/dusklight) | CC0 1.0 |
| aurora | `extern/aurora/` | [encounter](https://github.com/encounter/aurora) | MIT ([`extern/aurora/LICENSE`](extern/aurora/LICENSE)) |
| OpenXR SDK headers | `extern/openxr-sdk/` | the Khronos Group | Apache-2.0 ([`extern/openxr-sdk/LICENSE`](extern/openxr-sdk/LICENSE)) |

The build downloads [Dawn](https://github.com/encounter/dawn) (BSD-3-Clause),
[SDL](https://github.com/libsdl-org/SDL) 3.4.10 (zlib), [nod](https://github.com/encounter/nod)
(MIT or Apache-2.0) and aurora's other dependencies; [`licenses/`](licenses) has their licence
texts.

lm-decomp doesn't accept AI-assisted contributions. This port was written with AI assistance, so
please don't send any of it to lm-decomp.

*Luigi's Mansion* and its characters and art belong to Nintendo. Nothing from the game is in this
repository: the build recompiles the game's code from your own disc, on your Mac
([docs/DISC-DERIVED.md](docs/DISC-DERIVED.md)).
