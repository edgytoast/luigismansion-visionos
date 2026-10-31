#pragma once

#include "types.hpp"

namespace aurora::gfx {
inline constexpr bool UseTextureBuffer = true;
inline constexpr uint64_t UniformBufferSize = 25165824; // 24 MiB
inline constexpr uint64_t VertexBufferSize = 33554432;  // 32 MiB
inline constexpr uint64_t IndexBufferSize = 8388608;    // 8 MiB
inline constexpr uint64_t StorageBufferSize = 67108864; // 64 MiB
inline constexpr uint64_t TextureUploadSize = 25165824; // 24 MiB
// 0028: enhanced-lighting shadow masks (written on the GPU, read by world shaders; group 0 binding 2)
inline constexpr uint64_t MaskBufferSize = 134217728; // 128 MiB (0032: ambient occlusion; 0186: depth; 0187: 16 lights)
inline constexpr uint32_t MaskPlanes = 6;              // 0187: shadows 0-3, 4-7, 8-11, 12-15, ambient occlusion, pre-pass depth
inline constexpr uint32_t MaskAoPlane = 4;
inline constexpr uint32_t MaskDepthPlane = 5;

namespace detail {
struct Resources {
  wgpu::Buffer vertexBuffer;
  wgpu::Buffer uniformBuffer;
  wgpu::Buffer indexBuffer;
  wgpu::Buffer storageBuffer;
  wgpu::Buffer maskBuffer; // 0028
  wgpu::BindGroupLayout staticBindGroupLayout;
  wgpu::BindGroup staticBindGroup;
  wgpu::BindGroupLayout uniformBindGroupLayout;
  wgpu::BindGroup uniformBindGroup;
  wgpu::Limits limits;
  AuroraStats stats{};
};

Resources& resources() noexcept;
} // namespace detail
} // namespace aurora::gfx
