#include "gx.hpp"

#include "pipeline.hpp"
#include "shader_info.hpp"
#include "texture.hpp"
#include "../dolphin/vi/vi_internal.hpp"
#include "../webgpu/gpu.hpp"
#include "../internal.hpp"
#include "../window.hpp"
#include "../gfx/resources.hpp"
#include "../gfx/recording.hpp"
#include "../gfx/resource_cache.hpp"
#include "../gfx/texture.hpp"
#include "../gfx/enhanced.hpp"
#include "gx_fmt.hpp"
#include "dolphin/gx/GXAurora.h"

#include <absl/container/flat_hash_map.h>
#include <tracy/Tracy.hpp>

#include <atomic>
#include <bit>
#include <cfloat>
#include <cmath>
#include <mutex>
#include <utility>

static aurora::Module Log("aurora::gx");

namespace aurora::gx {
using webgpu::g_device;
using webgpu::g_graphicsConfig;

GXState g_gxState{};
std::atomic<u32> g_accuracyFlags{GX_AURORA_ACCURACY_INT_TEV | GX_AURORA_ACCURACY_EFB_DEPTH};
u8 g_drawEfbBlend6 = 0;
std::atomic<u32> g_texGenBeyondCount{GX_AURORA_TEXGEN_BEYOND_GENERATE};
wgpu::BindGroup g_emptyTextureBindGroup;

namespace {
wgpu::Sampler sEmptySampler;
wgpu::Texture sEmptyTexture;
wgpu::TextureView sEmptyTextureView;
std::mutex sBindGroupLayoutMutex;
absl::flat_hash_map<u32, wgpu::BindGroupLayout> sUniformBindGroupLayouts;
absl::flat_hash_map<u32, std::pair<wgpu::BindGroupLayout, wgpu::BindGroupLayout>> sTextureBindGroupLayouts;
wgpu::BindGroupLayout sTextureBindGroupLayout;
wgpu::BindGroupLayout sSamplerBindGroupLayout;
wgpu::PipelineLayout sPipelineLayout;

std::atomic<int> sPendingViewportPolicy{-1};

template <typename T>
T round_away_from_zero(float value) noexcept {
  return static_cast<T>(value < 0.0f ? std::floor(value) : std::ceil(value));
}

std::pair<f32, f32> polygon_offset_for_cull_mode(GXCullMode cullMode) noexcept {
  if (cullMode == GX_CULL_FRONT) {
    return {g_gxState.backOffset, g_gxState.backScale};
  }
  return {g_gxState.frontOffset, g_gxState.frontScale};
}

wgpu::BlendFactor to_blend_factor(GXBlendFactor fac, bool isDst) {
  switch (fac) {
    DEFAULT_FATAL("invalid blend factor {}", underlying(fac));
  case GX_BL_ZERO:
    return wgpu::BlendFactor::Zero;
  case GX_BL_ONE:
    return wgpu::BlendFactor::One;
  case GX_BL_SRCCLR: // + GX_BL_DSTCLR
    if (isDst) {
      return wgpu::BlendFactor::Src;
    } else {
      return wgpu::BlendFactor::Dst;
    }
  case GX_BL_INVSRCCLR: // + GX_BL_INVDSTCLR
    if (isDst) {
      return wgpu::BlendFactor::OneMinusSrc;
    } else {
      return wgpu::BlendFactor::OneMinusDst;
    }
  case GX_BL_SRCALPHA:
    return wgpu::BlendFactor::SrcAlpha;
  case GX_BL_INVSRCALPHA:
    return wgpu::BlendFactor::OneMinusSrcAlpha;
  case GX_BL_DSTALPHA:
    return wgpu::BlendFactor::DstAlpha;
  case GX_BL_INVDSTALPHA:
    return wgpu::BlendFactor::OneMinusDstAlpha;
  }
}

wgpu::CompareFunction to_compare_function(GXCompare func) {
  switch (func) {
    DEFAULT_FATAL("invalid depth fn {}", underlying(func));
  case GX_NEVER:
    return wgpu::CompareFunction::Never;
  case GX_LESS:
    return UseReversedZ ? wgpu::CompareFunction::Greater : wgpu::CompareFunction::Less;
  case GX_EQUAL:
    return wgpu::CompareFunction::Equal;
  case GX_LEQUAL:
    return UseReversedZ ? wgpu::CompareFunction::GreaterEqual : wgpu::CompareFunction::LessEqual;
  case GX_GREATER:
    return UseReversedZ ? wgpu::CompareFunction::Less : wgpu::CompareFunction::Greater;
  case GX_NEQUAL:
    return wgpu::CompareFunction::NotEqual;
  case GX_GEQUAL:
    return UseReversedZ ? wgpu::CompareFunction::LessEqual : wgpu::CompareFunction::GreaterEqual;
  case GX_ALWAYS:
    return wgpu::CompareFunction::Always;
  }
}

// Shader output for GX_BM_LOGIC (ShaderConfig::logicOut): 1 = white, 2 = inverted color, 0 = as is.
u8 logic_op_source(GXLogicOp op) noexcept {
  switch (op) {
  case GX_LO_SET:
  case GX_LO_INV:
  case GX_LO_NAND:
    return 1;
  case GX_LO_INVCOPY:
  case GX_LO_NOR:
  case GX_LO_INVOR:
  case GX_LO_EQUIV:
    return 2;
  default:
    return 0;
  }
}

wgpu::BlendState to_blend_state(GXBlendMode mode, GXBlendFactor srcFac, GXBlendFactor dstFac, GXLogicOp op,
                                u32 dstAlpha, bool dualSource) {
  wgpu::BlendComponent colorBlendComponent;
  switch (mode) {
    DEFAULT_FATAL("unsupported blend mode {}", underlying(mode));
  case GX_BM_NONE:
    colorBlendComponent = {
        .operation = wgpu::BlendOperation::Add,
        .srcFactor = wgpu::BlendFactor::One,
        .dstFactor = wgpu::BlendFactor::Zero,
    };
    break;
  case GX_BM_BLEND:
    colorBlendComponent = {
        .operation = wgpu::BlendOperation::Add,
        .srcFactor = to_blend_factor(srcFac, false),
        .dstFactor = to_blend_factor(dstFac, true),
    };
    break;
  case GX_BM_SUBTRACT:
    colorBlendComponent = {
        .operation = wgpu::BlendOperation::ReverseSubtract,
        .srcFactor = wgpu::BlendFactor::One,
        .dstFactor = wgpu::BlendFactor::One,
    };
    break;
  case GX_BM_LOGIC: {
    // The blend unit has no bitwise ops. Each GX logic op becomes the blend that matches it
    // exactly where it can (CLEAR, COPY, NOOP, SET, INV, INVCOPY: ~x = 1 - x on unorm) and
    // otherwise one that matches it for the masks logic ops are used with (0 and 1 per
    // channel): AND = min, OR = max, REVAND = s - d, INVAND = d - s, NOR = (1 - s) - d,
    // INVOR = max(1 - s, d). XOR, EQUIV, REVOR and NAND have no such form; they get the
    // closest one (3 of the 4 mask cases right). logic_op_source gives the shader's output
    // (white or the inverted color) for the ops that need it.
    const auto component = [](wgpu::BlendOperation operation, wgpu::BlendFactor src, wgpu::BlendFactor dst) {
      return wgpu::BlendComponent{.operation = operation, .srcFactor = src, .dstFactor = dst};
    };
    using Op = wgpu::BlendOperation;
    using F = wgpu::BlendFactor;
    switch (op) {
    default:
    case GX_LO_COPY:
    case GX_LO_INVCOPY:
    case GX_LO_SET:
    case GX_LO_REVOR: // approx. COPY
      colorBlendComponent = component(Op::Add, F::One, F::Zero);
      break;
    case GX_LO_CLEAR:
      colorBlendComponent = component(Op::Add, F::Zero, F::Zero);
      break;
    case GX_LO_NOOP:
      colorBlendComponent = component(Op::Add, F::Zero, F::One);
      break;
    case GX_LO_AND:
      colorBlendComponent = component(Op::Min, F::One, F::One);
      break;
    case GX_LO_OR:
    case GX_LO_XOR:   // approx. OR
    case GX_LO_INVOR: // source inverted
      colorBlendComponent = component(Op::Max, F::One, F::One);
      break;
    case GX_LO_REVAND:
    case GX_LO_INV:   // source white: 1 - d
    case GX_LO_NAND:  // approx. INV
    case GX_LO_NOR:   // source inverted: (1 - s) - d
    case GX_LO_EQUIV: // approx. NOR
      colorBlendComponent = component(Op::Subtract, F::One, F::One);
      break;
    case GX_LO_INVAND:
      colorBlendComponent = component(Op::ReverseSubtract, F::One, F::One);
      break;
    }
    break;
  }
  }
  if (dualSource) {
    const auto remap = [](wgpu::BlendFactor factor) {
      if (factor == wgpu::BlendFactor::SrcAlpha) {
        return wgpu::BlendFactor::Src1Alpha;
      }
      if (factor == wgpu::BlendFactor::OneMinusSrcAlpha) {
        return wgpu::BlendFactor::OneMinusSrc1Alpha;
      }
      return factor;
    };
    colorBlendComponent.srcFactor = remap(colorBlendComponent.srcFactor);
    colorBlendComponent.dstFactor = remap(colorBlendComponent.dstFactor);
  }
  wgpu::BlendComponent alphaBlendComponent;
  if (dstAlpha != UINT32_MAX) {
    alphaBlendComponent = wgpu::BlendComponent{
        .operation = wgpu::BlendOperation::Add,
        .srcFactor = wgpu::BlendFactor::Constant,
        .dstFactor = wgpu::BlendFactor::Zero,
    };
  } else {
    alphaBlendComponent = colorBlendComponent;
  }
  return {
      .color = colorBlendComponent,
      .alpha = alphaBlendComponent,
  };
}

wgpu::ColorWriteMask to_write_mask(bool colorUpdate, bool alphaUpdate) {
  wgpu::ColorWriteMask writeMask = wgpu::ColorWriteMask::None;
  if (colorUpdate) {
    writeMask |= wgpu::ColorWriteMask::Red | wgpu::ColorWriteMask::Green | wgpu::ColorWriteMask::Blue;
  }
  if (alphaUpdate) {
    writeMask |= wgpu::ColorWriteMask::Alpha;
  }
  return writeMask;
}

wgpu::PrimitiveState to_primitive_state(GXCullMode gx_cullMode, bool wireframe) {
  auto cullMode = wgpu::CullMode::None;
  switch (gx_cullMode) {
    DEFAULT_FATAL("unsupported cull mode {}", underlying(gx_cullMode));
  case GX_CULL_ALL: // triangle draws are dropped before reaching the GPU (push_gx_draw)
    break;
  case GX_CULL_FRONT:
    cullMode = wgpu::CullMode::Front;
    break;
  case GX_CULL_BACK:
    cullMode = wgpu::CullMode::Back;
    break;
  case GX_CULL_NONE:
    break;
  }
  return {
      .topology = wireframe ? wgpu::PrimitiveTopology::LineList : wgpu::PrimitiveTopology::TriangleList,
      .stripIndexFormat = wgpu::IndexFormat::Undefined,
      .frontFace = wgpu::FrontFace::CW,
      .cullMode = cullMode,
  };
}
} // namespace

void set_viewport_policy(AuroraViewportPolicy policy) noexcept {
  sPendingViewportPolicy.store(policy, std::memory_order_release);
}

void update() noexcept {
  if (const int pending = sPendingViewportPolicy.exchange(-1, std::memory_order_acq_rel); pending != -1) {
    const auto policy = static_cast<AuroraViewportPolicy>(pending);
    g_gxState.viewportPolicy = policy;
    window::set_frame_buffer_aspect_fit(policy == AURORA_VIEWPORT_FIT);
  }
}

Vec2<uint32_t> logical_fb_size() noexcept {
  return gfx::is_offscreen() ? gfx::get_render_target_size() : vi::configured_fb_size();
}

gfx::Viewport map_logical_viewport(const gfx::Viewport& logicalViewport) noexcept {
  if (g_gxState.viewportPolicy == AURORA_VIEWPORT_NATIVE) {
    return logicalViewport;
  }

  const auto [logicalFbWidth, logicalFbHeight] = logical_fb_size();
  const auto [targetWidth, targetHeight] = gfx::get_render_target_size();
  if (logicalFbWidth == 0 || logicalFbHeight == 0 || targetWidth == 0 || targetHeight == 0) {
    return logicalViewport;
  }

  const float scaleX = static_cast<float>(targetWidth) / static_cast<float>(logicalFbWidth);
  const float scaleY = static_cast<float>(targetHeight) / static_cast<float>(logicalFbHeight);
  return {
      .left = logicalViewport.left * scaleX,
      .top = logicalViewport.top * scaleY,
      .width = logicalViewport.width * scaleX,
      .height = logicalViewport.height * scaleY,
      .znear = logicalViewport.znear,
      .zfar = logicalViewport.zfar,
  };
}

gfx::ClipRect map_logical_scissor(const gfx::ClipRect& logicalScissor) noexcept {
  if (g_gxState.viewportPolicy == AURORA_VIEWPORT_NATIVE) {
    return logicalScissor;
  }

  const auto [logicalFbWidth, logicalFbHeight] = logical_fb_size();
  const auto [targetWidth, targetHeight] = gfx::get_render_target_size();
  if (logicalFbWidth == 0 || logicalFbHeight == 0 || targetWidth == 0 || targetHeight == 0) {
    return logicalScissor;
  }

  const float scaleX = static_cast<float>(targetWidth) / static_cast<float>(logicalFbWidth);
  const float scaleY = static_cast<float>(targetHeight) / static_cast<float>(logicalFbHeight);

  const float left = static_cast<float>(logicalScissor.x) * scaleX;
  const float top = static_cast<float>(logicalScissor.y) * scaleY;
  const float right = static_cast<float>(logicalScissor.x + logicalScissor.width) * scaleX;
  const float bottom = static_cast<float>(logicalScissor.y + logicalScissor.height) * scaleY;

  const auto mappedLeft = std::clamp(static_cast<int32_t>(std::floor(left)), 0, static_cast<int32_t>(targetWidth));
  const auto mappedTop = std::clamp(static_cast<int32_t>(std::floor(top)), 0, static_cast<int32_t>(targetHeight));
  const auto mappedRight =
      std::clamp(static_cast<int32_t>(std::ceil(right)), mappedLeft, static_cast<int32_t>(targetWidth));
  const auto mappedBottom =
      std::clamp(static_cast<int32_t>(std::ceil(bottom)), mappedTop, static_cast<int32_t>(targetHeight));

  return {
      .x = mappedLeft,
      .y = mappedTop,
      .width = mappedRight - mappedLeft,
      .height = mappedBottom - mappedTop,
  };
}

void set_logical_viewport(const gfx::Viewport& viewport) noexcept {
  if (viewport.left != g_gxState.logicalViewport.left || viewport.top != g_gxState.logicalViewport.top ||
      viewport.width != g_gxState.logicalViewport.width || viewport.height != g_gxState.logicalViewport.height) {
    g_gxState.dirty |= DirtyUniform;
  }
  g_gxState.logicalViewport = viewport;
  set_render_viewport(map_logical_viewport(viewport));
}

void set_render_viewport(const gfx::Viewport& viewport) noexcept {
  if (viewport.left != g_gxState.renderViewport.left || viewport.width != g_gxState.renderViewport.width ||
      viewport.height != g_gxState.renderViewport.height) {
    g_gxState.dirty |= DirtyUniform;
  }
  g_gxState.renderViewport = viewport;
  gfx::set_viewport(viewport);
}

void set_logical_scissor(const gfx::ClipRect& scissor) noexcept {
  g_gxState.logicalScissor = scissor;
  set_render_scissor(map_logical_scissor(g_gxState.logicalScissor));
}

void set_render_scissor(const gfx::ClipRect& scissor) noexcept {
  g_gxState.renderScissor = scissor;
  gfx::set_scissor(scissor);
}

const gfx::TextureBind& get_texture(GXTexMapID id) noexcept { return g_gxState.textures[static_cast<size_t>(id)]; }

wgpu::RenderPipeline build_pipeline(const PipelineConfig& config, const gfx::RenderTargetLayout& layout,
                                    const PipelineOptions& options, ArrayRef<wgpu::VertexBufferLayout> vtxBuffers,
                                    wgpu::ShaderModule shader, const char* label) noexcept {
  ZoneScoped;
  const float depthBias = (UseReversedZ ? -1.0f : 1.0f) * std::bit_cast<float>(config.polygonOffsetBits);
  const float depthBiasSlopeScale = (UseReversedZ ? -1.0f : 1.0f) * std::bit_cast<float>(config.polygonOffsetScaleBits);
  const float depthBiasClamp = webgpu::g_hasCoreFeatures ? std::bit_cast<float>(config.polygonOffsetClampBits) : 0.0f;
  const bool writesDepth = config.depthCompare && options.depthUpdate;
  const wgpu::DepthStencilState depthStencil{
      .format = layout.depthStencilFormat,
      .depthWriteEnabled = writesDepth,
      .depthCompare = config.depthCompare ? to_compare_function(config.depthFunc) : wgpu::CompareFunction::Always,
      .depthBias = round_away_from_zero<int32_t>(depthBias),
      .depthBiasSlopeScale = depthBiasSlopeScale,
      .depthBiasClamp = depthBiasClamp,
  };
  const auto blendState = to_blend_state(config.blendMode, config.blendFacSrc, config.blendFacDst, config.blendOp,
                                         config.dstAlpha, options.dstAlphaMode == DstAlphaMode::DualSource);
  std::array<wgpu::ColorTargetState, gfx::MaxColorAttachments> colorTargets{};
  for (uint32_t i = 0; i < layout.colorAttachmentCount; ++i) {
    colorTargets[i] = {
        .format = layout.colorAttachments[i].format,
        .writeMask = layout.colorAttachments[i].semantic == gfx::ColorAttachmentSemantic::Normal && writesDepth
                         ? wgpu::ColorWriteMask::All
                         : wgpu::ColorWriteMask::None,
    };
  }
  colorTargets[gfx::SceneColorAttachmentIndex].blend = &blendState;
  colorTargets[gfx::SceneColorAttachmentIndex].writeMask = to_write_mask(options.colorUpdate, options.alphaUpdate);
  const wgpu::FragmentState fragmentState{
      .module = shader,
      .entryPoint = "fs_main",
      .targetCount = layout.colorAttachmentCount,
      .targets = colorTargets.data(),
  };
  const wgpu::RenderPipelineDescriptor descriptor{
      .label = label,
      .layout = sPipelineLayout,
      .vertex =
          {
              .module = shader,
              .entryPoint = "vs_main",
              .bufferCount = static_cast<uint32_t>(vtxBuffers.size()),
              .buffers = vtxBuffers.data(),
          },
      .primitive = to_primitive_state(config.cullMode, config.shaderConfig.wireframe),
      .depthStencil = layout.depthStencilFormat != wgpu::TextureFormat::Undefined ? &depthStencil : nullptr,
      .multisample = wgpu::MultisampleState{.count = layout.sampleCount},
      .fragment = &fragmentState,
  };
  return g_device.CreateRenderPipeline(&descriptor);
}

void canonicalize_pipeline_config(PipelineConfig& config) noexcept {
  if (config.dstAlpha != UINT32_MAX && config.dstAlpha != 0) {
    config.dstAlpha = 0xFF; // create_pipeline / to_blend_state only test for UINT32_MAX and 0
  }
  auto& sc = config.shaderConfig;
  if (sc.depthOnly || sc.lightDepth) {
    // Depth-only copies (enhanced pre-pass, shadow casters) never run the enhanced shading; the
    // class only appended the enhanced uniforms to the struct (last, so nothing before moves).
    sc.enhClass = 0;
  }
  if (sc.tevStageCount > MaxTevStages || sc.numIndStages > MaxIndStages) {
    return; // (invalid; validate_config rejects it)
  }
  const ShaderInfo info = build_shader_info(sc);
  // Texgens: every coordinate the shader generates (sampled, emboss sources) plus any a TEV stage
  // names (the enhanced shadow receiver looks at the previous stage's texgen type).
  auto keepTc = info.sampledTexCoords;
  for (u32 i = 0; i < sc.tevStageCount; ++i) {
    if (const auto tc = sc.tevStages[i].texCoordId; tc < MaxTexCoord) {
      keepTc.set(tc);
    }
  }
  for (u32 i = 0; i < sc.numIndStages; ++i) {
    if (const auto tc = sc.indStages[i].texCoordId; tc < MaxTexCoord) {
      keepTc.set(tc);
    }
  }
  for (u32 i = 0; i < MaxTexCoord; ++i) {
    if (!keepTc.test(i)) {
      sc.tcgs[i] = {};
    }
  }
  // Color channels: the shader evaluates channel pair i (color i, alpha i) only when sampled.
  for (u32 i = 0; i < MaxColorChannels / 2; ++i) {
    if (!info.sampledColorChannels.test(i)) {
      sc.colorChannels[i] = {};
      sc.colorChannels[i + GX_ALPHA0] = {};
    }
  }
}

void populate_pipeline_config(PipelineConfig& config, GXPrimitive primitive, GXVtxFmt fmt) noexcept {
  ZoneScoped;

  const auto& vtxFmt = g_gxState.vtxFmts[fmt];
  config.shaderConfig = {};
  config.shaderConfig.fogType = g_gxState.fog.type;
  config.shaderConfig.fogRangeEnabled = g_gxState.fog.rangeEnabled;
  g_drawEfbBlend6 = 0;
  {
    const u32 accuracy = g_accuracyFlags.load(std::memory_order_relaxed);
    config.shaderConfig.intTev = (accuracy & GX_AURORA_ACCURACY_INT_TEV) != 0;
    if ((accuracy & GX_AURORA_ACCURACY_EFB_DEPTH) != 0) {
      const u8 efbFmt = g_gxState.pixelFmt == GX_PF_RGBA6_Z24 ? 1 : g_gxState.pixelFmt == GX_PF_RGB565_Z16 ? 2 : 0;
      config.shaderConfig.efbFmt = efbFmt;
      config.shaderConfig.dither = efbFmt != 0 && g_gxState.dither;
      config.shaderConfig.efbBlend = efbFmt != 0 && g_gxState.blendMode != GX_BM_NONE;
      // 0176 (GX_AURORA_ACCURACY_EFB_BLEND): the PE blends the TEV's 8-bit output with the stored
      // pixel, then dithers and truncates the result to RGBA6. The shader cannot see the
      // destination, so such a draw outputs its 8-bit color like an RGB8 one (no quantization in
      // the shader) and its pass's color target is quantized after the pass (gfx::efb_blend6).
      // A blend that only replaces (src * ONE + dst * ZERO, logic COPY) stores the shader's
      // already quantized color as is and is left alone.
      const bool replace = (g_gxState.blendMode == GX_BM_BLEND && g_gxState.blendFacSrc == GX_BL_ONE &&
                            g_gxState.blendFacDst == GX_BL_ZERO) ||
                           (g_gxState.blendMode == GX_BM_LOGIC && g_gxState.blendOp == GX_LO_COPY);
      if ((accuracy & GX_AURORA_ACCURACY_EFB_BLEND) != 0 && efbFmt == 1 && config.shaderConfig.efbBlend &&
          !replace) {
        g_drawEfbBlend6 = config.shaderConfig.dither ? 3 : 1;
        config.shaderConfig.efbFmt = 0;
        config.shaderConfig.dither = false;
        config.shaderConfig.efbBlend = false;
      }
    }
  }
  u8 vtxOffset = 0;
  for (int i = GX_VA_PNMTXIDX; i <= GX_VA_TEX7; ++i) {
    const auto attr = static_cast<GXAttr>(i);
    const auto type = g_gxState.vtxDesc[i];
    auto& mapping = config.shaderConfig.attrs[i];
    if (type == GX_NONE) {
      mapping = {};
      continue;
    }
    const auto& attrFmt = vtxFmt.attrs[i];
    const auto cnt = comp_cnt_count(attr, attrFmt.cnt);
    const bool nbt3 = attr == GX_VA_NRM && attrFmt.cnt == GX_NRM_NBT3;
    mapping = AttrConfig{
        .attrType = static_cast<u8>(type),
        .cnt = cnt,
        .compType = static_cast<u8>(attrFmt.type),
        .offset = vtxOffset,
        .stride = 0,
        .frac = attrFmt.frac,
        .le = false,
        .nbt3 = nbt3,
    };
    switch (type) {
    case GX_DIRECT: {
      vtxOffset += comp_type_size(attr, attrFmt.type) * cnt;
      break;
    }
    case GX_INDEX8:
      mapping.stride = g_gxState.arrays[i].stride;
      mapping.le = g_gxState.arrays[i].le;
      vtxOffset += nbt3 ? 3 : 1;
      break;
    case GX_INDEX16:
      mapping.stride = g_gxState.arrays[i].stride;
      mapping.le = g_gxState.arrays[i].le;
      vtxOffset += nbt3 ? 6 : 2;
      break;
    default:
      Log.fatal("populate_pipeline_config: Invalid vertex type {}", type);
    }
  }
  config.shaderConfig.vtxStride = vtxOffset;
  if (primitive == GX_LINES) {
    config.shaderConfig.lineMode = 1;
  } else if (primitive == GX_LINESTRIP) {
    config.shaderConfig.lineMode = 2;
  } else if (primitive == GX_POINTS) {
    config.shaderConfig.lineMode = 3;
  } else {
    config.shaderConfig.lineMode = 0;
  }
  config.shaderConfig.wireframe = g_gxState.wireframe && config.shaderConfig.lineMode == 0;
  config.shaderConfig.tevSwapTable = g_gxState.tevSwapTable;
  for (u8 i = 0; i < g_gxState.numTevStages; ++i) {
    config.shaderConfig.tevStages[i] = g_gxState.tevStages[i];
  }
  config.shaderConfig.tevStageCount = g_gxState.numTevStages;
  for (u8 i = 0; i < g_gxState.numIndStages; ++i) {
    config.shaderConfig.indStages[i] = g_gxState.indStages[i];
  }
  config.shaderConfig.numIndStages = g_gxState.numIndStages;
  for (u8 i = 0; i < MaxColorChannels; ++i) {
    const auto& cc = g_gxState.colorChannelConfig[i];
    if (cc.lightingEnabled) {
      config.shaderConfig.colorChannels[i] = cc;
    } else {
      // Only matSrc matters when lighting disabled
      config.shaderConfig.colorChannels[i] = {
          .matSrc = cc.matSrc,
      };
    }
  }
  for (u8 i = 0; i < g_gxState.numTexGens; ++i) {
    config.shaderConfig.tcgs[i] = g_gxState.tcgs[i];
  }
  // Texture coordinates read by a TEV or indirect stage at or beyond numTexGens (only these: every
  // other draw's config is unchanged). Default: generated from their texgen registers.
  if (const u32 beyond = g_texGenBeyondCount.load(std::memory_order_relaxed);
      beyond != GX_AURORA_TEXGEN_BEYOND_ZERO && g_gxState.numTexGens < MaxTexCoord) {
    const auto fill = [&](u32 coord) {
      if (coord < g_gxState.numTexGens || coord >= MaxTexCoord) {
        return;
      }
      const auto& tcg = beyond == GX_AURORA_TEXGEN_BEYOND_TEXCOORD0 && g_gxState.numTexGens > 0
                            ? g_gxState.tcgs[0]
                            : g_gxState.tcgs[coord];
      // Only matrix texgens whose source the vertex provides (otherwise keep (0, 0)).
      const bool srcOk = tcg.src == GX_TG_POS ||
                         (tcg.src >= GX_TG_TEX0 && tcg.src <= GX_TG_TEX7 &&
                          config.shaderConfig.attrs[GX_VA_TEX0 + (tcg.src - GX_TG_TEX0)].attrType != GX_NONE) ||
                         (tcg.src == GX_TG_NRM && config.shaderConfig.attrs[GX_VA_NRM].attrType != GX_NONE);
      if (srcOk && (tcg.type == GX_TG_MTX2x4 || tcg.type == GX_TG_MTX3x4)) {
        config.shaderConfig.tcgs[coord] = tcg;
        static u32 sLogged = 0; // bit per (numTexGens, coord)
        const u32 bit = 1u << ((g_gxState.numTexGens * 8 + coord) & 31);
        if (!(sLogged & bit)) {
          sLogged |= bit;
          Log.info("texcoord {} read beyond numTexGens {}: generated from its texgen (src {}, mtx {})", coord,
                   g_gxState.numTexGens, underlying(tcg.src), tcg.mtx);
        }
      }
    };
    const auto samples = [](const TevStage& st) {
      const auto& c = st.colorPass;
      const auto& a = st.alphaPass;
      const auto tc = [](GXTevColorArg x) { return x == GX_CC_TEXC || x == GX_CC_TEXA; };
      return tc(c.a) || tc(c.b) || tc(c.c) || tc(c.d) || a.a == GX_CA_TEXA || a.b == GX_CA_TEXA ||
             a.c == GX_CA_TEXA || a.d == GX_CA_TEXA;
    };
    for (u8 i = 0; i < g_gxState.numTevStages; ++i) {
      const auto& st = g_gxState.tevStages[i];
      if (st.texCoordId != GX_TEXCOORD_NULL && st.texMapId != GX_TEXMAP_NULL && samples(st)) {
        fill(st.texCoordId);
      }
    }
    for (u8 i = 0; i < g_gxState.numIndStages; ++i) {
      fill(g_gxState.indStages[i].texCoordId);
    }
  }
  if (g_gxState.alphaCompare) {
    config.shaderConfig.alphaCompare = g_gxState.alphaCompare;
  }
  // Z texture: the depth comes from (or is offset by) the texel of the last TEV stage's texture
  // lookup. Without a texture in that stage there is nothing to read; leave depth alone.
  config.shaderConfig.zTexOp = 0;
  config.shaderConfig.zTexFmt = 0;
  if (g_gxState.zTexOp == GX_ZT_ADD || g_gxState.zTexOp == GX_ZT_REPLACE) {
    if (g_gxState.numTevStages > 0) {
      const auto& last = g_gxState.tevStages[g_gxState.numTevStages - 1];
      if (last.texMapId != GX_TEXMAP_NULL && last.texCoordId != GX_TEXCOORD_NULL) {
        config.shaderConfig.zTexOp = g_gxState.zTexOp;
        config.shaderConfig.zTexFmt = g_gxState.zTexFmt > 2 ? 2 : g_gxState.zTexFmt;
      }
    }
  }
  config.shaderConfig.logicOut = g_gxState.blendMode == GX_BM_LOGIC ? logic_op_source(g_gxState.blendOp) : 0;
  const auto cullMode =
      config.shaderConfig.lineMode == 0 && !config.shaderConfig.wireframe && !g_gxState.twoSided ? g_gxState.cullMode
                                                                                                 : GX_CULL_NONE;
  auto [polygonOffset, polygonOffsetScale] = polygon_offset_for_cull_mode(cullMode);
  const bool wire = config.shaderConfig.wireframe;
  if (wire) {
    // WebGPU allows no depth bias on line topologies.
    polygonOffset = 0.f;
    polygonOffsetScale = 0.f;
  }
  const bool hasAlpha = efb_has_alpha(g_gxState.pixelFmt);
  const bool alphaUpdate = hasAlpha && g_gxState.alphaUpdate;
  const auto blendFactor = [hasAlpha](GXBlendFactor factor) {
    if (!hasAlpha) {
      if (factor == GX_BL_DSTALPHA) {
        return GX_BL_ONE;
      }
      if (factor == GX_BL_INVDSTALPHA) {
        return GX_BL_ZERO;
      }
    }
    return factor;
  };
  config = {
      .shaderConfig = config.shaderConfig,
      .depthFunc = g_gxState.depthFunc,
      .cullMode = cullMode,
      .blendMode = g_gxState.blendMode,
      .blendFacSrc = blendFactor(g_gxState.blendFacSrc),
      .blendFacDst = blendFactor(g_gxState.blendFacDst),
      .blendOp = g_gxState.blendOp,
      .dstAlpha = alphaUpdate ? g_gxState.dstAlpha : UINT32_MAX,
      .polygonOffsetBits = std::bit_cast<uint32_t>(polygonOffset),
      .polygonOffsetScaleBits = std::bit_cast<uint32_t>(polygonOffsetScale),
      .polygonOffsetClampBits = std::bit_cast<uint32_t>(wire ? 0.f : g_gxState.clamp),
      .depthCompare = g_gxState.depthCompare,
      .depthUpdate = g_gxState.depthUpdate,
      .alphaUpdate = alphaUpdate,
      .colorUpdate = g_gxState.colorUpdate,
  };
  config.shaderConfig.enhClass =
      gfx::enhanced::shader_class(config.shaderConfig, g_gxState.drawClass, config.blendMode != GX_BM_NONE,
                                  config.blendMode == GX_BM_BLEND && config.blendFacDst == GX_BL_ONE);
}

GXBindGroups build_bind_groups(const ShaderInfo& info) noexcept {
  ZoneScoped;

  if (!info.sampledTextures.any() && !info.sampledIndTextures.any()) {
    // Don't bother re-binding anything
    return {};
  }

  // Using C WGPU types instead of C++ wrappers to avoid destructor overhead
  std::array<WGPUBindGroupEntry, MaxTextures * 2> textureEntries{};
  for (u32 i = 0; i < MaxTextures; ++i) {
    const auto& tex = g_gxState.textures[i];
    WGPUBindGroupEntry& textureEntry = textureEntries[i * 2];
    WGPUBindGroupEntry& samplerEntry = textureEntries[i * 2 + 1];
    textureEntry.binding = i * 2;
    samplerEntry.binding = i * 2 + 1;
    if (tex && (info.sampledTextures[i] || info.sampledIndTextures[i])) {
      textureEntry.textureView = tex.ref->sampleTextureView.Get();
      samplerEntry.sampler = gfx::sampler_ref(tex.get_descriptor()).Get();
    } else {
      textureEntry.textureView = sEmptyTextureView.Get();
      samplerEntry.sampler = sEmptySampler.Get();
    }
  }
  const WGPUBindGroupDescriptor textureBindGroupDescriptor{
      .label = {"GX Texture Bind Group", WGPU_STRLEN},
      .layout = sTextureBindGroupLayout.Get(),
      .entryCount = textureEntries.size(),
      .entries = textureEntries.data(),
  };
  return {
      .textureBindGroup = gfx::bind_group_ref(textureBindGroupDescriptor),
  };
}

void initialize() noexcept {
  {
    std::array<wgpu::BindGroupLayoutEntry, MaxTextures * 2> textureEntries;
    for (u32 i = 0; i < MaxTextures; ++i) {
      textureEntries[i * 2] = {
          .binding = i * 2,
          .visibility = wgpu::ShaderStage::Fragment,
          .texture =
              {
                  .sampleType = wgpu::TextureSampleType::Float,
                  .viewDimension = wgpu::TextureViewDimension::e2D,
              },
      };
      textureEntries[i * 2 + 1] = {
          .binding = i * 2 + 1,
          .visibility = wgpu::ShaderStage::Fragment,
          .sampler = {.type = wgpu::SamplerBindingType::Filtering},
      };
    }
    const wgpu::BindGroupLayoutDescriptor descriptor{
        .label = "GX Texture Bind Group Layout",
        .entryCount = textureEntries.size(),
        .entries = textureEntries.data(),
    };
    sTextureBindGroupLayout = g_device.CreateBindGroupLayout(&descriptor);
  }
  {
    constexpr wgpu::SamplerDescriptor descriptor{.label = "Empty sampler"};
    sEmptySampler = gfx::sampler_ref(descriptor);
  }
  {
    constexpr wgpu::TextureDescriptor descriptor{
        .label = "Empty texture",
        .usage = wgpu::TextureUsage::TextureBinding,
        .size = {1, 1},
        .format = wgpu::TextureFormat::RGBA8Unorm,
    };
    sEmptyTexture = g_device.CreateTexture(&descriptor);
    sEmptyTextureView = sEmptyTexture.CreateView();
  }
  {
    std::array<wgpu::BindGroupEntry, MaxTextures * 2> entries;
    for (u32 i = 0; i < MaxTextures; ++i) {
      entries[i * 2] = {
          .binding = i * 2,
          .textureView = sEmptyTextureView,
      };
      entries[i * 2 + 1] = {
          .binding = i * 2 + 1,
          .sampler = sEmptySampler,
      };
    }
    const wgpu::BindGroupDescriptor desc{
        .label = "GX Empty Texture Bind Group",
        .layout = sTextureBindGroupLayout,
        .entryCount = entries.size(),
        .entries = entries.data(),
    };
    g_emptyTextureBindGroup = g_device.CreateBindGroup(&desc);
  }
  {
    const std::array layouts{
        gfx::detail::resources().staticBindGroupLayout,
        gfx::detail::resources().uniformBindGroupLayout,
        sTextureBindGroupLayout,
    };
    const wgpu::PipelineLayoutDescriptor desc{
        .label = "GX Pipeline Layout",
        .bindGroupLayoutCount = layouts.size(),
        .bindGroupLayouts = layouts.data(),
        .immediateSize = sizeof(DrawImmediateData),
    };
    sPipelineLayout = g_device.CreatePipelineLayout(&desc);
  }
}

void shutdown() noexcept {
  // TODO we should probably store this all in g_state.gx instead
  sSamplerBindGroupLayout = {};
  sTextureBindGroupLayout = {};
  {
    std::lock_guard lock{sBindGroupLayoutMutex};
    sUniformBindGroupLayouts.clear();
    sTextureBindGroupLayouts.clear();
  }
  for (auto& item : g_gxState.textures) {
    item.ref.reset();
  }
  g_gxState.loadedTextures.fill({});
  g_gxState.loadedTluts.fill({});
  clear_copy_texture_cache();
  texture::shutdown();
}
} // namespace aurora::gx
