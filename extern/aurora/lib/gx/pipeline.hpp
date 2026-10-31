#pragma once

#include "../gfx/pipeline_cache.hpp"
#include "gx.hpp"

namespace aurora::gx {
struct DrawData {
  gfx::PipelineRef pipeline;
  GXBindGroups bindGroups;
  gfx::Range vertRange;
  gfx::Range idxRange;
  gfx::Range uniformRange;
  DrawImmediateData immediateData;
  uint32_t vtxCount;
  uint32_t indexCount;
  uint32_t instanceCount;
  uint32_t dstAlpha;
};

constexpr uint32_t GXPipelineConfigVersion = 15;
struct PipelineConfig {
  uint32_t version = GXPipelineConfigVersion;
  uint32_t msaaSamples = 1; // deprecated
  ShaderConfig shaderConfig;
  GXCompare depthFunc;
  GXCullMode cullMode;
  GXBlendMode blendMode;
  GXBlendFactor blendFacSrc, blendFacDst;
  GXLogicOp blendOp;
  uint32_t dstAlpha;
  uint32_t polygonOffsetBits;
  uint32_t polygonOffsetScaleBits;
  uint32_t polygonOffsetClampBits;
  bool depthCompare, depthUpdate, alphaUpdate, colorUpdate;
};
static_assert(std::has_unique_object_representations_v<PipelineConfig>);

struct PipelineOptions {
  DstAlphaMode dstAlphaMode = DstAlphaMode::None;
  bool colorUpdate = false;
  bool alphaUpdate = false;
  bool depthUpdate = false;
};

gfx::CompiledPipeline create_pipeline(const PipelineConfig& config, const gfx::RenderTargetLayout& layout);
void render(const DrawData& data, const wgpu::RenderPassEncoder& pass);
// 0229: the pass state the last GX draw of a run of GX draws left set (encoding.cpp render_pass).
// render_seq() issues only what differs from it; the commands reaching the GPU have the same
// effect as render()'s. Reset it before any other command type touches the pass.
struct RenderSeqState {
  bool valid = false;
  DrawImmediateData immediates{};
  uint32_t uniformOffset = UINT32_MAX;
  gfx::BindGroupRef textureBindGroup{};
  WGPUBuffer indexBuffer = nullptr;  // bound whole (firstIndex addresses the draw's range)
  uint32_t blendAlpha = UINT32_MAX;
};
void render_seq(const DrawData& data, const wgpu::RenderPassEncoder& pass, RenderSeqState& state);

void queue_surface(const u8* dlStart, uint32_t dlSize, bool bigEndian) noexcept;
} // namespace aurora::gx
