import CompositorServices
import os
import SwiftUI
import TrevorbiltKit

/// The app: the launcher (TrevorbiltKit's: Play, Controls, Ports, About) and the immersive spaces
/// and window the game renders into. The game is reached only through its C bridge
/// (lmvr_visionos_*), driven by GameModel. What's this port's own is in PortContent.swift.
@main
struct LuigisMansionVisionApp: App {
    @StateObject private var model = GameModel()
    @State private var space = ImmersionSpaceStyle.shared

    init() {
        MemoryWatch.start()
        Trevorbilt.registerFonts()
    }

    var body: some Scene {
        // Only ever one launcher: it's opened with the same value each time (GameModel.showLauncher),
        // and visionOS brings the window already showing that value to the front instead of
        // opening another; the one at launch has it too (defaultValue).
        WindowGroup(id: GameModel.launcherWindowID, for: String.self) { _ in
            LauncherView()
                .environmentObject(model)
        } defaultValue: {
            GameModel.launcherWindowID
        }
        // The launcher's card is one size on every tab, and the window hugs it.
        .windowResizability(.contentSize)

        // Two spaces, never one. A space that lists the progressive style makes every drawable
        // "support progressive": each present must then end through the system's render context,
        // which only a layered layout can carry, and a dedicated layout aborts ("Layer
        // configuration not supported"). The Simulator's single view hides both crashes.
        //
        // Full: the game all around you. Mixed: the same, with the room showing wherever the
        // game's frames are transparent (menus, black screens).
        ImmersiveSpace(id: GameModel.immersiveSpaceID) {
            ImmersiveGame.layer(for: model, progressive: false)
        }
        .immersionStyle(selection: $space.style, in: .full, .mixed)
        .upperLimbVisibility(.automatic)

        // On your table: the mansion in a volume beside the user's other apps (GameVolumeView),
        // played with a gamepad. Only Play (or Resume) opens it.
        WindowGroup(id: GameModel.windowSceneID) {
            GameVolumeView()
                .environmentObject(model)
        }
        // Never restored at launch (after the app ended with it open): it starts the game, and
        // the launcher is the way in.
        .restorationBehavior(.disabled)
        .windowStyle(.volumetric)
        // A generous table: the set sizes itself to it (src/recomp/volume.cpp) and refits when
        // it's resized. A volume's automatic scaling is the fixed (true) scale, so a room is the
        // same size near or far, as a model on a table is (.dynamic would grow it with distance);
        // level however high it's lifted.
        .defaultSize(width: GameModel.volumeDefaultSize.x, height: GameModel.volumeDefaultSize.y,
                     depth: GameModel.volumeDefaultSize.z, in: .meters)
        .windowResizability(.automatic)
        .defaultWorldScaling(.automatic)
        .volumeWorldAlignment(.gravityAligned)

        // Progressive: the game through a portal the Digital Crown widens.
        ImmersiveSpace(id: GameModel.progressiveSpaceID) {
            ImmersiveGame.layer(for: model, progressive: true)
                // A Digital Crown recenter moves the portal to where you face; the game turns its
                // world to keep the view in it.
                .onWorldRecenter { lmvr_visionos_world_recentered() }
        }
        .immersionStyle(selection: $space.progressiveStyle, in: GameModel.progressiveStyle, .full)
        .upperLimbVisibility(.automatic)
    }
}

/// Logs visionOS's memory warnings with what's left before the app's limit, so a session that
/// ends early (jetsam) can be told apart from a crash in the device log.
@MainActor
enum MemoryWatch {
    private static var source: DispatchSourceMemoryPressure?
    // The unified log, which a headset's device log keeps (print goes to stdout only).
    nonisolated private static let log = Logger(subsystem: Bundle.main.bundleIdentifier ?? "dev.lmvr.vision", category: "memory")

    static func start() {
        guard source == nil else { return }
        let source = DispatchSource.makeMemoryPressureSource(eventMask: [.warning, .critical], queue: .main)
        source.setEventHandler { [weak source] in
            guard let event = source?.data else { return }
            let level = event.contains(.critical) ? "critical" : "warning"
            log.warning("[LMVR] memory pressure (\(level, privacy: .public)): \(os_proc_available_memory() / 1_048_576, privacy: .public) MB left")
        }
        source.resume()
        Self.source = source
        log.notice("[LMVR] memory: \(os_proc_available_memory() / 1_048_576, privacy: .public) MB available at launch")
    }
}
