@preconcurrency import Metal
import QuartzCore
import RealityKit
import Synchronization

/// The window's scene mirror: the game's own 3D draws, rebuilt each frame as one RealityKit mesh,
/// so RealityKit renders the game's world from the viewer's real eyes and the window shows it from
/// any angle, with nothing to fill in behind the hero however far you lean. The approach is the
/// SHAR port's; this file is the TPVR port's (visionos/App/Sources/MirrorScene.swift), fed by
/// host/scene_mirror.h: vertices in the camera's view space, each draw's material reduced to
/// texture x colour + colour per vertex, textures as RGBA.
///
/// One mesh with a part per solid material, refilled each frame (RealityKit's cost is in refilling a
/// mesh, not in its size: SHAR measured ~0.14 ms a mesh, so one per draw capped it at 45 Hz), and
/// each blended part in an entity of its own, in the game's order. It sits in the portal's world,
/// placed so the game's camera is where a viewer's eye would be for the picture to be exact: the
/// window spans the camera's view a little short of its focus.
@MainActor
final class MirrorScene {
    let root = Entity()
    // Under the root (which fades): `placed`, where each frame's view space goes (its placement in
    // the volume, or the window's glass), with the level and its blended parts; and `held`, the
    // room's shell as it stood in play, kept there through a cutscene (holdWorld).
    private let placed = Entity()
    private let held = Entity()
    private let entity = ModelEntity()
    // While the shell is held: the cutscene frames' solid parts but the shell's, in a mesh of their own.
    private let liveEntity = ModelEntity()
    private var liveMesh: LowLevelMesh?
    private var liveResourcePending = false
    private var liveParts: [LowLevelMesh.Part] = []
    /// The room's shell (its static model: PartWorld) is held as it stood in play: the game hides
    /// and shows walls for a cutscene's camera, which looks from elsewhere (walls came up between
    /// the viewer and Luigi in cutscenes, Trevor on 12ac2eb). Its furniture and characters move on.
    private(set) var holding = false
    private var holdSerial: UInt32 = 0  // the fit the held shell was placed by
    private var lastWorld: [Bool] = []  // for each of lastParts: the shell's
    // Frames of play shown with the set as placed now: a shell is held only once play has shown it
    // whole for a while (a map whose arrival is a cutscene has no shell from play to hold; its first
    // frame, still loading, had three parts).
    private var playFrames = 0
    private var playSerial: UInt32 = 0

    // Smoother motion (experimental; the launcher's Advanced options, volumeSmoothMotion): the set's
    // solid parts shown between the game's frames. Each solid part drawn as in the frame before (the
    // same run of the same indices: the same draw) that moved is shown first halfway between where it
    // stood in the frame before (in the volume) and where it stands now, then, at the next update,
    // where it stands: at the headset's 60 updates a second (the game's frames 2 apart) its 30 frames
    // show as 60, the moving parts a sixtieth of a second later. Two writes of the set's vertices a
    // game frame instead of one. Parts that differ, jump or are blended stay at the game's rate.
    private struct Smooth {
        var pending = false         // the frame as recorded still to be written (the update after)
        var justApplied = false     // ... but not in the update that showed its halfway
        var bytes: [UInt8] = []     // its vertices as recorded (view space)
        var moving: [UInt32] = []   // the vertices shown halfway (of solid parts that moved)
        var from: [SIMD3<Float>] = []  // where each stood the frame before, in this frame's view space
        // The frame before: its indices, its solid parts by look (texture, kind, index count) with
        // where their indices start and their lowest vertex, and its vertices in the volume.
        var indices: [UInt32] = []
        var parts: [SmoothPartKey: [SIMD2<UInt32>]] = [:]  // (first index, lowest vertex) of each
        var volume: [SIMD3<Float>] = []
        var vertexCount = -1
        var fit: UInt32 = 0, set: UInt32 = 0  // the frame before's fit and placing of the set
    }
    private struct SmoothPartKey: Hashable {
        var texture: UInt64
        var kind: UInt32
        var indexCount: UInt32
    }
    private var smooth = Smooth()
    /// Frames shown halfway and frames not, and the vertices shown halfway, since the timing log
    /// took them.
    var smoothPaired = 0, smoothSnapped = 0, smoothVertices = 0
    /// GameVolume: whether RealityKit's updates come fast enough (under 25 ms) for a halfway to show
    /// before the next frame; slower, nothing is gained and the moving parts would only lag.
    var smoothFast = true
    private static let kSmoothJump: Float = 0.03     // metres in the volume in one game frame: a jump
    private static let kSmoothStill: Float = 0.0002  // metres: hasn't moved
    // A mesh past this many vertices takes buffers over 3.2 MB, whose writes cost RealityKit 12-15 ms
    // (see fill): writing it twice a game frame would cost more than it shows.
    private static let kSmoothMaxVertices = 81_920
    private static var smoothWanted: Bool { UserDefaults.standard.bool(forKey: "volumeSmoothMotion") }
    private static let noHold = ProcessInfo.processInfo.environment["LMVR_TEST_NO_HOLD"] == "1"
    // Behind everything: where the game drew nothing the mirror can show (past the edges of its
    // view, seen from the side), black, as the game's own picture is, not the room.
    private let backdrop = ModelEntity(mesh: .generatePlane(width: 1, height: 1),
                                       materials: [UnlitMaterial(color: .black)])
    private var mesh: LowLevelMesh?
    // A new mesh's resource is made once the mesh has parts (update(upTo:)).
    private var resourcePending = false
    private let queue: MTLCommandQueue
    // Blended parts, each in an entity of its own in the window's sort group, in the game's order
    // (as the SHAR port does). In the one mesh, all with the frame's bounds, RealityKit ordered them
    // itself, by distance from the eye: layered ones (the sky's, the ground's second passes, a
    // horse's dust) took turns as the head moved, and flickered (TPVR).
    @MainActor private final class BlendSlot {
        let entity = ModelEntity()
        var mesh: LowLevelMesh?
        var material: MaterialKey?
        var order: Int32 = .min
        var indexCount = -1  // the part last set (the volume skips setting the same one again)
    }
    private var blendSlots: [BlendSlot] = []
    private var sortGroup: ModelSortGroup?
    private var sortOrder: Int32 = 0
    private var remap: [Int32] = []  // a frame vertex's index in the slot being filled, -1 when none

    // MirrorMaterials(Wrap).usda: kind ("Opaque", "Cutout", "Blend") + wrap ("RR", "CC"...).
    private var templates: [String: ShaderGraphMaterial] = [:]
    private var textures: [UInt64: TextureResource] = [:]
    // Their textures, for the ones the game updates (shadows read back each frame).
    private var lowLevelTextures: [UInt64: LowLevelTexture] = [:]
    private var failedTextures: Set<UInt64> = []
    private struct PendingTexture { let id: UInt64; let pixels: Data; let width: Int; let height: Int; let mipmapped: Bool; let cutout: Bool }
    private var pendingTextures: [PendingTexture] = []
    private var wantedTextures: Set<UInt64> = []
    private var texturesArrived = false
    // Textures the game let go of (unused for 15 s), kept with their materials, in the order they
    // went: one that comes back (an area returned to, a menu closed) has the id it had and finds
    // them as they were. Dropped, its return added a material to the level's list and set the list
    // again, which can cost the renderer a frame of the whole level: TPVR flashed for minutes after
    // a load. Up to parkedBudget bytes; past it, the longest gone go for good.
    private var parked: [UInt64] = []
    private var parkedSet: Set<UInt64> = []
    private static let parkedBudget = 48 << 20
    private var white: TextureResource?
    private var materials: [MaterialKey: ShaderGraphMaterial] = [:]
    // The entity's material list, which the solid parts index; it grows as materials appear, and is
    // set again only then (setting it may cost RealityKit's renderer a frame).
    private var materialList: [MaterialKey] = []
    private var materialIndex: [MaterialKey: Int] = [:]
    private var materialReady: [Bool] = []
    private var serial: UInt64 = 0
    private(set) var hasScene = false
    /// In the volume ("On your table", GameVolumeView): the set's placement comes with each frame
    /// (lmvr_visionos_mirror_frame.volume_from_view, the base's src/recomp/volume.cpp) instead of the
    /// window's glass; no backdrop (the room shows round the set), and no sky pushed past the scene
    /// (the volume's walls would cut it into a box).
    var inVolume = false
    /// The set's placement count (lmvr_visionos_mirror_frame.set_serial): a new one fades in.
    private(set) var setSerial: UInt32 = 0
    /// Its fit count (fit_serial): a resize's too.
    private var fitSerial: UInt32 = 0
    private var fadeStart: CFTimeInterval = 0
    private var volumePlacements = 0
    private var lastParts: [LowLevelMesh.Part] = []
    private static let volumeBounds = BoundingBox(min: [-200_000, -200_000, -200_000], max: [200_000, 200_000, 200_000])
    private static func sameParts(_ a: [LowLevelMesh.Part], _ b: [LowLevelMesh.Part]) -> Bool {
        guard a.count == b.count else { return false }
        for (x, y) in zip(a, b) where x.indexOffset != y.indexOffset || x.indexCount != y.indexCount
            || x.materialIndex != y.materialIndex {
            return false
        }
        return true
    }
    /// Bytes written into meshes since the volume's timing log last took them.
    var uploadedBytes = 0
    /// Frames whose geometry the set has shown (it counts up), and the game frame of the last.
    private(set) var appliedFrames = 0
    private(set) var appliedTag: UInt32 = 0
    /// Time spent in update(upTo:) since the timing log last took it (seconds), and how many.
    var updateSeconds = 0.0
    var updateCount = 0
    static let fadeSeconds: CFTimeInterval = 0.35
    /// The view distance the glass is at (game units), as last placed.
    private(set) var plane: Float = 1
    /// How much depths are compressed for a narrow view (1 for a normal one), as last placed.
    private(set) var depthScale: Float = 1
    /// The narrowest view laid out as it is (half-width as a tangent): the picture is exact from
    /// 1 / (2 tan) window widths in front of the glass, and a telephoto shot (TPVR's cutscenes go
    /// down to 0.27) put that point two widths out, so anyone nearer saw past what the game drew.
    static let minViewTangent: Float = 0.7
    private static let cullTest = ProcessInfo.processInfo.environment["LMVR_TEST_MIRROR_CULL"] ?? ""
    private static let addRed = ProcessInfo.processInfo.environment["LMVR_TEST_MIRROR_ADD_RED"] == "1"

    private var timing = (updates: 0, frames: 0, seconds: 0.0, since: CACurrentMediaTime(), vertices: 0, parts: 0, drawn: 0,
                          total: 0)
    private var counts = (texturesMade: 0, texturesFailed: 0, materialSets: 0, meshesMade: 0, fills: 0, partSets: 0)
    // Of an update's time: filling the mesh, and setting its parts (where RealityKit waits for its
    // renderer: a mesh with more vertex attributes doubled it).
    private var stages = (fill: 0.0, parts: 0.0)

    private struct MaterialKey: Hashable {
        var texture: UInt64
        var kind: UInt32
        var wrap: String
        var flags: UInt32
        var cutoff: UInt8
        var weights: SIMD8<Int8>

        init(_ part: lmvr_visionos_mirror_part) {
            texture = part.texture
            kind = part.kind
            let letter = { (mode: UInt8) in mode == UInt8(LMVR_VISIONOS_MIRROR_WRAP_CLAMP) ? "C" : mode == UInt8(LMVR_VISIONOS_MIRROR_WRAP_MIRROR) ? "M" : "R" }
            wrap = letter(part.wrap_s) + letter(part.wrap_t)
            flags = part.flags & (LMVR_VISIONOS_MIRROR_CULL_BACK | LMVR_VISIONOS_MIRROR_CULL_FRONT | LMVR_VISIONOS_MIRROR_DEPTH_WRITE | LMVR_VISIONOS_MIRROR_DEPTH_TEST
                                  | LMVR_VISIONOS_MIRROR_BACKGROUND)
            cutoff = part.kind == LMVR_VISIONOS_MIRROR_PART_CUTOUT ? UInt8(clamping: Int((part.cutoff * 255).rounded())) : 0
            let quantise = { (value: Float) in Int8(clamping: Int((value * 16).rounded())) }
            weights = part.kind == LMVR_VISIONOS_MIRROR_PART_BLEND
                ? SIMD8(quantise(part.colour_base), quantise(part.colour_alpha), quantise(part.opacity_base),
                        quantise(part.opacity_alpha), quantise(part.opacity_luma), 0, 0, 0)
                : .zero
        }
    }

    init() async throws {
        guard let queue = MTLCreateSystemDefaultDevice()?.makeCommandQueue() else { throw CancellationError() }
        self.queue = queue
        for kind in ["Opaque", "Cutout", "Blend"] {
            templates[kind + "RR"] = try await ShaderGraphMaterial(named: "/Root/Mirror\(kind)_RR", from: "MirrorMaterials.usda", in: .main)
        }
        // The other wrap modes, separately: without them, textures repeat.
        do {
            for wrap in ["RC", "RM", "CR", "CC", "CM", "MR", "MC", "MM"] {
                for kind in ["Opaque", "Cutout", "Blend"] {
                    templates[kind + wrap] = try await ShaderGraphMaterial(
                        named: "/Root/Mirror\(kind)_\(wrap)", from: "MirrorMaterialsWrap.usda", in: .main)
                }
            }
        } catch {
            print("[LMVR] mirror: clamped and mirrored texture materials failed to load, so they repeat: \(error)")
        }
        white = Self.makeTexture(queue: queue, pixels: [255, 255, 255, 255], width: 1, height: 1)
        root.addChild(placed)
        root.addChild(held)
        entity.isEnabled = false
        placed.addChild(entity)
        liveEntity.isEnabled = false
        placed.addChild(liveEntity)
        backdrop.isEnabled = false
        placed.addChild(backdrop)
    }

    /// Draws the level as `order` in `group`, its blended parts after it in the game's order, and the
    /// backdrop just before it; what the window lays over the level goes from `order` + 1 +
    /// maxBlendedParts. The backdrop must be in the group too: left out, RealityKit drew it after the
    /// level now and then (one Simulator launch in five or so) and, as the group's models don't
    /// settle depth against what's outside it, it covered the whole level.
    func sort(in group: ModelSortGroup, order: Int32) {
        backdrop.components.set(ModelSortGroupComponent(group: group, order: order - 1))
        entity.components.set(ModelSortGroupComponent(group: group, order: order))
        liveEntity.components.set(ModelSortGroupComponent(group: group, order: order))
        sortGroup = group
        sortOrder = order
        for slot in blendSlots { slot.order = .min }
    }
    static let maxBlendedParts: Int32 = 1 << 16
    // Test runs: LMVR_TEST_MIRROR_ONE_MESH=1 keeps the blended parts in the one mesh.
    private static let oneMesh = ProcessInfo.processInfo.environment["LMVR_TEST_MIRROR_ONE_MESH"] == "1"

    /// Takes the game's newest mirror frame no later than game frame `upTo` (the window frame shown
    /// with it) into the mesh. False while there's no 3D scene to show (the window then shows the
    /// game's flat picture).
    func update(upTo: UInt32) -> Bool {
        let start = CACurrentMediaTime()
        defer {
            report(start)
            updateSeconds += CACurrentMediaTime() - start
            updateCount += 1
        }
        var frame = lmvr_visionos_mirror_frame()
        guard lmvr_visionos_mirror_acquire(&frame, upTo) else { return false }
        takeTextures(frame, start: start)
        guard frame.serial != serial, frame.serial != hidden else { return hasScene }
        serial = frame.serial
        timing.frames += 1
        timing.total += 1
        guard frame.scene, frame.index_count > 0, frame.part_count > 0, frame.tan_half_x > 0,
              let vertices = frame.vertices, let indices = frame.indices, let parts = frame.parts else {
            entity.isEnabled = false
            liveEntity.isEnabled = false
            backdrop.isEnabled = false
            for slot in blendSlots { slot.entity.isEnabled = false }
            hasScene = false
            forgetSmooth()
            return false
        }
        // The materials first: a reset of the list (it ends a hold) must come before the hold's
        // choice of mesh (the review: the full parts went onto the cutscene's mesh, the shell's stayed
        // on the level's).
        let keys = (0..<Int(frame.part_count)).map { MaterialKey(parts[$0]) }
        let inSlot = { (index: Int) in parts[index].kind == LMVR_VISIONOS_MIRROR_PART_BLEND && !Self.oneMesh }
        // The level's list holds the solid parts' materials only: a new particle's doesn't set it.
        updateMaterials((0..<keys.count).filter { !inSlot($0) }.map { keys[$0] })
        // The shell held from the last frame of play, while this frame's camera is a cutscene's (only
        // for the fit it was placed by: a cutscene in another room, or a resize, shows the room as it
        // comes). The frame's own flag, not the panel's: the two can be a frame apart.
        let holdWorld = frame.cinematic
        let wantHold = inVolume && holdWorld && !Self.noHold
        if holding && (!wantHold || (frame.placed && frame.fit_serial != holdSerial)) {
            endHold()
        } else if !holding && wantHold && hasScene && entity.model != nil && frame.placed && frame.fit_serial == fitSerial,
                  playSerial == fitSerial, playFrames >= 15, lastWorld.contains(true), let mesh {
            finishSmooth(mesh)  // a frame shown halfway is held as recorded, not halfway
            holding = true
            holdSerial = fitSerial
            held.transform = placed.transform
            entity.removeFromParent()
            held.addChild(entity)
            mesh.parts.replaceAll(zip(lastParts, lastWorld).filter { $0.1 }.map { $0.0 })
            print("[LMVR] mirror: the room's shell held through the cutscene (\(lastWorld.filter { $0 }.count) of \(lastParts.count) parts)")
        }
        last = (Int(frame.vertex_count), Int(frame.index_count), Int(frame.part_count), last.blended)
        let fillStart = CACurrentMediaTime()
        let halfway = prepareSmooth(frame, vertices: vertices, indices: indices, parts: parts)
        let filled = holding
            ? fill(into: &liveMesh, pending: &liveResourcePending, vertices: vertices, vertexCount: Int(frame.vertex_count),
                   indices: indices, indexCount: Int(frame.index_count))
            : fill(into: &mesh, pending: &resourcePending, vertices: vertices, vertexCount: Int(frame.vertex_count),
                   indices: indices, indexCount: Int(frame.index_count), halfway: halfway)
        stages.fill += CACurrentMediaTime() - fillStart
        smooth.pending = halfway && filled
        smooth.justApplied = smooth.pending
        guard filled, let mesh = holding ? liveMesh : mesh else { return hasScene }

        let low = SIMD3(frame.bounds_min.0, frame.bounds_min.1, frame.bounds_min.2)
        let high = SIMD3(frame.bounds_max.0, frame.bounds_max.1, frame.bounds_max.2)
        let pad = (high - low) * 0.05 + 1
        // In the volume, one generous box for every part (game units of view space; the volume's own
        // walls cut the set): the parts then stay the same from frame to frame, and setting them
        // again, the dearest step of an update, is skipped.
        let bounds = inVolume ? Self.volumeBounds : BoundingBox(min: low - pad, max: high + pad)
        // A part whose texture is still on its way waits: drawn white, it flashed.
        let partsStart = CACurrentMediaTime()
        defer { stages.parts += CACurrentMediaTime() - partsStart }
        // The solid parts in the one mesh; each blended one in a slot of its own (fillBlend).
        var drawn: [LowLevelMesh.Part] = []
        var drawnWorld: [Bool] = []
        var blendedParts: [(index: Int, material: ShaderGraphMaterial)] = []
        for index in 0..<Int(frame.part_count) {
            let part = parts[index]
            if inVolume, (part.flags & LMVR_VISIONOS_MIRROR_BACKGROUND) != 0 { continue }
            let world = (part.flags & LMVR_VISIONOS_MIRROR_WORLD) != 0
            // (The shell's solid parts are the held ones; its blended ones, glass and the like, stay live.)
            if holding && world && !inSlot(index) { continue }
            if inSlot(index) {
                if let material = material(for: keys[index]) {
                    if blendedParts.count < Int(Self.maxBlendedParts) { blendedParts.append((index, material)) }
                } else {
                    wantedTextures.insert(keys[index].texture)
                }
                continue
            }
            guard let materialIndex = materialIndex[keys[index]], materialReady[materialIndex] else {
                wantedTextures.insert(keys[index].texture)
                continue
            }
            drawn.append(LowLevelMesh.Part(indexOffset: Int(part.first_index) * 4, indexCount: Int(part.index_count),
                                           topology: .triangle, materialIndex: materialIndex, bounds: bounds))
            drawnWorld.append(world)
        }
        if holding {
            if liveResourcePending || !Self.sameParts(drawn, liveParts) {
                mesh.parts.replaceAll(drawn)
                liveParts = drawn
                counts.partSets += 1
            }
        } else if !inVolume || resourcePending || !Self.sameParts(drawn, lastParts) || drawnWorld != lastWorld {
            mesh.parts.replaceAll(drawn)
            lastParts = drawn
            lastWorld = drawnWorld
            counts.partSets += 1
        }
        fillSlots(blendedParts, parts: parts, keys: keys, vertices: vertices, vertexCount: Int(frame.vertex_count),
                  indices: indices)
        timing.drawn += drawn.count + last.blended
        if holding, liveResourcePending, !drawn.isEmpty {
            do {
                let resource = try MeshResource(from: mesh)
                if liveEntity.model == nil {
                    liveEntity.model = ModelComponent(mesh: resource, materials: currentMaterials())
                } else {
                    liveEntity.model?.mesh = resource
                }
                liveResourcePending = false
            } catch {
                print("[LMVR] mirror: the cutscene mesh's resource failed: \(error)")
            }
        }
        if !holding, resourcePending, !drawn.isEmpty {
            // Made from the mesh only once it has parts. Made from a new mesh with none and given its
            // parts in the same update, the level stayed invisible for the whole run, now and then
            // (one Simulator launch in five or so), with the backdrop showing in its place.
            do {
                let resource = try MeshResource(from: mesh)
                if entity.model == nil {
                    entity.model = ModelComponent(mesh: resource, materials: currentMaterials())
                } else {
                    entity.model?.mesh = resource
                }
                resourcePending = false
            } catch {
                print("[LMVR] mirror: the mesh's resource failed: \(error)")
            }
        }
        if inVolume {
            guard placeInVolume(frame) else {
                entity.isEnabled = false
                liveEntity.isEnabled = false
                for slot in blendSlots { slot.entity.isEnabled = false }
                hasScene = false
                return false
            }
        } else {
            place(frame)
        }
        if Self.dumpFrame == timing.total {
            dump(frame)
        }
        entity.isEnabled = true
        liveEntity.isEnabled = holding && liveEntity.model != nil
        if !holding && !holdWorld {
            if playSerial != fitSerial { playSerial = fitSerial; playFrames = 0 }
            playFrames += 1
        } else if !holding {
            playFrames = 0  // a cutscene with nothing held: play must show the room again first
        }
        backdrop.isEnabled = !inVolume
        hasScene = true
        appliedFrames += 1
        appliedTag = frame.game_frame
        timing.vertices += Int(frame.vertex_count)
        timing.parts += Int(frame.part_count)
        return true
    }

    /// View space into window units: the window spans the camera's view at the frame's `plane` (a
    /// little short of its focus, nothing nearer), so the camera sits 1 / (2 tan) window widths in
    /// front of the glass. A narrower view than minViewTangent (a telephoto shot) has its depths
    /// compressed to the picture it would have from a camera that close, which is the same picture.
    private func place(_ frame: lmvr_visionos_mirror_frame) {
        let plane = max(frame.plane, 1e-3)
        let tan = frame.tan_half_x
        let depthScale = tan / max(tan, Self.minViewTangent)
        self.plane = plane
        self.depthScale = depthScale
        let scale = 1 / (2 * plane * tan)
        placed.scale = SIMD3(scale, scale, scale * depthScale)
        placed.position = [0, 0, scale * depthScale * plane]
        let far = max(-frame.bounds_min.2, plane) * 1.05
        let wide = 2 * far * max(tan, Self.minViewTangent) * 8
        backdrop.position = [0, 0, -far]
        backdrop.scale = [wide, wide, 1]
    }

    /// The volume: view space (game units) straight into the volume (metres), as the set places
    /// it this frame. A new placement (a cut: a new room, a camera cut, a resize) fades in.
    private func placeInVolume(_ frame: lmvr_visionos_mirror_frame) -> Bool {
        guard frame.placed else { return false }
        let m = frame.volume_from_view
        let matrix = float4x4(SIMD4(m.0, m.1, m.2, m.3), SIMD4(m.4, m.5, m.6, m.7),
                              SIMD4(m.8, m.9, m.10, m.11), SIMD4(m.12, m.13, m.14, m.15))
        placed.transform = Transform(matrix: matrix)
        volumePlacements += 1
        if volumePlacements == 1 || volumePlacements % 300 == 0 {
            // Where the frame's draws land in the volume (metres about its centre).
            var low = SIMD3<Float>(repeating: .infinity), high = SIMD3<Float>(repeating: -.infinity)
            for corner in 0..<8 {
                let p = SIMD4((corner & 1) != 0 ? frame.bounds_max.0 : frame.bounds_min.0,
                              (corner & 2) != 0 ? frame.bounds_max.1 : frame.bounds_min.1,
                              (corner & 4) != 0 ? frame.bounds_max.2 : frame.bounds_min.2, 1)
                let q = matrix * p
                low = simd_min(low, SIMD3(q.x, q.y, q.z))
                high = simd_max(high, SIMD3(q.x, q.y, q.z))
            }
            print(String(format: "[LMVR] mirror in the volume: draws x %.2f..%.2f y %.2f..%.2f z %.2f..%.2f m, scale %.5f, set %u",
                         low.x, high.x, low.y, high.y, low.z, high.z, frame.set_scale, frame.set_serial))
        }
        if frame.set_serial != setSerial {
            if setSerial != 0 { fadeStart = CACurrentMediaTime() }
            setSerial = frame.set_serial
        }
        fitSerial = frame.fit_serial
        fade()
        return true
    }

    /// No set until the next new frame (a game frame without a 3D scene).
    func hide() {
        guard hasScene else { return }
        entity.isEnabled = false
        liveEntity.isEnabled = false
        backdrop.isEnabled = false
        for slot in blendSlots { slot.entity.isEnabled = false }
        hasScene = false
        hidden = serial
        forgetSmooth()
    }
    /// The frame serial hidden by hide(): it isn't shown again (only a newer one is).
    private var hidden: UInt64 = 0

    /// The fade-in of a new placement (OpacityComponent on the mirror's root, which its parts take).
    func fade() {
        let t = (CACurrentMediaTime() - fadeStart) / Self.fadeSeconds
        let opacity = Float(min(max(t, 0), 1))
        if opacity >= 1 {
            if root.components.has(OpacityComponent.self) { root.components.remove(OpacityComponent.self) }
        } else {
            root.components.set(OpacityComponent(opacity: opacity * opacity))
        }
    }

    // MARK: - The mesh

    /// position float3, uv0 float2, the multiplier half4 (vertex colour), the addend half2 x 2
    /// (uv1, uv2): lmvr_visionos_mirror_vertex.
    private static func descriptor(vertices: Int, indices: Int) -> LowLevelMesh.Descriptor {
        LowLevelMesh.Descriptor(
            vertexCapacity: vertices,
            vertexAttributes: [.init(semantic: .position, format: .float3, layoutIndex: 0, offset: 0),
                               .init(semantic: .uv0, format: .float2, layoutIndex: 0, offset: 12),
                               .init(semantic: .color, format: .half4, layoutIndex: 0, offset: 20),
                               .init(semantic: .uv1, format: .half2, layoutIndex: 0, offset: 28),
                               .init(semantic: .uv2, format: .half2, layoutIndex: 0, offset: 32)],
            vertexLayouts: [.init(bufferIndex: 0, bufferStride: 36)],
            indexCapacity: indices, indexType: .uint32)
    }

    private func fill(into mesh: inout LowLevelMesh?, pending resourcePending: inout Bool,
                      vertices: UnsafePointer<lmvr_visionos_mirror_vertex>, vertexCount: Int,
                      indices: UnsafePointer<UInt32>, indexCount: Int, halfway: Bool = false) -> Bool {
        let fits = mesh.map { $0.vertexCapacity >= vertexCount && $0.indexCapacity >= indexCount } ?? false
        if !fits {
            do {
                // Grown in steps, not doubled: each write goes into a fresh buffer of the mesh's
                // capacity, and past some size between 3.2 and 4.3 MB those cost RealityKit 12 to 15
                // ms instead of 0.3 (TPVR, 83,500 vertices: 0.35 ms in a 3.2 MB buffer, 11 to 15 ms
                // in the 4.7 MB that doubling made). 36 bytes a vertex.
                mesh = try LowLevelMesh(descriptor: Self.descriptor(
                    vertices: max(65536, vertexCount.roundedUp(to: 8192)),
                    indices: max(131072, indexCount.roundedUp(to: 16384))))
                resourcePending = true
                counts.meshesMade += 1
            } catch {
                print("[LMVR] mirror: a mesh of \(vertexCount) vertices failed: \(error)")
                return false
            }
        }
        guard let mesh else { return false }
        counts.fills += 1
        // Into fresh buffers RealityKit swaps in once written (SHAR: written in place, the
        // headset's renderer drew between the vertices and the indices).
        mesh.replaceUnsafeMutableBytes(bufferIndex: 0) { raw in
            raw.copyMemory(from: UnsafeRawBufferPointer(start: vertices, count: vertexCount * 36))
            if halfway { Self.writeHalfway(raw, moving: smooth.moving, from: smooth.from) }
            // Test runs: LMVR_TEST_MIRROR_ADD_RED=1 makes every vertex mul 0, add opaque red: the
            // window should be solid red where the mirror draws (it checks uv1/uv2 reach the materials).
            if Self.addRed {
                for index in 0..<vertexCount { Self.paintRed(raw.baseAddress! + index * 36) }
            }
        }
        mesh.replaceUnsafeMutableIndices { raw in
            raw.copyMemory(from: UnsafeRawBufferPointer(start: indices, count: indexCount * 4))
        }
        uploadedBytes += vertexCount * 36 + indexCount * 4
        return true
    }

    /// LMVR_TEST_MIRROR_ADD_RED: the vertex at `vertex` gets mul 0, add opaque red.
    private static func paintRed(_ vertex: UnsafeMutableRawPointer) {
        let red: [UInt16] = [0, 0, 0, 0, 0x3C00, 0, 0, 0x3C00]  // half 1.0 = 0x3C00
        red.withUnsafeBytes { (vertex + 20).copyMemory(from: $0.baseAddress!, byteCount: 16) }
    }

    /// Each blended part into the slot that had its material last frame, if one did: by position, a
    /// dust puff appearing mid-list moved every later part to another slot, its geometry and
    /// material changing at once (which RealityKit may not show in the same frame).
    private func fillSlots(_ blendedParts: [(index: Int, material: ShaderGraphMaterial)],
                           parts: UnsafePointer<lmvr_visionos_mirror_part>, keys: [MaterialKey],
                           vertices: UnsafePointer<lmvr_visionos_mirror_vertex>, vertexCount: Int,
                           indices: UnsafePointer<UInt32>) {
        var taken = [Bool](repeating: false, count: blendSlots.count)
        var byMaterial: [MaterialKey: [Int]] = [:]
        for (s, slot) in blendSlots.enumerated().reversed() {
            if let key = slot.material { byMaterial[key, default: []].append(s) }
        }
        var slotFor = [Int](repeating: -1, count: blendedParts.count)
        for (k, part) in blendedParts.enumerated() {
            if let s = byMaterial[keys[part.index]]?.popLast() {
                slotFor[k] = s
                taken[s] = true
            }
        }
        var free = 0
        var blended = 0
        for (k, part) in blendedParts.enumerated() {
            var s = slotFor[k]
            if s < 0 {
                while free < taken.count && taken[free] { free += 1 }
                if free == taken.count {
                    let slot = BlendSlot()
                    placed.addChild(slot.entity)
                    blendSlots.append(slot)
                    taken.append(false)
                }
                s = free
                taken[s] = true
            }
            if fillBlend(blendSlots[s], order: k, part: parts[part.index], key: keys[part.index], material: part.material,
                         vertices: vertices, vertexCount: vertexCount, indices: indices) {
                blended += 1
            } else {
                blendSlots[s].entity.isEnabled = false
            }
        }
        for (s, slot) in blendSlots.enumerated() where !taken[s] { slot.entity.isEnabled = false }
        last.blended = blended
    }

    /// The frame's blended part `order` (the game's order) into `slot`: only the vertices it uses
    /// (most of the frame's are solid), its indices renumbered, one part. False: the slot shows nothing.
    private func fillBlend(_ slot: BlendSlot, order k: Int, part: lmvr_visionos_mirror_part, key: MaterialKey,
                           material: ShaderGraphMaterial, vertices: UnsafePointer<lmvr_visionos_mirror_vertex>,
                           vertexCount: Int, indices: UnsafePointer<UInt32>) -> Bool {
        let first = Int(part.first_index), count = Int(part.index_count)
        guard count > 0 else { return false }
        if remap.count < vertexCount { remap = [Int32](repeating: -1, count: vertexCount) }
        var used: [UInt32] = []
        used.reserveCapacity(count)
        var local = [UInt32](repeating: 0, count: count)
        for i in 0..<count {
            let v = Int(indices[first + i])
            guard v < vertexCount else { continue }
            if remap[v] < 0 {
                remap[v] = Int32(used.count)
                used.append(UInt32(v))
            }
            local[i] = UInt32(remap[v])
        }
        for v in used { remap[Int(v)] = -1 }
        guard !used.isEmpty else { return false }
        var low = SIMD3<Float>(repeating: .infinity), high = SIMD3<Float>(repeating: -.infinity)
        for v in used {
            let p = vertices[Int(v)].position
            let point = SIMD3(p.0, p.1, p.2)
            low = simd_min(low, point)
            high = simd_max(high, point)
        }
        let pad = (high - low) * 0.05 + 1
        let bounds = inVolume ? Self.volumeBounds : BoundingBox(min: low - pad, max: high + pad)
        // A bigger mesh replaces the slot's only once its resource is made: kept on failure, the
        // slot would go on drawing the old resource while the next frames wrote into the new mesh.
        var fresh = false
        var target = slot.mesh
        if target.map({ $0.vertexCapacity < used.count || $0.indexCapacity < count }) ?? true {
            guard let made = try? LowLevelMesh(descriptor: Self.descriptor(
                vertices: max(256, used.count.nextPowerOfTwo), indices: max(768, count.nextPowerOfTwo))) else {
                return false
            }
            target = made
            fresh = true
        }
        guard let mesh = target else { return false }
        mesh.replaceUnsafeMutableBytes(bufferIndex: 0) { raw in
            for (j, v) in used.enumerated() {
                let at = raw.baseAddress! + j * 36
                at.copyMemory(from: vertices + Int(v), byteCount: 36)
                if Self.addRed { Self.paintRed(at) }
            }
        }
        mesh.replaceUnsafeMutableIndices { raw in
            local.withUnsafeBytes { raw.copyMemory(from: $0) }
        }
        uploadedBytes += used.count * 36 + local.count * 4
        if fresh || !inVolume || slot.indexCount != count {
            mesh.parts.replaceAll([LowLevelMesh.Part(indexOffset: 0, indexCount: count, topology: .triangle,
                                                     materialIndex: 0, bounds: bounds)])
            slot.indexCount = count
        }
        // The resource only once the mesh has its part (RealityKit rules: made from a mesh without
        // one, it now and then never drew). The material only when the slot's key changes: set again
        // each frame, it may cost the renderer a frame.
        if fresh {
            guard let resource = try? MeshResource(from: mesh) else { return false }
            slot.mesh = mesh
            slot.entity.model = ModelComponent(mesh: resource, materials: [material])
            slot.material = key
        } else if slot.material != key {
            slot.entity.model?.materials = [material]
            slot.material = key
        }
        let order = sortOrder + 1 + Int32(k)
        if slot.order != order, let sortGroup {
            slot.entity.components.set(ModelSortGroupComponent(group: sortGroup, order: order))
            slot.order = order
        }
        slot.entity.isEnabled = true
        return true
    }

    // MARK: - Materials

    private func updateMaterials(_ keys: [MaterialKey]) {
        var changed = false
        // Counting new materials, not parts: blended parts in the game's order repeat theirs, and
        // counting parts cleared and set the list every frame of a busy scene (SHAR's review), which
        // may cost the renderer a frame of the whole level.
        let fresh = Set(keys).filter { materialIndex[$0] == nil }.count
        if materialList.count + fresh > 1024 {
            // (The held shell's parts index the old list, and so would a hold begun from lastParts.)
            if holding { endHold() }
            lastParts = []
            lastWorld = []
            materialList = []
            materialIndex = [:]
            materialReady = []
            changed = true
        }
        if texturesArrived {
            texturesArrived = false
            changed = changed || materialList.indices.contains { !materialReady[$0] && material(for: materialList[$0]) != nil }
        }
        for key in keys where materialIndex[key] == nil {
            materialIndex[key] = materialList.count
            materialList.append(key)
            changed = true
        }
        guard changed else { return }
        counts.materialSets += 1
        materialReady = materialList.map { material(for: $0) != nil }
        let list = currentMaterials()
        entity.model?.materials = list
        liveEntity.model?.materials = list
    }

    /// Smoother motion: whether this frame's moving vertices are shown halfway first (it's made like
    /// the frame before, in parts), with what they go from; and what the next frame goes from.
    private func prepareSmooth(_ frame: lmvr_visionos_mirror_frame, vertices: UnsafePointer<lmvr_visionos_mirror_vertex>,
                               indices: UnsafePointer<UInt32>, parts: UnsafePointer<lmvr_visionos_mirror_part>) -> Bool {
        guard inVolume, !holding, frame.placed, Self.smoothWanted, !Self.addRed,
              Int(frame.vertex_count) <= Self.kSmoothMaxVertices else {
            forgetSmooth()
            return false
        }
        let count = Int(frame.vertex_count), indexCount = Int(frame.index_count)
        let m = frame.volume_from_view
        let toVolume = float4x4(SIMD4(m.0, m.1, m.2, m.3), SIMD4(m.4, m.5, m.6, m.7),
                                SIMD4(m.8, m.9, m.10, m.11), SIMD4(m.12, m.13, m.14, m.15))
        let fromVolume = toVolume.inverse
        let raw = UnsafeRawPointer(vertices)
        var volume = [SIMD3<Float>](repeating: .zero, count: count)
        for i in 0..<count {
            let p = SIMD3(raw.loadUnaligned(fromByteOffset: i * 36, as: Float.self),
                          raw.loadUnaligned(fromByteOffset: i * 36 + 4, as: Float.self),
                          raw.loadUnaligned(fromByteOffset: i * 36 + 8, as: Float.self))
            let q = toVolume * SIMD4(p, 1)
            volume[i] = SIMD3(q.x, q.y, q.z)
        }
        // This frame's solid parts, each with its lowest vertex (indices are the frame's own numbers:
        // a draw that grows or shrinks earlier in the frame renumbers every later one).
        var mine: [SmoothPartKey: [SIMD2<UInt32>]] = [:]
        var lowest = [UInt32](repeating: 0, count: Int(frame.part_count))
        for part in 0..<Int(frame.part_count) where parts[part].kind != LMVR_VISIONOS_MIRROR_PART_BLEND {
            let first = Int(parts[part].first_index), n = Int(parts[part].index_count)
            guard n > 0, first + n <= indexCount else { continue }
            var low = UInt32.max
            for k in first..<(first + n) { low = min(low, indices[k]) }
            lowest[part] = low
            mine[SmoothPartKey(texture: parts[part].texture, kind: parts[part].kind, indexCount: UInt32(n)),
                 default: []].append(SIMD2(UInt32(first), low))
        }
        // Part by part: a solid part drawn as one of the frame before's (the same look, the same
        // triangles over its own vertices) goes halfway; the rest stand where they are now (a dust
        // puff or a part that changed doesn't stop the others). Blended parts (ghosts, light,
        // effects) stay at the game's rate; a part that jumps further than kSmoothJump in a frame (a
        // warp, a respawn) snaps, as does everything at a new fit or placing, or with updates too slow
        // for a halfway to show.
        var moving: [UInt32] = [], from: [SIMD3<Float>] = []
        let before = smooth.volume, old = smooth.indices
        if smoothFast, smooth.vertexCount > 0, !old.isEmpty,
           frame.fit_serial == smooth.fit, frame.set_serial == smooth.set {
            var candidates = smooth.parts
            var taken = [Bool](repeating: false, count: count)
            old.withUnsafeBufferPointer { old in
                for part in 0..<Int(frame.part_count) where parts[part].kind != LMVR_VISIONOS_MIRROR_PART_BLEND {
                    let first = Int(parts[part].first_index), n = Int(parts[part].index_count)
                    guard n > 0, first + n <= indexCount else { continue }
                    let key = SmoothPartKey(texture: parts[part].texture, kind: parts[part].kind, indexCount: UInt32(n))
                    guard let list = candidates[key], !list.isEmpty else { continue }
                    let low = lowest[part]
                    // The first of the frame before's like parts with the same triangles.
                    var match: (index: Int, first: Int, low: UInt32)?
                    for (c, earlier) in list.enumerated() {
                        let oFirst = Int(earlier.x), oLow = earlier.y
                        guard oFirst + n <= old.count else { continue }
                        var same = true
                        for k in 0..<n where old[oFirst + k] &- oLow != indices[first + k] &- low {
                            same = false
                            break
                        }
                        if same { match = (c, oFirst, oLow); break }
                    }
                    guard let match else { continue }
                    candidates[key]!.remove(at: match.index)
                    var most: Float = 0
                    for k in first..<(first + n) {
                        let v = Int(indices[k]), w = Int(indices[k] &- low &+ match.low)
                        guard v < count, w < before.count else { continue }
                        most = max(most, simd_length_squared(volume[v] - before[w]))
                    }
                    // Still (nothing to show between) or a jump (snap): where it is now.
                    guard most > Self.kSmoothStill * Self.kSmoothStill,
                          most < Self.kSmoothJump * Self.kSmoothJump else { continue }
                    for k in first..<(first + n) {
                        let v = Int(indices[k]), w = Int(indices[k] &- low &+ match.low)
                        guard v < count, w < before.count, !taken[v] else { continue }
                        taken[v] = true
                        let q = fromVolume * SIMD4(before[w], 1)
                        moving.append(UInt32(v))
                        from.append(SIMD3(q.x, q.y, q.z))
                    }
                }
            }
        }
        let halfway = !moving.isEmpty
        if halfway {
            smooth.bytes = [UInt8](UnsafeRawBufferPointer(start: raw, count: count * 36))
            smoothPaired += 1
            smoothVertices += moving.count
        } else {
            smoothSnapped += 1
        }
        smooth.moving = moving
        smooth.from = from
        smooth.volume = volume
        smooth.vertexCount = count
        smooth.indices = [UInt32](UnsafeBufferPointer(start: indices, count: indexCount))
        smooth.parts = mine
        smooth.fit = frame.fit_serial
        smooth.set = frame.set_serial
        return halfway
    }

    /// Smoother motion: nothing from the frame before to go from (a gap in the set, the setting off).
    private func forgetSmooth() {
        smooth.vertexCount = -1
        smooth.pending = false
        smooth.justApplied = false
    }

    /// The moving vertices halfway between where they stood (from) and where they're recorded (in
    /// the buffer already).
    private static func writeHalfway(_ out: UnsafeMutableRawBufferPointer, moving: [UInt32], from: [SIMD3<Float>]) {
        guard let base = out.baseAddress, moving.count == from.count else { return }
        for (k, v) in moving.enumerated() {
            let at = Int(v) * 36
            guard at + 12 <= out.count else { continue }
            let to = SIMD3(base.loadUnaligned(fromByteOffset: at, as: Float.self),
                           base.loadUnaligned(fromByteOffset: at + 4, as: Float.self),
                           base.loadUnaligned(fromByteOffset: at + 8, as: Float.self))
            let p = (from[k] + to) * 0.5
            base.storeBytes(of: p.x, toByteOffset: at, as: Float.self)
            base.storeBytes(of: p.y, toByteOffset: at + 4, as: Float.self)
            base.storeBytes(of: p.z, toByteOffset: at + 8, as: Float.self)
        }
    }

    /// Smoother motion, each RealityKit update: the update after a frame showed its moving parts
    /// halfway, the frame as recorded.
    func easeSmooth() {
        guard smooth.pending else { return }
        if smooth.justApplied {
            smooth.justApplied = false
            return
        }
        guard !holding, let mesh else {
            smooth.pending = false
            return
        }
        finishSmooth(mesh)
    }

    /// The frame shown halfway, as recorded.
    private func finishSmooth(_ mesh: LowLevelMesh) {
        guard smooth.pending else { return }
        smooth.pending = false
        smooth.justApplied = false
        guard smooth.vertexCount > 0, smooth.bytes.count == smooth.vertexCount * 36,
              mesh.vertexCapacity >= smooth.vertexCount else { return }
        smooth.bytes.withUnsafeBytes { recorded in
            mesh.replaceUnsafeMutableBytes(bufferIndex: 0) { $0.copyMemory(from: recorded) }
        }
        uploadedBytes += smooth.bytes.count
    }

    /// The held shell goes back to the level (its parts set again by the next frame in play).
    private func endHold() {
        guard holding else { return }
        holding = false
        entity.removeFromParent()
        placed.addChild(entity)
        liveEntity.isEnabled = false
        lastParts = []  // the main mesh's parts are the shell's only: set them again
        playFrames = 0  // play shows the room again before another hold
        print("[LMVR] mirror: the room's shell let go")
    }

    /// The material list the parts index (a stand-in where a texture is still on its way).
    private func currentMaterials() -> [any Material] {
        materialList.map { material(for: $0) ?? templates["OpaqueRR"]! }
    }

    private func material(for key: MaterialKey) -> ShaderGraphMaterial? {
        if let made = materials[key] { return made }
        let texture = key.texture == 0 || failedTextures.contains(key.texture) ? white : textures[key.texture]
        guard let texture else { return nil }
        let kind = key.kind == LMVR_VISIONOS_MIRROR_PART_CUTOUT ? "Cutout" : key.kind == LMVR_VISIONOS_MIRROR_PART_BLEND ? "Blend" : "Opaque"
        guard var made = templates[kind + key.wrap] ?? templates[kind + "RR"] else { return nil }
        try? made.setParameter(name: "Frame", value: .textureResource(texture))
        switch key.kind {
        case LMVR_VISIONOS_MIRROR_PART_CUTOUT:
            try? made.setParameter(name: "Cutoff", value: .float(Float(max(key.cutoff, 1)) / 255))
        case LMVR_VISIONOS_MIRROR_PART_BLEND:
            for (index, name) in ["ColourBase", "ColourAlpha", "OpacityBase", "OpacityAlpha", "OpacityLuma"].enumerated() {
                try? made.setParameter(name: name, value: .float(Float(key.weights[index]) / 16))
            }
            // Blended surfaces hide what's behind them only where the game's do.
            made.writesDepth = key.flags & LMVR_VISIONOS_MIRROR_DEPTH_WRITE != 0
        default:
            break
        }
        // A blend the game draws without a depth test draws over everything, except the sky's
        // layers: pushed behind the level, the level hides them where it stands.
        made.readsDepth = key.flags & (LMVR_VISIONOS_MIRROR_DEPTH_TEST | LMVR_VISIONOS_MIRROR_BACKGROUND) != 0 || key.kind != LMVR_VISIONOS_MIRROR_PART_BLEND
        made.faceCulling = key.flags & LMVR_VISIONOS_MIRROR_CULL_BACK != 0 ? .back
            : key.flags & LMVR_VISIONOS_MIRROR_CULL_FRONT != 0 ? .front : .none
        // Test runs: LMVR_TEST_MIRROR_CULL=none draws both sides, =flip culls the other side.
        switch Self.cullTest {
        case "none": made.faceCulling = .none
        case "flip": made.faceCulling = made.faceCulling == .back ? .front : made.faceCulling == .front ? .back : .none
        default: break
        }
        if materials.count > 4096 { materials.removeAll(keepingCapacity: true) }
        materials[key] = made
        return made
    }

    // MARK: - Textures

    private func takeTextures(_ frame: lmvr_visionos_mirror_frame, start: CFTimeInterval) {
        if let list = frame.textures {
            for index in 0..<Int(frame.texture_count) {
                let texture = list[index]
                let count = Int(texture.width) * Int(texture.height) * 4
                guard let rgba = texture.rgba, count > 0 else { continue }
                // Back after being let go: the same texture, still here (below).
                if parkedSet.remove(texture.id) != nil {
                    parked.removeAll { $0 == texture.id }
                }
                // A mipmapped id is its pixels (scene_mirror.h): the one made stands.
                if texture.mipmapped, textures[texture.id] != nil { continue }
                // New pixels for one already made (a shadow, each frame): replaced in place.
                if let existing = lowLevelTextures[texture.id], Self.updatesInPlace(existing, width: Int(texture.width),
                                                                                    height: Int(texture.height),
                                                                                    cutout: texture.cutout) {
                    Self.upload(queue: queue, pixels: rgba, width: Int(texture.width), height: Int(texture.height), into: existing)
                    continue
                }
                let pending = PendingTexture(id: texture.id, pixels: Data(bytes: rgba, count: count),
                                             width: Int(texture.width), height: Int(texture.height),
                                             mipmapped: texture.mipmapped, cutout: texture.cutout)
                // A newer read-back of one still waiting replaces it.
                if let waiting = pendingTextures.firstIndex(where: { $0.id == texture.id }) {
                    pendingTextures[waiting] = pending
                } else {
                    pendingTextures.append(pending)
                }
            }
        }
        // After the new ones: a texture can arrive and be gone in the same frame. An area's
        // textures go together, a few hundred at once: one pass over each collection.
        // Those made are parked, not dropped; one that couldn't be made (drawn white) loses its
        // materials, so that when it comes back and is made, its parts wait for it and the list is
        // set once.
        if let removed = frame.removed, frame.removed_count > 0 {
            let gone = Set((0..<Int(frame.removed_count)).map { removed[$0] })
            var failedGone: Set<UInt64> = []
            for id in gone where !parkedSet.contains(id) {
                if textures[id] != nil {
                    parked.append(id)
                    parkedSet.insert(id)
                } else if failedTextures.remove(id) != nil {
                    failedGone.insert(id)
                }
            }
            if !failedGone.isEmpty { forgetMaterials(using: failedGone) }
            pendingTextures.removeAll { gone.contains($0.id) }
            dropParked(beyond: Self.parkedBudget)
        }
        // Wanted ones first, then a few milliseconds' worth an update, at least one.
        if !wantedTextures.isEmpty {
            let wanted = pendingTextures.filter { wantedTextures.contains($0.id) }
            pendingTextures = wanted + pendingTextures.filter { !wantedTextures.contains($0.id) }
            wantedTextures.removeAll()
        }
        var made = 0
        while !pendingTextures.isEmpty && (made == 0 || CACurrentMediaTime() - start < 0.004) {
            let pending = pendingTextures.removeFirst()
            if Self.dumpFrame >= 0 {
                dumpTextures[pending.id] = pending
            }
            // Read back again before its first was made: into the texture already there.
            if let existing = lowLevelTextures[pending.id],
               Self.updatesInPlace(existing, width: pending.width, height: pending.height, cutout: pending.cutout) {
                pending.pixels.withUnsafeBytes {
                    Self.upload(queue: queue, pixels: $0.bindMemory(to: UInt8.self).baseAddress!, width: pending.width,
                                height: pending.height, into: existing)
                }
                made += 1
                continue
            }
            if let (lowLevel, resource) = makeTexture(pending) {
                // A new resource for an id already drawn (a resized render target): materials made
                // with the old one would keep drawing it.
                if textures[pending.id] != nil || failedTextures.contains(pending.id) {
                    forgetMaterials(using: [pending.id])
                }
                failedTextures.remove(pending.id)
                textures[pending.id] = resource
                lowLevelTextures[pending.id] = lowLevel
                counts.texturesMade += 1
            } else {
                failedTextures.insert(pending.id)
                counts.texturesFailed += 1
            }
            texturesArrived = true
            made += 1
        }
    }

    /// Drops the materials made with these textures and marks their list entries not ready, so the
    /// parts wait for the texture (again) and the list is set anew once it's there. A blended part's
    /// slot forgets its material too: it sets one only when its key changes, so it stayed white.
    private func forgetMaterials(using ids: Set<UInt64>) {
        materials = materials.filter { !ids.contains($0.key.texture) }
        for (index, key) in materialList.enumerated() where ids.contains(key.texture) {
            materialReady[index] = false
        }
        for slot in blendSlots where slot.material.map({ ids.contains($0.texture) }) == true {
            slot.material = nil
        }
    }

    /// Lets the longest-gone parked textures go for good, with their materials, until the rest fit.
    private func dropParked(beyond budget: Int) {
        var bytes = parked.reduce(0) { $0 + Self.bytes(of: lowLevelTextures[$1]) }
        guard bytes > budget else { return }
        var dropped: Set<UInt64> = []
        while bytes > budget, !parked.isEmpty {
            let id = parked.removeFirst()
            parkedSet.remove(id)
            bytes -= Self.bytes(of: lowLevelTextures[id])
            textures[id] = nil
            lowLevelTextures[id] = nil
            dropped.insert(id)
        }
        forgetMaterials(using: dropped)
    }

    private static func bytes(of texture: LowLevelTexture?) -> Int {
        guard let descriptor = texture?.descriptor else { return 0 }
        let level = descriptor.width * descriptor.height * 4
        return descriptor.mipmapLevelCount > 1 ? level * 4 / 3 : level
    }

    /// Whether new pixels can go into the texture already made: the same size, and its mipmaps (if
    /// any) ones the GPU makes (a cut-out's are made on the CPU, so it gets a new texture).
    private static func updatesInPlace(_ texture: LowLevelTexture, width: Int, height: Int, cutout: Bool) -> Bool {
        texture.descriptor.width == width && texture.descriptor.height == height
            && (texture.descriptor.mipmapLevelCount == 1 || !cutout)
    }

    private func makeTexture(_ texture: PendingTexture) -> (LowLevelTexture, TextureResource)? {
        let levels = texture.mipmapped && texture.cutout ? Self.coverageMipmaps(texture) : nil
        return (levels ?? texture.pixels).withUnsafeBytes {
            Self.makeTexture(queue: queue, pixels: $0.baseAddress!, width: texture.width, height: texture.height,
                             mipmapped: texture.mipmapped, levelsIncluded: levels != nil)
        }
    }

    private static func makeTexture(queue: MTLCommandQueue, pixels: [UInt8], width: Int, height: Int) -> TextureResource? {
        pixels.withUnsafeBytes { makeTexture(queue: queue, pixels: $0.baseAddress!, width: width, height: height)?.1 }
    }

    /// New level-zero pixels into a texture already made.
    private static func upload(queue: MTLCommandQueue, pixels: UnsafePointer<UInt8>, width: Int, height: Int,
                               into texture: LowLevelTexture) {
        guard let staging = queue.device.makeBuffer(bytes: pixels, length: width * height * 4) else {
            report("a \(width)x\(height) texture update: no staging buffer")
            return
        }
        blit(staging, into: texture, sizes: [(width, height)], generate: texture.descriptor.mipmapLevelCount > 1,
             queue: queue, label: "a \(width)x\(height) texture update")
    }

    /// The staging buffer's levels into the texture (the GPU making the rest with `generate`). One the
    /// GPU reports failing is tried again, once, into the same texture, so what draws it needn't
    /// change. (The SHAR port's headset-only "black void", ground and sky gone, was failed uploads
    /// the Simulator never showed: they'd have gone unnoticed here.)
    @discardableResult
    private static func blit(_ staging: MTLBuffer, into texture: LowLevelTexture, sizes: [(Int, Int)], generate: Bool,
                             queue: MTLCommandQueue, label: String, retries: Int = 1) -> Bool {
        guard let commands = queue.makeCommandBuffer(), let blit = commands.makeBlitCommandEncoder() else {
            report("\(label): no command buffer")
            return false
        }
        let destination = texture.replace(using: commands)
        var offset = 0
        for (level, (levelWidth, levelHeight)) in sizes.enumerated() {
            blit.copy(from: staging, sourceOffset: offset, sourceBytesPerRow: levelWidth * 4,
                      sourceBytesPerImage: levelWidth * levelHeight * 4,
                      sourceSize: MTLSize(width: levelWidth, height: levelHeight, depth: 1), to: destination,
                      destinationSlice: 0, destinationLevel: level, destinationOrigin: MTLOrigin())
            offset += levelWidth * levelHeight * 4
        }
        if generate { blit.generateMipmaps(for: destination) }
        blit.endEncoding()
        uploadsInFlight.add(1, ordering: .relaxed)
        commands.addCompletedHandler { done in
            uploadsInFlight.subtract(1, ordering: .relaxed)
            guard let error = done.error else { return }
            let message = "\(label): upload failed\(retries > 0 ? ", trying again" : ""): \(error)"
            DispatchQueue.main.async {
                report(message)
                if retries > 0 {
                    Self.blit(staging, into: texture, sizes: sizes, generate: generate, queue: queue, label: label,
                              retries: retries - 1)
                }
            }
        }
        commands.commit()
        return true
    }

    // The first few failures, word for word.
    private static var reports = 0
    private static func report(_ message: String) {
        reports += 1
        if reports <= 20 { print("[LMVR] mirror: \(message)") }
    }

    /// RGBA8 (sRGB) into a texture, its mipmaps made by the GPU or, with `levelsIncluded`, given
    /// one after another in `pixels`.
    private static func makeTexture(queue: MTLCommandQueue, pixels: UnsafeRawPointer, width: Int, height: Int,
                                    mipmapped: Bool = false, levelsIncluded: Bool = false) -> (LowLevelTexture, TextureResource)? {
        let levels = mipmapped ? Int(log2(Double(max(width, height)))) + 1 : 1
        let descriptor = LowLevelTexture.Descriptor(pixelFormat: .rgba8Unorm_srgb, width: width, height: height,
                                                    mipmapLevelCount: levels, textureUsage: [.shaderRead, .renderTarget])
        let label = "a \(width)x\(height) texture"
        let texture: LowLevelTexture
        do { texture = try LowLevelTexture(descriptor: descriptor) } catch {
            report("\(label): LowLevelTexture failed: \(error)")
            return nil
        }
        let sizes = (0..<(levelsIncluded ? levels : 1)).map { (max(1, width >> $0), max(1, height >> $0)) }
        guard let staging = queue.device.makeBuffer(bytes: pixels, length: sizes.reduce(0) { $0 + $1.0 * $1.1 * 4 }) else {
            report("\(label): no staging buffer")
            return nil
        }
        // (It reports its own failures.)
        guard Self.blit(staging, into: texture, sizes: sizes, generate: levels > 1 && !levelsIncluded, queue: queue,
                        label: label) else { return nil }
        do { return (texture, try TextureResource(from: texture)) } catch {
            report("\(label): TextureResource failed: \(error)")
            return nil
        }
    }

    /// A cut-out texture's mipmaps (SHAR's): each texel the alpha-weighted mean of the four below
    /// it, its alpha scaled so as many texels pass a half alpha test as at full size. A box filter
    /// alone thins foliage with distance, and its edges shimmer.
    private static func coverageMipmaps(_ texture: PendingTexture) -> Data? {
        var width = texture.width, height = texture.height
        guard width > 1 || height > 1 else { return nil }
        var level = [UInt8](texture.pixels)
        let covered = { (pixels: [UInt8], scale: Float) -> Int in
            stride(from: 3, to: pixels.count, by: 4).reduce(0) { $0 + (Float(pixels[$1]) * scale >= 127.5 ? 1 : 0) }
        }
        let coverage = Float(covered(level, 1)) / Float(width * height)
        var chain = Data(level)
        while width > 1 || height > 1 {
            let nextWidth = max(1, width / 2), nextHeight = max(1, height / 2)
            var next = [UInt8](repeating: 0, count: nextWidth * nextHeight * 4)
            for y in 0..<nextHeight {
                for x in 0..<nextWidth {
                    var colour = SIMD3<Float>.zero, plain = SIMD3<Float>.zero, alpha: Float = 0
                    for (dx, dy) in [(0, 0), (1, 0), (0, 1), (1, 1)] {
                        let at = (min(y * 2 + dy, height - 1) * width + min(x * 2 + dx, width - 1)) * 4
                        let texel = SIMD3<Float>(Float(level[at]), Float(level[at + 1]), Float(level[at + 2]))
                        let a = Float(level[at + 3])
                        colour += texel * a
                        plain += texel
                        alpha += a
                    }
                    let rgb = alpha > 0 ? colour / alpha : plain / 4
                    let at = (y * nextWidth + x) * 4
                    next[at] = UInt8(rgb.x.rounded())
                    next[at + 1] = UInt8(rgb.y.rounded())
                    next[at + 2] = UInt8(rgb.z.rounded())
                    next[at + 3] = UInt8((alpha / 4).rounded())
                }
            }
            var low: Float = 0, high: Float = 8
            for _ in 0..<12 {
                let middle = (low + high) / 2
                if Float(covered(next, middle)) / Float(nextWidth * nextHeight) < coverage { low = middle } else { high = middle }
            }
            for at in stride(from: 3, to: next.count, by: 4) { next[at] = UInt8(min(255, (Float(next[at]) * high).rounded())) }
            chain.append(contentsOf: next)
            (level, width, height) = (next, nextWidth, nextHeight)
        }
        return chain
    }

    // MARK: - Test runs

    // LMVR_TEST_MIRROR_DUMP=<frame> writes that mirror frame raw into Documents/mirror-dump:
    // vertices.bin (36-byte vertices), indices.bin (u32), parts.txt, camera.txt, and the
    // textures made so far (tex-<id>-<w>x<h>.rgba).
    private static let dumpFrame = ProcessInfo.processInfo.environment["LMVR_TEST_MIRROR_DUMP"].flatMap(Int.init) ?? -1
    private var dumpTextures: [UInt64: PendingTexture] = [:]

    private func dump(_ frame: lmvr_visionos_mirror_frame) {
        let folder = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("mirror-dump")
        try? FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        if let vertices = frame.vertices {
            try? Data(bytes: vertices, count: Int(frame.vertex_count) * 36).write(to: folder.appendingPathComponent("vertices.bin"))
        }
        if let indices = frame.indices {
            try? Data(bytes: indices, count: Int(frame.index_count) * 4).write(to: folder.appendingPathComponent("indices.bin"))
        }
        var parts = ""
        for index in 0..<Int(frame.part_count) {
            let p = frame.parts![index]
            parts += "\(p.first_index) \(p.index_count) \(p.texture) \(p.kind) \(p.flags) \(p.cutoff) "
                + "\(p.colour_base) \(p.colour_alpha) \(p.opacity_base) \(p.opacity_alpha) \(p.opacity_luma) "
                + "\(p.wrap_s) \(p.wrap_t)\n"
        }
        try? parts.write(to: folder.appendingPathComponent("parts.txt"), atomically: true, encoding: .utf8)
        let m = frame.volume_from_view
        let camera = "\(frame.tan_half_x) \(frame.tan_half_y) \(frame.focus) \(frame.bounds_min) \(frame.bounds_max)\n"
            + "volume_from_view \(m.0) \(m.1) \(m.2) \(m.3) \(m.4) \(m.5) \(m.6) \(m.7) \(m.8) \(m.9) \(m.10) \(m.11) \(m.12) \(m.13) \(m.14) \(m.15) placed \(frame.placed)\n"
        try? camera.write(to: folder.appendingPathComponent("camera.txt"), atomically: true, encoding: .utf8)
        for (id, texture) in dumpTextures {
            try? texture.pixels.write(to: folder.appendingPathComponent("tex-\(id)-\(texture.width)x\(texture.height).rgba"))
        }
        print("[LMVR] mirror frame dumped: \(frame.vertex_count) vertices, \(frame.part_count) parts, \(dumpTextures.count) textures")
    }

    // MARK: - Report

    /// What the mirror holds, for the window's memory log (WindowMemoryGuard).
    var diagnostics: String {
        let textureBytes = lowLevelTextures.values.reduce(0) { $0 + Self.bytes(of: $1) }
        let waitingBytes = pendingTextures.reduce(0) { $0 + $1.pixels.count }
        return "\(lastReport); last frame \(last.vertices) vertices, \(last.indices) indices, \(last.parts) parts "
            + "(\(last.blended) blended, in \(blendSlots.count) slots); "
            + "\(lowLevelTextures.count) textures (~\(textureBytes >> 20) MB, \(parked.count) parked; "
            + "\(counts.texturesMade) made, \(counts.texturesFailed) failed), \(pendingTextures.count) waiting "
            + "(\(waitingBytes >> 20) MB), \(Self.uploadsInFlight.load(ordering: .relaxed)) uploads in flight, "
            + "\(materials.count) materials (\(materialList.count) listed), mesh room for "
            + "\(mesh?.vertexCapacity ?? 0) vertices (\(counts.meshesMade) meshes made, \(counts.fills) fills)"
    }
    private var last = (vertices: 0, indices: 0, parts: 0, blended: 0)
    /// The last 5-s report's rates, for the memory log (the headset doesn't keep stdout).
    private var lastReport = "no report yet"
    /// Texture uploads committed to the GPU and not yet done, for the memory log.
    nonisolated private static let uploadsInFlight = Atomic<Int>(0)

    /// Every 5 s: how often RealityKit updates, how many game frames came, and what they cost here.
    private func report(_ start: CFTimeInterval) {
        timing.updates += 1
        timing.seconds += CACurrentMediaTime() - start
        guard start - timing.since > 5 else { return }
        let updates = Double(timing.updates), frames = Double(max(timing.frames, 1))
        print(String(format: "[LMVR] mirror: %.0f updates/s, %.0f frames/s, %.2f ms an update (fill %.2f, parts %.2f), "
                     + "%.0f vertices and %.0f parts (%.0f drawn) "
                     + "a frame; %d materials, list set %d times; textures %d made, %d failed, %d waiting",
                     updates / (start - timing.since), Double(timing.frames) / (start - timing.since),
                     timing.seconds / updates * 1000, stages.fill / updates * 1000, stages.parts / updates * 1000,
                     Double(timing.vertices) / frames, Double(timing.parts) / frames, Double(timing.drawn) / frames,
                     materials.count, counts.materialSets, counts.texturesMade, counts.texturesFailed, pendingTextures.count))
        lastReport = String(format: "%.0f updates/s, %.0f frames/s, %.2f ms an update (fill %.2f, parts %.2f)",
                            updates / (start - timing.since), Double(timing.frames) / (start - timing.since),
                            timing.seconds / updates * 1000, stages.fill / updates * 1000, stages.parts / updates * 1000)
        timing = (0, 0, 0, start, 0, 0, 0, timing.total)
        stages = (0, 0)
        counts.materialSets = 0
    }
}

private extension Int {
    var nextPowerOfTwo: Int { self <= 1 ? 1 : 1 << (Int.bitWidth - (self - 1).leadingZeroBitCount) }
    func roundedUp(to step: Int) -> Int { (self + step - 1) / step * step }
}
