#include <aurora/aurora.h>
#include <aurora/time.hpp>
#include <aurora/present_timing.h>
#include "activity.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <deque>
#include <mutex>
#include <thread>

#ifdef AURORA_ENABLE_GX
#include "gfx/resources.hpp"
#include "gfx/frame.hpp"
#include "gfx/pipeline_cache.hpp"
#include "gfx/recording.hpp"
#include "gfx/render_worker.hpp"
#include "gx/command_processor.hpp"
#include "gx/fifo.hpp"
#include "gx/gx.hpp"
#include "gx/texture.hpp"
#include "imgui.hpp"
#include "webgpu/gpu.hpp"
#include "webgpu/gpu_prof.hpp"
#include "webgpu/postfx.hpp"
#include <webgpu/webgpu_cpp.h>
#endif

#ifdef AURORA_ENABLE_RMLUI
#include "rmlui.hpp"
#endif

#include "gamepad.hpp"
#include "internal.hpp"
#include "thread.hpp"
#include "window.hpp"

#include <SDL3/SDL_filesystem.h>
#include <magic_enum.hpp>

#include "system_info.hpp"
#include "tracy/Tracy.hpp"

namespace aurora {
AuroraConfig g_config;
uint32_t g_sdlCustomEventsStart;
char g_gameName[4];

namespace {
constexpr Module Log{"aurora"};

#ifdef AURORA_ENABLE_GX
// GPU
using webgpu::g_device;
using webgpu::g_queue;
using webgpu::g_surface;

// 0224 test aid (aurora_request_screenshot): the frame's game image (before post-process and UI)
// at render resolution as a binary PPM. Read back synchronously on the render worker right after
// the frame's submission (test runs only: it stalls that frame).
std::mutex g_shotMutex;
std::string g_shotPath; // requested, not yet encoded
struct ShotJob {
  std::string path;
  wgpu::Buffer buffer;
  uint32_t width = 0, height = 0, pitch = 0;
  wgpu::TextureFormat format = wgpu::TextureFormat::Undefined;
};
ShotJob g_shot; // render worker only

void shot_encode(const wgpu::CommandEncoder& encoder) {
  {
    std::lock_guard lock{g_shotMutex};
    if (g_shotPath.empty() || g_shot.buffer) {
      return;
    }
    g_shot.path = std::move(g_shotPath);
    g_shotPath.clear();
  }
  const auto& src = webgpu::present_source();
  if (!src.texture || (src.format != wgpu::TextureFormat::BGRA8Unorm && src.format != wgpu::TextureFormat::RGBA8Unorm)) {
    g_shot = {};
    return;
  }
  g_shot.width = src.size.width;
  g_shot.height = src.size.height;
  g_shot.pitch = AURORA_ALIGN(g_shot.width * 4, 256);
  g_shot.format = src.format;
  const wgpu::BufferDescriptor bd{.label = "Screenshot readback",
                                  .usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst,
                                  .size = uint64_t(g_shot.pitch) * g_shot.height};
  g_shot.buffer = g_device.CreateBuffer(&bd);
  const wgpu::TexelCopyTextureInfo s{.texture = src.texture};
  const wgpu::TexelCopyBufferInfo d{.layout = {.bytesPerRow = g_shot.pitch, .rowsPerImage = g_shot.height},
                                    .buffer = g_shot.buffer};
  const wgpu::Extent3D extent{g_shot.width, g_shot.height, 1};
  encoder.CopyTextureToBuffer(&s, &d, &extent);
}

void shot_finish() {
  if (!g_shot.buffer) {
    return;
  }
  bool ok = false;
  const auto future = g_shot.buffer.MapAsync(wgpu::MapMode::Read, 0, g_shot.buffer.GetSize(),
                                             wgpu::CallbackMode::WaitAnyOnly,
                                             [&ok](wgpu::MapAsyncStatus status, wgpu::StringView) {
                                               ok = status == wgpu::MapAsyncStatus::Success; // no Dawn call here
                                             });
  webgpu::g_instance.WaitAny(future, UINT64_MAX);
  if (ok) {
    const auto* px = static_cast<const uint8_t*>(g_shot.buffer.GetConstMappedRange(0, g_shot.buffer.GetSize()));
    std::vector<uint8_t> rgb(size_t(g_shot.width) * g_shot.height * 3);
    const bool bgra = g_shot.format == wgpu::TextureFormat::BGRA8Unorm;
    for (uint32_t y = 0; y < g_shot.height; ++y) {
      const uint8_t* row = px + size_t(y) * g_shot.pitch;
      uint8_t* o = rgb.data() + size_t(y) * g_shot.width * 3;
      for (uint32_t x = 0; x < g_shot.width; ++x) {
        o[x * 3 + 0] = row[x * 4 + (bgra ? 2 : 0)];
        o[x * 3 + 1] = row[x * 4 + 1];
        o[x * 3 + 2] = row[x * 4 + (bgra ? 0 : 2)];
      }
    }
    g_shot.buffer.Unmap();
    if (FILE* f = std::fopen(g_shot.path.c_str(), "wb")) {
      std::fprintf(f, "P6\n%u %u\n255\n", g_shot.width, g_shot.height);
      std::fwrite(rgb.data(), 1, rgb.size(), f);
      std::fclose(f);
    }
  } else {
    Log.warn("Screenshot readback failed ({})", g_shot.path);
  }
  g_shot = {};
}

uint32_t clamp_scissor_coord(double value, uint32_t maximum) noexcept {
  if (!std::isfinite(value)) {
    return 0;
  }
  return static_cast<uint32_t>(std::clamp(value, 0.0, static_cast<double>(maximum)));
}

void set_present_viewport(const wgpu::RenderPassEncoder& pass, const gfx::Viewport& viewport, uint32_t surfaceWidth,
                          uint32_t surfaceHeight) noexcept {
  pass.SetViewport(viewport.left, viewport.top, viewport.width, viewport.height, viewport.znear, viewport.zfar);
  const auto scissorX = clamp_scissor_coord(std::floor(viewport.left), surfaceWidth);
  const auto scissorY = clamp_scissor_coord(std::floor(viewport.top), surfaceHeight);
  const auto scissorRight = clamp_scissor_coord(std::ceil(viewport.left + viewport.width), surfaceWidth);
  const auto scissorBottom = clamp_scissor_coord(std::ceil(viewport.top + viewport.height), surfaceHeight);
  pass.SetScissorRect(scissorX, scissorY, scissorRight - scissorX, scissorBottom - scissorY);
}
#endif

#ifdef AURORA_ENABLE_GX
constexpr std::array PreferredBackendOrder{
#ifdef ENABLE_BACKEND_WEBGPU
    BACKEND_WEBGPU,
#endif
#ifdef DAWN_ENABLE_BACKEND_D3D12
    BACKEND_D3D12,
#endif
#ifdef DAWN_ENABLE_BACKEND_METAL
    BACKEND_METAL,
#endif
#ifdef DAWN_ENABLE_BACKEND_VULKAN
    BACKEND_VULKAN,
#endif
#ifdef DAWN_ENABLE_BACKEND_D3D11
    BACKEND_D3D11,
#endif
// #ifdef DAWN_ENABLE_BACKEND_DESKTOP_GL
//     BACKEND_OPENGL,
// #endif
#ifdef DAWN_ENABLE_BACKEND_OPENGLES
    BACKEND_OPENGLES,
#endif
#ifdef DAWN_ENABLE_BACKEND_NULL
    BACKEND_NULL,
#endif
};
#else
constexpr std::array<AuroraBackend, 0> PreferredBackendOrder{};
#endif

bool g_initialFrame = false;

AuroraInfo initialize(int argc, char* argv[], const AuroraConfig& config) noexcept {
  g_config = config;
  Log.info("Aurora initializing");
  log_system_information();
  if (g_config.appName == nullptr) {
    g_config.appName = "Aurora";
  } else {
    g_config.appName = strdup(g_config.appName);
  }
  if (g_config.userPath == nullptr) {
    g_config.userPath = SDL_GetPrefPath(nullptr, g_config.appName);
  } else {
    g_config.userPath = strdup(g_config.userPath);
  }
  if (g_config.cachePath == nullptr) {
    g_config.cachePath = SDL_GetPrefPath(nullptr, g_config.appName);
  } else {
    g_config.cachePath = strdup(g_config.cachePath);
  }
  if (g_config.resourcesPath == nullptr) {
    g_config.resourcesPath = SDL_GetBasePath();
  } else {
    g_config.resourcesPath = strdup(g_config.resourcesPath);
  }
  if (g_config.msaa == 0) {
    g_config.msaa = 1;
  }
  if (g_config.maxTextureAnisotropy == 0) {
    g_config.maxTextureAnisotropy = 16;
  }
  AURORA_ASSERT(window::initialize(), "Error initializing window");

  g_sdlCustomEventsStart = SDL_RegisterEvents(2);
  AURORA_ASSERT(g_sdlCustomEventsStart, "Failed to allocate user events: {}", SDL_GetError());
  AURORA_ASSERT(window::initialize_event_watch(), "Error initializing SDL event watch");

#ifdef AURORA_ENABLE_GX
  /* Attempt to create a window using the calling application's desired backend */
  AuroraBackend selectedBackend = config.desiredBackend;
  bool windowCreated = false;
  if (selectedBackend != BACKEND_AUTO && window::create_window(selectedBackend)) {
    if (webgpu::initialize(selectedBackend, config.allowCpuAdapter)) {
      windowCreated = true;
    } else {
      window::destroy_window();
    }
  }

  if (!windowCreated) {
    for (const auto backendType : PreferredBackendOrder) {
      selectedBackend = backendType;
      if (!window::create_window(selectedBackend)) {
        continue;
      }
      if (webgpu::initialize(selectedBackend, config.allowCpuAdapter)) {
        windowCreated = true;
        break;
      } else {
        window::destroy_window();
      }
    }
  }

  AURORA_ASSERT(windowCreated, "Error creating window: {}", SDL_GetError());

  // Initialize SDL_Renderer for ImGui when we can't use a Dawn backend
  if (webgpu::g_backendType == wgpu::BackendType::Null) {
    AURORA_ASSERT(window::create_renderer(), "Failed to initialize SDL renderer: {}", SDL_GetError());
  }
#else
  AuroraBackend selectedBackend = BACKEND_NULL;
  AURORA_ASSERT(window::create_window(BACKEND_NULL), "Error creating window: {}", SDL_GetError());
  AURORA_ASSERT(window::create_renderer(), "Failed to initialize SDL renderer: {}", SDL_GetError());
#endif

  window::show_window();
  thread::set_current({
      .name = "Main thread",
      .affinity = thread::Affinity::SharedCache,
  });

#ifdef AURORA_ENABLE_GX
  gfx::initialize();
  gx::fifo::init();
  imgui::create_context();
#endif
  const auto size = window::get_window_size();
  Log.info("Using framebuffer size {}x{} scale {}", size.fb_width, size.fb_height, size.scale);
#ifdef AURORA_ENABLE_GX
  if (g_config.imGuiInitCallback != nullptr) {
    g_config.imGuiInitCallback(&size);
  }
  imgui::initialize();
#endif

#ifdef AURORA_ENABLE_RMLUI
  rmlui::initialize(size);
#endif

  g_initialFrame = true;
  g_config.desiredBackend = selectedBackend;
  return {
      .backend = selectedBackend,
      .userPath = g_config.userPath,
      .cachePath = g_config.cachePath,
      .window = window::get_sdl_window(),
      .windowSize = size,
  };
}

#ifdef AURORA_ENABLE_GX
namespace present_timing {
void shutdown();
}
#endif

void shutdown() noexcept {
#ifdef AURORA_ENABLE_GX
  gx::fifo::shutdown();
  gfx::render_worker::synchronize();
  present_timing::shutdown();
#ifdef AURORA_ENABLE_RMLUI
  rmlui::shutdown();
#endif
  imgui::shutdown();
  webgpu::postfx::shutdown();
  gfx::shutdown();
  webgpu::shutdown();
#endif
  gamepad::shutdown();
  window::shutdown();
}

const AuroraEvent* update() noexcept {
  ZoneScoped;
  if (g_initialFrame) {
    g_initialFrame = false;
    gamepad::initialize();
  }
#ifdef AURORA_ENABLE_GX
  gx::update();
#endif
  return window::poll_events();
}

#ifdef AURORA_ENABLE_GX
// Present timing (present_timing.h). The render worker hands each presented frame's
// OnSubmittedWorkDone future to a helper thread that waits on it and stamps when the GPU finished.
namespace present_timing {
std::atomic<bool> g_enabled{false};
std::atomic<uint64_t> g_counter{0};
std::atomic<uint64_t> g_lastDone{0};
std::atomic<uint64_t> g_timeouts{0}; // 0236: frames whose completion the waiter gave up on
std::mutex g_mutex;
std::condition_variable g_cv;
struct Pending {
  AuroraPresentTiming t;
  wgpu::Future future;
};
std::deque<Pending> g_pending;
std::deque<AuroraPresentTiming> g_done;
std::thread g_thread;
bool g_stop = false;

int64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void waiter() {
  for (;;) {
    Pending p;
    {
      std::unique_lock lk(g_mutex);
      g_cv.wait(lk, [] { return g_stop || !g_pending.empty(); });
      if (g_stop) {
        return;
      }
      p = g_pending.front();
    }
    bool ok = false;
    // 0216: polled, never a blocking WaitAny. With implicit device synchronization a WaitAny with a
    // timeout holds Dawn's device lock until the GPU finishes, so every other thread's Dawn call
    // (render worker encoding the next frame, FIFO thread creating textures, the game thread's
    // EFB readbacks) waited for the GPU too: the original-30 path (no frame-ahead limit) fell from
    // 28 to 18.5 fps at 4K with this record on. A zero-timeout poll every ~0.25 ms takes the lock
    // only briefly; the stamp is up to that late.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    const activity::Scope busy{activity::Thread::PresentWaiter, "polling a frame's GPU completion (OnSubmittedWorkDone)"};
    while (!ok && std::chrono::steady_clock::now() < deadline) {
      ok = webgpu::g_instance.WaitAny(p.future, 0) == wgpu::WaitStatus::Success;
      if (g_stop) {
        return;
      }
      if (!ok) {
        std::this_thread::sleep_for(std::chrono::microseconds(250));
      }
    }
    p.t.doneNs = ok ? now_ns() : 0;
    if (!ok) {
      // 0236: say so (once, then every 100th): without completions the application's frame-ahead
      // wait (aurora_wait_frame_done) only ends by its timeout.
      const uint64_t n = g_timeouts.fetch_add(1, std::memory_order_relaxed) + 1;
      if (n == 1 || n % 100 == 0) {
        Log.warn("Present timing: frame {} not reported finished by the GPU within 5 s ({} such frames)", p.t.frame, n);
      }
    }
    {
      std::lock_guard lk(g_mutex);
      g_pending.pop_front();
      g_done.push_back(p.t);
      while (g_done.size() > 512) {
        g_done.pop_front();
      }
      g_lastDone.store(p.t.frame, std::memory_order_release);
    }
    g_cv.notify_all();
  }
}

// Render worker, after the frame's last submit and its Present.
void frame_presented(uint64_t frame, int64_t submitNs, int64_t presentNs) {
  if (!g_enabled.load(std::memory_order_acquire)) {
    g_lastDone.store(frame, std::memory_order_release);
    return;
  }
  wgpu::Future f = g_queue.OnSubmittedWorkDone(wgpu::CallbackMode::WaitAnyOnly,
                                               [](wgpu::QueueWorkDoneStatus, wgpu::StringView) {});
  {
    std::lock_guard lk(g_mutex);
    if (!g_thread.joinable()) {
      g_thread = std::thread(waiter);
    }
    g_pending.push_back({AuroraPresentTiming{frame, submitNs, presentNs, 0}, f});
  }
  g_cv.notify_all();
}

// 0215: GPU keep-warm. A light frame (a menu over a still picture) lets the GPU drop its clocks
// within half a second (RTX 3090 Ti, measured with nvidia-smi: P0 1995/10501 MHz core/memory ->
// P3 ~1000/5001 -> P5/P8 345-450/405-810); the first frames of the full scene after it then took
// 30-60 ms instead of 6 (4K, enhanced lighting) until the clocks came back. The application asks
// for `units` of filler work per frame, sized by its own feedback so the GPU stays about as busy
// as in play (src/recomp/pacing.cpp in LM). The filler streams through a 64 MiB buffer (one unit =
// one read pass): ALU-only filler kept the core busy but the driver still lowered the memory
// clock (P3), and the scene is bandwidth-bound at 4K.
std::atomic<uint32_t> g_warmUnits{0};
struct Warm {
  wgpu::ComputePipeline pipeline;
  wgpu::Buffer data, params;
  wgpu::BindGroup bindGroup;
  bool failed = false;
} g_warm;
// 0230: 128 invocations per workgroup, 32768 workgroups: within the compatibility-mode limits
// (maxComputeWorkgroupSizeX / maxComputeInvocationsPerWorkgroup 128, 65535 workgroups per
// dimension). 256 was a fatal validation error on devices without core limits (Linux on OpenGL ES).
constexpr uint32_t WarmElems = 4u << 20, WarmGroupSize = 128; // 4 Mi vec4f = 64 MiB

void encode_keep_warm(wgpu::CommandEncoder& encoder) {
  const uint32_t units = std::min(g_warmUnits.load(std::memory_order_relaxed), 400u);
  if (units == 0 || g_warm.failed) {
    return;
  }
  if (!g_warm.pipeline) {
    static constexpr char Source[] = R"(
struct Params { iters: u32, mask: u32, p1: u32, p2: u32 };
@group(0) @binding(0) var<storage, read_write> data: array<vec4f>;
@group(0) @binding(1) var<uniform> params: Params;
@compute @workgroup_size(128)
fn cs_main(@builtin(global_invocation_id) id: vec3u) {
  var v = vec4f(0.0);
  for (var i = 0u; i < params.iters; i++) {
    v = v * 0.5 + data[(id.x + i * 1048573u) & params.mask] * 0.25;
  }
  data[id.x] = v;
}
)";
    // 0230: an error here turns keep-warm off instead of being a fatal uncaptured error.
    gfx::push_shader_error_scope();
    const wgpu::ShaderSourceWGSL wgsl{wgpu::ShaderSourceWGSL::Init{.code = Source}};
    const wgpu::ShaderModuleDescriptor md{.nextInChain = &wgsl, .label = "GPU keep-warm"};
    const auto module = g_device.CreateShaderModule(&md);
    const wgpu::ComputePipelineDescriptor pd{.label = "GPU keep-warm",
                                             .compute = wgpu::ComputeState{.module = module, .entryPoint = "cs_main"}};
    const ::aurora::webgpu::CreateTimer createTimer_378{"pipeline aurora.cpp:378"}; // 0223 (AURORA_CREATE_LOG)
    g_warm.pipeline = g_device.CreateComputePipeline(&pd);
    if (!gfx::pop_shader_error_scope("GPU keep-warm")) {
      g_warm.pipeline = {};
      g_warm.failed = true;
      return;
    }
    const wgpu::BufferDescriptor bd{.label = "GPU keep-warm data",
                                    .usage = wgpu::BufferUsage::Storage,
                                    .size = uint64_t(WarmElems) * 16};
    g_warm.data = g_device.CreateBuffer(&bd);
    const wgpu::BufferDescriptor pb{.label = "GPU keep-warm params",
                                    .usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst,
                                    .size = 16};
    g_warm.params = g_device.CreateBuffer(&pb);
    if (!g_warm.pipeline || !g_warm.data || !g_warm.params) {
      g_warm.failed = true;
      return;
    }
    const std::array entries{
        wgpu::BindGroupEntry{.binding = 0, .buffer = g_warm.data, .size = bd.size},
        wgpu::BindGroupEntry{.binding = 1, .buffer = g_warm.params, .size = 16},
    };
    const wgpu::BindGroupDescriptor bgd{.label = "GPU keep-warm",
                                        .layout = g_warm.pipeline.GetBindGroupLayout(0),
                                        .entryCount = entries.size(),
                                        .entries = entries.data()};
    g_warm.bindGroup = g_device.CreateBindGroup(&bgd);
  }
  const uint32_t params[4] = {units, WarmElems - 1, 0, 0};
  g_queue.WriteBuffer(g_warm.params, 0, params, sizeof(params));
  const wgpu::ComputePassDescriptor cpd{.label = "GPU keep-warm"};
  auto pass = encoder.BeginComputePass(&cpd);
  pass.SetPipeline(g_warm.pipeline);
  pass.SetBindGroup(0, g_warm.bindGroup);
  pass.DispatchWorkgroups(WarmElems / WarmGroupSize);
  pass.End();
}

void shutdown() {
  {
    std::lock_guard lk(g_mutex);
    g_stop = true;
  }
  g_cv.notify_all();
  if (g_thread.joinable()) {
    g_thread.join();
  }
}
} // namespace present_timing
#endif

bool begin_frame() noexcept {
  ZoneScoped;
#ifdef AURORA_ENABLE_GX
  const activity::Scope busy{activity::Thread::Host, "aurora_begin_frame"};
  {
    if (!window::is_presentable()) {
      webgpu::release_surface();
      return false;
    }
    if (window::is_paused()) {
      return false;
    }
    if (!g_surface) {
      webgpu::refresh_surface(true);
      if (!g_surface) {
        return false;
      }
    }
  }

  imgui::new_frame(window::get_window_size());
  if (!gfx::begin_frame()) {
    return false;
  }
  present_timing::g_counter.fetch_add(1, std::memory_order_acq_rel);
  gx::fifo::begin_frame();
#endif
  return true;
}

void end_frame(bool presentToWindow = true) noexcept {
  ZoneScoped;
#ifdef AURORA_ENABLE_GX
  const activity::Scope busy{activity::Thread::Host, "aurora_end_frame"};
  gx::fifo::drain();
  gx::fifo::end_frame();
  gx::texture::end_frame();
  gfx::finish();
  auto imguiDrawData = imgui::freeze();

  const auto& presentSource = webgpu::present_source();
  const auto viewport = webgpu::calculate_present_viewport(webgpu::g_graphicsConfig.surfaceConfiguration.width,
                                                           webgpu::g_graphicsConfig.surfaceConfiguration.height,
                                                           presentSource.size.width, presentSource.size.height);

  wgpu::BindGroup rmlBindGroup;
  bool rmlOverlay = false;
#if AURORA_ENABLE_RMLUI
  if (rmlui::is_initialized()) {
    auto rmlFrame = rmlui::record_frame(viewport);
    rmlBindGroup = std::move(rmlFrame.bindGroup);
    rmlOverlay = rmlFrame.overlay;
  }
#endif

  gfx::end_frame([presentToWindow, rmlBindGroup = std::move(rmlBindGroup), rmlOverlay, viewport,
                  imguiDrawData = std::move(imguiDrawData),
                  timingFrame = present_timing::g_counter.load(std::memory_order_acquire)](
                     wgpu::CommandEncoder& encoder, std::vector<gfx::AfterSubmitCallback> afterSubmitCallbacks) {
    // The frame's own GPU work is submitted before the surface texture is acquired, and its
    // after-submit callbacks (readback maps: EFB copies to RAM, peeks) run right away. Acquiring
    // the surface texture and presenting can block for a display interval, which used to delay
    // every readback of the frame by that long; the present blit goes in a second command buffer.
    // AURORA_LATE_SUBMIT=1: the previous single submission after the acquire (A/B).
    static const bool sLateSubmit = std::getenv("AURORA_LATE_SUBMIT") != nullptr;
    const activity::Scope busy{activity::Thread::RenderWorker, "finishing the frame (keep-warm, submit)"};
    present_timing::encode_keep_warm(encoder);  // 0215: after the frame's own work
    shot_encode(encoder);                       // 0224 test aid
    // The XR transfer reads a game texture, not the desktop swapchain. Composite
    // its per-eye UI explicitly, including offscreen right-eye submissions.
    static const bool vrOverlay = [] { const char* value = std::getenv("LM_VR"); return value && std::string_view(value) == "1"; }();
    if (vrOverlay) {
      const auto& target = webgpu::vr_present_target();
      const auto bindGroup = webgpu::create_copy_bind_group(webgpu::present_source());
      const wgpu::RenderPassColorAttachment attachment{
          .view = target.view, .loadOp = wgpu::LoadOp::Clear, .storeOp = wgpu::StoreOp::Store};
      const wgpu::RenderPassDescriptor descriptor{
          .label = "VR game and overlay", .colorAttachmentCount = 1, .colorAttachments = &attachment};
      const auto pass = encoder.BeginRenderPass(&descriptor);
      pass.SetViewport(0, 0, float(target.size.width), float(target.size.height), 0, 1);
      pass.SetPipeline(webgpu::g_CopyPipeline);
      pass.SetBindGroup(0, bindGroup, 0, nullptr);
      pass.Draw(3);
      imgui::render(pass, imguiDrawData, float(target.size.width), float(target.size.height));
      pass.End();
      // A transparent UI-only image can be placed over stereo gameplay by XR.
      const auto& overlay=webgpu::vr_overlay_target();
      const wgpu::RenderPassColorAttachment uiAttachment{
          .view=overlay.view,.loadOp=wgpu::LoadOp::Clear,.storeOp=wgpu::StoreOp::Store,.clearValue={0,0,0,0}};
      const wgpu::RenderPassDescriptor uiDescriptor{.label="VR separate settings overlay",.colorAttachmentCount=1,.colorAttachments=&uiAttachment};
      const auto uiPass=encoder.BeginRenderPass(&uiDescriptor);
      imgui::render(uiPass,imguiDrawData,float(overlay.size.width),float(overlay.size.height));
      uiPass.End();
    }
    if (!sLateSubmit) {
      ZoneScopedN("Frame Submit");
      const wgpu::CommandBufferDescriptor frameBufDescriptor{.label = "Frame command buffer"};
      const auto frameBuffer = encoder.Finish(&frameBufDescriptor);
      g_queue.Submit(1, &frameBuffer);
      shot_finish();
      for (auto& callback : afterSubmitCallbacks) {
        if (callback) {
          callback();
        }
      }
      afterSubmitCallbacks.clear();
      constexpr wgpu::CommandEncoderDescriptor presentEncoderDescriptor{.label = "Present encoder"};
      encoder = g_device.CreateCommandEncoder(&presentEncoderDescriptor);
    }
    wgpu::Texture currentTexture;
    wgpu::TextureView currentView;
    auto surfaceStatus = wgpu::SurfaceGetCurrentTextureStatus::Error;
    {
      window::SurfaceLock surfaceLock;
      if (presentToWindow && window::is_presentable() && g_surface) {
        ZoneScopedN("Acquire texture");
        wgpu::SurfaceTexture surfaceTexture;
        const activity::Scope acquire{activity::Thread::RenderWorker,
                                      "acquiring the swap chain texture (GetCurrentTexture)"};
        // AURORA_TEST_ACQUIRE_STALL=<frame>[,<seconds>] (test aid): the acquire of that frame takes
        // <seconds> (default 40), like a swap chain that hands out no image (hang reports).
        static const std::pair<uint64_t, int> sStall = [] {
          const char* e = std::getenv("AURORA_TEST_ACQUIRE_STALL");
          if (e == nullptr) {
            return std::pair<uint64_t, int>{0, 0};
          }
          const char* comma = std::strchr(e, ',');
          return std::pair<uint64_t, int>{std::strtoull(e, nullptr, 10), comma != nullptr ? std::atoi(comma + 1) : 40};
        }();
        if (sStall.first != 0 && timingFrame == sStall.first) {
          std::this_thread::sleep_for(std::chrono::seconds(sStall.second));
        }
        g_surface.GetCurrentTexture(&surfaceTexture);
        surfaceStatus = surfaceTexture.status;
        if (surfaceStatus == wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal) {
          currentTexture = std::move(surfaceTexture.texture);
          currentView = currentTexture.CreateView();
        }
      }
    }

    const bool canPresent = currentTexture && currentView;
    if (canPresent) {
      wgpu::BindGroup presentBindGroup;
      // Post-process chain (if any) reads the game frame directly; the UI passes below stay unfiltered.
      const bool postfx = !(rmlBindGroup && !rmlOverlay) && webgpu::postfx::prepare(encoder, viewport);
      if (rmlBindGroup && !rmlOverlay) {
        presentBindGroup = rmlBindGroup;
      } else if (!postfx) {
        const auto& resampledSource = webgpu::resample_present_source(encoder, viewport);
        presentBindGroup = webgpu::create_copy_bind_group(resampledSource);
      }
      {
        const std::array attachments{
            wgpu::RenderPassColorAttachment{
                .view = currentView,
                .loadOp = wgpu::LoadOp::Clear,
                .storeOp = wgpu::StoreOp::Store,
            },
        };
        const wgpu::RenderPassDescriptor renderPassDescriptor{
            .label = "EFB copy render pass",
            .colorAttachmentCount = attachments.size(),
            .colorAttachments = attachments.data(),
            .timestampWrites = webgpu::gpu_prof::pass_writes("Present blit"),
        };
        const auto pass = encoder.BeginRenderPass(&renderPassDescriptor);
        // Copy EFB -> XFB (swapchain)
        set_present_viewport(pass, viewport, webgpu::g_graphicsConfig.surfaceConfiguration.width,
                             webgpu::g_graphicsConfig.surfaceConfiguration.height);
        if (postfx) {
          webgpu::postfx::draw(pass);
        } else {
          pass.SetPipeline(webgpu::g_CopyPipeline);
          pass.SetBindGroup(0, presentBindGroup, 0, nullptr);
          pass.Draw(3);
        }
        if (rmlBindGroup && rmlOverlay) {
          pass.SetPipeline(webgpu::g_CopyPremultipliedAlphaPipeline);
          pass.SetBindGroup(0, rmlBindGroup, 0, nullptr);
          pass.Draw(3);
        }
        pass.End();
      }
      {
        const std::array attachments{
            wgpu::RenderPassColorAttachment{
                .view = currentView,
                .loadOp = wgpu::LoadOp::Load,
                .storeOp = wgpu::StoreOp::Store,
            },
        };
        const wgpu::RenderPassDescriptor renderPassDescriptor{
            .label = "ImGui render pass",
            .colorAttachmentCount = attachments.size(),
            .colorAttachments = attachments.data(),
            .timestampWrites = webgpu::gpu_prof::pass_writes("ImGui"),
        };
        const auto pass = encoder.BeginRenderPass(&renderPassDescriptor);
        pass.SetViewport(0.f, 0.f, static_cast<float>(webgpu::g_graphicsConfig.surfaceConfiguration.width),
                         static_cast<float>(webgpu::g_graphicsConfig.surfaceConfiguration.height), 0.f, 1.f);
        imgui::render(pass, imguiDrawData);
        pass.End();
      }
    } else if (presentToWindow) {
      Log.info("Skipping present; window not presentable");
    }
    webgpu::gpu_prof::frame_end(encoder);
    const wgpu::CommandBufferDescriptor cmdBufDescriptor{.label = "Redraw command buffer"};
    const auto buffer = encoder.Finish(&cmdBufDescriptor);
    {
      ZoneScopedN("Queue Submit");
      const activity::Scope submit{activity::Thread::RenderWorker, "submitting the present blit (Queue::Submit)"};
      g_queue.Submit(1, &buffer);
    }
    const int64_t timingSubmitNs = present_timing::now_ns();
    webgpu::gpu_prof::after_submit();
    if (canPresent && g_surface) {
      ZoneScopedN("Present");
      wgpu::ConvertibleStatus status = wgpu::Status::Error;
      {
        window::SurfaceLock surfaceLock;
        if (window::is_presentable()) {
          const activity::Scope present{activity::Thread::RenderWorker, "presenting (Surface::Present)"};
          status = g_surface.Present();
        }
      }
      if (status) {
        gfx::after_present();
        present_timing::frame_presented(timingFrame, timingSubmitNs, present_timing::now_ns());
      } else {
        Log.warn("Surface present failed");
        webgpu::release_surface();
      }
    } else if (presentToWindow && g_surface) {
      present_timing::frame_presented(timingFrame, timingSubmitNs, 0);
      switch (surfaceStatus) {
      case wgpu::SurfaceGetCurrentTextureStatus::Timeout:
        Log.warn("Surface texture acquisition timed out");
        break;
      case wgpu::SurfaceGetCurrentTextureStatus::SuccessSuboptimal:
      case wgpu::SurfaceGetCurrentTextureStatus::Outdated:
        Log.info("Surface texture is {}, reconfiguring swapchain", magic_enum::enum_name(surfaceStatus));
        window::push_custom_event(window::CustomEvent::RefreshSurface);
        break;
      case wgpu::SurfaceGetCurrentTextureStatus::Lost:
        Log.warn("Surface texture is {}, releasing surface", magic_enum::enum_name(surfaceStatus));
        webgpu::release_surface();
        break;
      case wgpu::SurfaceGetCurrentTextureStatus::Error:
        Log.warn("Surface texture is {}, dropping surface", magic_enum::enum_name(surfaceStatus));
        g_surface = {};
        break;
      default:
        if (!window::is_presentable()) {
          webgpu::release_surface();
        } else {
          Log.error("Failed to get surface texture: {}", magic_enum::enum_name(surfaceStatus));
        }
        break;
      }
    }
    {
      const activity::Scope after{activity::Thread::RenderWorker, "after-submit callbacks (readback maps)"};
      for (auto& callback : afterSubmitCallbacks) {
        if (callback) {
          callback();
        }
      }
      gfx::after_submit();
    }

    TracyPlotConfig("aurora: lastVertSize", tracy::PlotFormatType::Memory, false, true, 0);
    TracyPlotConfig("aurora: lastUniformSize", tracy::PlotFormatType::Memory, false, true, 0);
    TracyPlotConfig("aurora: lastIndexSize", tracy::PlotFormatType::Memory, false, true, 0);
    TracyPlotConfig("aurora: lastStorageSize", tracy::PlotFormatType::Memory, false, true, 0);
    TracyPlotConfig("aurora: lastTextureUploadSize", tracy::PlotFormatType::Memory, false, true, 0);

    const auto& stats = gfx::detail::resources().stats;
    TracyPlot("aurora: queuedPipelines", static_cast<int64_t>(stats.queuedPipelines));
    TracyPlot("aurora: createdPipelines", static_cast<int64_t>(stats.createdPipelines));
    TracyPlot("aurora: drawCallCount", static_cast<int64_t>(stats.drawCallCount));
    TracyPlot("aurora: mergedDrawCallCount", static_cast<int64_t>(stats.mergedDrawCallCount));
    TracyPlot("aurora: lastVertSize", static_cast<int64_t>(stats.lastVertSize));
    TracyPlot("aurora: lastUniformSize", static_cast<int64_t>(stats.lastUniformSize));
    TracyPlot("aurora: lastIndexSize", static_cast<int64_t>(stats.lastIndexSize));
    TracyPlot("aurora: lastStorageSize", static_cast<int64_t>(stats.lastStorageSize));
    TracyPlot("aurora: lastTextureUploadSize", static_cast<int64_t>(stats.lastTextureUploadSize));
  });

#endif
}
} // namespace
} // namespace aurora

// C API bindings
AuroraInfo aurora_initialize(int argc, char* argv[], const AuroraConfig* config) {
  return aurora::initialize(argc, argv, *config);
}
void aurora_shutdown() { aurora::shutdown(); }
const AuroraEvent* aurora_update() { return aurora::update(); }
bool aurora_begin_frame() { return aurora::begin_frame(); }
#ifdef AURORA_ENABLE_GX
void aurora_present_timing_enable(bool enable) { aurora::present_timing::g_enabled.store(enable, std::memory_order_release); }
void aurora_set_gpu_keep_warm(uint32_t units) { aurora::present_timing::g_warmUnits.store(units, std::memory_order_relaxed); }
uint64_t aurora_frame_counter() { return aurora::present_timing::g_counter.load(std::memory_order_acquire); }
size_t aurora_present_timings(AuroraPresentTiming* out, size_t max) {
  std::lock_guard lk(aurora::present_timing::g_mutex);
  size_t n = 0;
  auto& d = aurora::present_timing::g_done;
  while (n < max && !d.empty()) {
    out[n++] = d.front();
    d.pop_front();
  }
  return n;
}
uint64_t aurora_present_timing_timeouts() {
  return aurora::present_timing::g_timeouts.load(std::memory_order_relaxed);
}
size_t aurora_describe_activity(char* out, size_t size, uint32_t minMs) {
  using namespace aurora::activity;
  static constexpr const char* Names[] = {"host", "render worker", "GX FIFO thread", "present-timing waiter"};
  if (out == nullptr || size == 0) {
    return 0;
  }
  out[0] = 0;
  size_t len = 0;
  const int64_t now = now_ns();
  for (size_t i = 0; i < static_cast<size_t>(Thread::Count); ++i) {
    const char* what = g_slots[i].what.load(std::memory_order_acquire);
    const int64_t since = g_slots[i].sinceNs.load(std::memory_order_relaxed);
    if (what == nullptr || since == 0 || now - since < int64_t(minMs) * 1'000'000) {
      continue;
    }
    const int n = std::snprintf(out + len, size - len, "%s%s: %s for %.1f s", len ? "; " : "", Names[i], what,
                                double(now - since) * 1e-9);
    if (n < 0 || size_t(n) >= size - len) {
      break;
    }
    len += size_t(n);
  }
  return len;
}
bool aurora_wait_frame_done(uint64_t frame, int64_t timeoutNs) {
  using namespace aurora::present_timing;
  if (g_lastDone.load(std::memory_order_acquire) >= frame) {
    return true;
  }
  std::unique_lock lk(g_mutex);
  return g_cv.wait_for(lk, std::chrono::nanoseconds(timeoutNs),
                       [frame] { return g_stop || g_lastDone.load(std::memory_order_acquire) >= frame; });
}
#endif
void aurora_end_frame() { aurora::end_frame(); }
void aurora_end_frame_offscreen() { aurora::end_frame(false); }
SDL_Window* aurora_get_window() { return aurora::window::get_sdl_window(); }
AuroraWindowSize aurora_get_window_size() {
  return aurora::window::get_sdl_window() != nullptr ? aurora::window::get_window_size() : AuroraWindowSize{};
}
AuroraBackend aurora_get_backend() { return aurora::g_config.desiredBackend; }
const AuroraBackend* aurora_get_available_backends(size_t* count) {
  if (count != nullptr) {
    *count = aurora::PreferredBackendOrder.size();
  }
  return aurora::PreferredBackendOrder.data();
}
void aurora_set_log_level(AuroraLogLevel level) { aurora::g_config.logLevel = level; }
void aurora_set_pause_on_focus_lost(bool value) { aurora::g_config.pauseOnFocusLost = value; }
void aurora_set_background_input(bool value) {
  aurora::g_config.allowJoystickBackgroundEvents = value;
  aurora::window::set_background_input(value);
}
void aurora_set_resampler(AuroraSampler sampler) {
#ifdef AURORA_ENABLE_GX
  aurora::webgpu::set_resampler(sampler);
#else
  (void)sampler;
#endif
}
void aurora_set_timescale(float scale) { aurora::time::set_scale(scale); }
uint32_t aurora_get_msaa_support() {
#ifdef AURORA_ENABLE_GX
  return aurora::webgpu::g_msaaSupportMask;
#else
  return 1;
#endif
}
uint32_t aurora_get_msaa() {
#ifdef AURORA_ENABLE_GX
  return std::max(aurora::webgpu::g_graphicsConfig.msaaSamples, 1u);
#else
  return 1;
#endif
}
void aurora_request_screenshot(const char* path) {
#ifdef AURORA_ENABLE_GX
  std::lock_guard lock{aurora::g_shotMutex};
  aurora::g_shotPath = path != nullptr ? path : "";
#endif
}
float aurora_get_timescale() { return aurora::time::scale(); }
