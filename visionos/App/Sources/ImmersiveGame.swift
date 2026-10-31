import CompositorServices
import SwiftUI

/// The frame the game renders into. The CompositorLayer's renderer goes to the
/// game through its bridge; the game (usually through the vendored OpenXR
/// provider) paces frames, places the eyes and composites into the drawable.
enum ImmersiveGame {
    /// `progressive`: the layer of the progressive space, which must be layered
    /// (its every frame ends through the system's render context).
    @MainActor
    static func layer(for model: GameModel, progressive: Bool) -> CompositorLayer {
        // Foveation stays off in the progressive space until it has been tried
        // there with a rasterization rate map on the headset.
        let configuration = GameLayerConfiguration(progressive: progressive, foveated: model.foveated && !progressive)
        return CompositorLayer(configuration: configuration) { layerRenderer in
            model.attach(layerRenderer)
        }
    }
}

/// sRGB colour, 32-bit float depth (reverse-Z), foveation when asked for and
/// supported. One texture per eye (dedicated) for the full/mixed space; one
/// layered texture for the progressive space, whose portal edge the system's
/// render context draws, which needs both eyes in one render pass.
struct GameLayerConfiguration: CompositorLayerConfiguration {
    var progressive = false
    var foveated = false

    func makeConfiguration(capabilities: LayerRenderer.Capabilities,
                           configuration: inout LayerRenderer.Configuration) {
        configuration.layout = .dedicated
        configuration.colorFormat = .bgra8Unorm_srgb
        if progressive, #available(visionOS 26.0, *) {
            let options: LayerRenderer.Capabilities.SupportedLayoutsOptions = .progressiveImmersionEnabled
            if capabilities.supportedLayouts(options: options).contains(.layered) {
                configuration.layout = .layered
            }
            let colors = capabilities.supportedColorFormats(options: .progressiveImmersionEnabled)
            if !colors.isEmpty, !colors.contains(.bgra8Unorm_srgb) {
                configuration.colorFormat = colors[0]
            }
        }
        configuration.depthFormat = .depth32Float
        configuration.isFoveationEnabled = foveated && capabilities.supportsFoveation
        print("[LMVR] layer: layout \(configuration.layout), colour \(configuration.colorFormat.rawValue), "
              + "progressive \(progressive), foveated \(configuration.isFoveationEnabled)")
    }
}
