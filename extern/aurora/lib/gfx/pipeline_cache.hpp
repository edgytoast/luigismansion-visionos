#pragma once

#include "types.hpp"

namespace aurora::gfx::clear {
struct PipelineConfig;
} // namespace aurora::gfx::clear

namespace aurora::gx {
struct PipelineConfig;
} // namespace aurora::gx

namespace aurora::rmlui {
struct PipelineConfig;
} // namespace aurora::rmlui

namespace aurora::gfx {

enum class ShaderType : uint8_t {
  Clear = 0,
  GX = 1,
  Rml = 2,
};

struct CompiledPipeline {
  wgpu::RenderPipeline main;
  wgpu::RenderPipeline prepass;

  [[nodiscard]] uint32_t pipeline_count() const {
    return static_cast<uint32_t>(static_cast<bool>(main)) + static_cast<uint32_t>(static_cast<bool>(prepass));
  }
};

void initialize_pipeline_cache();
void shutdown_pipeline_cache();
void begin_pipeline_frame();
void end_pipeline_frame();
void rebuild_pipeline_cache();

PipelineRef find_pipeline(const gx::PipelineConfig& config, const RenderTargetLayout& layout);
PipelineRef find_pipeline(const clear::PipelineConfig& config, const RenderTargetLayout& layout);
PipelineRef find_pipeline(const rmlui::PipelineConfig& config);

// 0221: `what` names the draw in the skipped-draw log ("draw", "shadow caster").
bool get_pipeline(PipelineRef ref, CompiledPipeline& pipeline, const char* what = "draw");
bool try_get_pipeline(PipelineRef ref, CompiledPipeline& pipeline);

// Around shader module creation: false when the module failed to compile (logged). A pipeline
// must not be created from such a module (patch 0017).
void push_shader_error_scope();
bool pop_shader_error_scope(const char* what);

// 0226: where GX pipeline creation spends its time (summed over all threads; startup diagnostics):
// WGSL generation, shader module creation (Tint parse/validate), render pipeline creation (Dawn:
// cache lookups, shader compile on a miss, D3D12/Vulkan pipeline creation).
enum class CreateStage : uint8_t { Wgsl, Module, Pipeline, Count };
void add_create_time(CreateStage stage, uint64_t ns) noexcept;
void note_module_reuse() noexcept; // 0227: a pipeline reused a shader module
uint64_t now_ns() noexcept;

} // namespace aurora::gfx
