# Installing Luigi's Mansion VR on Apple Vision Pro

Luigi's Mansion VR is [Trevorbilt](https://trevorbilt.com)'s native visionOS port of the Luigi's
Mansion VR mod by [u/sonarix](https://www.reddit.com/user/sonarix), which is built on
[Luigi's Mansion Recompiled](https://github.com/rob646464/luigis-mansion-recompiled) by rob646464;
the [README](README.md#credits) credits everyone it builds on.

*Luigi's Mansion* runs natively on Apple Vision Pro, as a diorama in your room (Full) or a model on
your table beside your other apps (On your table), once you build this project with Xcode and
install it on your own headset. There's no ready-made app to download, and no game data here:
this repository holds the source (the recompilation, the VR mod and the visionOS app), and the build
recompiles the game's code from your own disc, on your Mac.

## Requirements

- **Apple Vision Pro** on visionOS 26 or later, with Developer Mode on.
- **A Mac with Apple silicon** and **Xcode 27** (built and tested with Xcode 27.1 on macOS 27),
  installed at `/Applications/Xcode.app` (CMake can't find Xcode at a path with a space in it).
  Open Xcode once so it finishes installing, then add its visionOS platform in Xcode › Settings ›
  Components.
- **An Apple Account** signed in to Xcode (Xcode › Settings › Accounts). A free account can work,
  but apps it signs stop opening after 7 days until you install them again, and some free accounts
  can't sign the app's Increased Memory Limit (see Troubleshooting).
- **Homebrew** ([brew.sh](https://brew.sh)) with `cmake`, `ninja`, `xcodegen` and `rustup`, and
  **Python 3** (macOS's own is fine).
- **About 10 GB of free disk space.** The first build takes about an hour, most of it building Dawn
  (the graphics layer) for visionOS; later builds reuse it and take minutes.
- **A gamepad** (Xbox, PlayStation or another MFi controller), paired with the headset: the only
  input the port supports. PS VR2 Sense controllers and bare hands aren't supported.
- **Your own *Luigi's Mansion* disc image**, as below.

## Game files

You supply your own disc. This repository contains nothing from it, and the build never downloads
any of the game.

1. **Dump your own disc** to a plain `.iso` (or `.gcm`). Dolphin's guide covers it:
   https://dolphin-emu.org/docs/guides/ripping-games/. If you already have an `.rvz` or `.ciso`,
   convert it in Dolphin (right-click the game › Convert File › ISO): the build reads only plain
   images.
2. **Use the USA release, GLME01.** It's the only release this port is built and tested with: the
   app is recompiled from that disc and plays only it. (The build has an untested path for the
   European release, GLMP01.)
3. **Check it.** A full 1:1 dump of the USA disc matches these:

   | | Value |
   | --- | --- |
   | Size | 1,459,978,240 bytes |
   | MD5 | `6e3d9ae0ed2fbd2f77fa1ca09a60c494` |
   | SHA-1 | `96a7d2e6b6e0a355f6e57ef76bd37f6ee837ed25` |
   | CRC32 | `0b0c339d` |
   | `main.dol` inside it, SHA-1 | `77d49225e7d28b9777916cdbe5809345c570ecbc` |

   ```bash
   md5 "Luigi's Mansion (USA).iso"
   shasum -a 1 "Luigi's Mansion (USA).iso"
   ```

   The `main.dol` hash is the one that matters: it's the game's code, and the build recompiles
   exactly that file. A trimmed or scrubbed image won't match the whole-image hashes but still works
   if its `main.dol` matches. The build copies it to `.scratch/disc/main.dol`, so after your first
   build you can check it with `shasum -a 1 .scratch/disc/main.dol`. (For the record, the European
   release's `main.dol` is `a1e2bd82813787cfe0b11afb5d138f08fdbd94f1`.)

## Build

Start from a checkout of this repository. If you came from the AVP Ports Index and already cloned
the commit it lists, skip the clone.

```bash
git clone https://github.com/edgytoast/luigismansion-visionos.git luigismansion-visionos
cd luigismansion-visionos
```

1. **Install the build tools,** once:

   ```bash
   brew install cmake ninja xcodegen rustup
   export PATH="$(brew --prefix rustup)/bin:$PATH"
   rustup toolchain install nightly-2026-09-30 --profile minimal
   rustup target add --toolchain nightly-2026-09-30 aarch64-apple-visionos aarch64-apple-visionos-sim
   ```

   (aurora's disc reader, nod, is written in Rust; the nightly is pinned so every build gets the
   same compiler.)

2. **Find your team ID.** It's ten characters, such as `A1B2C3D4E5`. With a paid membership it's on
   https://developer.apple.com/account under Membership details. For any account, once Xcode has
   made you an Apple Development certificate (signing anything once does it, including the build
   below), this prints it as `OU=`:

   ```bash
   security find-certificate -c "Apple Development" -p | openssl x509 -noout -subject
   ```

3. **Build, with your disc and your team:**

   ```bash
   LMVR_DISC="/path/to/Luigi's Mansion (USA).iso" \
     visionos/scripts/build-visionos.sh --team YOURTEAMID
   ```

   In order, the build (all from this checkout, with build products in `.scratch/`):
   - builds the recompilation's recompiler (`lmrecompile`) for your Mac;
   - copies `main.dol` out of your disc into `.scratch/disc/` and recompiles it into C++ in
     `.scratch/recomp-GLME01/` (under a second);
   - downloads Dawn's source from [encounter/dawn](https://github.com/encounter/dawn) at the exact
     revision aurora pins, with its third-party sources, and builds it for visionOS (the long part,
     once);
   - builds the game, aurora, SDL and nod into `LuigisMansionVisionGame.framework` (inside
     `.scratch/`);
   - generates `visionos/App/LuigisMansionVision.xcodeproj` with xcodegen and builds the app, signed
     with your team, as `dev.lmvr.vision.YOURTEAMID`.

   Your team goes in on the command line and nowhere else: nothing in this repository is tied to
   anyone's team. Keep the team suffix on the bundle ID: a bare ID can already belong to another
   team, and signing then falls back to a profile without the memory entitlement.

   Want the visionOS Simulator instead? `visionos/scripts/build-visionos.sh --simulator` (with
   `LMVR_DISC` as above) needs no team. It's handy for poking around, not for judging looks or
   speed.

## Install on Apple Vision Pro

1. **Turn on Developer Mode** on the headset: Settings › Privacy & Security › Developer Mode (it
   restarts).
2. **Pair it with your Mac:** Xcode › Window › Devices and Simulators on the Mac, and Settings ›
   General › Remote Devices on the headset. Keep the headset on and unlocked while installing.
   Apple's guide: https://developer.apple.com/documentation/xcode/running-your-app-on-simulated-or-physical-devices
3. **Install it,** either way:
   - **From Terminal:** run the build command again with `--install` on the end. It installs on the
     first paired Vision Pro it finds (`--device <UDID>` picks one).
   - **From Xcode:** open `visionos/App/LuigisMansionVision.xcodeproj` (the build made it, already
     set to your team), choose your Apple Vision Pro as the run destination, and press Run (⌘R).
     To sign with another team, run the build script again with that `--team`: the project is
     regenerated each build, so changes made in Xcode don't stick.
4. **If visionOS says the developer isn't trusted,** allow it in Settings › General › VPN & Device
   Management.

## Add your game and play

1. **Put the same disc image on the headset:** AirDrop it and choose **Luigi's Mansion VR** to open
   it, copy it into Files › On My Apple Vision Pro › Luigi's Mansion VR, or tap **Import** in the
   app. The headset also reads the same release as `.rvz` or `.ciso`. **Wait for the copy to finish
   before you open the app.** The app plays only the release it was built from, and says so if you
   give it another.
2. **Connect your gamepad** and press **Play**. The first start prepares
   about 3,000 shaders behind a progress panel, which takes a few minutes; later starts skip it.
3. **Full** (the default) puts the mansion in your room as a diorama, with menus floating beside it.
   Turn off "The mansion in my room" to play it in the dark. Click the right stick to recenter it,
   and click both sticks for the game's settings (the diorama's scale, distance and height, the HUD,
   the controls). Walk about 1.2 m from where you started and the game fades into your room. Press
   the Digital Crown to pause: the launcher comes back with Resume and Quit.
4. **On your table** (pick it before Play) puts the mansion in a volume beside your other apps,
   played with a gamepad. Up on the stick is away from you, from whichever side you look. The HUD
   and text face you on a panel, and in cutscenes the game's own shot shows in a small picture in
   its corner (look at it and pinch to drag it somewhere else). Click both sticks for the game's
   settings, on the panel. Resize the volume with its corner handle; close it to quit.
5. **Smoother motion on the table** (the launcher's Advanced options, before Play) is experimental
   and off by default: on, Luigi and what moves around him show at up to 60 frames a second instead
   of the game's 30, a sixtieth of a second later.

Settings, memory cards and the VR settings for each mode live in Files › On My Apple Vision Pro ›
Luigi's Mansion VR › Luigi's Mansion VR. Deleting the app deletes them: copy that folder off first.

### Controls

With a gamepad the buttons sit where they did on the GameCube pad (the game's settings, under
Controls, change them):

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

In the game's settings, use the left stick and A.

## Updating

```bash
git pull
LMVR_DISC="/path/to/Luigi's Mansion (USA).iso" visionos/scripts/build-visionos.sh --team YOURTEAMID --install
```

Your disc, saves and settings on the headset stay put.

## Troubleshooting

- **CMake can't find Xcode:** Xcode must be at `/Applications/Xcode.app`, not a path with a space.
- **Rust says the toolchain or target is missing:** run the two `rustup` commands from step 1 of
  Build, with `$(brew --prefix rustup)/bin` on your `PATH`.
- **`extract-dol.py`: not a plain GameCube disc image:** the build needs an `.iso` or `.gcm`; convert
  an `.rvz` or `.ciso` with Dolphin first.
- **Signing fails on "Increased Memory Limit":** some free accounts can't sign it. A paid membership
  can. Building without it means deleting the `com.apple.developer.kernel.increased-memory-limit`
  line from `visionos/App/project.yml`, and the game may then be stopped for using too much memory.
- **The install hangs:** wake and unlock the headset, then run the command again.
- **"This app was built from the ... disc":** the app plays only the release it was recompiled from.
  Use that disc on the headset, or build again with `LMVR_DISC` pointing at the other one.
- **The app closes while the disc is still copying:** wait for the copy to finish, then open it
  again.
- **The app won't open after a week:** apps signed with a free account expire after 7 days. Install
  it again; your disc and saves stay put.
- **Anything else:** the game writes its log to `Library/Logs/game.log` in the app's container.
  Running from Xcode shows it in the console too.

## Known issues

- Only the USA disc (GLME01) and a gamepad. PS VR2 Sense controllers and bare hands aren't
  supported.
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

## Credits and licences

Ported to Apple Vision Pro by [Trevorbilt](https://trevorbilt.com) (MIT for Trevorbilt's code in
`visionos/`: [visionos/LICENSE](visionos/LICENSE)).

- The Luigi's Mansion VR mod by [u/sonarix](https://www.reddit.com/user/sonarix), build 150, shared
  as source and handed on in [u/sonarix's release on r/virtualreality](https://www.reddit.com/r/virtualreality/comments/1xg3kvj/), and
  [Luigi's Mansion Recompiled](https://github.com/rob646464/luigis-mansion-recompiled) by rob646464,
  shared publicly as source; both are in this repository with their history.
- [lm-decomp](https://github.com/SomeJakeGuy/lm-decomp) by SomeJakeGuy and contributors, and the
  JSystem code from [Dusklight](https://github.com/TwilitRealm/dusklight) by TwilitRealm: CC0.
- [aurora](https://github.com/encounter/aurora) by encounter: MIT, with its dependencies' own
  licences (Dawn, SDL, nod and others).
- The OpenXR provider from [WiiCompiled VR](https://github.com/iChris4/Wiicompiled_VR) by iChris4
  (`visionos/openxr-provider`): GPL-3.0-or-later.
- Everything else is in [NOTICE.md](NOTICE.md), and what the build makes from your disc is in
  [docs/DISC-DERIVED.md](docs/DISC-DERIVED.md).

A built app holds code recompiled from your own disc: build it for yourself and don't pass it on. *Luigi's Mansion* belongs to Nintendo; this project isn't
affiliated with or endorsed by Nintendo and includes nothing from the game.
