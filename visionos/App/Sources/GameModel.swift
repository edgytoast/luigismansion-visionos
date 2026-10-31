import AVFAudio
import CompositorServices
import Foundation
import SwiftUI
import UIKit

/// The game as the launcher sees it: the user's game files in the app's Documents folder, the
/// immersion choices, and, once Play is pressed, the game's C bridge (lmvr_visionos_*). What's this
/// port's own (titles, modes, inputs, pause capabilities, the files check) is in PortContent.swift.
@MainActor
final class GameModel: ObservableObject {
    static let launcherWindowID = "launcher"
    static let immersiveSpaceID = "game"
    static let progressiveSpaceID = "game-progressive"
    static let windowSceneID = "game-window"
    /// The volume's size when it opens (metres: width, height, depth). Test runs:
    /// LMVR_VOLUME_SIZE=<w>x<h>x<d> asks for another (visionOS may give less).
    static let volumeDefaultSize: SIMD3<Double> = {
        if let text = ProcessInfo.processInfo.environment["LMVR_VOLUME_SIZE"] {
            let parts = text.split(separator: "x").compactMap { Double($0) }
            if parts.count == 3 { return SIMD3(parts[0], parts[1], parts[2]) }
        }
        return SIMD3(1.2, 0.6, 1.0)
    }()

    enum Phase: Equatable {
        case idle
        /// The immersive space is opening; the game starts once its layer exists.
        case opening
        case running
        /// The Digital Crown closed the space (or the window closed, where that pauses): the game
        /// holds where it was, its clock and sound paused, and the launcher offers Resume.
        case paused
        /// Resume was pressed: the space or window is opening again for the same game.
        case resuming
        /// The game returned with an error. It cannot run twice in one process.
        case ended(exitCode: Int32)
        case failed(message: String)
    }

    /// How the game surrounds you, picked in the launcher before Play (visionOS fixes a space's
    /// style when it opens). The port offers its own subset (Port.modes).
    enum Immersion: String, CaseIterable, Identifiable {
        case full
        case progressive
        /// The game in a window beside other apps, played flat with a controller (GameWindowView).
        /// No head or hand tracking reaches a window, so the VR layer is off.
        case window

        var id: String { rawValue }
        var title: String {
            switch self {
            case .full: "Full"
            case .progressive: "Progressive"
            case .window: "On your table"
            }
        }
    }

    @Published private(set) var phase: Phase = .idle
    /// The game files the game will load (the newest matching file in Documents), or nil.
    @Published private(set) var asset: URL?
    /// Why the files can't play (Port.validate), nil when they can or while they're checked.
    @Published private(set) var assetProblem: String?
    @Published private(set) var checkingAsset = false
    @Published private(set) var importing = false
    /// How much of an import has copied (nil while unknown, or for a move).
    @Published private(set) var importProgress: Double?
    /// Why the last import didn't work, until the next one starts.
    @Published private(set) var importFailure: String?
    @Published var message = ""
    @Published var immersion: Immersion {
        didSet { UserDefaults.standard.set(immersion.rawValue, forKey: Self.immersionKey) }
    }
    /// Full with the room around the game's menus and black screens: a mixed space whose game
    /// frames are opaque except where the game makes them see-through.
    @Published var roomBehindMenus: Bool {
        didSet { UserDefaults.standard.set(roomBehindMenus, forKey: Self.roomBehindMenusKey) }
    }
    /// Foveated rendering (the system's rasterization rate map). Off until the headset shows it
    /// holds frame rate for this game.
    @Published var foveated: Bool {
        didSet { UserDefaults.standard.set(foveated, forKey: Self.foveatedKey) }
    }
    /// The game's window is on screen: the launcher then leaves the controller to it. Keyed on
    /// the window, never on "the game is running" (that stays true while a paused game's
    /// launcher shows, and the launcher must then take the controller).
    @Published var gameWindowShowing = false
    /// Counts the launcher being brought back (a pause, the game window closing, a failure): it
    /// comes back on Play, where Resume and the reason are.
    @Published private(set) var launcherReturns = 0
    let space = ImmersionSpaceStyle.shared

    private static let immersionKey = "immersion"
    private static let roomBehindMenusKey = "roomBehindMenus"
    private static let foveatedKey = "foveated"

    /// Progressive immersion needs the render context CompositorServices gained in visionOS 26.
    static var progressiveAvailable: Bool {
        if #available(visionOS 26.0, *) { return true }
        return false
    }

    /// Portrait: the wide default portal cuts off what's below eye level (hands, weapons,
    /// dashboards). The system's own range: a custom range showed nothing on the headset (SHAR).
    nonisolated static var progressiveStyle: any ImmersionStyle {
        if #available(visionOS 26.0, *) {
            return ProgressiveImmersionStyle.progressive(aspectRatio: .portrait)
        }
        return .progressive
    }

    /// The modes this port offers on this system.
    static var offeredModes: [Immersion] {
        Port.modes.filter { $0 != .progressive || progressiveAvailable }
    }

    var playsProgressive: Bool { immersion == .progressive && Self.progressiveAvailable }
    var playsWindow: Bool { immersion == .window }
    var playsMixed: Bool { !playsProgressive && !playsWindow && Port.offersRoomAroundMenus && roomBehindMenus }

    func styleForPlay() -> any ImmersionStyle {
        playsMixed ? .mixed : .full
    }

    var spaceIDForPlay: String {
        playsProgressive ? Self.progressiveSpaceID : Self.immersiveSpaceID
    }

    /// File extensions the game can load (lower case); empty: the game needs no files.
    static let assetExtensions: Set<String> = ["iso", "gcm", "rvz", "ciso"]

    let documents: URL = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
    /// Reopens the launcher (set by the launcher, which has the scene actions): with the
    /// launcher's own value, so visionOS brings back the one there is rather than open a second.
    var showLauncher: (() -> Void)?
    private var watchdog: Timer?
    private var quitRequested = false

    init() {
        // Test runs: LMVR_IMMERSION=full|progressive|window picks the mode (with
        // LMVR_AUTO_PLAY=1, a headless run of it; the launcher can't be tapped there).
        let requested = ProcessInfo.processInfo.environment["LMVR_IMMERSION"].flatMap(Immersion.init(rawValue:))
            ?? UserDefaults.standard.string(forKey: Self.immersionKey).flatMap(Immersion.init(rawValue:))
        immersion = requested.flatMap { Self.offeredModes.contains($0) ? $0 : nil } ?? Self.offeredModes.first ?? .full
        roomBehindMenus = UserDefaults.standard.object(forKey: Self.roomBehindMenusKey) as? Bool ?? true
        foveated = UserDefaults.standard.bool(forKey: Self.foveatedKey)
        // Decide per game: if the game's audio listener already follows the headset (most VR
        // mods move it with the HMD pose), visionOS's head-tracked soundstage turns every sound a
        // second time, so bypass it. Bypassed audio is anchored to no window, so it also keeps
        // playing once the launcher closes. If the game's audio is NOT head-relative, use
        // .headTracked(.automatic, anchoringStrategy: .front) instead (the default anchoring
        // follows the launcher and goes silent when it closes).
        do {
            try AVAudioSession.sharedInstance().setIntendedSpatialExperience(.bypassed)
        } catch {
            print("[LMVR] setIntendedSpatialExperience failed: \(error)")
        }
        GameAudio.observeInterruptions()
        removeStalePartials()
        refreshAsset()
    }

    var canPlay: Bool {
        guard phase == .idle, !importing else { return false }
        if Self.assetExtensions.isEmpty { return true }
        return asset != nil && !checkingAsset && assetProblem == nil
    }

    var assetReady: Bool {
        Self.assetExtensions.isEmpty || (asset != nil && assetProblem == nil && !checkingAsset)
    }

    // MARK: Game files

    /// The newest matching file in Documents (Files shows the folder as On My Apple Vision Pro ›
    /// Luigi's Mansion VR), kept out of iCloud backups (the player can copy it in again; saves
    /// beside it are backed up), then checked (Port.validate).
    func refreshAsset() {
        let keys: [URLResourceKey] = [.contentModificationDateKey, .isRegularFileKey]
        let files = (try? FileManager.default.contentsOfDirectory(
            at: documents, includingPropertiesForKeys: keys, options: [.skipsHiddenFiles])) ?? []
        let matches = files.filter { Self.assetExtensions.contains($0.pathExtension.lowercased()) }
        for var file in matches {
            var values = URLResourceValues()
            values.isExcludedFromBackup = true
            try? file.setResourceValues(values)
        }
        asset = matches.max { lhs, rhs in
            let l = (try? lhs.resourceValues(forKeys: Set(keys)).contentModificationDate) ?? .distantPast
            let r = (try? rhs.resourceValues(forKeys: Set(keys)).contentModificationDate) ?? .distantPast
            return l < r
        }
        checkAsset()
    }

    /// Asks the port whether the files are ones the game plays, before Play: a wrong one would
    /// otherwise fail inside the space.
    private func checkAsset() {
        guard let asset else {
            assetProblem = nil
            checkingAsset = false
            return
        }
        checkingAsset = true
        Task.detached(priority: .userInitiated) {
            let problem = Port.validate(asset)
            await MainActor.run {
                // Only the answer for the file still chosen counts.
                guard self.asset == asset else { return }
                self.assetProblem = problem
                self.checkingAsset = false
            }
        }
    }

    /// Copies a picked file into Documents, where the game reads it in place. AirDrop and "Open
    /// with" deliver into Documents/Inbox: those are moved (a rename). Anything else is copied to
    /// a .partial file, which is never picked, and renamed once whole, so an interrupted copy
    /// can't leave a truncated file behind.
    func importAsset(from url: URL) {
        guard !importing else { return }
        importing = true
        importProgress = nil
        importFailure = nil
        let documents = self.documents
        let destination = documents.appendingPathComponent(url.lastPathComponent)
        // Keep copying if the wearer looks away; if background time runs out, the task ends so
        // visionOS suspends the app rather than ending it.
        importTask = UIApplication.shared.beginBackgroundTask(withName: "Import") { [weak self] in
            MainActor.assumeIsolated { self?.endImportTask() }
        }
        Task.detached(priority: .userInitiated) {
            let scoped = url.startAccessingSecurityScopedResource()
            defer { if scoped { url.stopAccessingSecurityScopedResource() } }
            let failure: String?
            do {
                if url.resolvingSymlinksInPath().standardizedFileURL != destination.resolvingSymlinksInPath().standardizedFileURL {
                    let fileManager = FileManager.default
                    let staged: URL
                    if Self.isInside(url, documents) {
                        staged = url
                    } else {
                        staged = destination.appendingPathExtension(Self.partialExtension)
                        try? fileManager.removeItem(at: staged)
                        do {
                            try Self.copy(url, to: staged) { fraction in
                                Task { @MainActor in self.importProgress = fraction }
                            }
                        } catch {
                            try? fileManager.removeItem(at: staged)
                            throw error
                        }
                    }
                    try? fileManager.removeItem(at: destination)
                    try fileManager.moveItem(at: staged, to: destination)
                    // Copies keep the source's date, and the newest file plays.
                    var imported = destination
                    var values = URLResourceValues()
                    values.contentModificationDate = Date()
                    try? imported.setResourceValues(values)
                }
                failure = nil
            } catch {
                failure = error.localizedDescription
            }
            await MainActor.run {
                self.importing = false
                self.importProgress = nil
                self.importFailure = failure.map { "The file didn't copy: \($0)" }
                self.refreshAsset()
                self.endImportTask()
            }
        }
    }

    /// A file that arrived while the game is open or another import runs: not brought in (the
    /// game reads its files in place), and AirDrop's copy in Documents/Inbox isn't left unseen.
    func declineImport(of url: URL, because reason: String) {
        importFailure = reason
        if Self.isInside(url, documents.appendingPathComponent("Inbox", isDirectory: true)) {
            try? FileManager.default.removeItem(at: url)
        }
    }

    /// Copies in 8 MB pieces, reporting progress (FileManager says nothing until it's done).
    nonisolated private static func copy(_ source: URL, to destination: URL,
                                         progress: @escaping @Sendable (Double) -> Void) throws {
        let total = (try? source.resourceValues(forKeys: [.fileSizeKey]).fileSize).map(Double.init) ?? 0
        guard FileManager.default.createFile(atPath: destination.path, contents: nil) else {
            throw CocoaError(.fileWriteUnknown, userInfo: [NSFilePathErrorKey: destination.path])
        }
        let reader = try FileHandle(forReadingFrom: source)
        defer { try? reader.close() }
        let writer = try FileHandle(forWritingTo: destination)
        defer { try? writer.close() }
        var copied = 0.0
        var reported = -1
        // A pool per piece, or every piece waits for the copy's end to be freed.
        while try autoreleasepool(invoking: { () throws -> Bool in
            guard let chunk = try reader.read(upToCount: 8 << 20), !chunk.isEmpty else { return false }
            try writer.write(contentsOf: chunk)
            copied += Double(chunk.count)
            if total > 0, Int(copied / total * 100) != reported {
                reported = Int(copied / total * 100)
                progress(min(copied / total, 1))
            }
            return true
        }) {}
        try writer.synchronize()
    }

    /// Symlinks resolved: a headset may hand AirDrop's file over as /private/var/... while the
    /// app's own folders read /var/....
    nonisolated private static func isInside(_ url: URL, _ folder: URL) -> Bool {
        let path = url.resolvingSymlinksInPath().standardizedFileURL.path
        let base = folder.resolvingSymlinksInPath().standardizedFileURL.path
        return path.hasPrefix(base.hasSuffix("/") ? base : base + "/")
    }

    private var importTask: UIBackgroundTaskIdentifier = .invalid

    private func endImportTask() {
        guard importTask != .invalid else { return }
        UIApplication.shared.endBackgroundTask(importTask)
        importTask = .invalid
    }

    nonisolated static let partialExtension = "partial"

    /// .partial copies left by an import the app was closed during.
    private func removeStalePartials() {
        let files = (try? FileManager.default.contentsOfDirectory(
            at: documents, includingPropertiesForKeys: nil, options: [.skipsHiddenFiles])) ?? []
        for file in files where file.pathExtension == Self.partialExtension {
            try? FileManager.default.removeItem(at: file)
        }
    }

    // MARK: Play, pause, resume, quit

    func markOpening() {
        if phase == .idle {
            phase = .opening
            space.style = styleForPlay()
        }
    }

    func openingFailed(_ reason: String) {
        phase = .failed(message: reason)
    }

    /// Resume, from the launcher: the same space (or the window) opens again and the game carries
    /// on (attach, windowAppeared).
    func markResuming() {
        if phase == .paused {
            phase = .resuming
        }
    }

    /// The space didn't open again: still paused, and the launcher says why.
    func resumeFailed(_ reason: String) {
        if phase == .resuming {
            phase = .paused
            message = reason
        }
    }

    /// Quit, from the paused launcher (Port.quitsFromPause): the game saves and returns, and the
    /// app ends with it (checkGame).
    func quitFromPause() {
        guard phase == .paused else { return }
        quitRequested = true
        lmvr_visionos_request_quit()
    }

    private func bringBackLauncher() {
        launcherReturns += 1
        showLauncher?()
    }

    /// Called from the CompositorLayer closure once the immersive space has a layer renderer.
    /// The bridge retains it.
    nonisolated func attach(_ layerRenderer: LayerRenderer) {
        lmvr_visionos_set_layer_renderer(Unmanaged.passUnretained(layerRenderer).toOpaque())
        nonisolated(unsafe) let renderer = layerRenderer
        Task { @MainActor in
            // visionOS's look-and-pinch: each pinch carries the ray from the eyes to where the
            // user looked; the provider turns it into a pointer.
            renderer.onSpatialEvent = { events in
                for event in events { Self.forward(event) }
            }
            if self.phase == .resuming {
                // The bridge took the new layer above; the game carries on where it was.
                self.message = ""
                self.phase = .running
            } else {
                self.startGame()
            }
        }
    }

    private nonisolated static func forward(_ event: SpatialEventCollection.Event) {
        let phase: Int32
        switch event.phase {
        case .active: phase = 0
        case .ended: phase = 1
        default: phase = 2
        }
        let chirality: Int32
        switch event.chirality {
        case .left?: chirality = 1
        case .right?: chirality = 2
        default: chirality = 0
        }
        let id = UInt64(bitPattern: Int64(event.id.hashValue))
        let ray = event.selectionRay
        let pose = event.inputDevicePose?.pose3D.position
        lmvr_visionos_spatial_event(
            id, phase, chirality, ray != nil,
            Float(ray?.origin.x ?? 0), Float(ray?.origin.y ?? 0), Float(ray?.origin.z ?? 0),
            Float(ray?.direction.x ?? 0), Float(ray?.direction.y ?? 0), Float(ray?.direction.z ?? 0),
            pose != nil, Float(pose?.x ?? 0), Float(pose?.y ?? 0), Float(pose?.z ?? 0))
    }

    /// The game window appeared: the game starts flat (no layer renderer, so no VR) and hands its
    /// frames to the window, which paces it; or, resumed, carries on.
    func windowAppeared() {
        gameWindowShowing = true
        if phase == .resuming {
            lmvr_visionos_set_paused(false)
            message = ""
            phase = .running
            return
        }
        guard phase == .opening || phase == .idle else { return }
        // The game's listener follows its own camera, not the user's head: let visionOS place the
        // sound at the window.
        do {
            try AVAudioSession.sharedInstance().setIntendedSpatialExperience(
                .headTracked(soundStageSize: .automatic, anchoringStrategy: .automatic))
        } catch {
            print("[LMVR] window audio: setIntendedSpatialExperience failed: \(error)")
        }
        lmvr_visionos_set_window_mode(true)
        startGame()
    }

    /// The game window closed. Port.windowClosePauses: the game holds and the launcher comes
    /// back with Resume (an engine that can't quit from here, as SHAR's). Otherwise the game saves
    /// and returns, and the app ends with it; the request repeats from the watchdog until the
    /// game returns (one sent before its event loop is up is dropped).
    func windowDisappeared() {
        gameWindowShowing = false
        guard phase == .running else {
            if phase == .opening { quitRequested = true }
            return
        }
        if Port.windowClosePauses {
            lmvr_visionos_set_paused(true)
            // Off, so the next window's mirror is sent every texture again.
            lmvr_visionos_set_mirror_enabled(false)
            phase = .paused
            bringBackLauncher()
        } else {
            quitRequested = true
            lmvr_visionos_request_quit()
        }
    }

    private func startGame() {
        guard phase == .opening || phase == .idle else { return }
        lmvr_visionos_set_asset_path(asset?.path)
        // Menus over the room only in the mixed space (a progressive portal shows black where
        // frames are transparent). A mixed space has no visionOS movement boundary, so the game
        // supplies one there.
        lmvr_visionos_set_room_behind_menus(playsMixed)
        lmvr_visionos_set_safety_boundary(playsMixed)
        if lmvr_visionos_start_game() {
            phase = .running
            startWatchdog()
        } else {
            phase = .failed(message: String(cString: lmvr_visionos_last_error()))
            // The launcher closed when the space opened; bring it back to say why.
            bringBackLauncher()
        }
    }

    /// Notices the game returning, the space closing (the Digital Crown), and a quit request
    /// still to deliver.
    private func startWatchdog() {
        watchdog?.invalidate()
        watchdog = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.checkGame() }
        }
    }

    private func checkGame() {
        if !lmvr_visionos_game_running() {
            watchdog?.invalidate()
            watchdog = nil
            let code = lmvr_visionos_exit_code()
            if code == 0 {
                // A clean quit: the game saved on its way out and can't run twice in one process,
                // so the app ends too. Opening it again starts fresh at the launcher.
                exit(0)
            }
            phase = .ended(exitCode: code)
            bringBackLauncher()
        } else if quitRequested {
            lmvr_visionos_request_quit()
        } else if phase == .running && !playsWindow && lmvr_visionos_layer_invalidated() {
            if Port.crownPauses && lmvr_visionos_can_resume() {
                // The Digital Crown closed the space. The game holds where it is (waiting for a
                // new layer, its clock and sound paused); the launcher offers Resume. If visionOS
                // doesn't show it now, opening the app from Home does.
                phase = .paused
                bringBackLauncher()
            } else {
                // Closed before the game was ever seen in it, or a port that can't hold: quit.
                quitRequested = true
                lmvr_visionos_request_quit()
            }
        }
    }
}

/// The immersive spaces' styles, set at Play before the space opens.
@Observable
final class ImmersionSpaceStyle {
    static let shared = ImmersionSpaceStyle()
    /// The full space's: .full, or .mixed for the room around menus.
    var style: any ImmersionStyle = .full
    /// The progressive space's, which has only the one.
    var progressiveStyle: any ImmersionStyle = GameModel.progressiveStyle
}

/// Siri, calls and alarms interrupt the app's audio: the game's sound pauses and comes back once
/// the session is active again. The end isn't always announced, so coming back to the foreground
/// retries too (SHAR).
@MainActor
enum GameAudio {
    private static var interrupted = false
    private static var observers: [NSObjectProtocol] = []

    static func observeInterruptions() {
        guard observers.isEmpty else { return }
        let center = NotificationCenter.default
        observers.append(center.addObserver(forName: AVAudioSession.interruptionNotification,
                                            object: AVAudioSession.sharedInstance(), queue: .main) { note in
            let raw = note.userInfo?[AVAudioSessionInterruptionTypeKey] as? UInt
            let type = raw.flatMap(AVAudioSession.InterruptionType.init(rawValue:))
            MainActor.assumeIsolated {
                if type == .began { began() } else if type == .ended { resume(attempts: 5) }
            }
        })
        observers.append(center.addObserver(forName: UIApplication.didBecomeActiveNotification,
                                            object: nil, queue: .main) { _ in
            MainActor.assumeIsolated { resume(attempts: 5) }
        })
    }

    private static func began() {
        guard !interrupted else { return }
        interrupted = true
        print("[LMVR] audio interrupted")
        lmvr_visionos_set_audio_interrupted(true)
    }

    private static func resume(attempts: Int) {
        guard interrupted else { return }
        do {
            try AVAudioSession.sharedInstance().setActive(true)
            interrupted = false
            print("[LMVR] audio interruption over")
            lmvr_visionos_set_audio_interrupted(false)
        } catch {
            guard attempts > 1 else { return }
            Task { @MainActor in
                try? await Task.sleep(for: .seconds(1))
                resume(attempts: attempts - 1)
            }
        }
    }
}
