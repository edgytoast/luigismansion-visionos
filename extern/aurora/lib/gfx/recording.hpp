#pragma once

#include "types.hpp"
#include "efb_ram.hpp"

#include <cstddef>
#include <string>
#include <utility>

namespace aurora::gx {
struct DrawData;
}

namespace aurora::gfx::detail {

struct FramePacket;
struct DrawCommand;

struct RecordedFrame {
  FramePacket* packet;
  size_t frameSlot;
};

void begin_recording(FramePacket& packet, size_t frameSlot);
RecordedFrame end_recording();
void shutdown_recording();
void increment_merged_draw_count() noexcept;

namespace testing {
void suppress_render_worker(bool suppress) noexcept;
void seed_offscreen_cache(uint32_t width, uint32_t height, wgpu::TextureFormat colorFormat,
                          wgpu::TextureFormat depthFormat);
}

} // namespace aurora::gfx::detail

namespace aurora::gfx {
struct ColorPassDescriptor {
  const char* label = nullptr;
  wgpu::TextureView colorView;
  wgpu::TextureView resolveView;
  wgpu::TextureFormat colorFormat = wgpu::TextureFormat::Undefined;
  wgpu::TextureView depthStencilView;
  wgpu::TextureFormat depthStencilFormat = wgpu::TextureFormat::Undefined;
  wgpu::Extent3D targetSize;
  uint32_t sampleCount = 1;
  wgpu::LoadOp colorLoadOp = wgpu::LoadOp::Clear;
  wgpu::StoreOp colorStoreOp = wgpu::StoreOp::Store;
  wgpu::Color clearColor{0.f, 0.f, 0.f, 0.f};
  bool hasDepth = false;
  wgpu::LoadOp depthLoadOp = wgpu::LoadOp::Undefined;
  wgpu::StoreOp depthStoreOp = wgpu::StoreOp::Undefined;
  float depthClearValue = 0.f;
  bool hasStencil = false;
  wgpu::LoadOp stencilLoadOp = wgpu::LoadOp::Undefined;
  wgpu::StoreOp stencilStoreOp = wgpu::StoreOp::Undefined;
  uint32_t stencilClearValue = 0;
};

void finish();
void begin_color_pass(const ColorPassDescriptor& desc);
void end_color_pass();
void queue_texture_copy(wgpu::TexelCopyTextureInfo src, wgpu::TexelCopyTextureInfo dst, wgpu::Extent3D size);
void begin_offscreen(uint32_t width, uint32_t height);
void end_offscreen();
// 0176: the current pass has a blended RGBA6 draw (GX_AURORA_ACCURACY_EFB_BLEND): its color target
// is quantized once the pass ends (efb_blend6::resolve).
void note_efb_blend6(bool dither) noexcept;
bool has_normal_attachment() noexcept;
RenderTargetLayout get_render_target_layout() noexcept;
void clear_caches() noexcept;

namespace tex_palette_conv {
struct ConvRequest;
}
void queue_palette_conv(tex_palette_conv::ConvRequest req);

Range push_verts(const uint8_t* data, size_t length, size_t alignment);
template <typename T>
Range push_verts(ArrayRef<T> data, size_t alignment) {
  return push_verts(reinterpret_cast<const uint8_t*>(data.data()), data.size() * sizeof(T), alignment);
}
Range push_indices(const uint8_t* data, size_t length, size_t alignment);
template <typename T>
Range push_indices(ArrayRef<T> data, size_t alignment) {
  return push_indices(reinterpret_cast<const uint8_t*>(data.data()), data.size() * sizeof(T), alignment);
}
Range push_uniform(const uint8_t* data, size_t length);
template <typename T>
Range push_uniform(const T& data) {
  return push_uniform(reinterpret_cast<const uint8_t*>(&data), sizeof(T));
}
Range push_storage(const uint8_t* data, size_t length);
template <typename T>
Range push_storage(ArrayRef<T> data) {
  return push_storage(reinterpret_cast<const uint8_t*>(data.data()), data.size() * sizeof(T));
}
template <typename T>
Range push_storage(const T& data) {
  return push_storage(reinterpret_cast<const uint8_t*>(&data), sizeof(T));
}
Range push_texture_data(const uint8_t* data, uint32_t bytesPerRow, uint32_t rowsPerImage);

template <typename DrawData>
void push_draw_command(DrawData data);
template <typename DrawData>
DrawData* get_last_draw_command();
// The draw command before the last one (0182: the depth half of an early-Z split pair).
gx::DrawData* get_draw_command_before_last();
template <typename PipelineConfig>
PipelineRef pipeline_ref(const PipelineConfig& config);

void resolve_pass_into(TextureHandle texture, ClipRect rect, bool clearColor, bool clearAlpha, bool clearDepth,
                       Vec4<float> clearColorValue, float clearDepthValue, GXTexFmt resolveFormat,
                       GXPixelFmt sourceFormat);
// Enhanced lighting scene-end effects: the EFB targets as the sealed pass leaves them.
struct ScenePassTargets {
  wgpu::TextureView colorView;   // EFB color attachment (multisampled with MSAA)
  wgpu::TextureView resolveView; // its resolve target (MSAA) or null
  wgpu::TextureFormat colorFormat = wgpu::TextureFormat::Undefined;
  wgpu::Texture colorSource;     // single-sample color after the pass (copy source)
  wgpu::TextureView depthSource; // depth attachment view (multisampled with MSAA)
  uint32_t msaa = 1;
  uint32_t width = 0;
  uint32_t height = 0;
};
using SceneOp = std::function<void(const wgpu::CommandEncoder& cmd, const ScenePassTargets& targets)>;
// Seals the current EFB pass, runs `op` on the frame encoder right after it and continues on a
// pass that loads the existing contents. False (nothing recorded) outside an EFB pass.
bool push_scene_op(SceneOp op);
// Where the next recorded command goes (pass, index), and a recorded GX draw by that position
// (its final state: later draws may have been merged into it). Enhanced lighting glow.
std::pair<uint32_t, uint32_t> next_command_position() noexcept;
const gx::DrawData* gx_draw_at(uint32_t pass, uint32_t index) noexcept;
// The GX draw recorded in `cmd`, or null for any other draw type (0229: pass encoding skips the
// state a GX draw shares with the GX draw encoded before it).
const gx::DrawData* gx_draw_payload(const detail::DrawCommand& cmd) noexcept;
// The viewport / scissor the next recorded draw uses (target pixels).
Viewport current_viewport() noexcept;
ClipRect current_scissor() noexcept;
// EFB copy to RAM work: run before the current pass / right after the next resolve.
bool queue_pre_pass_ops(std::vector<efb_ram::PassOp> ops);
void set_next_resolve_op(efb_ram::PassOp op);
uint32_t align_uniform(uint32_t value);
Vec2<uint32_t> get_render_target_size() noexcept;
void set_viewport(const Viewport& viewport) noexcept;
void set_scissor(const ClipRect& scissor) noexcept;
void push_debug_group(std::string label);
void insert_debug_marker(std::string label);
} // namespace aurora::gfx
