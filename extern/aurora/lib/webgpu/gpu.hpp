#pragma once

#include <aurora/aurora.h>
#include <aurora/math.hpp>

#include "wgpu.hpp"

#include <array>
#include <cstdint>

struct SDL_Window;

namespace aurora::webgpu {
inline constexpr wgpu::TextureFormat NormalBufferFormat = wgpu::TextureFormat::RGB10A2Unorm;

struct GraphicsConfig {
  wgpu::SurfaceConfiguration surfaceConfiguration;
  wgpu::TextureFormat depthFormat;
  uint32_t msaaSamples;
  uint16_t textureAnisotropy;
  bool normalBuffer = false;
};
struct TextureWithSampler {
  wgpu::Texture texture;
  wgpu::TextureView view;
  wgpu::Extent3D size;
  wgpu::TextureFormat format;
  wgpu::Sampler sampler;
};
struct Viewport {
  float left;
  float top;
  float width;
  float height;
  float znear;
  float zfar;

  bool operator==(const Viewport& rhs) const {
    return left == rhs.left && top == rhs.top && width == rhs.width && height == rhs.height && znear == rhs.znear &&
           zfar == rhs.zfar;
  }
  bool operator!=(const Viewport& rhs) const { return !(*this == rhs); }
};

extern wgpu::Device g_device;
extern wgpu::Queue g_queue;
extern wgpu::Surface g_surface;
extern wgpu::BackendType g_backendType;
extern GraphicsConfig g_graphicsConfig;
extern TextureWithSampler g_frameBuffer;
extern TextureWithSampler g_frameBufferResolved;
extern TextureWithSampler g_depthBuffer;
extern TextureWithSampler g_normalBuffer;
extern wgpu::RenderPipeline g_CopyPipeline;
extern wgpu::RenderPipeline g_CopyPremultipliedAlphaPipeline;
extern wgpu::BindGroup g_CopyBindGroup;
extern wgpu::Instance g_instance;
extern wgpu::AdapterInfo g_adapterInfo;
extern bool g_hasCoreFeatures;
extern bool g_bcTexturesSupported;
extern bool g_astcTexturesSupported;
extern bool g_textureComponentSwizzleSupported;
// Apple Vision Pro: SharedTextureMemoryIOSurface and SharedFenceMTLSharedEvent were both enabled on
// the device (the OpenXR provider's swapchain images and the window's frames are IOSurfaces).
extern bool g_metalSharedImageSupported;
extern bool g_dualSourceBlendingSupported;
extern bool g_clipDistancesSupported; // 0028
// 0233: MSAA sample counts this device can render with (bit n = 2^n samples; bit 0 always set) and
// the count asked for before it was limited to them.
extern uint32_t g_msaaSupportMask;
extern uint32_t g_msaaRequested;

bool initialize(AuroraBackend backend, bool allowCpu);
void shutdown();
void release_surface() noexcept;
bool refresh_surface(bool recreate = true);
void resize_swapchain(uint32_t width, uint32_t height, uint32_t nativeWidth, uint32_t nativeHeight, bool force = false);
TextureWithSampler create_render_texture(uint32_t width, uint32_t height, bool multisampled);
bool enable_normal_buffer();
const TextureWithSampler& present_source() noexcept;
TextureWithSampler& vr_present_target() noexcept;
const TextureWithSampler& vr_present_source() noexcept;
TextureWithSampler& vr_overlay_target() noexcept;
void set_vr_overlay_separate(bool separate) noexcept;
wgpu::BindGroup create_copy_bind_group(const TextureWithSampler& source);
void set_resampler(AuroraSampler sampler) noexcept;
AuroraSampler get_resampler() noexcept;
Viewport calculate_present_viewport(uint32_t surface_width, uint32_t surface_height, uint32_t content_width,
                                    uint32_t content_height) noexcept;
const TextureWithSampler& resample_present_source(const wgpu::CommandEncoder& encoder, const Viewport& viewport);
void draw_clear(const wgpu::RenderPassEncoder& pass, bool clearColor, bool clearAlpha, bool clearDepth,
                const Vec4<float>& clearColorValue, float clearDepthValue);

size_t load_from_cache(void const* key, size_t keySize, void* value, size_t valueSize, void* userdata);
void store_to_cache(void const* key, size_t keySize, void const* value, size_t valueSize, void* userdata);
void cache_prune();
void cache_shutdown();
// 0226: what the Dawn blob cache did this session (startup diagnostics: loaded vs compiled).
struct CacheStats {
  uint32_t hits;      // entries Dawn read (compiled shaders, pipeline state blobs)
  uint32_t misses;    // lookups that found nothing (Dawn then compiles)
  uint32_t stores;    // entries Dawn wrote (something was compiled)
  uint64_t loadNs;    // time in lookups, lock waits included
  uint64_t storeNs;   // time in stores, lock waits included
  bool broken;        // the database could not be opened: no cache this session
  uint32_t entriesAtOpen;  // entries the database had when this session opened it
  uint32_t session;        // launches counted in this database (1 = first)
};
CacheStats cache_stats();
// Deletes dawn_cache.db (crash recovery). Only valid before the cache was first opened.
void cache_delete_files();
// Called once before the Dawn cache is first opened (the pipeline crash journal check).
extern void (*g_cacheOpenHook)();
// 0226: run PRAGMA quick_check on the cache databases when they are opened. It reads the whole
// file (dawn_cache.db: 60-250 MB; 2.2 s for 244 MB from an NVMe SSD after a reboot, far more from
// a hard disk), so it only runs when the previous session did not exit cleanly (crash journal);
// damaged entries are still caught by the per-entry checksums and SQLITE_CORRUPT handling.
extern bool g_cacheVerifyOnOpen;

// 0223: AURORA_CREATE_LOG=1 (debug aid): every synchronous pipeline / large texture creation that
// took over 0.5 ms, with what it was and the thread (first-use hitches, e.g. the first foyer frame).
struct CreateTimer {
  const char* what;
  int64_t t0;
  explicit CreateTimer(const char* w);
  ~CreateTimer();
};
bool create_log_enabled();

} // namespace aurora::webgpu
