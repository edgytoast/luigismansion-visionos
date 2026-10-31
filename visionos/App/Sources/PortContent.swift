import SwiftUI
import TrevorbiltKit

/// Everything in the launcher that's this port's own: its name, the ways it plays and what each
/// plays with, what pausing can do, its game files, Advanced options, the controls, credits and
/// licences. The layout and scene plumbing (LauncherView, ControlsView, GameModel) and
/// TrevorbiltKit are shared by every Trevorbilt port: change them only to fix them for all.
///
/// Fill every TODO. Copy: plain, warm, low pressure; short direct verbs ("Play", "Resume"); no em
/// dashes; errors say what to do. Run new copy past the trevorbilt-critic agent.
enum Port {
    // MARK: Who and where

    /// The port's name, its last word bold in the header.
    static let title = "Luigi's Mansion "
    static let boldTitle = "VR"
    /// "<the game's full title> on Apple Vision Pro".
    static let subtitle = "Luigi's Mansion on Apple Vision Pro"
    /// The public release, "owner/name", and its branch: the update check, Report a problem (its
    /// issues), the GPL source offer. The "Build commit" phase in project.yml names it too.
    static let repository = "edgytoast/luigismansion-visionos"
    static let releaseBranch = "main"
    /// The port's id in the AVP Ports Index once listed (the Ports tab marks its own card).
    static let indexID = "luigis-mansion-vr"
    /// How a player updates once the update check finds a newer version.
    static let updateInstructions = "git pull, then build again with your disc: LMVR_DISC=<your disc> visionos/scripts/build-visionos.sh --team <your team> --install (AVP-INSTALL.md has the details). Your game files and saves stay."

    // MARK: Modes

    /// The ways this port plays: Diorama in a Full Space (the VR mod's default and tested mode,
    /// first), and on your table: the same world-locked set in a volume in the shared space,
    /// beside other apps, with a gamepad (GameVolumeView).
    static let modes: [GameModel.Immersion] = [.full, .window]
    /// The mansion in your room: a mixed space where the diorama sits in your real room (the eyes
    /// are see-through wherever the game drew nothing) and menus float in it on their panel.
    static let offersRoomAroundMenus = true
    static let roomToggleTitle = "The mansion in my room"

    /// One line on each mode's card.
    static func line(_ mode: GameModel.Immersion) -> String {
        switch mode {
        case .full: "The mansion as a diorama in front of you. Lean in and look around."
        case .progressive: "A portal in your room. The Digital Crown widens it."
        case .window: "A model of the mansion on your table, beside your other apps. Plays with a gamepad."
        }
    }

    /// One plain sentence under the cards on what the selected mode (and its option) does.
    static func note(_ mode: GameModel.Immersion, roomBehindMenus: Bool) -> String {
        switch mode {
        case .full where roomBehindMenus:
            "The diorama sits in your room, and menus float beside it. Walk about 1.2 m from where you started and the game fades into the room."
        case .full:
            "The diorama in the dark, all around you. Click both sticks for the game's settings, and the right stick to recenter."
        case .progressive:
            "Turn the Digital Crown to widen or narrow the portal. Press it to pause."
        case .window:
            "In the shared space, with a gamepad only: visionOS gives an app no hand or controller tracking there. The text and HUD show on the side facing you. Close the volume to quit."
        }
    }

    // MARK: Pause capabilities

    /// The Digital Crown closing Full or Progressive pauses the game, and the launcher comes back
    /// with Resume. Needs a game that holds its session and takes a new layer (lmvr_visionos_can_resume).
    static let crownPauses = true
    /// Quit beside Resume: the game leaves through its own exit path, writing its settings, and
    /// the memory card keeps what was saved in the game.
    static let quitsFromPause = true
    /// Closing the game's window pauses it rather than ending the game and the app.
    static let windowClosePauses = false
    /// What Quit keeps, said beside Resume.
    static let pausedNote = "Resume carries on where you were. Quit keeps your progress up to your last save in the game."

    // MARK: Inputs (mode → inputs)

    /// Which inputs each mode plays with: a gamepad, the only input the port supports and was
    /// tested with (README, AVP-INSTALL). The VR mod's Sense-controller and hand bindings are still
    /// there, unpromised.
    @MainActor static func verdict(_ mode: GameModel.Immersion, _ inputs: InputMonitor) -> InputVerdict {
        if inputs.gamepad != nil { return InputVerdict("Ready to play with your gamepad.", ready: true) }
        if inputs.anySense {
            return InputVerdict("Luigi's Mansion VR plays with a gamepad; Sense controllers aren't supported. Connect a gamepad.", ready: false)
        }
        return InputVerdict("Luigi's Mansion VR plays with a gamepad. Connect one to play.", ready: false)
    }
    /// The Controls tab's inputs: the gamepad only.
    static let controlInputs: [ControlsView.Input] = [.gamepad]

    // MARK: Game files

    /// What the player calls the game's files, as the card says it.
    static let filesNoun = "disc"
    static let filesSymbol = "opticaldisc.fill"
    /// Which files, which releases, and the ways in.
    static var filesHelp: String {
        "Your own Luigi's Mansion disc image (\(builtRelease)), the same one this app was built from. AirDrop it to this Vision Pro, put it in Files › On My Apple Vision Pro › Luigi's Mansion VR, or import it here."
    }

    /// The release the game's code was recompiled from when the app was built: only that one plays.
    nonisolated static var builtGameID: String { String(cString: lmvr_visionos_built_game_id()) }
    nonisolated static var builtRelease: String {
        switch builtGameID {
        case "GLME01": "USA, GLME01"
        case "GLMP01": "Europe, GLMP01"
        default: "the app was built without one"
        }
    }

    /// Whether `file` is the release the game plays, read cheaply: a plain image's header (its
    /// magic word and game ID). RVZ and CISO images are compressed: their header isn't the disc's,
    /// so only their own magic is checked and the game itself checks the release when it starts.
    nonisolated static func validate(_ file: URL) -> String? {
        let built = builtGameID
        guard built != "NONE" else {
            return "This build of the app has no game in it: it was built without a disc. Build it again with your disc (AVP-INSTALL.md)."
        }
        guard let handle = try? FileHandle(forReadingFrom: file) else { return "The file can't be read. Import it again." }
        defer { try? handle.close() }
        guard let header = try? handle.read(upToCount: 0x20), header.count == 0x20 else {
            return "The file is too short to be a disc image. Import it again."
        }
        let bytes = [UInt8](header)
        if bytes[0..<4] == [0x52, 0x56, 0x5A, 0x01] || bytes[0..<4] == [0x43, 0x49, 0x53, 0x4F] { return nil }  // RVZ, CISO
        guard bytes[0x1C..<0x20] == [0xC2, 0x33, 0x9F, 0x3D] else {
            return "That isn't a GameCube disc image. Use your Luigi's Mansion .iso, .gcm, .rvz or .ciso."
        }
        let id = String(decoding: bytes[0..<6], as: UTF8.self)
        if id == built { return nil }
        if id == "GLME01" || id == "GLMP01" {
            return "This is the \(id == "GLME01" ? "USA" : "European") disc, but this app was built from the \(builtRelease) one. Use that disc."
        }
        return "That disc is \(id), not Luigi's Mansion."
    }

    // MARK: Advanced options

    /// Options that must be set before Play and that most players never touch. The game's own
    /// settings (both sticks clicked: diorama scale, distance and height, HUD placement, controls)
    /// are in its menu.
    struct AdvancedOptions: View {
        @ObservedObject var model: GameModel
        // MirrorScene reads it (volumeSmoothMotion).
        @AppStorage("volumeSmoothMotion") private var smoothMotion = false

        var body: some View {
            TrevorbiltCard {
                VStack(alignment: .leading, spacing: 16) {
                Toggle(isOn: $model.foveated) {
                    VStack(alignment: .leading, spacing: 4) {
                        Text("Foveated rendering").font(.tbBody(14, weight: .bold))
                        Text("In Full and Progressive: sharper where you look. The game renders larger eye images for it, which costs GPU time, so lower the game's render resolution if it stutters.")
                            .font(.tbBody(12)).foregroundStyle(.white.opacity(0.8))
                            .fixedSize(horizontal: false, vertical: true)
                    }
                }
                .tint(Trevorbilt.orange)
                Toggle(isOn: $smoothMotion) {
                    VStack(alignment: .leading, spacing: 4) {
                        Text("Smoother motion on the table (experimental)").font(.tbBody(14, weight: .bold))
                        Text("On your table: Luigi and what moves around him also show halfway between the game's own frames: up to 60 a second instead of 30, a sixtieth of a second later. Ghosts, effects and the HUD stay at 30.")
                            .font(.tbBody(12)).foregroundStyle(.white.opacity(0.8))
                            .fixedSize(horizontal: false, vertical: true)
                    }
                }
                .tint(Trevorbilt.orange)
                }
            }
        }
    }

    // MARK: Controls

    /// One page per input.
    static let controlPages: [String] = []

    /// The controls, read from the game and its VR mod: the gamepad's defaults put the GameCube's
    /// buttons where they sat on its pad (src/recomp/controls.cpp, docs/CONTROLS.md: A south, B
    /// west, X east, Y north, Z either shoulder, L and R the triggers); the Sense controllers play
    /// the mod's Touch bindings through the provider (src/vr/openxr_session.cpp: A confirm, B back,
    /// X map, Y search, left stick click pause; src/recomp/hle_pad.cpp turns them into the
    /// GameCube's buttons), with the left stick moving, the right aiming and the triggers as L and R
    /// (vr_host.cpp, Diorama); bare hands play the provider's Touch gestures.
    static func controls(_ input: ControlsView.Input, window: Bool, page: Int) -> [ControlItem] {
        func button(_ n: Int, _ action: String, _ how: String, _ anchor: String?, also: [String: String] = [:]) -> ControlItem {
            ControlItem(n, action, how, anchor: anchor, also: also)
        }
        func hand(_ n: Int, _ action: String, _ how: String, _ anchor: String, _ pose: HandPose) -> ControlItem {
            ControlItem(n, action, how, anchor: anchor, pose: .hand(pose))
        }
        // On your table: the gamepad, its settings on the volume's panel (both sticks), nothing to
        // recenter.
        if window {
            return [button(1, "Move", "Left stick (up: away from you)", "leftStick"),
                    button(2, "Aim", "Right stick", "rightStick"),
                    button(3, "Vacuum", "RT / R2", "rightTrigger"),
                    button(4, "Blow", "LT / L2", "leftTrigger"),
                    button(5, "Talk, open, look", "A / Cross", "a"),
                    button(6, "Flashlight, back", "X / Square", "x"),
                    button(7, "Search camera", "B / Circle", "b"),
                    button(8, "Map", "Y / Triangle", "y"),
                    button(9, "Status", "LB or RB / L1 or R1", "leftShoulder", also: ["rightShoulder": "Status"]),
                    button(10, "Pause", "Menu / Options", "menu", also: ["view": "Pause"]),
                    button(11, "Settings", "Click both sticks", nil)]
        }
        switch input {
        case .hands:
            return [hand(1, "Walk", "Pinch your left thumb and middle finger, hold, and move your hand.", "leftMiddle", .pinchMiddleLeftMove),
                    hand(2, "Aim", "Pinch your right thumb and little finger, hold, and move it sideways.", "rightLittle", .pinchLittleRightTurn),
                    hand(3, "Vacuum", "Pinch your right thumb and index finger.", "rightIndex", .pinchIndexRight),
                    hand(4, "Talk, open, look", "Pinch your right thumb and middle finger.", "rightMiddle", .pinchMiddleRight),
                    hand(5, "Flashlight, back", "Pinch your right thumb and ring finger.", "rightRing", .pinchRingRight),
                    hand(6, "Blow", "Pinch your left thumb and index finger.", "leftIndex", .pinchIndexLeft),
                    hand(7, "Search camera", "Pinch your left thumb and ring finger.", "leftRing", .pinchRingLeft)]
        case .sense:
            return [button(1, "Move", "Left stick (click it to pause)", "leftStick", also: ["leftStick": "Pause (click)"]),
                    button(2, "Aim", "Right stick (click it to recenter)", "rightStick", also: ["rightStick": "Recenter (click)"]),
                    button(3, "Vacuum", "Right trigger", "rightTrigger"),
                    button(4, "Blow", "Left trigger", "leftTrigger"),
                    button(5, "Talk, open, look", "Cross", "a"),
                    button(6, "Flashlight, back", "Circle", "b"),
                    button(7, "Map", "Square", "x"),
                    button(8, "Search camera", "Triangle", "y"),
                    button(9, "Settings", "Click both sticks", nil)]
        case .gamepad:
            return [button(1, "Move", "Left stick", "leftStick"),
                    button(2, "Aim", "Right stick (click it to recenter)", "rightStick", also: ["rightStick": "Recenter (click)"]),
                    button(3, "Vacuum", "RT / R2", "rightTrigger"),
                    button(4, "Blow", "LT / L2", "leftTrigger"),
                    button(5, "Talk, open, look", "A / Cross", "a"),
                    button(6, "Flashlight, back", "X / Square", "x"),
                    button(7, "Search camera", "B / Circle", "b"),
                    button(8, "Map", "Y / Triangle", "y"),
                    button(9, "Status", "LB or RB / L1 or R1", "leftShoulder", also: ["rightShoulder": "Status"]),
                    button(10, "Pause", "Menu / Options", "menu", also: ["view": "Pause"]),
                    button(11, "Settings", "Click both sticks", nil)]
        }
    }

    /// One note under each page: what no drawing shows, and where a setting changes it.
    static func controlsNote(_ input: ControlsView.Input, window: Bool) -> String {
        if window {
            return "On your table plays with a gamepad. Up on the stick is away from you, from whichever side you look. Click both sticks for the game's settings: they open on the panel in front of the set; the left stick and A / Cross choose, B / Circle goes back, both sticks again close them."
        }
        return switch input {
        case .hands: "Bare hands aren't supported: play with a gamepad."
        case .sense: "Sense controllers aren't supported: play with a gamepad."
        case .gamepad: "The buttons sit where they did on the GameCube pad. The game's settings change them, under Controls."
        }
    }

    // MARK: About

    /// The kit's GPL notice says the whole app is GPL-3.0. This one isn't that simple: it links the
    /// GPL-3.0-or-later provider with code that has no licence at all (the recompilation and its VR
    /// mod), so its own notices below say what covers what, and no one may share the built app.
    static let appIsGPL = false

    /// The repository at the commit this build came from (a development build's isn't public, so
    /// the release branch): the GPL source offer.
    static var sourceOfThisBuild: URL? {
        let build = BuildInfo.current
        return URL(string: "https://github.com/\(repository)/tree/\(build.dirty ? releaseBranch : build.commit ?? releaseBranch)")
    }

    static var about: AboutContent {
        let notices: [AboutContent.Notice] = [
            .init("The visionOS port: MIT", "Trevorbilt's code in visionos/. NOTICE.md lists every part of this repository and its terms.",
                  URL(string: "https://github.com/\(repository)/blob/\(releaseBranch)/NOTICE.md")),
            .init("Luigi's Mansion Recompiled and its VR mod", "rob646464's recompilation and u/sonarix's VR mod, shared publicly as source, with no licence file. u/sonarix handed his mod on to the community."),
            .init("Not for sharing: the built app", "It holds code recompiled from your own disc. Build it for yourself; don't pass it on."),
            .init("The visionOS OpenXR provider: GPL-3.0-or-later", "From WiiCompiled Vision, in visionos/openxr-provider, with its source and changes there.",
                  URL(string: "https://github.com/\(repository)/tree/\(releaseBranch)/visionos/openxr-provider")),
            .init("The game's other parts", "lm-decomp's symbols and Dusklight's JSystem: CC0. aurora and its libraries: MIT and their own licences. The Khronos OpenXR headers: Apache-2.0."),
            .init("The launcher's shared code: MIT", "TrevorbiltKit, in visionos/TrevorbiltKit.",
                  URL(string: "https://github.com/\(repository)/blob/\(releaseBranch)/visionos/TrevorbiltKit/LICENSE")),
            .init("The launcher's fonts", "Space Mono and Roboto: SIL Open Font License 1.1.",
                  URL(string: "https://github.com/\(repository)/tree/\(releaseBranch)/visionos/TrevorbiltKit/Sources/TrevorbiltKit/Resources/Fonts")),
            .init("Trevorbilt's name, badge and app icons", "All rights reserved; not covered by the repository's licences."),
        ]
        return AboutContent(
            appName: "Luigi's Mansion VR",
            repository: repository,
            releaseBranch: releaseBranch,
            updateInstructions: updateInstructions,
            // Trevorbilt first, then the lineage by name and link, as the README lists it.
            credits: [
                .init("Trevorbilt", "The Vision Pro port: the visionOS app, the Metal path into the headset, the launcher and the build.", "https://trevorbilt.com"),
                .init("rob646464", "Luigi's Mansion Recompiled: the game recompiled to run natively, with its enhancements.", "https://github.com/rob646464/luigis-mansion-recompiled"),
                .init("u/sonarix", "Luigi's Mansion VR: the Diorama, Life-size and first-person VR mod this port plays.", "https://www.reddit.com/user/sonarix"),
                .init("SomeJakeGuy and lm-decomp", "The Luigi's Mansion decompilation: its symbols and splits drive the recompiler.", "https://github.com/SomeJakeGuy/lm-decomp"),
                .init("encounter and aurora", "The GameCube graphics and system layer, on Dawn and Metal.", "https://github.com/encounter/aurora"),
                .init("TwilitRealm and Dusklight", "The JSystem code the recompilation builds on.", "https://github.com/TwilitRealm/dusklight"),
                .init("iChris4 and WiiCompiled VR", "The OpenXR provider for visionOS.", "https://github.com/iChris4/Wiicompiled_VR"),
            ],
            notices: notices,
            disclaimer: "Luigi's Mansion VR is a fan project. It isn't affiliated with or endorsed by Nintendo, who own the game, its characters and its art. Your copy of the game is yours to bring.",
            otherPorts: [
                (name: "Twilight Princess VR", url: URL(string: "https://github.com/edgytoast/avp-ports-index/blob/main/ports/twilight-princess-vr.md")!),
                (name: "SHAR VR", url: URL(string: "https://github.com/edgytoast/avp-ports-index/blob/main/ports/shar-visionos.md")!),
            ],
            // Stays nil until Trevor gives a support link: never a placeholder.
            supportURL: nil)
    }
}
