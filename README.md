<p align="center">
  <a href="https://trevorbilt.com"><img src="https://trevorbilt.com/assets/logo-BdrEikAq.png" alt="Trevorbilt" width="240"></a>
</p>

<h1 align="center">Luigi's Mansion VR for Apple Vision Pro</h1>

<p align="center"><b>A haunted house, at coffee-table scale.</b></p>

<div align="center">

[![Listed in the AVP Ports Index](https://img.shields.io/badge/AVP_Ports_Index-listed-0A84FF)](https://github.com/edgytoast/avp-ports-index/blob/main/ports/luigis-mansion-vr.md)

</div>

*Luigi's Mansion* (2001) as a haunted diorama on Apple Vision Pro. The whole mansion stands in your
room as a miniature: lean in to peer through a doorway, crouch to see under the stairs, walk round
it while Luigi creeps through the Foyer with his flashlight. Or put it on your table in the shared
space, beside your other apps, and keep half an eye on the ghosts while you answer an email. It runs
natively on the headset: no PC, no streaming.

This is [Trevorbilt](https://trevorbilt.com)'s native visionOS port of Luigi's Mansion VR, the VR
mod by [u/sonarix](https://www.reddit.com/user/sonarix), which is built on rob646464's
[Luigi's Mansion Recompiled](https://github.com/rob646464/luigis-mansion-recompiled). Everything you
need to build it is in this repository. You bring the disc, you build the app, the ghosts take it
from there.

> [!IMPORTANT]
> There are no game files and no app download here. You need your own *Luigi's Mansion* disc (USA,
> GLME01) and a Mac to build with. Luigi's Mansion VR is a fan project. It isn't affiliated with or
> endorsed by Nintendo.

## What you get

- **Two ways to play**, picked on the launcher's Play tab before you press **Play**:
  - **Full**: the mansion as a miniature set in your room ("The mansion in my room", on by default),
    or in the dark if you turn that off. The set holds still while you move: it's placed when Luigi
    enters a room or the camera cuts, and if you wander far from it, it recentres behind a short
    fade. Cutscenes don't swing it around.
  - **On your table**: the same set as a model in a volume in the shared space, beside your other
    apps. The HUD and the text sit on a panel facing you, and up on the stick is always away from
    you, from whichever side you're looking.
- **Gamepad play.** An Xbox, PlayStation or other MFi gamepad, with the buttons where they sat on the
  GameCube pad. A gamepad is the only input the port supports.
- **The VR mod's own settings.** Click both sticks for the game's settings: the diorama's scale,
  distance and height, where the HUD sits, the controls, and the recompilation's enhancements. On
  your table they open on the panel.
- **Smoother motion on the table**, if you'd like to try it: an experimental option in the
  launcher's Advanced options, off by default, that shows Luigi and what moves around him at up to
  60 frames a second instead of the game's 30.
- **A pause that behaves.** Press the Digital Crown in Full and the launcher comes back with
  **Resume** and **Quit**.

## What you'll need

- **A headset:** Apple Vision Pro on visionOS 26 or later.
- **A Mac** with Apple silicon and **Xcode 27** (built and tested with Xcode 27.1) at
  `/Applications/Xcode.app`, with its visionOS platform installed.
- **Some tools:** [Homebrew](https://brew.sh), then `brew install cmake ninja xcodegen rustup`, plus
  Python 3 and a Rust nightly (the install guide has the two commands).
- **A gamepad** (Xbox, PlayStation or another MFi controller), paired with the headset. PS VR2 Sense
  controllers and bare hands aren't supported.
- **An Apple Account**, signed in to Xcode. A paid developer membership saves you reinstalling every
  7 days.
- **The game:** your own *Luigi's Mansion* disc image as a plain `.iso`, of the USA release (GLME01),
  the only one this port is built and tested with.
- **Patience:** about 10 GB of free disk, and about an hour for the first build (most of it is
  Dawn, the graphics layer, which later builds reuse).

## Build it

1. **Get the code.**

   ```bash
   git clone https://github.com/edgytoast/luigismansion-visionos.git luigismansion-visionos
   cd luigismansion-visionos
   ```

2. **Build and install, with your disc and your own team.**

   ```bash
   LMVR_DISC="$HOME/Games/Luigi's Mansion (USA).iso" \
     visionos/scripts/build-visionos.sh --team YOURTEAMID --install
   ```

   The build recompiles the game from your disc's `main.dol`, on your Mac, then builds the app and
   signs it with your team. Prefer Xcode? Leave off `--install`, then open
   `visionos/App/LuigisMansionVision.xcodeproj`, pick your Vision Pro and press Run.

[AVP-INSTALL.md](AVP-INSTALL.md) has every step, including how to check your disc, how to find your
team ID, and what to do when something goes sideways.

## Bring your game

AirDrop the same disc image to your Vision Pro and open it with **Luigi's Mansion VR**, or put it in
Files › On My Apple Vision Pro › Luigi's Mansion VR, or use **Import** in the app. Wait for the copy
to finish before you open the app. The app plays the release it was built from, and tells you if
you hand it another one.

The first start prepares about 3,000 shaders behind a progress panel. Later starts skip that.

## Play

| | Gamepad |
| --- | --- |
| Move | Left stick |
| Aim | Right stick |
| Vacuum | RT / R2 |
| Blow | LT / L2 |
| Talk, open, look | A / Cross |
| Flashlight, back | X / Square |
| Search camera | B / Circle |
| Map | Y / Triangle |
| Status | LB or RB / L1 or R1 |
| Pause | Menu / Options |
| Recenter the diorama (Full) | Right stick click |
| The game's settings | Click both sticks |

Both modes play with a gamepad. On your table, up on the stick is away from you, from whichever side
you look, and the game's settings open on the panel. In cutscenes the game's own shot shows in a
small picture on the panel; look at it and pinch to drag it somewhere else. Resize the volume with
its corner handle, and close it to quit. Each room fits whole whatever the volume's size; the long
halls and big outdoor rooms follow Luigi instead.

The launcher's **Controls** tab draws the gamepad and labels every button. **Ports**
lists other games ported to Vision Pro from the
[AVP Ports Index](https://github.com/edgytoast/avp-ports-index), and **About** has the credits,
the licences and a button to check for updates.

Settings, memory cards and the VR settings for each mode live in Files › On My Apple Vision Pro ›
Luigi's Mansion VR › Luigi's Mansion VR. Deleting the app deletes them, so copy that folder off
first.

## Known issues

- Only the USA disc (GLME01) and a gamepad. PS VR2 Sense controllers and bare hands aren't
  supported. The build has an untested path for the European disc (GLMP01).
- The first start prepares about 3,000 shaders (a few minutes); later starts skip that.
- On your table:
  - the set shows the game's own lighting only: no enhanced lighting, shadow maps or screen
    effects, and surfaces the game draws from its own picture (the mirror room's mirror, TV static)
    show black;
  - its motion is the game's own 30 frames a second. **Smoother motion on the table**
    (experimental, off by default, in the launcher's Advanced options) shows Luigi and what moves
    around him at up to 60, a sixtieth of a second later; ghosts, effects and the HUD stay at 30;
  - ghosts glow over the room as in the game, but crisper than the game's soft glow;
  - in a cutscene the room's walls stay as they were in play (so none come up between you and
    Luigi), with the room's lighting from then too;
  - in dark rooms the flashlight's beam reads as a pale cone;
  - the game's settings open small on the panel: Visuals › Menu size makes them bigger;
  - a room whose open space reaches past its own box can be cut by the volume's walls (the Foyer
    and its upper landing are fitted together);
  - in a small volume the rooms shrink to fit, so Luigi can be under 2 cm tall; the long halls and
    big outdoor rooms follow him instead.
- Some of the game's settings are for PCs (Fullscreen, VSync, window aspect) and do nothing here.
- The VR mod's Life-size and First person modes, and Progressive immersion, aren't here yet.

## What's in this repository

The whole game port, in the order it was made:

- **Luigi's Mansion Recompiled** by rob646464, with its history, up to v1.1.4 (commit `850a2b9`).
- **Luigi's Mansion VR, build 150** by u/sonarix, as one commit, exactly as he shared it in
  [u/sonarix's release on r/virtualreality](https://www.reddit.com/r/virtualreality/comments/1xg3kvj/). In that post he says he doesn't plan to keep
  developing it, and he included the source, so it lives on here. His notes for build 150 are in [README-LMVR.md](README-LMVR.md) and
  [RELEASE-README.md](RELEASE-README.md), and the recompilation's own README is
  [UPSTREAM-README.md](UPSTREAM-README.md).
- **Trevorbilt's changes** to the recompilation, the VR mod and aurora for visionOS, as one commit.
- **The visionOS app** in `visionos/`: the app and launcher, the host that runs the game, the
  OpenXR provider and the build scripts.

What it never holds is anything from your disc. The build recompiles the game's code from your
disc's `main.dol` on your Mac, and everything it makes stays in `.scratch/`, which git ignores.
[docs/DISC-DERIVED.md](docs/DISC-DERIVED.md) lists exactly what that is.

## How it works

Luigi's Mansion Recompiled turns the game's PowerPC code into C++. On a PC it does that the first
time you play; visionOS can't compile code at run time, so here the build does it on your Mac, from
your disc's `main.dol`, and links the result into the app. [aurora](https://github.com/encounter/aurora)
draws the GameCube's graphics with Dawn on Metal. The VR mod draws each eye of its diorama, and its
OpenXR calls are answered by a small OpenXR runtime for visionOS (from iChris4's WiiCompiled VR, by
way of Trevorbilt's Twilight Princess VR port) on CompositorServices and ARKit. On your table
recreates the game's 3D draws live in RealityKit, inside a volume, so the mansion is real geometry
you can look at from any side: it's the scene mirror from Trevorbilt's
[Wind Waker VR port](https://github.com/edgytoast/windwaker-visionos), carried over to this game's aurora.

## Support

Luigi's Mansion VR is made and maintained by [Trevorbilt](https://trevorbilt.com). If it made your
Halloween, come see what else is on the workbench, like [Loose Papers](https://trevorbilt.com/loose-papers)
(a bookshelf for your Vision Pro) and [Papa's Ball & Tee](https://trevorbilt.com/papas-ball-and-tee).

## Credits

This port stands on a lot of other people's ghost-hunting:

- **[u/sonarix](https://www.reddit.com/user/sonarix)** made Luigi's Mansion VR: the Diorama mode,
  the stereo story scenes, the VR settings and the comfort work this port plays. He shared its
  source, and handed the project on, in [u/sonarix's release on r/virtualreality](https://www.reddit.com/r/virtualreality/comments/1xg3kvj/).
- **[rob646464](https://github.com/rob646464/luigis-mansion-recompiled)** made Luigi's Mansion
  Recompiled: the game recompiled to run natively, with its enhancements.
- **[SomeJakeGuy and the lm-decomp contributors](https://github.com/SomeJakeGuy/lm-decomp)** made
  the decompilation whose symbols and splits drive the recompiler.
- **[encounter](https://github.com/encounter/aurora)** made aurora, the GameCube graphics and system
  layer, and nod, its disc reader.
- **[TwilitRealm](https://github.com/TwilitRealm/dusklight)** made Dusklight, whose JSystem code the
  recompilation builds on.
- **[iChris4](https://github.com/iChris4/Wiicompiled_VR)** made WiiCompiled VR, the source of the
  OpenXR provider for visionOS.
- **Nintendo** made *Luigi's Mansion* (2001).

The Vision Pro port is [Trevorbilt](https://trevorbilt.com)'s: the Metal path into the headset, the
world-locked set, On your table, the launcher and the build.

## License

Trevorbilt's visionOS code in `visionos/` is MIT ([visionos/LICENSE](visionos/LICENSE)), apart from
the OpenXR provider in `visionos/openxr-provider`, which is GPL-3.0-or-later. Everything else keeps
its own terms, listed in [NOTICE.md](NOTICE.md): lm-decomp and Dusklight's JSystem are CC0, aurora
is MIT, and the recompilation and the VR mod are shared as source by their authors. The game, its
characters and its art belong to Nintendo, and the disc is yours to bring.

A built app holds code recompiled from your own disc. Build it for yourself, and don't pass it on.

<p align="center">
  <sub>Made with a slightly nervous flashlight by <a href="https://trevorbilt.com">Trevorbilt</a>.</sub>
</p>
