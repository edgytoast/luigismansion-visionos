#pragma once

#include "types.hpp"

#include <dolphin/gx/GXEnum.h>

#include <array>

namespace aurora::gfx::tex_copy_conv {

enum class SampleFilter : uint8_t {
  Nearest,
  Linear,
};

struct alignas(16) Uniforms {
  Vec2<float> offset;
  Vec2<float> scale{1.f, 1.f};
  uint32_t opaqueAlpha = 0;
  uint32_t quantize = 0; // RGB565/RGB5A3 copies: store the format's precision (GX_AURORA_ACCURACY_EFB_DEPTH); 2 = smoothed (0190)
  // Vertical copy filter (GXSetCopyFilter, GX_AURORA_ACCURACY_COPY_FILTER): one source row in uv
  // units (the shader derives it from the source texture; one EFB line at native resolution), and the weights (out of 64) of the line above (taps 0+1), the line itself (2+3+4)
  // and the line below (5+6) in bytes 0-2. 0 = no filter (pass-through taps, or a depth copy).
  float lineStep = 0.f;
  uint32_t filterTaps = 0;
  // GXSetCopyClamp: the lines the filter reads above / below the source rectangle repeat its
  // top / bottom line (uv range of the row centers it may sample; unbounded when not clamped).
  float clampTop = -1.f;
  float clampBottom = 2.f;
};
static_assert(sizeof(Uniforms) == 48);

struct ConvRequest {
  GXTexFmt fmt;
  GXPixelFmt srcFmt;
  wgpu::TextureView srcView; // View of resolved EFB / offscreen color/depth
  Range uniformRange;        // Uniforms
  TextureHandle dst;         // Destination texture
  SampleFilter sampleFilter = SampleFilter::Nearest;
  uint32_t srcSamples = 1;   // depth copies: sample count of srcView (MSAA EFB: sample 0 is copied)
};

bool needs_conversion(GXTexFmt fmt);

void initialize();
void shutdown();
void run(const wgpu::CommandEncoder& cmd, const ConvRequest& req);
void blit(const wgpu::CommandEncoder& cmd, const ConvRequest& req);

bool snapshot_depth_supported() noexcept;
void snapshot_depth(const wgpu::CommandEncoder& cmd, const wgpu::TextureView& srcDepth, uint32_t msaaSamples,
                    const wgpu::TextureView& dst);

} // namespace aurora::gfx::tex_copy_conv
