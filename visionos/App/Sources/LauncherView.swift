import GameController
import SwiftUI
import TrevorbiltKit
import UniformTypeIdentifiers

/// The launcher: Play (the game files, how to play, Play), Controls, Ports (the AVP Ports Index)
/// and About, in TrevorbiltKit's launcher every Trevorbilt port shares. What you'll play with sits
/// in an ornament under the window. It closes when the game opens, and comes back on Play with
/// Resume when the Digital Crown pauses Full or Progressive (Port's pause capabilities). The
/// port's own content is in PortContent.swift.
struct LauncherView: View {
    enum Tab: String, CaseIterable {
        case play, controls, ports, about
    }

    @EnvironmentObject private var model: GameModel
    @Environment(\.openImmersiveSpace) private var openImmersiveSpace
    @Environment(\.dismissImmersiveSpace) private var dismissImmersiveSpace
    @Environment(\.dismissWindow) private var dismissWindow
    @Environment(\.openWindow) private var openWindow
    @Environment(\.scenePhase) private var scenePhase
    @State private var tab = Tab(rawValue: TestHooks.value("LMVR_TEST_TAB") ?? "") ?? .play
    @State private var picking = false
    @State private var pickingFromSheet = false
    @State private var managing = TestHooks.value("LMVR_TEST_SHEET") == "manage"
    @State private var advanced = TestHooks.value("LMVR_TEST_SHEET") == "advanced"
    @State private var inputs = InputMonitor(testOverride: TestHooks.value("LMVR_TEST_INPUTS"))
    @State private var ports = PortsIndex(testFeed: TestHooks.value("LMVR_TEST_FEED").map { URL(fileURLWithPath: $0) },
                                          offline: TestHooks.value("LMVR_TEST_OFFLINE") == "1")

    var body: some View {
        TabView(selection: $tab) {
            playTab
                .tabItem { Label("Play", systemImage: "play.fill") }
                .tag(Tab.play)
            ControlsView()
                .tabItem { Label("Controls", systemImage: "gamecontroller.fill") }
                .tag(Tab.controls)
            // Text only once the game is loaded (it runs once per process, its memory first).
            PortsBrowser(index: ports, ownID: Port.indexID, buildCommit: BuildInfo.current.commit,
                         buildDirty: BuildInfo.current.dirty,
                         opening: TestHooks.value("LMVR_TEST_SHEET").flatMap { $0.hasPrefix("port:") ? String($0.dropFirst(5)) : nil },
                         showsMedia: model.phase == .idle)
                .tabItem { Label("Ports", systemImage: "square.grid.2x2.fill") }
                .tag(Tab.ports)
            AboutView(content: Port.about, diagnostics: diagnostics,
                      sheet: TestHooks.value("LMVR_TEST_SHEET").flatMap(AboutView.Sheet.init(rawValue:)))
                .tabItem { Label("About", systemImage: "info.circle.fill") }
                .tag(Tab.about)
        }
        // A glanceable glass card, the same size on every tab (the window hugs it: .contentSize).
        .frame(width: 640, height: 600)
        // visionOS turns a controller's buttons into pinches on whatever window the player looks
        // at. While the game's window is on screen, a look at the launcher mustn't take the
        // controller from it; otherwise (before Play, paused) the controller works the launcher.
        // Keyed on the window showing, never on the game running.
        .handlesGameControllerEvents(matching: model.gameWindowShowing ? .gamepad : [])
        // Brought back by a pause, the game window closing, or to say why the game stopped: on
        // Play, where Resume and the reason are.
        .onChange(of: model.launcherReturns) { tab = .play }
        // What you'll play with, centred under the window, while Play is showing.
        .ornament(visibility: tab == .play && model.assetReady && !model.importing && showsInputs ? .visible : .hidden,
                  attachmentAnchor: .scene(.bottom), contentAlignment: .top) {
            InputStatus(monitor: inputs, handsUsed: !model.playsWindow, handsUnusedReason: "Not used on your table",
                        combinedSenseTitle: model.playsWindow ? "Sense (as a gamepad)" : nil,
                        verdict: Port.verdict(model.immersion, inputs))
                .padding(.top, 16)
        }
        .fileImporter(isPresented: $picking, allowedContentTypes: [.data]) { result in
            if case let .success(url) = result { importAsset(url) }
        }
        // AirDrop or the share sheet's "Open with Luigi's Mansion VR".
        .onOpenURL { url in importAsset(url) }
        .sheet(isPresented: $managing) { manageSheet }
        .sheet(isPresented: $advanced) {
            SheetContent(done: { advanced = false }) {
                TrevorbiltHeading("advanced ", bold: "options", size: 22)
                Port.AdvancedOptions(model: model)
            }
        }
        .onAppear {
            let openWindow = openWindow
            model.showLauncher = { openWindow(id: GameModel.launcherWindowID, value: GameModel.launcherWindowID) }
        }
        // The guide opens on what's connected, for the selected mode (not over a test run's page).
        .onChange(of: tab) { _, tab in
            if tab == .controls && TestHooks.value("LMVR_TEST_CONTROLS") == nil { showGuide() }
        }
        // Files copied in with the Files app while the launcher was away, or hand tracking allowed
        // in Settings.
        .onChange(of: scenePhase) { _, phase in
            guard phase == .active else { return }
            if !model.importing && model.phase == .idle { model.refreshAsset() }
            Task { await inputs.refresh() }
        }
        .task {
            await inputs.start()
            if tab == .controls && TestHooks.value("LMVR_TEST_CONTROLS") == nil { showGuide() }
        }
        .task { await runTestHooks() }
    }

    // MARK: Play

    private var playTab: some View {
        VStack(spacing: 0) {
            ScrollView {
                VStack(spacing: 14) {
                    TrevorbiltHeader(title: Port.title, boldTitle: Port.boldTitle, subtitle: Port.subtitle)
                    if !GameModel.assetExtensions.isEmpty { filesStatus }
                    if model.assetReady && !model.importing {
                        switch model.phase {
                        case .idle: playAs
                        case .paused, .resuming: paused
                        default: EmptyView()
                        }
                    }
                }
                .padding(.horizontal, 24)
                .padding(.top, 20)
                .padding(.bottom, 8)
                .frame(maxWidth: .infinity)
            }
            VStack(spacing: 6) {
                if model.phase == .paused || model.phase == .resuming {
                    HStack(spacing: 12) {
                        Button("Resume") { Task { await resume() } }
                            .buttonStyle(TrevorbiltPrimaryButtonStyle(width: 200))
                        if Port.quitsFromPause {
                            Button("Quit", role: .destructive) { model.quitFromPause() }
                                .buttonStyle(TrevorbiltSecondaryButtonStyle())
                        }
                    }
                    .disabled(model.phase != .paused)
                } else {
                    Button("Play") { Task { await play() } }
                        .buttonStyle(TrevorbiltPrimaryButtonStyle(width: 240))
                        .disabled(!model.canPlay)
                }
                if let line = statusLine {
                    Text(line).font(.tbBody(12)).foregroundStyle(.white.opacity(0.8))
                        .multilineTextAlignment(.center)
                }
                if let problem = gameProblem {
                    TrevorbiltCard { StatusHeadline(.problem, title: problem.title, detail: problem.detail) }
                        .frame(maxWidth: 460)
                }
            }
            .padding(.top, 6)
            .padding(.bottom, 20)
        }
    }

    /// The game files at a glance: one capsule once they're in and checked (Manage holds the
    /// rest), or the way to bring them.
    @ViewBuilder private var filesStatus: some View {
        if model.importing {
            StatusCapsule {
                if let progress = model.importProgress {
                    ProgressRing(progress: progress)
                } else {
                    ProgressView().controlSize(.small)
                }
                Text("Copying the \(Port.filesNoun)\(model.importProgress.map { " · \(Int(($0 * 100).rounded()))%" } ?? "…")")
            }
        } else if let file = model.asset, model.checkingAsset {
            StatusCapsule {
                ProgressView().controlSize(.small)
                Text("Checking \(file.lastPathComponent)…").lineLimit(1).truncationMode(.middle)
            }
        } else if let file = model.asset, let problem = model.assetProblem {
            TrevorbiltCard(padding: 20) {
                VStack(alignment: .leading, spacing: 12) {
                    StatusHeadline(.problem, title: "\(file.lastPathComponent) won't play", detail: problem)
                    if model.phase == .idle {
                        Button("Import another \(Port.filesNoun)") { picking = true }
                            .buttonStyle(TrevorbiltSecondaryButtonStyle())
                    }
                }
            }
            .frame(maxWidth: 560)
        } else if let file = model.asset {
            VStack(spacing: 6) {
                StatusCapsule {
                    Image(systemName: "checkmark.circle.fill")
                        .symbolRenderingMode(.hierarchical)
                        .font(.system(size: 18))
                        .accessibilityHidden(true)
                    Text("Ready · \(file.lastPathComponent)").lineLimit(1).truncationMode(.middle)
                } action: {
                    Button("Manage") { managing = true }
                }
                if model.importFailure != nil {
                    HStack(spacing: 6) {
                        Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(Trevorbilt.orangeTint)
                            .accessibilityHidden(true)
                        Text("The last import didn't work. Manage has the details.")
                    }
                    .font(.tbBody(12, weight: .medium))
                }
            }
        } else {
            TrevorbiltCard(padding: 28, alignment: .center) {
                VStack(spacing: 8) {
                    Image(systemName: Port.filesSymbol)
                        .symbolRenderingMode(.hierarchical)
                        .font(.system(size: 28))
                        .accessibilityHidden(true)
                    Text("Bring your copy of the game").font(.tbHeader(18, bold: true, relativeTo: .title3))
                    Text(Port.filesHelp)
                        .font(.tbBody(13))
                        .foregroundStyle(.white.opacity(0.8))
                        .multilineTextAlignment(.center)
                        .fixedSize(horizontal: false, vertical: true)
                        .frame(maxWidth: 460)
                    Button("Import \(Port.filesNoun)") { picking = true }
                        .buttonStyle(TrevorbiltPrimaryButtonStyle(width: 240))
                        .padding(.top, 4)
                    if let failure = model.importFailure {
                        StatusHeadline(.problem, title: "That didn't work", detail: failure)
                            .frame(maxWidth: 460)
                    }
                }
            }
            .frame(maxWidth: 560)
        }
    }

    private var playAs: some View {
        VStack(spacing: 8) {
            TrevorbiltSectionTitle("Play as")
            HStack(spacing: 12) {
                ForEach(GameModel.offeredModes) { mode in
                    if mode == .window {
                        // On your table: a volume, drawn as one (TableModeCard).
                        TableModeCard(name: mode.title, line: Port.line(mode),
                                      selected: model.immersion == mode) { model.immersion = mode }
                    } else {
                        ModeCard(Self.playMode(mode), name: mode.title, line: Port.line(mode),
                                 selected: model.immersion == mode) { model.immersion = mode }
                    }
                }
            }
            .fixedSize(horizontal: false, vertical: true)
            // Only what the selected mode has (hidden, not greyed): the room around menus in Full;
            // Advanced (foveation) in Full and Progressive.
            if !model.playsWindow {
                HStack(spacing: 10) {
                    if model.immersion == .full && Port.offersRoomAroundMenus {
                        Toggle(isOn: $model.roomBehindMenus) {
                            Text(Port.roomToggleTitle).font(.tbBody(13, weight: .medium))
                        }
                        .tint(Trevorbilt.orange)
                        .fixedSize()
                        .padding(.leading, 16)
                        .padding(.trailing, 8)
                        .frame(minHeight: 44)
                        .background(.white.opacity(0.1), in: .capsule)
                    }
                    Button("Advanced \u{203A}") { advanced = true }
                        .buttonStyle(TrevorbiltSecondaryButtonStyle())
                }
            }
            Text(Port.note(model.immersion, roomBehindMenus: model.roomBehindMenus))
                .font(.tbBody(12))
                .foregroundStyle(.white.opacity(0.8))
                .multilineTextAlignment(.center)
                .fixedSize(horizontal: false, vertical: true)
                .frame(maxWidth: 520)
        }
    }

    private static func playMode(_ mode: GameModel.Immersion) -> PlayMode {
        switch mode {
        case .full: .full
        case .progressive: .progressive
        case .window: .window
        }
    }

    private var paused: some View {
        VStack(spacing: 6) {
            StatusCapsule {
                Image(systemName: "pause.circle.fill")
                    .symbolRenderingMode(.hierarchical)
                    .font(.system(size: 18))
                    .accessibilityHidden(true)
                Text(model.phase == .resuming ? "Back to the game…" : "Paused in \(model.immersion.title)")
            }
            Text(Port.quitsFromPause ? Port.pausedNote : "Resume carries on where you were.")
                .font(.tbBody(12))
                .foregroundStyle(.white.opacity(0.8))
                .multilineTextAlignment(.center)
                .fixedSize(horizontal: false, vertical: true)
        }
    }

    /// Under the button: why Play can't be pressed, or what the game is doing.
    private var statusLine: String? {
        switch model.phase {
        case .opening: return "Opening…"
        case .running: return model.playsWindow ? "Playing on your table." : "Playing. Press the Digital Crown to pause."
        case .ended, .failed: return "Close Luigi's Mansion VR and open it again to play."
        case .paused, .resuming: return model.message.isEmpty ? nil : model.message
        case .idle: break
        }
        if GameModel.assetExtensions.isEmpty { return nil }
        if model.importing { return "Play is ready as soon as the \(Port.filesNoun) is in." }
        if model.checkingAsset { return "Checking the \(Port.filesNoun)…" }
        if model.asset == nil { return "Bring your \(Port.filesNoun) first." }
        if model.assetProblem != nil { return "Import a \(Port.filesNoun) the game plays." }
        return nil
    }

    private var gameProblem: (title: String, detail: String)? {
        switch model.phase {
        case let .ended(code): ("The game stopped", "It ended with an error (code \(code)).")
        case let .failed(reason): ("The game didn't open", reason)
        default: nil
        }
    }

    /// The input ornament has something to say before Play and while paused.
    private var showsInputs: Bool {
        switch model.phase {
        case .idle, .paused, .resuming: true
        default: false
        }
    }

    private var manageSheet: some View {
        SheetContent(done: { managing = false }) {
            TrevorbiltHeading("your ", bold: Port.filesNoun, size: 22)
            TrevorbiltCard {
                VStack(alignment: .leading, spacing: 10) {
                    if model.importing {
                        StatusHeadline(.working, title: "Copying the \(Port.filesNoun)", detail: "Keep the app open until this finishes.")
                        if let progress = model.importProgress { ProgressView(value: progress).tint(.white) }
                    } else if let file = model.asset {
                        if let problem = model.assetProblem {
                            StatusHeadline(.problem, title: "\(file.lastPathComponent) won't play", detail: problem)
                        } else {
                            StatusHeadline(.ready, title: file.lastPathComponent,
                                           detail: "\(Self.size(of: file).map { "\($0), in" } ?? "In") Files › On My Apple Vision Pro › Luigi's Mansion VR. The newest one there plays. It's kept out of iCloud backups; your saves aren't.")
                        }
                    } else {
                        StatusHeadline(.missing, title: "Nothing here yet", detail: "Import it from Play, or AirDrop it to this Vision Pro.")
                    }
                    if let failure = model.importFailure {
                        StatusHeadline(.problem, title: "The last import didn't work", detail: failure)
                    }
                }
            }
            if !model.importing && model.phase == .idle {
                Button(model.asset == nil ? "Import \(Port.filesNoun)" : "Import another \(Port.filesNoun)") { pickingFromSheet = true }
                    .buttonStyle(TrevorbiltSecondaryButtonStyle())
            }
        }
        // A sheet can't show the launcher's own importer over itself.
        .fileImporter(isPresented: $pickingFromSheet, allowedContentTypes: [.data]) { result in
            if case let .success(url) = result { importAsset(url) }
        }
    }

    private static func size(of file: URL) -> String? {
        (try? file.resourceValues(forKeys: [.fileSizeKey]).fileSize)
            .map { ByteCountFormatter.string(fromByteCount: Int64($0), countStyle: .file) }
    }

    /// The guide, on what's connected, for the selected mode.
    private func showGuide() {
        let guide = ControlsGuide.shared
        guide.windowView = model.playsWindow
        guide.input = .gamepad  // the only input supported (Port.controlInputs)
    }

    private func importAsset(_ url: URL) {
        tab = .play
        // Never under a game that's open (it reads its files in place).
        guard model.phase == .idle else {
            model.declineImport(of: url, because: "The game's open, so the file wasn't brought in. Quit, then bring it in again.")
            return
        }
        guard !model.importing else {
            model.declineImport(of: url, because: "One at a time: bring this one in once the copy that's running is done.")
            return
        }
        model.importAsset(from: url)
    }

    // MARK: Scenes

    private func play() async {
        // The Ports tab's pictures go now: none stays in memory beside the game.
        PortsBrowser.letGoOfPictures()
        model.markOpening()
        if model.playsWindow {
            // The game window starts the game when it appears, and closes this one.
            openWindow(id: GameModel.windowSceneID)
            return
        }
        switch await openImmersiveSpace(id: model.spaceIDForPlay) {
        case .opened:
            // Out of the way while you play. Only once the space is open: closing the last window
            // earlier (or after a failed open) aborts SwiftUI.
            dismissWindow(id: GameModel.launcherWindowID, value: GameModel.launcherWindowID)
            closeSpaceForTest()
        case .userCancelled:
            model.openingFailed("The immersive space was cancelled.")
        default:
            model.openingFailed("visionOS couldn't open the immersive space.")
        }
    }

    /// The same space (or the window) opens again; the game carries on once it has the new layer
    /// (GameModel.attach) or the window (GameModel.windowAppeared). The launcher goes, as at Play.
    private func resume() async {
        guard model.phase == .paused else { return }
        model.markResuming()
        if model.playsWindow {
            openWindow(id: GameModel.windowSceneID)
            return
        }
        switch await openImmersiveSpace(id: model.spaceIDForPlay) {
        case .opened:
            dismissWindow(id: GameModel.launcherWindowID, value: GameModel.launcherWindowID)
        case .userCancelled:
            model.resumeFailed("The immersive space was cancelled.")
        default:
            model.resumeFailed("visionOS couldn't open the immersive space again.")
        }
    }

    // MARK: Test hooks

    /// LMVR_TEST_CROWN=<seconds>: closes the immersive space that long after it opened, as the
    /// Digital Crown does (the layer is invalidated, the game holds, the launcher returns with
    /// Resume). Headless runs can't press the Simulator's Home button. Simulator builds only.
    private func closeSpaceForTest() {
        guard let value = TestHooks.value("LMVR_TEST_CROWN"), let seconds = Double(value) else { return }
        let dismiss = dismissImmersiveSpace
        Task {
            try? await Task.sleep(for: .seconds(seconds))
            await dismiss()
        }
    }

    private func runTestHooks() async {
        if let value = TestHooks.value("LMVR_TEST_CONTROLS") {
            let parts = value.split(separator: ",").map(String.init)
            let guide = ControlsGuide.shared
            guide.windowView = parts.contains("window")
            guide.input = ControlsView.Input.allCases.first { parts.contains($0.rawValue.lowercased()) }
                ?? (guide.windowView ? .gamepad : .hands)
            guide.page = parts.first { $0.hasPrefix("page=") }.flatMap { Int($0.dropFirst(5)) } ?? 0
            tab = .controls
        }
        if let path = TestHooks.value("LMVR_TEST_IMPORT") {
            importAsset(URL(fileURLWithPath: path))
        }
        // Headless runs: SIMCTL_CHILD_LMVR_AUTO_PLAY=1 (or the same variable in a devicectl
        // launch) presses Play. Synthesized taps don't reach visionOS Simulator windows.
        if ProcessInfo.processInfo.environment["LMVR_AUTO_PLAY"] == "1", model.phase == .idle {
            await play()
        }
        // LMVR_TEST_PAUSED=resume@5 (or quit@5): Resume (Quit) that long after a pause brings
        // the launcher back. The Simulator's Home button is the Digital Crown.
        if model.phase == .paused, let test = TestHooks.value("LMVR_TEST_PAUSED") {
            let parts = test.split(separator: "@")
            let delay = parts.count > 1 ? Double(parts[1]) ?? 5 : 5
            do { try await Task.sleep(for: .seconds(delay)) } catch { return }
            guard model.phase == .paused else { return }
            if parts.first == "quit" { model.quitFromPause() } else { await resume() }
        }
    }

    // MARK: About

    private var diagnostics: [(String, String)] {
        let os = ProcessInfo.processInfo.operatingSystemVersion
        func controller(_ c: InputMonitor.Controller?) -> String {
            c.map { "\($0.name)\($0.battery.map { ", \(Int(($0 * 100).rounded()))%" } ?? "")" } ?? "None"
        }
        let phase = switch model.phase {
        case .idle: "Not started"
        case .opening: "Opening"
        case .running: "Running"
        case .paused: "Paused"
        case .resuming: "Resuming"
        case let .ended(code): "Ended (code \(code))"
        case .failed: "Didn't open"
        }
        return [("Build", BuildInfo.current.summary),
                ("visionOS", "\(os.majorVersion).\(os.minorVersion).\(os.patchVersion)"),
                ("Game files", model.asset.map { "\($0.lastPathComponent)\(Self.size(of: $0).map { ", \($0)" } ?? "")" } ?? "None"),
                ("Play as", model.immersion.title),
                ("Game", phase),
                ("Hand tracking", ["Allowed", "Off in Settings", "Not asked yet", "Not available"][
                    [InputMonitor.Permission.allowed, .denied, .notAsked, .unavailable].firstIndex(of: inputs.hands) ?? 2]),
                ("Sense L", controller(inputs.senseLeft)),
                ("Sense R", controller(inputs.senseRight)),
                ("Gamepad", controller(inputs.gamepad))]
    }
}

/// One line of status in a glass capsule, with an optional button at its end.
private struct StatusCapsule<Label: View, Action: View>: View {
    private let label: Label
    private let action: Action

    init(@ViewBuilder label: () -> Label, @ViewBuilder action: () -> Action = { EmptyView() }) {
        self.label = label()
        self.action = action()
    }

    var body: some View {
        HStack(spacing: 10) {
            HStack(spacing: 8) { label }
                .font(.tbBody(14, weight: .medium, relativeTo: .body))
                .accessibilityElement(children: .combine)
            action.buttonStyle(TrevorbiltSecondaryButtonStyle())
        }
        .padding(.leading, 16)
        .padding(.trailing, Action.self == EmptyView.self ? 16 : 4)
        .padding(.vertical, 4)
        .frame(minHeight: 52)
        .background(.white.opacity(0.08), in: .capsule)
        .overlay { Capsule().strokeBorder(.white.opacity(0.1), lineWidth: 1) }
    }
}

/// How far an import has got, as a ring.
private struct ProgressRing: View {
    let progress: Double

    var body: some View {
        ZStack {
            Circle().stroke(.white.opacity(0.15), lineWidth: 3)
            Circle().trim(from: 0, to: progress).stroke(.white, style: StrokeStyle(lineWidth: 3, lineCap: .round))
                .rotationEffect(.degrees(-90))
        }
        .frame(width: 18, height: 18)
        .accessibilityHidden(true)
    }
}
