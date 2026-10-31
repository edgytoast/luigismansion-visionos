#include "pipeline.hpp"

#include "../gfx/encoding.hpp"
#include "../gfx/resources.hpp"
#include "../gfx/pipeline_cache.hpp"
#include "../gfx/resource_cache.hpp"

#include "gx_fmt.hpp"
#include "shader_info.hpp"

#include <absl/container/flat_hash_map.h>
#include <tracy/Tracy.hpp>

#include <array>
#include <cstring>
#include <mutex>

namespace aurora::gx {

// 0227: shader modules of this session by shader_module_key. A module costs WGSL generation and a
// Tint parse/validation (about a quarter of a cached pipeline's creation time); the pipelines of
// one shader for other blend/depth/cull states, render targets and the alpha pre-pass reuse it.
// Only modules that compiled are kept. The pipelines hold their modules anyway.
static std::mutex g_moduleMutex;
static absl::flat_hash_map<HashType, wgpu::ShaderModule> g_modules;

void clear_shader_module_cache() noexcept {
  std::lock_guard lock{g_moduleMutex};
  g_modules.clear();
}

gfx::CompiledPipeline create_pipeline(const PipelineConfig& config, const gfx::RenderTargetLayout& layout) {
  ZoneScoped;
  const auto hash = xxh3_hash(layout.key, xxh3_hash(config, static_cast<HashType>(gfx::ShaderType::GX)));
  const auto create = [&](const PipelineOptions& options, const char* passName) {
    // Dawn throws (terminating the process) when a pipeline is created from a shader module that
    // failed to compile, e.g. one generated from a damaged cached config: check the module first.
    const auto moduleKey = shader_module_key(config.shaderConfig, layout, options.dstAlphaMode);
    wgpu::ShaderModule shader;
    {
      std::lock_guard lock{g_moduleMutex};
      if (const auto it = g_modules.find(moduleKey); it != g_modules.end()) {
        shader = it->second;
      }
    }
    if (shader) {
      gfx::note_module_reuse();
    } else {
      gfx::push_shader_error_scope();
      shader = build_shader(config.shaderConfig, layout, options.dstAlphaMode);
      if (!gfx::pop_shader_error_scope("GX shader")) {
        return wgpu::RenderPipeline{};
      }
      std::lock_guard lock{g_moduleMutex};
      g_modules.try_emplace(moduleKey, shader);
    }
    const auto label = fmt::format("GX Pipeline {:x} {}", hash, passName);
    const uint64_t t0 = gfx::now_ns(); // 0226
    auto pipeline = build_pipeline(config, layout, options, {}, shader, label.c_str());
    gfx::add_create_time(gfx::CreateStage::Pipeline, gfx::now_ns() - t0);
    return pipeline;
  };

  PipelineOptions options{
      .colorUpdate = config.colorUpdate,
      .alphaUpdate = config.alphaUpdate,
      .depthUpdate = config.depthUpdate,
  };
  gfx::CompiledPipeline pipeline;
  if (config.alphaUpdate && config.dstAlpha != UINT32_MAX && config.dstAlpha != 0) {
    const auto usesSourceAlpha = [](GXBlendFactor factor) {
      return factor == GX_BL_SRCALPHA || factor == GX_BL_INVSRCALPHA;
    };
    const bool needsSourceAlpha = config.colorUpdate && config.blendMode == GX_BM_BLEND &&
                                  (usesSourceAlpha(config.blendFacSrc) || usesSourceAlpha(config.blendFacDst));
    if (!needsSourceAlpha) {
      options.dstAlphaMode = DstAlphaMode::Replace;
    } else if (webgpu::g_dualSourceBlendingSupported && layout.colorAttachmentCount == 1) {
      options.dstAlphaMode = DstAlphaMode::DualSource;
    } else {
      // Write alpha before RGB, no depth write
      auto alpha = options;
      alpha.dstAlphaMode = DstAlphaMode::Replace;
      alpha.colorUpdate = false;
      alpha.depthUpdate = false;
      pipeline.prepass = create(alpha, "alpha");
      if (!pipeline.prepass) {
        return {};
      }
      options.alphaUpdate = false;
    }
  }
  pipeline.main = create(options, "main");
  if (!pipeline.main) {
    return {};
  }
  return pipeline;
}

void render(const DrawData& data, const wgpu::RenderPassEncoder& pass) {
  gfx::CompiledPipeline pipeline;
  if (!gfx::get_pipeline(data.pipeline, pipeline)) {
    return;
  }
  const auto& resources = gfx::detail::resources();
  pass.SetImmediates(0, &data.immediateData, sizeof(data.immediateData));
  const std::array offsets{data.uniformRange.offset};
  pass.SetBindGroup(1, resources.uniformBindGroup, offsets.size(), offsets.data());
  if (data.bindGroups.textureBindGroup) {
    pass.SetBindGroup(2, gfx::find_bind_group(data.bindGroups.textureBindGroup));
  }
  pass.SetIndexBuffer(resources.indexBuffer, wgpu::IndexFormat::Uint16, data.idxRange.offset, data.idxRange.size);
  if (data.dstAlpha != UINT32_MAX) {
    const wgpu::Color color{0.f, 0.f, 0.f, data.dstAlpha / 255.f};
    pass.SetBlendConstant(&color);
  }
  const auto draw = [&] {
    if (data.indexCount == 0) {
      pass.Draw(data.vtxCount, data.instanceCount);
    } else {
      pass.DrawIndexed(data.indexCount, data.instanceCount);
    }
  };
  if (pipeline.prepass) {
    gfx::bind_pipeline(pipeline.prepass, pass);
    draw();
  }
  gfx::bind_pipeline(pipeline.main, pass);
  draw();
}

void render_seq(const DrawData& data, const wgpu::RenderPassEncoder& pass, RenderSeqState& state) {
  // Same commands as render(), minus the state calls that set what is already set. Matters most
  // for the early-Z split (0170/0179) at the original frame rate, where every GX draw becomes a
  // depth-only and a colour command with identical immediates, uniforms, textures and indices:
  // the forest of the opening had ~40,000 commands per frame, and their encoding plus Dawn's
  // submit took the render worker 50-90 ms per game frame (the game fell to 18-23 fps).
  gfx::CompiledPipeline pipeline;
  if (!gfx::get_pipeline(data.pipeline, pipeline)) {
    return;
  }
  const auto& resources = gfx::detail::resources();
  if (!state.valid) {
    state = RenderSeqState{};
  }
  if (!state.valid || std::memcmp(&state.immediates, &data.immediateData, sizeof(data.immediateData)) != 0) {
    pass.SetImmediates(0, &data.immediateData, sizeof(data.immediateData));
    state.immediates = data.immediateData;
  }
  if (!state.valid || state.uniformOffset != data.uniformRange.offset) {
    const std::array offsets{data.uniformRange.offset};
    pass.SetBindGroup(1, resources.uniformBindGroup, offsets.size(), offsets.data());
    state.uniformOffset = data.uniformRange.offset;
  }
  if (data.bindGroups.textureBindGroup && data.bindGroups.textureBindGroup != state.textureBindGroup) {
    pass.SetBindGroup(2, gfx::find_bind_group(data.bindGroups.textureBindGroup));
    state.textureBindGroup = data.bindGroups.textureBindGroup;
  }
  // Indexed draws: the whole index buffer stays bound and firstIndex selects the draw's range
  // (the same indices as binding the range itself).
  const bool whole = data.indexCount != 0 && (data.idxRange.offset % sizeof(uint16_t)) == 0;
  if (data.indexCount != 0) {
    if (whole) {
      if (state.indexBuffer != resources.indexBuffer.Get()) {
        pass.SetIndexBuffer(resources.indexBuffer, wgpu::IndexFormat::Uint16, 0, wgpu::kWholeSize);
        state.indexBuffer = resources.indexBuffer.Get();
      }
    } else {
      pass.SetIndexBuffer(resources.indexBuffer, wgpu::IndexFormat::Uint16, data.idxRange.offset, data.idxRange.size);
      state.indexBuffer = nullptr;
    }
  }
  if (data.dstAlpha != UINT32_MAX && (!state.valid || state.blendAlpha != data.dstAlpha)) {
    const wgpu::Color color{0.f, 0.f, 0.f, data.dstAlpha / 255.f};
    pass.SetBlendConstant(&color);
    state.blendAlpha = data.dstAlpha;
  }
  state.valid = true;
  const uint32_t firstIndex = whole ? data.idxRange.offset / static_cast<uint32_t>(sizeof(uint16_t)) : 0;
  const auto draw = [&] {
    if (data.indexCount == 0) {
      pass.Draw(data.vtxCount, data.instanceCount);
    } else {
      pass.DrawIndexed(data.indexCount, data.instanceCount, firstIndex);
    }
  };
  if (pipeline.prepass) {
    gfx::bind_pipeline(pipeline.prepass, pass);
    draw();
  }
  gfx::bind_pipeline(pipeline.main, pass);
  draw();
}

} // namespace aurora::gx
