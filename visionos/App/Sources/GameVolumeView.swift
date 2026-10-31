import GameController
import Metal
import QuartzCore
import RealityKit
import SwiftUI
import Synchronization

/// "On your table": the mansion in a volume in the shared space, beside other apps, moved and
/// resized with visionOS's own controls. visionOS gives an app no head or controller pose outside a
/// Full Space, so the game plays flat, with its own camera and a gamepad. The scene mirror
/// (MirrorScene) rebuilds the game's 3D draws each frame as a world-locked set on the volume's
/// floor (placed by the base's src/recomp/volume.cpp, which sizes it to the room and moves it only
/// at a cut), and RealityKit renders it from the viewer's real eyes. 2D is never inside the set: the
/// HUD, text and full-screen pictures go on a panel at the volume's edge facing the viewer, and in
/// a cutscene the game's own shot shows there too, in a corner (picture in picture, a toggle).
/// From the window mode of the TPVR and Wind Waker ports (GameWindowView.swift).
struct GameVolumeView: View {
    @EnvironmentObject private var model: GameModel
    @Environment(\.dismissWindow) private var dismissWindow
    @Environment(\.openWindow) private var openWindow
    @Environment(\.scenePhase) private var scenePhase
    @State private var updates: EventSubscription?
    @State private var volume: GameVolume?
    @State private var waiting = true
    @Environment(\.windowClippingMargins) private var clippingMargins

    var body: some View {
        GeometryReader3D { geometry in
            RealityView { content, attachments in
                let volume: GameVolume
                do { volume = try await GameVolume() } catch {
                    print("[LMVR] the volume's materials failed to load: \(error)")
                    return
                }
                volume.firstFrame = { waiting = false }
                content.add(volume.root)
                if let note = attachments.entity(for: "waiting") {
                    volume.setWaitingNote(note)
                }
                volume.fit(to: content.convert(geometry.frame(in: .local), from: .local, to: .scene))
                // Each RealityKit update shows the newest finished frame (the game keeps its own time).
                updates = content.subscribe(to: SceneEvents.Update.self) { _ in volume.update() }
                self.volume = volume
            } update: { content, _ in
                volume?.fit(to: content.convert(geometry.frame(in: .local), from: .local, to: .scene))
            } attachments: {
                Attachment(id: "waiting") {
                    if waiting {
                        VStack(spacing: 8) {
                            ProgressView()
                            Text("The mansion is getting ready…")
                                .font(.title3)
                            Text("The first start prepares its shaders, which takes a few minutes.")
                                .font(.callout)
                                .foregroundStyle(.secondary)
                        }
                        .padding(24)
                        .glassBackgroundEffect()
                    }
                }
            }
            // The cutscene square moves with a pinch-drag: left, right, up and down on the panel's
            // face, never nearer or farther, and stays inside the volume. The only gesture: a tap
            // gesture before it (12ac2eb, there so that pinches stopped at the volume, which its
            // input target and collision box already do) took precedence over it.
            .gesture(DragGesture(minimumDistance: 0).targetedToAnyEntity()
                .onChanged { value in
                    guard let volume, GameVolume.isPip(value.entity) else { return }
                    volume.dragPip(to: value.convert(value.location3D, from: .local, to: volume.root),
                                   from: value.convert(value.startLocation3D, from: .local, to: volume.root))
                }
                .onEnded { value in
                    guard let volume, GameVolume.isPip(value.entity) else { return }
                    volume.dragPip(to: value.convert(value.location3D, from: .local, to: volume.root),
                                   from: value.convert(value.startLocation3D, from: .local, to: volume.root), ended: true)
                })
        }
        // The viewer walked round the table: the panel turns to face them, and the stick is
        // theirs (up: away from them), in quarter turns.
        .onVolumeViewpointChange { _, viewpoint in
            volume?.viewpoint = GameVolume.quarter(of: viewpoint.squareAzimuth)
        }
        .supportedVolumeViewpoints(.all)
        // visionOS turns a game controller's buttons into pinches on whatever the player looks at,
        // unless the view says it reads the controller itself (SHAR).
        .handlesGameControllerEvents(matching: .gamepad)
        .task {
            print("[LMVR] volume: clipping margins granted \(clippingMargins)")
        }
        .onAppear {
            let openWindow = openWindow
            model.showLauncher = { openWindow(id: GameModel.launcherWindowID, value: GameModel.launcherWindowID) }
            model.windowAppeared()
            if model.phase == .running {
                dismissWindow(id: GameModel.launcherWindowID, value: GameModel.launcherWindowID)
            }
        }
        .onDisappear {
            model.windowDisappeared()
        }
        .onChange(of: scenePhase) { _, phase in
            lmvr_visionos_set_paused(phase != .active)
        }
    }
}

/// What the volume shows, in metres about its centre (+y up, +z towards its front): the set (the
/// mirror's root, placed by each frame) and the panel on the face the viewer looks from.
@MainActor
final class GameVolume {
    let root = Entity()
    var firstFrame: (() -> Void)?
    static let pipName = "cutscene square"
    /// The side the viewer looks from: 0 front, 1 right, 2 back, 3 left (onVolumeViewpointChange).
    var viewpoint = 0 {
        didSet {
            if viewpoint != oldValue {
                placePanel()
                sendVolume()
                print("[LMVR] volume: seen from the \(["front", "right", "back", "left"][viewpoint])")
            }
        }
    }

    private let panel = ModelEntity()
    // The game's own shot in a cutscene: a square on the panel's face, under the HUD (dialogue stays
    // on top of it), shown by itself while the game's camera is a cutscene's. Its place on the face
    // (shares of the face's width and height from its centre) is remembered. A faint frame round it
    // says it can be taken hold of; under the viewer's gaze the frame lights up and the shot stays as
    // it is (Trevor: the highlight washed the video out).
    private let pip = ModelEntity()
    private let pipFrame: ModelEntity = {
        var material = UnlitMaterial(color: .init(white: 0.92, alpha: 1))
        material.blending = .transparent(opacity: .init(floatLiteral: 0.4))
        material.readsDepth = false
        material.writesDepth = false
        return ModelEntity(mesh: .generatePlane(width: 1, height: 1), materials: [material])
    }()
    // The pinch's offset from the square's place, for the drag whose pinch began at pipGrabStart
    // (a drag cancelled without an end, as when a cutscene ends mid-pinch, leaves its offset behind:
    // the next drag, which starts elsewhere, takes a new one).
    private var pipGrab: SIMD2<Float>?
    private var pipGrabStart: SIMD3<Float>?
    static func isPip(_ entity: Entity) -> Bool { entity.name == pipName || entity.parent?.name == pipName }
    private var pipMaterial: ShaderGraphMaterial
    private var pipTexture: LowLevelTexture?
    private var pipSize = SIMD2<Int>.zero
    private var pipPlace: SIMD2<Float> = {
        let x = UserDefaults.standard.object(forKey: "volumePipX") as? Double ?? 0.3
        let y = UserDefaults.standard.object(forKey: "volumePipY") as? Double ?? 0.3
        return SIMD2(Float(x), Float(y))
    }()
    private var pipDragging = false
    // The timing log (Library/Logs/volume-timing.log), once a second.
    private var timingStart = CACurrentMediaTime()
    private var shownFrames = 0
    private let timingLog = VolumeTimingLog()
    private var waitingNote: Entity?
    private var loggedBounds = false
    private var fitted = false
    private var hudMaterial: ShaderGraphMaterial
    private var hudTexture: LowLevelTexture?
    private var hudSize = SIMD2<Int>.zero
    private var aspect: Float = 16.0 / 9.0
    private var size = SIMD3<Float>(1.2, 0.6, 1.0)
    private var sentSize = SIMD3<Float>.zero
    private var sentViewpoint = -1
    private var surfaces: [UInt: MTLTexture] = [:]
    private var surfaceGeneration: UInt64 = .max
    private let device: MTLDevice
    private let queue: MTLCommandQueue
    private let pipelines: VolumePipelines
    private var frames = 0
    private var mirror: MirrorScene?
    private var mirrorShowing = false
    private let memoryGuard: WindowMemoryGuard
    private let sortGroup = ModelSortGroup(depthPass: nil)
    nonisolated private static let framesInFlight = Atomic<Int>(0)
    private var shownGameFrame: UInt32 = 0

    init() async throws {
        hudMaterial = try await ShaderGraphMaterial(named: "/Root/HudFrame", from: "WindowFrame.usda", in: .main)
        // Over the set (the sort group), never depth-tested: what of the set comes nearer than the
        // panel doesn't cover the HUD.
        hudMaterial.readsDepth = false
        hudMaterial.writesDepth = false
        pipMaterial = hudMaterial
        guard let device = MTLCreateSystemDefaultDevice(), let queue = device.makeCommandQueue() else {
            throw CancellationError()
        }
        self.device = device
        self.queue = queue
        memoryGuard = WindowMemoryGuard(device: device)
        pipelines = try VolumePipelines(device: device)
        panel.components.set(ModelSortGroupComponent(group: sortGroup, order: MirrorScene.maxBlendedParts + 4))
        root.addChild(panel)
        pip.name = Self.pipName
        pip.components.set(ModelSortGroupComponent(group: sortGroup, order: MirrorScene.maxBlendedParts + 3))
        pip.components.set(InputTargetComponent())
        // The hover: the whole square is what's looked at (and pinched), and the frame is what lights
        // up. One hover group: the square's own effect is a shader effect, which its material (no
        // hover node) doesn't show, so the shot stays clean; the frame's, in the same group, is the
        // highlight (not the default spotlight), and it has its own, so the square's doesn't reach it.
        let hoverGroup = HoverEffectComponent.GroupID()
        pip.components.set(HoverEffectComponent(.shader(.default, groupID: hoverGroup)))
        pipFrame.components.set(HoverEffectComponent(.highlight(.init(color: .white, strength: 1.0), groupID: hoverGroup)))
        // The pinch target: the square's face, 2.4 cm deep, wholly inside the volume (from 2.5 cm to
        // 1 mm inside its face; placePip puts the square 6 mm inside) and in front of the volume's own
        // box (3 cm inside). 12ac2eb's stood 2 cm out past the face: input outside a volume's bounds
        // isn't delivered, so a look at the square met the volume's box first, and the drag never
        // began on the headset. Set once: the entity's scale sizes it.
        pip.components.set(CollisionComponent(shapes: [.generateBox(width: 1, height: 1, depth: 0.024)
            .offsetBy(translation: [0, 0, -0.007])]))
        pip.isEnabled = false
        root.addChild(pip)
        pipFrame.name = Self.pipName + " frame"
        pipFrame.components.set(ModelSortGroupComponent(group: sortGroup, order: MirrorScene.maxBlendedParts + 2))
        pipFrame.position = [0, 0, -0.0005]
        pip.addChild(pipFrame)
        // Taps and pinches stop at the volume, not at what's behind it.
        root.components.set(InputTargetComponent())
        if ProcessInfo.processInfo.environment["LMVR_TEST_WINDOW_MIRROR"] != "0" {
            do {
                let mirror = try await MirrorScene()
                mirror.inVolume = true
                root.addChild(mirror.root)
                mirror.sort(in: sortGroup, order: 0)
                self.mirror = mirror
                lmvr_visionos_set_mirror_enabled(true)
            } catch {
                print("[LMVR] the scene mirror failed to load, so the volume shows the game flat: \(error)")
                lmvr_visionos_set_mirror_enabled(false)
            }
        }
        // No glass in front of the set: nothing of the scene is cut or eased for one.
        lmvr_visionos_set_mirror_front_allowance(0, MirrorScene.minViewTangent)
        makePanel()
    }

    private static let testTilt: SIMD3<Float>? = {
        guard let text = ProcessInfo.processInfo.environment["LMVR_TEST_VOLUME_TILT"] else { return nil }
        let parts = text.split(separator: ",").compactMap { Float($0) }
        guard parts.count >= 2 else { return nil }
        return SIMD3(parts[0], parts[1], parts.count > 2 ? parts[2] : 0.8)
    }()

    static func quarter(of azimuth: SquareAzimuth) -> Int {
        switch azimuth {
        case .front: 0
        case .right: 1
        case .back: 2
        case .left: 3
        }
    }

    func setWaitingNote(_ note: Entity) {
        waitingNote = note
        root.addChild(note)
        note.position = [0, -size.y * 0.5 + 0.12, size.z * 0.5 - 0.05]
    }

    /// The volume's bounds in the scene: the set refits to a new size (the game's set, volume.cpp).
    func fit(to bounds: BoundingBox) {
        // A volume's RealityView has its origin at the volume's centre. (The view's frame converted
        // into the scene came out from z 0 to the depth, half a volume forward of where the volume
        // is: content placed by its centre was all cut off. Only its size is taken.)
        root.position = .zero
        // Test runs: LMVR_TEST_VOLUME_TILT=<pitch>,<yaw> (degrees) turns the content towards the
        // Simulator's fixed camera, as a viewer looking down on the table (or from a side) sees it.
        if let tilt = Self.testTilt {
            root.orientation = simd_quatf(angle: tilt.x * .pi / 180, axis: [1, 0, 0])
                * simd_quatf(angle: tilt.y * .pi / 180, axis: [0, 1, 0])
            root.scale = SIMD3(repeating: tilt.z)
        }
        let extents = bounds.extents
        if !loggedBounds {
            loggedBounds = true
            print("[LMVR] volume: bounds in the scene \(bounds.min) to \(bounds.max)")
            // Test runs: LMVR_TEST_VOLUME_BOX=1 outlines the volume's bounds.
            if ProcessInfo.processInfo.environment["LMVR_TEST_VOLUME_BOX"] == "1" {
                let box = ModelEntity(mesh: .generateBox(size: extents * 0.98),
                                      materials: [UnlitMaterial(color: .init(red: 1, green: 0.4, blue: 0.1, alpha: 0.25))])
                box.components.set(OpacityComponent(opacity: 0.3))
                root.addChild(box)
            }
        }
        guard extents.x > 0.05, extents.y > 0.05, extents.z > 0.05 else { return }
        if simd_distance(extents, size) > 0.002 || !fitted {
            fitted = true
            size = extents
            placePanel()
            // Taps and pinches stop at the volume, not at what's behind it: a box 3 cm inside its
            // walls, so the square's (just inside the face, placePip) is always nearer the viewer.
            root.components.set(CollisionComponent(shapes: [.generateBox(size: simd_max(size - 0.06, SIMD3(repeating: 0.02)))]))
            waitingNote?.position = [0, -size.y * 0.5 + 0.12, size.z * 0.5 - 0.05]
            print(String(format: "[LMVR] volume: %.3f x %.3f x %.3f m", size.x, size.y, size.z))
        }
        sendVolume()
    }

    private func sendVolume() {
        guard size != sentSize || viewpoint != sentViewpoint else { return }
        sentSize = size
        sentViewpoint = viewpoint
        lmvr_visionos_set_volume(size.x, size.y, size.z, Int32(viewpoint))
    }

    func update() {
        let now = CACurrentMediaTime()
        if lastUpdate > 0 {
            let dt = now - lastUpdate
            if dt > 0, dt < 0.1 { updatePeriod += (dt - updatePeriod) * 0.05 }
        }
        lastUpdate = now
        logTiming()
        switch memoryGuard.check(hasMirror: mirror != nil, mirror: { self.mirror?.diagnostics ?? "off" },
                                 extra: { "\(self.surfaces.count) frame surfaces, \(self.frames) volume frames, "
                                          + "\(Self.framesInFlight.load(ordering: .relaxed)) in flight" }) {
        case .dropMirror: dropMirror()
        case .quit: lmvr_visionos_request_quit()
        case .forceExit: _exit(0)
        case .none: break
        }
        lmvr_visionos_window_tick()
        var frame = lmvr_visionos_window_frame()
        let fresh = lmvr_visionos_window_acquire(&frame)
        if fresh {
            shownGameFrame = frame.game_frame
        }
        // The mirror's frame from the same game frame as the panel's. A game frame without the main
        // 3D view (title, loading, a film) sends the mirror nothing: its last set goes away rather
        // than standing behind the panel.
        if fresh, frame.base == nil {
            mirror?.hide()
        }
        mirror?.smoothFast = updatePeriod < 0.025
        mirrorShowing = mirror?.update(upTo: shownGameFrame) ?? false
        mirror?.easeSmooth()
        mirror?.fade()
        if let mirror, mirror.appliedFrames != cadence.applied {
            cadence.applied = mirror.appliedFrames
            noteSetFrame(at: now, tag: mirror.appliedTag)
        }
        probePip()
        guard fresh else { return }
        let serial = frame.serial
        guard let commands = queue.makeCommandBuffer() else {
            lmvr_visionos_window_release(serial)
            return
        }
        commands.label = "Luigi's Mansion volume frame"
        if let event = frame.event {
            commands.encodeWaitForEvent(Unmanaged<AnyObject>.fromOpaque(event).takeUnretainedValue() as! MTLSharedEvent,
                                        value: frame.value)
        }
        if frame.generation != surfaceGeneration {
            surfaces.removeAll()
            surfaceGeneration = frame.generation
        }
        let encoded = encode(frame, into: commands)
        Self.framesInFlight.add(1, ordering: .relaxed)
        commands.addCompletedHandler { _ in
            lmvr_visionos_window_release(serial)
            Self.framesInFlight.subtract(1, ordering: .relaxed)
        }
        commands.commit()
        if encoded, frames == 0 {
            waitingNote?.isEnabled = false
            firstFrame?()
        }
        if encoded {
            frames += 1
            shownFrames += 1
        }
        if frames == 1 || frames % 900 == 0 {
            print("[LMVR] volume frame \(frames): mirror \(mirrorShowing), base \(frame.base != nil), "
                  + "cutscene \(frame.cinematic), 2D \(frame.flat_2d), set \(mirror?.setSerial ?? 0)")
        }
    }

    // MARK: - Cadence

    /// When each new frame of the set showed (what the viewer sees move), for the timing log: the
    /// intervals in display updates (a steady 30 on a 90 Hz display is all 3s), their spread, and
    /// the game frames the set never showed. Each one also goes to Library/Logs/volume-frames.log.
    private struct Cadence {
        var applied = 0
        var last: CFTimeInterval = 0
        var lastTag: UInt32 = 0
        var lastBytes = 0
        var intervals: [Double] = []
        var updates = [Int](repeating: 0, count: 5)  // 1, 2, 3, 4, 5+ display updates
        var missed = 0
        var frames = 0
    }
    private var cadence = Cadence()
    private var lastUpdate: CFTimeInterval = 0
    private var updatePeriod = 1.0 / 90
    private let frameLog = VolumeTimingLog(name: "volume-frames", limit: 30_000,
                                           header: "time(ms) interval(ms) updates game-frame missed bytes(KB)")
    private let logStart = CACurrentMediaTime()

    private func noteSetFrame(at now: CFTimeInterval, tag: UInt32) {
        let bytes = (mirror?.uploadedBytes ?? 0) - cadence.lastBytes
        cadence.lastBytes = mirror?.uploadedBytes ?? 0
        cadence.frames += 1
        var interval = 0.0, updates = 0, missed = 0
        if cadence.last > 0, now - cadence.last < 0.5 {
            interval = now - cadence.last
            updates = max(1, Int((interval / updatePeriod).rounded()))
            cadence.intervals.append(interval * 1000)
            cadence.updates[min(updates, 5) - 1] += 1
            let step = Int((tag &- cadence.lastTag) & 0xFFFFFF)
            if step > 1, step < 30 { missed = step - 1 }
            cadence.missed += missed
        }
        cadence.last = now
        cadence.lastTag = tag
        frameLog.write(String(format: "%.1f %.1f %ld %u %ld %.0f", (now - logStart) * 1000, interval * 1000, updates, tag, missed,
                              Double(bytes) / 1024), stamp: false)
    }

    /// Once a second: the game's pace, what reached the volume, what the mirror recorded, skipped and
    /// dropped, and what drawing the set cost here (Library/Logs/volume-timing.log, to copy off the
    /// headset: xcrun devicectl device copy from ... --source Library/Logs/volume-timing.log). Then
    /// the set's cadence (above), the pacer's (host/lmvr_window.cpp), the game's retraces a frame and
    /// what a set frame uploaded.
    private func logTiming() {
        let now = CACurrentMediaTime()
        guard now - timingStart >= 1 else { return }
        let seconds = now - timingStart
        timingStart = now
        var stats = lmvr_visionos_volume_stats_t()
        lmvr_visionos_volume_stats(&stats)
        let updates = mirror?.updateCount ?? 0
        let updateMs = updates > 0 ? (mirror?.updateSeconds ?? 0) * 1000 / Double(updates) : 0
        let bytes = mirror?.uploadedBytes ?? 0
        let paired = mirror?.smoothPaired ?? 0, snapped = mirror?.smoothSnapped ?? 0
        let halfwayVertices = paired > 0 ? (mirror?.smoothVertices ?? 0) / paired : 0
        mirror?.smoothPaired = 0
        mirror?.smoothSnapped = 0
        mirror?.smoothVertices = 0
        mirror?.updateCount = 0
        mirror?.updateSeconds = 0
        mirror?.uploadedBytes = 0
        cadence.lastBytes = 0
        let line = String(format: "game %.1f fps (%.1f ms avg, %.1f max); to the volume %llu (replaced %llu, no slot %llu), shown %ld; "
                          + "mirror recorded %llu, skipped %llu, dropped %llu (record %.1f ms, finish %.1f ms); "
                          + "set update %.1f ms x %ld, %.2f MB uploaded",
                          Double(stats.game_frames) / seconds, stats.game_ms_avg, stats.game_ms_max,
                          stats.frames_published, stats.frames_replaced, stats.frames_no_slot, shownFrames,
                          stats.mirror_recorded, stats.mirror_skipped, stats.mirror_dropped,
                          stats.mirror_record_ms_avg, stats.mirror_finish_ms_avg, updateMs, updates,
                          Double(bytes) / 1_048_576)
        let n = Double(cadence.intervals.count)
        let mean = n > 0 ? cadence.intervals.reduce(0, +) / n : 0
        let spread = n > 1 ? (cadence.intervals.reduce(0) { $0 + ($1 - mean) * ($1 - mean) } / (n - 1)).squareRoot() : 0
        let cadenceLine = String(format: "; set shown %ld (updates apart 1:%ld 2:%ld 3:%ld 4:%ld 5+:%ld; %.1f ms +- %.1f), %ld game frames not shown; "
                                 + "beat %.1f ms, %llu late, %llu trimmed, %llu passed over, %.1f ms wait; "
                                 + "game frames of 1/2/3/4+ retraces %llu/%llu/%llu/%llu; mirror CPU %.1f ms a scene; "
                                 + "%.2f MB a set frame; display update %.2f ms; smoother motion %@ (%ld frames halfway, %ld vertices each; %ld not)",
                                 cadence.frames, cadence.updates[0], cadence.updates[1], cadence.updates[2], cadence.updates[3],
                                 cadence.updates[4], mean, spread, cadence.missed, stats.pace_beat_ms, stats.pace_late,
                                 stats.pace_trims, stats.pace_passed, stats.pace_wait_ms_avg, stats.game_fields.0,
                                 stats.game_fields.1, stats.game_fields.2, stats.game_fields.3, stats.mirror_cpu_ms_avg,
                                 cadence.frames > 0 ? Double(bytes) / 1_048_576 / Double(cadence.frames) : 0, updatePeriod * 1000,
                                 UserDefaults.standard.bool(forKey: "volumeSmoothMotion") ? "on" : "off", paired, halfwayVertices, snapped)
        cadence.intervals.removeAll(keepingCapacity: true)
        cadence.updates = [Int](repeating: 0, count: 5)
        cadence.missed = 0
        cadence.frames = 0
        shownFrames = 0
        timingLog.write(line + cadenceLine)
    }

    private func dropMirror() {
        lmvr_visionos_set_mirror_enabled(false)
        mirror?.root.removeFromParent()
        mirror = nil
        mirrorShowing = false
    }

    // MARK: - One frame

    private func encode(_ frame: lmvr_visionos_window_frame, into commands: MTLCommandBuffer) -> Bool {
        guard let final = texture(frame.final) else { return false }
        let size = SIMD2(final.width, final.height)
        let frameAspect = Float(size.x) / Float(max(size.y, 1))
        if abs(frameAspect - aspect) > 0.01 {
            aspect = frameAspect
            placePanel()
        }
        if size != hudSize, !makeHud(size: size) { return false }
        guard let hudTexture else { return false }
        let hud = hudTexture.replace(using: commands)
        let base = texture(frame.base)
        if mirrorShowing, let base, !frame.flat_2d {
            // The set shows the 3D; the panel what the game drew over it, and in a cutscene the
            // game's own shot in its square (under the HUD).
            pipelines.hud(commands, final: final, base: base, exact: texture(frame.exact), ui: texture(frame.ui), hud: hud)
            showPip(frame.cinematic || frame.capture || Self.testPip, final: final, commands: commands)
        } else {
            showPip(false, final: final, commands: commands)
            // No 3D scene (title, loading, the map, films), or a full-screen 2D screen over it: the
            // whole picture on the panel, opaque.
            pipelines.flat(commands, final: final, ui: texture(frame.ui), hud: hud)
        }
        pipelines.mipmaps(commands, hud)
        if Self.dumpFrame == frames || (Self.dumpAfter > 0 && !dumped && CACurrentMediaTime() - started > Self.dumpAfter) {
            dumped = true
            Self.dump(frame)
        }
        return true
    }

    // Test runs: LMVR_TEST_WINDOW_DUMP=<frame> writes that volume frame's finished frame and scene
    // (BGRA8) raw into Documents, named with their sizes, and logs its camera.
    private static let dumpFrame = ProcessInfo.processInfo.environment["LMVR_TEST_WINDOW_DUMP"].flatMap(Int.init) ?? -1
    // Or LMVR_TEST_WINDOW_DUMP_AFTER=<seconds> after the volume opened.
    private static let dumpAfter = ProcessInfo.processInfo.environment["LMVR_TEST_WINDOW_DUMP_AFTER"].flatMap(Double.init) ?? 0
    private var dumped = false
    private let started = CACurrentMediaTime()

    private static func dump(_ frame: lmvr_visionos_window_frame) {
        let documents = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
        for (name, pointer) in [("final", frame.final), ("base", frame.base)] {
            guard let pointer else { continue }
            let surface = Unmanaged<IOSurfaceRef>.fromOpaque(pointer).takeUnretainedValue()
            IOSurfaceLock(surface, .readOnly, nil)
            let width = IOSurfaceGetWidth(surface), height = IOSurfaceGetHeight(surface)
            let rowBytes = IOSurfaceGetBytesPerRow(surface)
            var data = Data(capacity: width * height * 4)
            let base = IOSurfaceGetBaseAddress(surface)
            for row in 0..<height {
                data.append(base.advanced(by: row * rowBytes).assumingMemoryBound(to: UInt8.self), count: width * 4)
            }
            IOSurfaceUnlock(surface, .readOnly, nil)
            try? data.write(to: documents.appendingPathComponent("volume-\(name)-\(width)x\(height).raw"))
        }
        print("[LMVR] volume frame dumped: tangents \(frame.tan_half_x) x \(frame.tan_half_y), focus \(frame.focus)")
    }

    /// A Metal texture over one of the game's IOSurfaces (gamma values, as the game drew them),
    /// made once per surface.
    private func texture(_ pointer: UnsafeMutableRawPointer?) -> MTLTexture? {
        guard let pointer else { return nil }
        let key = UInt(bitPattern: pointer)
        if let cached = surfaces[key] { return cached }
        let surface = Unmanaged<IOSurfaceRef>.fromOpaque(pointer).takeUnretainedValue()
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .bgra8Unorm, width: IOSurfaceGetWidth(surface), height: IOSurfaceGetHeight(surface), mipmapped: false)
        descriptor.usage = [.shaderRead]
        descriptor.storageMode = .shared
        let texture = device.makeTexture(descriptor: descriptor, iosurface: surface, plane: 0)
        surfaces[key] = texture
        return texture
    }

    // MARK: - The cutscene square

    private func showPip(_ show: Bool, final: MTLTexture, commands: MTLCommandBuffer) {
        guard show else {
            pip.isEnabled = false
            return
        }
        let size = SIMD2(final.width, final.height)
        if size != pipSize {
            let descriptor = LowLevelTexture.Descriptor(pixelFormat: .bgra8Unorm_srgb, width: size.x, height: size.y,
                                                        mipmapLevelCount: Int(log2(Double(max(size.x, size.y)))) + 1,
                                                        textureUsage: [.shaderRead, .shaderWrite, .renderTarget])
            guard let texture = try? LowLevelTexture(descriptor: descriptor),
                  let resource = try? TextureResource(from: texture) else { return }
            try? pipMaterial.setParameter(name: "Frame", value: .textureResource(resource))
            pip.model = ModelComponent(mesh: .generatePlane(width: 1, height: 1), materials: [pipMaterial])
            pipTexture = texture
            pipSize = size
            placePip()
        }
        guard let pipTexture else { return }
        let target = pipTexture.replace(using: commands)
        pipelines.flat(commands, final: final, ui: nil, hud: target)
        pipelines.mipmaps(commands, target)
        pip.isEnabled = true
    }

    /// The square's size on the face (a third of the panel's width) and its place, kept inside the
    /// face the viewer looks at.
    private func pipExtent() -> SIMD2<Float> {
        let across = viewpoint % 2 == 0 ? size.x : size.z
        let width = min(across, size.y * aspect) * 0.34
        return [width, width / aspect]
    }

    private func faceExtent() -> SIMD2<Float> {
        [viewpoint % 2 == 0 ? size.x : size.z, size.y]
    }

    private func placePip() {
        let extent = pipExtent(), face = faceExtent()
        let limit = (face - extent) * 0.5 - 0.01
        let local = simd_clamp(pipPlace * face, -limit, limit)
        let turn = Float(viewpoint) * .pi / 2
        let deep = viewpoint % 2 == 0 ? size.z : size.x
        let out = deep * 0.5 - 0.006  // just behind the HUD panel
        let right = SIMD3<Float>(cos(turn), 0, -sin(turn))
        pip.orientation = simd_quatf(angle: turn, axis: [0, 1, 0])
        pip.position = SIMD3(sin(turn) * out, 0, cos(turn) * out) + right * local.x + SIMD3(0, local.y, 0)
        pip.scale = [extent.x, extent.y, 1]
        pipFrame.scale = [1 + 0.012 / max(extent.x, 0.01), 1 + 0.012 / max(extent.y, 0.01), 1]
    }

    /// Where a point of the volume lies on the face the viewer looks at, as shares of its width and
    /// height from its centre.
    private func facePlace(of position: SIMD3<Float>) -> SIMD2<Float> {
        let turn = Float(viewpoint) * .pi / 2
        let right = SIMD3<Float>(cos(turn), 0, -sin(turn))
        let face = faceExtent()
        return SIMD2(simd_dot(position, right) / face.x, position.y / face.y)
    }

    /// A pinch-drag of the square, to `position` in the volume's space: only its place on the face
    /// changes (never its depth), by as much as the pinch moved (it doesn't jump to the pinch),
    /// clamped inside the volume; remembered once let go.
    func dragPip(to position: SIMD3<Float>, from start: SIMD3<Float>, ended: Bool = false) {
        let at = facePlace(of: position)
        if pipGrab == nil || pipGrabStart.map({ simd_distance($0, start) > 0.001 }) ?? true {
            pipGrabStart = start
            pipGrab = pipPlace - facePlace(of: start)
            print("[LMVR] volume: the cutscene square taken hold of at \(at)")
        }
        let face = faceExtent()
        let extent = pipExtent()
        let limit = ((face - extent) * 0.5 - 0.01) / face
        pipPlace = simd_clamp(at + (pipGrab ?? .zero), -limit, limit)
        placePip()
        if ended {
            pipGrab = nil
            pipGrabStart = nil
            UserDefaults.standard.set(Double(pipPlace.x), forKey: "volumePipX")
            UserDefaults.standard.set(Double(pipPlace.y), forKey: "volumePipY")
            print(String(format: "[LMVR] volume: the cutscene square let go at %.3f, %.3f of the face", pipPlace.x, pipPlace.y))
        }
    }

    // Test runs: LMVR_TEST_PIP=1 shows the square whenever the set shows (not only in cutscenes);
    // LMVR_TEST_PIP_PROBE=1 then, a second after it first shows, casts rays at it from where a
    // viewer's eyes would be (in front, above, to a side; at its centre and corners) and logs what
    // each meets first (what a look and a pinch there would pick), and drags it by script.
    private static let testPip = ProcessInfo.processInfo.environment["LMVR_TEST_PIP"] == "1"
    private static let testProbe = ProcessInfo.processInfo.environment["LMVR_TEST_PIP_PROBE"] == "1"
    private var probeAt: CFTimeInterval = 0
    private var probed = false

    private func probePip() {
        guard Self.testProbe, !probed, pip.isEnabled, let scene = root.scene else { return }
        if probeAt == 0 { probeAt = CACurrentMediaTime() + 1 }
        guard CACurrentMediaTime() > probeAt else { return }
        probed = true
        let deep = viewpoint % 2 == 0 ? size.z : size.x
        let turn = Float(viewpoint) * .pi / 2
        let outward = SIMD3<Float>(sin(turn), 0, cos(turn))
        let right = SIMD3<Float>(cos(turn), 0, -sin(turn))
        let eyes: [(String, SIMD3<Float>)] = [
            ("front", outward * (deep * 0.5 + 1.0) + [0, 0.1, 0]),
            ("above", outward * (deep * 0.5 + 0.6) + [0, 0.7, 0]),
            ("side", outward * (deep * 0.5 + 0.8) + right * 0.7 + [0, 0.2, 0]),
        ]
        let extent = pipExtent()
        var points: [(String, SIMD3<Float>)] = [("centre", pip.position)]
        for (name, sx, sy) in [("top left", -0.45, 0.45), ("top right", 0.45, 0.45), ("bottom left", -0.45, -0.45),
                               ("bottom right", 0.45, -0.45)] as [(String, Float, Float)] {
            points.append((name, pip.position + right * extent.x * sx + SIMD3(0, extent.y * sy, 0)))
        }
        var picked = 0, total = 0
        for (eyeName, eye) in eyes {
            for (pointName, point) in points {
                let direction = simd_normalize(point - eye)
                let hits = scene.raycast(origin: eye, direction: direction, length: 5, query: .all, relativeTo: root)
                    .sorted { $0.distance < $1.distance }
                let first = hits.first.map { $0.entity.name.isEmpty ? "(the volume's box)" : $0.entity.name } ?? "nothing"
                total += 1
                if let entity = hits.first?.entity, Self.isPip(entity) { picked += 1 }
                print(String(format: "[LMVR] pip probe: from %@ at %@: first %@ at %.3f m (%ld hits)", eyeName, pointName, first,
                             hits.first?.distance ?? -1, hits.count))
            }
        }
        print("[LMVR] pip probe: the square is what \(picked) of \(total) looks at it meet first")
        // What its texture holds (the middle texel, as the GPU has it), beside the panel's.
        for (name, low) in [("square", pipTexture), ("panel", hudTexture)] {
            guard let texture = low?.read(), let buffer = device.makeBuffer(length: 4, options: .storageModeShared),
                  let commands = queue.makeCommandBuffer(), let blit = commands.makeBlitCommandEncoder() else { continue }
            blit.copy(from: texture, sourceSlice: 0, sourceLevel: 0,
                      sourceOrigin: MTLOrigin(x: texture.width / 2, y: texture.height / 2, z: 0),
                      sourceSize: MTLSize(width: 1, height: 1, depth: 1), to: buffer, destinationOffset: 0,
                      destinationBytesPerRow: 4, destinationBytesPerImage: 4)
            blit.endEncoding()
            commands.commit()
            commands.waitUntilCompleted()
            let p = buffer.contents().bindMemory(to: UInt8.self, capacity: 4)
            print("[LMVR] pip probe: the \(name)'s texture \(texture.width)x\(texture.height), middle texel BGRA \(p[0]) \(p[1]) \(p[2]) \(p[3]), "
                  + "model \(name == "square" ? (pip.model != nil) : (panel.model != nil))")
        }
        // A scripted drag: from its centre, 20 cm left and 10 cm down, then far past the volume's
        // corner (kept inside), then back; nearer/farther parts of the moves are ignored.
        let start = pip.position, before = pipPlace
        dragPip(to: start, from: start)
        dragPip(to: start - right * 0.2 - SIMD3(0, 0.1, 0) + outward * 0.15, from: start)
        print(String(format: "[LMVR] pip probe: dragged 20 cm left, 10 cm down (and 15 cm nearer): place %.3f, %.3f (from %.3f, %.3f), depth %.4f m",
                     pipPlace.x, pipPlace.y, before.x, before.y, simd_dot(pip.position, outward)))
        dragPip(to: start + right * 5 + SIMD3(0, 5, 0), from: start, ended: true)
        print(String(format: "[LMVR] pip probe: dragged far past the corner: place %.3f, %.3f, saved %.3f, %.3f",
                     pipPlace.x, pipPlace.y, UserDefaults.standard.double(forKey: "volumePipX"),
                     UserDefaults.standard.double(forKey: "volumePipY")))
        // Back where it was: a drag by the difference.
        let back = pip.position
        dragPip(to: back, from: back)
        let face = faceExtent()
        dragPip(to: back + right * (before.x - pipPlace.x) * face.x + SIMD3(0, (before.y - pipPlace.y) * face.y, 0), from: back,
                ended: true)
        print(String(format: "[LMVR] pip probe: back: place %.3f, %.3f", pipPlace.x, pipPlace.y))
    }

    // MARK: - The panel

    private func makePanel() {
        panel.model = ModelComponent(mesh: .generatePlane(width: 1, height: 1), materials: [hudMaterial])
        placePanel()
    }

    /// On the face the viewer looks from, as wide as the picture can be there, standing on the
    /// volume's floor: the game's 2D at the edge of the table, never inside the set.
    private func placePanel() {
        let across = viewpoint % 2 == 0 ? size.x : size.z
        let deep = viewpoint % 2 == 0 ? size.z : size.x
        let width = min(across, size.y * aspect)
        let height = width / aspect
        panel.scale = [width, height, 1]
        let turn = Float(viewpoint) * .pi / 2
        panel.orientation = simd_quatf(angle: turn, axis: [0, 1, 0])
        let out = deep * 0.5 - 0.004
        panel.position = [sin(turn) * out, -size.y * 0.5 + height * 0.5, cos(turn) * out]
        if pipSize != .zero { placePip() }
    }

    private func makeHud(size: SIMD2<Int>) -> Bool {
        // With mipmaps, made by the GPU each frame: written at the game's resolution and shown
        // smaller, text sparkled without them (TPVR, SHAR). Premultiplied.
        let descriptor = LowLevelTexture.Descriptor(pixelFormat: .bgra8Unorm_srgb, width: size.x, height: size.y,
                                                    mipmapLevelCount: Int(log2(Double(max(size.x, size.y)))) + 1,
                                                    textureUsage: [.shaderRead, .shaderWrite, .renderTarget])
        guard let texture = try? LowLevelTexture(descriptor: descriptor),
              let resource = try? TextureResource(from: texture) else {
            print("[LMVR] the volume's \(size.x)x\(size.y) panel texture failed")
            return false
        }
        try? hudMaterial.setParameter(name: "Frame", value: .textureResource(resource))
        panel.model?.materials = [hudMaterial]
        hudTexture = texture
        hudSize = size
        return true
    }
}

@MainActor
private struct VolumePipelines {
    let hud, flat: MTLComputePipelineState

    init(device: MTLDevice) throws {
        let library = try device.makeLibrary(source: Self.source, options: nil)
        func make(_ name: String) throws -> MTLComputePipelineState {
            guard let function = library.makeFunction(name: name) else { throw CancellationError() }
            return try device.makeComputePipelineState(function: function)
        }
        hud = try make("VolumeHud")
        flat = try make("VolumeFlat")
    }

    func hud(_ commands: MTLCommandBuffer, final: MTLTexture, base: MTLTexture, exact: MTLTexture?, ui: MTLTexture?,
             hud: MTLTexture) {
        guard let compute = commands.makeComputeCommandEncoder() else { return }
        var flags: UInt32 = (exact != nil ? 2 : 0) | (ui != nil ? 4 : 0)
        compute.setTexture(ui ?? final, index: 4)
        compute.setComputePipelineState(self.hud)
        compute.setTexture(final, index: 0)
        compute.setTexture(base, index: 1)
        compute.setTexture(hud, index: 2)
        compute.setTexture(exact ?? base, index: 3)
        compute.setBytes(&flags, length: 4, index: 0)
        compute.dispatchThreads(MTLSize(width: hud.width, height: hud.height, depth: 1),
                                threadsPerThreadgroup: MTLSize(width: 16, height: 16, depth: 1))
        compute.endEncoding()
    }

    func flat(_ commands: MTLCommandBuffer, final: MTLTexture, ui: MTLTexture?, hud: MTLTexture) {
        guard let compute = commands.makeComputeCommandEncoder() else { return }
        var flags: UInt32 = ui != nil ? 4 : 0
        compute.setComputePipelineState(flat)
        compute.setTexture(final, index: 0)
        compute.setTexture(hud, index: 1)
        compute.setTexture(ui ?? final, index: 4)
        compute.setBytes(&flags, length: 4, index: 0)
        compute.dispatchThreads(MTLSize(width: hud.width, height: hud.height, depth: 1),
                                threadsPerThreadgroup: MTLSize(width: 16, height: 16, depth: 1))
        compute.endEncoding()
    }

    func mipmaps(_ commands: MTLCommandBuffer, _ texture: MTLTexture) {
        guard texture.mipmapLevelCount > 1, let blit = commands.makeBlitCommandEncoder() else { return }
        blit.generateMipmaps(for: texture)
        blit.endEncoding()
    }

    static let source = """
    #include <metal_stdlib>
    using namespace metal;

    static float3 Linear(float3 c) { return select(pow((c + 0.055) / 1.055, 2.4), c / 12.92, c <= 0.04045); }

    // A pixel the 2D changed, as the least cover `a` and the colour that explain it
    // (after = before * (1 - a) + colour), in the game's own (gamma) values.
    static float4 Cover(float3 before, float3 after)
    {
        float a = 0.0;
        for (int i = 0; i < 3; ++i)
        {
            if (after[i] > before[i]) a = max(a, (after[i] - before[i]) / max(1.0 - before[i], 1e-4));
            else if (after[i] < before[i]) a = max(a, (before[i] - after[i]) / max(before[i], 1e-4));
        }
        a = saturate(a);
        return float4(clamp(after - before * (1.0 - a), 0.0, a), a);
    }

    // The settings overlay (flags & 4: the game's F1 menu, its own alpha, premultiplied gamma colour)
    // over a panel value, at the panel's texel `id` of `size`.
    static float4 OverUi(float4 under, texture2d<float, access::read> ui, uint2 id, uint2 size, uint flags)
    {
        if ((flags & 4) == 0) return under;
        const uint2 at = min(uint2(float2(id) * float2(ui.get_width(), ui.get_height()) / float2(size)),
                             uint2(ui.get_width() - 1, ui.get_height() - 1));
        const float4 u = ui.read(at);
        if (u.a <= 1e-4) return under;
        const float3 linear = select(pow((saturate(u.rgb / u.a) + 0.055) / 1.055, 2.4), saturate(u.rgb / u.a) / 12.92,
                                     saturate(u.rgb / u.a) <= 0.04045);
        return float4(linear * u.a, u.a) + under * (1.0 - u.a);
    }

    // Premultiplied gamma colour into the panel's (linear, premultiplied) values.
    static float4 Store(float4 c)
    {
        if (c.a <= 1e-4) return float4(0);
        return float4(Linear(saturate(c.rgb / c.a)) * c.a, c.a);
    }

    // The game's own shot in the panel's top right corner: a third of its width, a thin frame.
    static bool Inset(uint2 id, uint2 size, thread float2& uv, thread bool& edge)
    {
        const float2 p = float2(id) / float2(size);
        const float2 lo = float2(0.64, 0.03), hi = float2(0.97, 0.36);
        if (any(p < lo - 0.004) || any(p > hi + 0.004)) return false;
        edge = any(p < lo) || any(p > hi);
        uv = saturate((p - lo) / (hi - lo));
        return true;
    }

    // The HUD is whatever the game drew over its scene: pixels the 2D changed. Against `base` (the
    // picture from the end of the main 3D view), changes of more than the scene's own round trip
    // (the game copies its picture to a 16-bit texture and pastes it back before the 2D: up to a
    // few levels off), which thinned the faint parts of dark text boxes. With `exact` (flags & 2:
    // the picture taken again as the 2D began, lmvr_window_overlay_begins), any change of more than
    // a level, wherever exact and base agree within the round trip; where they don't, something
    // drew between the two (the game over's greying and its "Good night"), keyed against base.
    kernel void VolumeHud(texture2d<float, access::read> final [[texture(0)]],
                          texture2d<float, access::read> base [[texture(1)]],
                          texture2d<float, access::write> hud [[texture(2)]],
                          texture2d<float, access::read> exact [[texture(3)]],
                          texture2d<float, access::read> ui [[texture(4)]],
                          constant uint& flags [[buffer(0)]],
                          uint2 id [[thread_position_in_grid]])
    {
        const uint2 size = uint2(hud.get_width(), hud.get_height());
        if (id.x >= size.x || id.y >= size.y) return;
        float2 uv;
        bool edge = false;
        if ((flags & 1) != 0 && Inset(id, size, uv, edge))
        {
            if (edge) { hud.write(float4(0.02, 0.02, 0.03, 0.9), id); return; }
            const uint2 at = min(uint2(uv * float2(final.get_width(), final.get_height())),
                                 uint2(final.get_width() - 1, final.get_height() - 1));
            hud.write(float4(Linear(final.read(at).rgb), 1), id);
            return;
        }
        const uint2 bat = uint2(float2(id) * float2(base.get_width(), base.get_height()) / float2(size));
        const float3 drawn = final.read(id).rgb;
        float3 under = base.read(bat).rgb;
        bool fine = false;
        if ((flags & 2) != 0) {
            const float3 again = exact.read(bat).rgb;
            const float3 apart = abs(again - under);
            if (max(apart.r, max(apart.g, apart.b)) < 0.045) { under = again; fine = true; }
        }
        const float3 delta = abs(drawn - under);
        const float most = max(delta.r, max(delta.g, delta.b));
        const float least = fine ? 1.5 / 255.0 : 0.045;
        if (most < least) { hud.write(OverUi(float4(0), ui, id, size, flags), id); return; }
        float4 c = Cover(under, drawn);
        // Eased in just above the threshold, so the round trip's noise doesn't speckle.
        if (!fine) c *= smoothstep(0.045, 0.09, most);
        hud.write(OverUi(Store(c), ui, id, size, flags), id);
    }

    kernel void VolumeFlat(texture2d<float, access::read> final [[texture(0)]],
                           texture2d<float, access::write> hud [[texture(1)]],
                           texture2d<float, access::read> ui [[texture(4)]],
                           constant uint& flags [[buffer(0)]],
                           uint2 id [[thread_position_in_grid]])
    {
        if (id.x >= hud.get_width() || id.y >= hud.get_height()) return;
        hud.write(OverUi(float4(Linear(final.read(id).rgb), 1), ui, id, uint2(hud.get_width(), hud.get_height()), flags), id);
    }
    """
}

/// Library/Logs/volume-timing.log: a line a second while the volume plays (the previous run's kept
/// as volume-timing.previous.log), for timing a session on the headset after the fact.
/// (Also Library/Logs/volume-frames.log: a line for each frame the set showed, the first
/// `limit` of a run.)
final class VolumeTimingLog {
    private var handle: FileHandle?
    private let name: String
    private let limit: Int
    private var lines = 0
    private let formatter: DateFormatter = {
        let formatter = DateFormatter()
        formatter.dateFormat = "HH:mm:ss"
        return formatter
    }()

    init(name: String = "volume-timing", limit: Int = .max, header: String? = nil) {
        self.name = name
        self.limit = limit
        guard let library = FileManager.default.urls(for: .libraryDirectory, in: .userDomainMask).first else { return }
        let logs = library.appendingPathComponent("Logs")
        try? FileManager.default.createDirectory(at: logs, withIntermediateDirectories: true)
        let url = logs.appendingPathComponent("\(name).log")
        let previous = logs.appendingPathComponent("\(name).previous.log")
        try? FileManager.default.removeItem(at: previous)
        try? FileManager.default.moveItem(at: url, to: previous)
        FileManager.default.createFile(atPath: url.path, contents: nil)
        handle = try? FileHandle(forWritingTo: url)
        if let header { try? handle?.write(contentsOf: Data("# \(header)\n".utf8)) }
    }

    func write(_ line: String, stamp: Bool = true) {
        guard lines < limit else { return }
        lines += 1
        let text = stamp ? "\(formatter.string(from: Date())) \(line)\n" : "\(line)\n"
        try? handle?.write(contentsOf: Data(text.utf8))
        if stamp, ProcessInfo.processInfo.environment["LMVR_TEST_TIMING_STDOUT"] == "1" {
            print("[LMVR] \(name): \(line)")
        }
    }
}
