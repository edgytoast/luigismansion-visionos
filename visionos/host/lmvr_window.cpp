// The game in a volume in the shared space ("On your table"): the game's side of the window mode
// (lmvr_visionos.h's window and mirror calls; the hooks in lmvr_game_hooks.h). visionOS gives an
// app no head pose outside a Full Space, so the game plays flat, with its own camera; its main 3D
// view is recorded by the scene mirror (aurora/mirror.h, the port's aurora patch) for the app to
// draw again in the volume (MirrorScene.swift), placed by the base's world-locked set
// (src/recomp/volume.cpp), and its 2D (the HUD, text, full-screen pictures) goes to the volume's
// front panel from the frames handed over here. Adapted from the Wind Waker port's
// visionos/host/wwvr_window.cpp (itself from the TPVR port's): the same aurora, Dawn and app side.
//
// Data path, per frame:
//   host thread   lmvr_window_begin_frame()  waits for the window's tick (the app's RealityKit update)
//   game thread   lmvr_window_scene_drawn()  the main 3D view is drawn: resolve_pass(colour) ->
//                                            encoder task: the scene before the 2D (base)
//   host thread   lmvr_window_frame_done()   the game's frame is done: resolve_pass(colour) ->
//                                            encoder task: the finished frame (final)
//   render worker the tasks copy the snapshots into the slot's IOSurfaces (Dawn imports them),
//                 then, after the frame's command buffer is submitted, end Dawn's access and
//                 publish the slot with the MTLSharedEvent the reads must wait on.
//   app (main)    lmvr_visionos_window_acquire() takes the slot due now (the pacer, below), its
//                 GPU reads wait on that event, and lmvr_visionos_window_release() hands the slot
//                 back once they finish; lmvr_visionos_mirror_acquire() the mirror's frame of the
//                 same game frame, with that frame's placement in the volume.
//
// The pacer. The game makes a frame every two retraces (30 a second), but each is ready whenever
// its work happens to end: on the headset 33 ms on average with 40 to 55 ms ones every second.
// Shown as each finished, at the next of the display's 90 updates a second, frames stood for two,
// three or four updates at random, which reads as choppy motion even at a full 30 (12ac2eb). Each
// frame is given a time to show: a game frame's time (the two retraces) after the one before, or,
// if it comes later than that, when it comes (the beat moves with it). Every five seconds the beat is
// brought earlier by what the earliest frame of them waited beyond 2 ms, so the delay settles at the
// least that keeps the beat through the latest frames of the last few seconds (on the headset a
// late one came every second or so: each would otherwise move the beat). A frame passed over by a
// later one due at the same update goes back unseen.
// Every Dawn call happens on the render worker (inside encoder tasks and their after-submit
// callbacks), the thread aurora encodes on.

#include "lmvr_game_hooks.h"
#include "lmvr_visionos.h"
#include "sense_pad.h"

#include <aurora/aurora.h>
#include <aurora/gfx.hpp>
#include <aurora/mirror.h>
#include <webgpu/webgpu_cpp.h>

#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurfaceRef.h>
#include <dispatch/dispatch.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <vector>

namespace recomp {
uint32_t viRetraceCount();  // the base's src/recomp/hle_vi.cpp
}

#define WINDOW_LOG(...) std::fprintf(stderr, "[LMVR] volume: " __VA_ARGS__)

namespace lmvr::window {
namespace {

std::atomic_bool g_enabled{false};
dispatch_semaphore_t g_tick = dispatch_semaphore_create(0);
// Bumped whenever any slot's surfaces are (re)made: the app drops its textures over them.
std::atomic<uint64_t> g_surfaceGeneration{1};
// The volume's size (metres) and the side the viewer looks from (lmvr_visionos_set_volume).
std::atomic<float> g_volume[3] = {1.2f, 0.6f, 1.0f};
std::atomic<int> g_viewpoint{0};
// The settings: open now (host thread), and whether the frame before rendered their overlay (read on
// the render worker by the next frame's task).
bool g_settingsOpen = false;
std::atomic_bool g_uiRendered{false};
// Frames since the settings closed (host thread), and whether the slots' ui surfaces can go (a
// second after: each slot's own frame releases its surface; render worker).
int g_settingsClosedFrames = 0;
std::atomic_bool g_uiRelease{false};

// One IOSurface the app reads, imported into Dawn.
struct Surface {
    IOSurfaceRef surface = nullptr;
    wgpu::SharedTextureMemory memory;
    wgpu::Texture texture;
    wgpu::TextureFormat format = wgpu::TextureFormat::Undefined;
    uint32_t width = 0, height = 0;
    bool accessOpen = false;
};

enum class SlotState { Free, Writing, Ready, Reading };

struct Slot {
    SlotState state = SlotState::Free;
    Surface final, base;
    uint64_t serial = 0;
    uint32_t gameFrame = 0;
    lmvr_window_scene scene{};
    bool hasScene = false, hasFinal = false, hasBase = false, flat2d = false, capture = false, hasExact = false;
    // The scene's picture taken again as the frame's 2D began (lmvr_window_overlay_begins), beside
    // `base` (taken at the end of the main 3D view): the app keys the HUD against it where the two
    // agree within the game's copy round trip, and against `base` where something drew between
    // (the game over's greying and its "Good night").
    Surface exact;
    wgpu::TextureView pendingExact;
    // The settings overlay (lmvr_window_settings), copied with its alpha from the last frame that
    // rendered it.
    Surface ui;
    bool hasUi = false;
    uint32_t exactWidth = 0, exactHeight = 0;
    bool exactValid = false;  // this frame's retake stands (one guest frame: its final is that frame's)
    void* event = nullptr;  // MTLSharedEvent (Dawn's), borrowed
    uint64_t value = 0;
    double ready = 0, due = 0;  // the pacer's: when it was handed over, and when it should show (s)
    // Snapshots taken on the game or host thread, copied on the render worker.
    wgpu::TextureView pendingFinal, pendingBase;
    uint32_t finalWidth = 0, finalHeight = 0, baseWidth = 0, baseHeight = 0;
};

// One the game writes, one the app reads, two waiting for their time, one spare.
constexpr size_t kSlots = 5;
constexpr size_t kMaxWaiting = 2;
std::mutex g_mutex;  // slot states, serials, the pacer, g_scenes
std::array<Slot, kSlots> g_slots;
uint64_t g_nextSerial = 1;
uint64_t g_handed = 0;

// The placements of the last 64 game frames (two seconds), for the mirror's frame of each.
struct FrameScene {
    uint32_t gameFrame = 0xFFFFFFFFu;
    lmvr_window_scene scene{};
};
std::array<FrameScene, 64> g_scenes;

// Per-frame state (the host thread opens the frame, the game thread fills it, the host closes it:
// they take turns, lockstep).
int g_frameSlot = -1;
bool g_sceneThisFrame = false;
bool g_basePushed = false;
bool g_exactPushed = false;  // the picture was retaken as the 2D began (lmvr_window_overlay_begins)
bool g_severalFrames = false;  // this window frame ran several guest frames (a fast-forward)
std::atomic<uint32_t> g_gameFrame{0};

// The timing log's counts (lmvr_visionos_volume_stats), under g_mutex.
struct Stats {
    uint64_t frames = 0, published = 0, replaced = 0, noSlot = 0;
    double frameMs = 0, frameMsMax = 0;
    uint64_t fields[4] = {};
    uint64_t late = 0, trims = 0, passed = 0, taken = 0;
    double waitMs = 0;
} g_stats;
std::chrono::steady_clock::time_point g_lastFrameStart{};
uint32_t g_lastRetrace = 0;

// The pacer (above), under g_mutex: the interval frames come at (eased), the last time given, and
// the least a frame waited for its time since the last trim.
constexpr double kField = 1001.0 / 60000.0;  // an NTSC retrace (s)
constexpr double kTolerance = 0.003;          // a frame due this soon after an update shows at it (s)
struct Pacer {
    double lastReady = 0, interval = 2 * kField, lastDue = 0, beat = 2 * kField;
    double leastEarly = 1e9;
    int sinceTrim = 0;
} g_pace;
const bool g_unpaced = std::getenv("LMVR_TEST_VOLUME_UNPACED") != nullptr;

double Seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// A slot just handed over: its time to show (g_mutex held).
double ScheduleFrame(double now) {
    if (g_pace.lastReady > 0) {
        const double dt = now - g_pace.lastReady;
        if (dt > 0 && dt < 0.25) g_pace.interval += (dt - g_pace.interval) * 0.1;
    }
    g_pace.lastReady = now;
    // The game's beat: a whole number of retraces (two in play, one in the 60 Hz menus).
    g_pace.beat = kField * std::clamp(std::round(g_pace.interval / kField), 1.0, 4.0);
    if (g_unpaced) return now;
    double due = g_pace.lastDue + g_pace.beat;
    const double early = due - now;
    // A new beat: the first frame, after a stall, or a frame that would wait longer than a beat (the
    // game going faster, 30 to 60 in its pause screen and Game Boy Horror: the beat, still the old
    // one for a few frames, ran ahead of the frames, and every frame went back unseen until the trim,
    // five seconds of a still panel; the review's model).
    if (g_pace.lastDue == 0 || early < -0.25 || early > g_pace.beat) {
        due = now;
        g_pace.leastEarly = 1e9;
        g_pace.sinceTrim = 0;
    } else if (early < 0) {
        due = now;  // late: the beat moves with it
        g_stats.late++;
        g_pace.leastEarly = 0;
    } else {
        g_pace.leastEarly = std::min(g_pace.leastEarly, early);
    }
    if (++g_pace.sinceTrim >= 150) {
        // Five seconds without a frame needing all its wait: the beat comes earlier by the spare.
        if (g_pace.leastEarly > 0.004 && g_pace.leastEarly < 1) {
            due = std::max(now, due - (g_pace.leastEarly - 0.002));
            g_stats.trims++;
        }
        g_pace.leastEarly = 1e9;
        g_pace.sinceTrim = 0;
    }
    g_pace.lastDue = due;
    return due;
}

aurora::gfx::EncoderTaskId g_finalTask = aurora::gfx::InvalidEncoderTask;
aurora::gfx::EncoderTaskId g_baseTask = aurora::gfx::InvalidEncoderTask;
aurora::gfx::EncoderTaskId g_exactTask = aurora::gfx::InvalidEncoderTask;

struct TaskPayload {
    uint32_t slot;
};

// A fullscreen triangle that loads the source texel for texel.
constexpr const char* kShader = R"(
@group(0) @binding(0) var source: texture_2d<f32>;

@vertex fn vs(@builtin(vertex_index) index: u32) -> @builtin(position) vec4f {
    let p = vec2f(f32((index << 1u) & 2u), f32(index & 2u));
    return vec4f(p * 2.0 - 1.0, 0.0, 1.0);
}

@fragment fn fs_copy(@builtin(position) at: vec4f) -> @location(0) vec4f {
    return vec4f(textureLoad(source, vec2i(at.xy), 0).rgb, 1.0);
}

@fragment fn fs_copy_alpha(@builtin(position) at: vec4f) -> @location(0) vec4f {
    return textureLoad(source, vec2i(at.xy), 0);
}
)";

struct Pipelines {
    wgpu::BindGroupLayout layout;
    wgpu::RenderPipeline copyBgra8, copyAlpha;
    bool failed = false;
};
Pipelines g_pipelines;

bool EnsurePipelines(const wgpu::Device& device) {
    if (g_pipelines.copyBgra8 || g_pipelines.failed) {
        return !g_pipelines.failed;
    }
    wgpu::ShaderSourceWGSL wgsl{};
    wgsl.code = kShader;
    wgpu::ShaderModuleDescriptor moduleDesc{};
    moduleDesc.nextInChain = &wgsl;
    moduleDesc.label = "visionOS volume copy";
    wgpu::ShaderModule module = device.CreateShaderModule(&moduleDesc);
    wgpu::BindGroupLayoutEntry entry{};
    entry.binding = 0;
    entry.visibility = wgpu::ShaderStage::Fragment;
    entry.texture.sampleType = wgpu::TextureSampleType::UnfilterableFloat;
    entry.texture.viewDimension = wgpu::TextureViewDimension::e2D;
    wgpu::BindGroupLayoutDescriptor layoutDesc{};
    layoutDesc.entryCount = 1;
    layoutDesc.entries = &entry;
    g_pipelines.layout = device.CreateBindGroupLayout(&layoutDesc);
    wgpu::PipelineLayoutDescriptor pipelineLayoutDesc{};
    pipelineLayoutDesc.bindGroupLayoutCount = 1;
    pipelineLayoutDesc.bindGroupLayouts = &g_pipelines.layout;
    wgpu::PipelineLayout layout = device.CreatePipelineLayout(&pipelineLayoutDesc);
    wgpu::ColorTargetState target{};
    target.format = wgpu::TextureFormat::BGRA8Unorm;
    wgpu::FragmentState fs{};
    fs.module = module;
    fs.entryPoint = "fs_copy";
    fs.targetCount = 1;
    fs.targets = &target;
    wgpu::RenderPipelineDescriptor desc{};
    desc.label = "visionOS volume copy";
    desc.layout = layout;
    desc.vertex.module = module;
    desc.vertex.entryPoint = "vs";
    desc.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
    desc.fragment = &fs;
    g_pipelines.copyBgra8 = device.CreateRenderPipeline(&desc);
    fs.entryPoint = "fs_copy_alpha";
    desc.label = "visionOS volume copy with alpha";
    g_pipelines.copyAlpha = device.CreateRenderPipeline(&desc);
    g_pipelines.failed = !g_pipelines.copyBgra8 || !g_pipelines.copyAlpha;
    if (g_pipelines.failed) {
        WINDOW_LOG("the copy pipeline failed\n");
    }
    return !g_pipelines.failed;
}

IOSurfaceRef CreateSurface(uint32_t width, uint32_t height) {
    CFMutableDictionaryRef properties = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    const auto set = [&](CFStringRef key, int value) {
        CFNumberRef number = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &value);
        CFDictionarySetValue(properties, key, number);
        CFRelease(number);
    };
    set(kIOSurfaceWidth, static_cast<int>(width));
    set(kIOSurfaceHeight, static_cast<int>(height));
    set(kIOSurfaceBytesPerElement, 4);
    set(kIOSurfacePixelFormat, static_cast<int>('BGRA'));
    IOSurfaceRef surface = IOSurfaceCreate(properties);
    CFRelease(properties);
    return surface;
}

void Release(Surface& surface) {
    surface.texture = nullptr;
    surface.memory = nullptr;
    if (surface.surface != nullptr) {
        CFRelease(surface.surface);
    }
    surface = Surface{};
}

// An IOSurface of this size, imported into Dawn (made, or remade on a size change).
bool EnsureSurface(const wgpu::Device& device, Surface& surface, uint32_t width, uint32_t height) {
    if (surface.surface != nullptr && surface.width == width && surface.height == height) {
        return surface.texture != nullptr;
    }
    Release(surface);
    surface.surface = CreateSurface(width, height);
    if (surface.surface == nullptr) {
        WINDOW_LOG("IOSurfaceCreate %ux%u failed\n", width, height);
        return false;
    }
    wgpu::SharedTextureMemoryIOSurfaceDescriptor ioDesc{};
    ioDesc.ioSurface = surface.surface;
    wgpu::SharedTextureMemoryDescriptor memoryDesc{};
    memoryDesc.nextInChain = &ioDesc;
    memoryDesc.label = "visionOS volume surface";
    surface.memory = device.ImportSharedTextureMemory(&memoryDesc);
    if (surface.memory) {
        wgpu::SharedTextureMemoryProperties props{};
        surface.memory.GetProperties(&props);
        wgpu::TextureDescriptor textureDesc{};
        textureDesc.label = "visionOS volume surface";
        textureDesc.usage = wgpu::TextureUsage::RenderAttachment;
        textureDesc.size = {width, height, 1};
        textureDesc.format = props.format;
        surface.texture = surface.memory.CreateTexture(&textureDesc);
        surface.format = props.format;
    }
    if (!surface.texture) {
        WINDOW_LOG("Dawn could not import a %ux%u surface\n", width, height);
        Release(surface);
        return false;
    }
    surface.width = width;
    surface.height = height;
    g_surfaceGeneration.fetch_add(1);
    return true;
}

bool BeginAccess(Surface& surface) {
    if (surface.accessOpen) {
        return true;
    }
    wgpu::SharedTextureMemoryBeginAccessDescriptor begin{};
    begin.initialized = true;
    if (surface.memory.BeginAccess(surface.texture, &begin) != wgpu::Status::Success) {
        WINDOW_LOG("BeginAccess failed\n");
        return false;
    }
    surface.accessOpen = true;
    return true;
}

// Ends Dawn's access, keeping the MTLSharedEvent and the value its writes complete at.
void EndAccess(Surface& surface, void*& event, uint64_t& value) {
    if (!surface.accessOpen) {
        return;
    }
    surface.accessOpen = false;
    wgpu::SharedTextureMemoryEndAccessState end{};
    surface.memory.EndAccess(surface.texture, &end);
    for (size_t i = 0; i < end.fenceCount && i < end.signaledValueCount; ++i) {
        wgpu::SharedFenceMTLSharedEventExportInfo metal{};
        wgpu::SharedFenceExportInfo info{};
        info.nextInChain = &metal;
        end.fences[i].ExportInfo(&info);
        if (info.type == wgpu::SharedFenceType::MTLSharedEvent && metal.sharedEvent != nullptr) {
            event = metal.sharedEvent;
            value = std::max(value, end.signaledValues[i]);
        }
    }
}

void Draw(const wgpu::Device& device, wgpu::CommandEncoder& encoder, const wgpu::TextureView& source, Surface& target,
          bool alpha = false) {
    wgpu::BindGroupEntry entry{};
    entry.binding = 0;
    entry.textureView = source;
    wgpu::BindGroupDescriptor groupDesc{};
    groupDesc.layout = g_pipelines.layout;
    groupDesc.entryCount = 1;
    groupDesc.entries = &entry;
    wgpu::BindGroup group = device.CreateBindGroup(&groupDesc);
    wgpu::RenderPassColorAttachment colour{};
    colour.view = target.texture.CreateView();
    colour.loadOp = wgpu::LoadOp::Clear;
    colour.storeOp = wgpu::StoreOp::Store;
    colour.clearValue = {0, 0, 0, alpha ? 0.0 : 1.0};
    wgpu::RenderPassDescriptor passDesc{};
    passDesc.label = "visionOS volume copy";
    passDesc.colorAttachmentCount = 1;
    passDesc.colorAttachments = &colour;
    wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&passDesc);
    pass.SetPipeline(alpha ? g_pipelines.copyAlpha : g_pipelines.copyBgra8);
    pass.SetBindGroup(0, group);
    pass.Draw(3);
    pass.End();
}

// Render worker: the scene before the 2D into the slot.
void BaseTask(const aurora::gfx::EncoderTaskContext& ctx, const wgpu::CommandEncoder& cmd, const void* payload,
              size_t, void*) {
    Slot& slot = g_slots[static_cast<const TaskPayload*>(payload)->slot];
    wgpu::CommandEncoder encoder = cmd;
    slot.hasBase = false;
    if (!EnsurePipelines(ctx.device) || !slot.pendingBase ||
        !EnsureSurface(ctx.device, slot.base, slot.baseWidth, slot.baseHeight) || !BeginAccess(slot.base)) {
        return;
    }
    Draw(ctx.device, encoder, slot.pendingBase, slot.base);
    slot.hasBase = true;
}

// Render worker: the scene's picture as the 2D began into the slot.
void ExactTask(const aurora::gfx::EncoderTaskContext& ctx, const wgpu::CommandEncoder& cmd, const void* payload,
               size_t, void*) {
    Slot& slot = g_slots[static_cast<const TaskPayload*>(payload)->slot];
    wgpu::CommandEncoder encoder = cmd;
    slot.hasExact = false;
    if (!EnsurePipelines(ctx.device) || !slot.pendingExact ||
        !EnsureSurface(ctx.device, slot.exact, slot.exactWidth, slot.exactHeight) || !BeginAccess(slot.exact)) {
        return;
    }
    Draw(ctx.device, encoder, slot.pendingExact, slot.exact);
    slot.hasExact = true;
}

// Render worker: the finished frame into the slot.
void FinalTask(const aurora::gfx::EncoderTaskContext& ctx, const wgpu::CommandEncoder& cmd, const void* payload,
               size_t, void*) {
    Slot& slot = g_slots[static_cast<const TaskPayload*>(payload)->slot];
    wgpu::CommandEncoder encoder = cmd;
    slot.hasFinal = false;
    if (!EnsurePipelines(ctx.device) || !slot.pendingFinal ||
        !EnsureSurface(ctx.device, slot.final, slot.finalWidth, slot.finalHeight) || !BeginAccess(slot.final)) {
        return;
    }
    Draw(ctx.device, encoder, slot.pendingFinal, slot.final);
    slot.hasFinal = true;
    // The settings overlay the frame before rendered (aurora's end of frame, after this frame's
    // tasks: hence the frame before's), with its alpha.
    slot.hasUi = false;
    wgpu::TextureView ui;
    uint32_t uiWidth = 0, uiHeight = 0;
    if (g_uiRendered.load(std::memory_order_relaxed) && aurora::gfx::overlay_capture(ui, uiWidth, uiHeight) &&
        EnsureSurface(ctx.device, slot.ui, uiWidth, uiHeight) && BeginAccess(slot.ui)) {
        Draw(ctx.device, encoder, ui, slot.ui, true);
        slot.hasUi = true;
    } else if (g_uiRelease.load(std::memory_order_relaxed) && slot.ui.surface != nullptr && !slot.ui.accessOpen) {
        // The settings closed a while ago: the slot's ui surface goes (the app drops its texture
        // over it at the generation's change).
        Release(slot.ui);
        g_surfaceGeneration.fetch_add(1);
    }
}

// Render worker, after the frame's command buffer went to the GPU: end Dawn's access and hand
// the slot to the app.
void FinalSubmitted(const aurora::gfx::EncoderTaskCompletionContext&, const void* payload, size_t, void*) {
    const uint32_t index = static_cast<const TaskPayload*>(payload)->slot;
    Slot& slot = g_slots[index];
    void* event = nullptr;
    uint64_t value = 0;
    EndAccess(slot.final, event, value);
    EndAccess(slot.base, event, value);
    EndAccess(slot.exact, event, value);
    EndAccess(slot.ui, event, value);
    slot.pendingFinal = nullptr;
    slot.pendingBase = nullptr;
    slot.pendingExact = nullptr;
    std::lock_guard lock(g_mutex);
    if (!slot.hasFinal) {
        slot.state = SlotState::Free;
        return;
    }
    slot.event = event;
    slot.value = value;
    slot.serial = g_nextSerial++;
    slot.ready = Seconds();
    slot.due = ScheduleFrame(slot.ready);
    // At most kMaxWaiting frames wait for their time: the oldest goes back unseen.
    size_t waiting = 0;
    int oldest = -1;
    for (size_t i = 0; i < kSlots; ++i) {
        if (g_slots[i].state != SlotState::Ready) continue;
        ++waiting;
        if (oldest < 0 || g_slots[i].serial < g_slots[oldest].serial) oldest = static_cast<int>(i);
    }
    if (waiting >= kMaxWaiting && oldest >= 0) {
        g_slots[oldest].state = SlotState::Free;
        g_stats.replaced++;
    }
    g_stats.published++;
    slot.state = SlotState::Ready;
}

void RegisterTasks() {
    if (g_finalTask != aurora::gfx::InvalidEncoderTask) {
        return;
    }
    aurora::gfx::EncoderTaskDescriptor final{};
    final.label = "visionOS volume frame";
    final.callback = &FinalTask;
    final.afterSubmit = &FinalSubmitted;
    g_finalTask = aurora::gfx::register_encoder_task_type(final);
    aurora::gfx::EncoderTaskDescriptor base{};
    base.label = "visionOS volume base";
    base.callback = &BaseTask;
    g_baseTask = aurora::gfx::register_encoder_task_type(base);
    aurora::gfx::EncoderTaskDescriptor exact{};
    exact.label = "visionOS volume exact base";
    exact.callback = &ExactTask;
    g_exactTask = aurora::gfx::register_encoder_task_type(exact);
}

// A free slot for this frame, or -1 (the app holds them all: skip this frame).
int ClaimSlot() {
    if (g_frameSlot >= 0) {
        return g_frameSlot;
    }
    std::lock_guard lock(g_mutex);
    for (size_t i = 0; i < kSlots; ++i) {
        if (g_slots[i].state == SlotState::Free) {
            g_slots[i].state = SlotState::Writing;
            g_frameSlot = static_cast<int>(i);
            return g_frameSlot;
        }
    }
    g_stats.noSlot++;
    return -1;
}

}  // namespace
}  // namespace lmvr::window

using namespace lmvr::window;

extern "C" {

// --- Game -> host (lmvr_game_hooks.h).

bool lmvr_window_enabled(void) {
    return g_enabled.load(std::memory_order_relaxed);
}

void lmvr_window_begin_frame(void) {
    if (!lmvr_window_enabled()) {
        return;
    }
    static bool started = false;
    if (!started) {
        started = true;
        // Nobody sees the offscreen SDL window's surface.
        aurora::gfx::set_surface_present_suppressed(true);
        RegisterTasks();
        WINDOW_LOG("the game paces to the volume and hands it each frame\n");
    }
    // The game keeps its own time (its 30 frames, paced by its retraces): it never waits for the
    // volume. Paced by the window's updates, it ran in slow motion whenever drawing the set took
    // longer than a frame (Trevor, c4432e5). The volume shows the newest finished frame; the ones it
    // had no time for are dropped (here, and in the scene mirror: aurora::mirror::Stats).
    while (dispatch_semaphore_wait(g_tick, DISPATCH_TIME_NOW) == 0) {
    }
    {
        const auto now = std::chrono::steady_clock::now();
        const uint32_t retrace = recomp::viRetraceCount();
        if (g_lastFrameStart != std::chrono::steady_clock::time_point{}) {
            const double ms = std::chrono::duration<double, std::milli>(now - g_lastFrameStart).count();
            std::lock_guard lock(g_mutex);
            g_stats.frames++;
            g_stats.frameMs += ms;
            g_stats.frameMsMax = std::max(g_stats.frameMsMax, ms);
            const uint32_t fields = retrace - g_lastRetrace;
            if (fields >= 1) g_stats.fields[std::min<uint32_t>(fields, 4) - 1]++;
        }
        g_lastFrameStart = now;
        g_lastRetrace = retrace;
    }
    g_frameSlot = -1;
    g_sceneThisFrame = false;
    g_basePushed = false;
    g_exactPushed = false;
    g_severalFrames = false;
    g_gameFrame.store((g_gameFrame.load() + 1) & 0xFFFFFFu);
    // The Sense controllers, if any, as a gamepad (no controller tracking outside a Full Space).
    sense_pad::update_sdl_gamepad();
}

bool lmvr_window_scene_seen(void) {
    return g_sceneThisFrame;
}

uint32_t lmvr_window_game_frame(void) {
    return g_gameFrame.load(std::memory_order_relaxed);
}

void lmvr_window_scene_drawn(const lmvr_window_scene* scene) {
    if (!lmvr_window_enabled() || scene == nullptr) {
        return;
    }
    // Only the first scene of a window frame: the game fast-forwarding a cutscene runs several guest
    // frames in one, and the mirror records the first (volume.cpp's sceneBegin); each snapshot would
    // be another full-size texture.
    if (g_sceneThisFrame) {
        g_severalFrames = true;  // the final will be a later guest frame's than any retake
        return;
    }
    const uint32_t frame = g_gameFrame.load();
    {
        std::lock_guard lock(g_mutex);
        FrameScene& entry = g_scenes[frame % g_scenes.size()];
        entry.gameFrame = frame;
        entry.scene = *scene;
    }
    g_sceneThisFrame = true;
    // Test runs: LMVR_TEST_NO_SCENE_SNAPSHOT=1 takes no picture of the scene before the 2D.
    static const bool noSnapshot = std::getenv("LMVR_TEST_NO_SCENE_SNAPSHOT") != nullptr;
    if (noSnapshot) {
        return;
    }
    const int index = ClaimSlot();
    if (index < 0) {
        return;
    }
    Slot& slot = g_slots[index];
    slot.scene = *scene;
    aurora::gfx::ResolvedTargets targets;
    if (!aurora::gfx::resolve_pass({.color = true}, targets) || !targets.color) {
        return;
    }
    slot.pendingBase = targets.color;
    slot.baseWidth = targets.width;
    slot.baseHeight = targets.height;
    const TaskPayload payload{static_cast<uint32_t>(index)};
    g_basePushed = aurora::gfx::push_encoder_task(g_baseTask, &payload, sizeof(payload));
}

void lmvr_window_overlay_begins(void) {
    // Once a frame, after its scene's own snapshot (lmvr_window_scene_drawn), into the same slot: a
    // second copy into the base surface, of the picture as the game's 2D will be drawn over it.
    if (!lmvr_window_enabled() || !g_sceneThisFrame || g_exactPushed || g_frameSlot < 0 || !g_basePushed) {
        return;
    }
    Slot& slot = g_slots[g_frameSlot];
    aurora::gfx::ResolvedTargets targets;
    if (!aurora::gfx::resolve_pass({.color = true}, targets) || !targets.color || targets.width != slot.baseWidth ||
        targets.height != slot.baseHeight) {
        return;
    }
    slot.pendingExact = targets.color;
    slot.exactWidth = targets.width;
    slot.exactHeight = targets.height;
    const TaskPayload payload{static_cast<uint32_t>(g_frameSlot)};
    g_exactPushed = aurora::gfx::push_encoder_task(g_exactTask, &payload, sizeof(payload));
}

void lmvr_window_frame_done(bool flat_2d, bool capture) {
    if (!lmvr_window_enabled()) {
        return;
    }
    // A 3D frame whose scene found no slot free: its finished frame would go without it (the panel
    // would show the whole picture over the set). Skipped.
    if (g_sceneThisFrame && g_frameSlot < 0) {
        return;
    }
    const int index = ClaimSlot();
    if (index < 0) {
        return;
    }
    Slot& slot = g_slots[index];
    slot.hasScene = g_sceneThisFrame;
    slot.exactValid = g_exactPushed && !g_severalFrames;
    slot.flat2d = flat_2d;
    slot.capture = capture;
    if (!g_sceneThisFrame) {
        slot.scene = lmvr_window_scene{};
    }
    if (!g_basePushed) {
        slot.hasBase = false;
        slot.pendingBase = nullptr;
    }
    aurora::gfx::ResolvedTargets targets;
    if (aurora::gfx::resolve_pass({.color = true}, targets) && targets.color) {
        slot.pendingFinal = targets.color;
        slot.finalWidth = targets.width;
        slot.finalHeight = targets.height;
    } else {
        slot.pendingFinal = nullptr;
    }
    slot.gameFrame = g_gameFrame.load();
    const TaskPayload payload{static_cast<uint32_t>(index)};
    if (!aurora::gfx::push_encoder_task(g_finalTask, &payload, sizeof(payload))) {
        std::lock_guard lock(g_mutex);
        slot.state = SlotState::Free;
    }
    g_frameSlot = -1;
}

void lmvr_window_settings(bool open) {
    // The frame now ending renders the overlay if open; the next frame's task copies it.
    aurora::gfx::set_overlay_capture(open);
    g_uiRendered.store(g_settingsOpen && open, std::memory_order_relaxed);
    g_settingsOpen = open;
    g_settingsClosedFrames = open ? 0 : std::min(g_settingsClosedFrames + 1, 1000);
    g_uiRelease.store(g_settingsClosedFrames > 60, std::memory_order_relaxed);
}

void lmvr_window_volume(float size[3], int* viewpoint) {
    for (int k = 0; k < 3; ++k) {
        size[k] = g_volume[k].load(std::memory_order_relaxed);
    }
    if (viewpoint != nullptr) {
        *viewpoint = g_viewpoint.load(std::memory_order_relaxed);
    }
}

// --- App -> game (lmvr_visionos.h).

void lmvr_visionos_set_window_mode(bool enabled) {
    g_enabled.store(enabled);
}

void lmvr_visionos_window_tick(void) {
    dispatch_semaphore_signal(g_tick);
}

void lmvr_visionos_set_volume(float width, float height, float depth, int viewpoint) {
    const float size[3] = {width, height, depth};
    for (int k = 0; k < 3; ++k) {
        if (std::isfinite(size[k]) && size[k] > 0.05f) {
            g_volume[k].store(size[k], std::memory_order_relaxed);
        }
    }
    g_viewpoint.store(((viewpoint % 4) + 4) % 4, std::memory_order_relaxed);
}

void lmvr_visionos_volume_stats(lmvr_visionos_volume_stats_t* out) {
    if (out == nullptr) {
        return;
    }
    *out = lmvr_visionos_volume_stats_t{};
    {
        std::lock_guard lock(g_mutex);
        out->game_frames = g_stats.frames;
        out->game_ms_avg = g_stats.frames > 0 ? g_stats.frameMs / double(g_stats.frames) : 0;
        out->game_ms_max = g_stats.frameMsMax;
        out->frames_published = g_stats.published;
        out->frames_replaced = g_stats.replaced;
        out->frames_no_slot = g_stats.noSlot;
        for (int k = 0; k < 4; ++k) out->game_fields[k] = g_stats.fields[k];
        out->pace_late = g_stats.late;
        out->pace_trims = g_stats.trims;
        out->pace_passed = g_stats.passed;
        out->pace_wait_ms_avg = g_stats.taken > 0 ? g_stats.waitMs / double(g_stats.taken) : 0;
        out->pace_beat_ms = g_pace.beat * 1000.0;
        g_stats = Stats{};
    }
    aurora::mirror::Stats mirror;
    aurora::mirror::take_stats(mirror);
    out->mirror_recorded = mirror.recorded;
    out->mirror_skipped = mirror.skipped;
    out->mirror_dropped = mirror.dropped;
    out->mirror_record_ms_avg = mirror.recorded > 0 ? mirror.recordMs / double(mirror.recorded) : 0;
    out->mirror_finish_ms_avg = mirror.finished > 0 ? mirror.finishMs / double(mirror.finished) : 0;
    out->mirror_cpu_ms_avg = mirror.recorded > 0 ? mirror.sceneCpuMs / double(mirror.recorded) : 0;
}

bool lmvr_visionos_window_acquire(lmvr_visionos_window_frame* frame) {
    if (frame == nullptr) {
        return false;
    }
    std::lock_guard lock(g_mutex);
    // The newest waiting frame whose time has come; older waiting ones go back unseen.
    const double now = Seconds();
    int pick = -1;
    for (size_t i = 0; i < kSlots; ++i) {
        const Slot& candidate = g_slots[i];
        if (candidate.state != SlotState::Ready || candidate.serial == g_handed) continue;
        if (candidate.due > now + kTolerance) continue;
        if (pick < 0 || candidate.serial > g_slots[pick].serial) pick = static_cast<int>(i);
    }
    if (pick < 0) {
        return false;
    }
    for (Slot& older : g_slots) {
        if (older.state == SlotState::Ready && older.serial < g_slots[pick].serial) {
            older.state = SlotState::Free;
            g_stats.passed++;
        }
    }
    Slot& slot = g_slots[pick];
    g_stats.taken++;
    g_stats.waitMs += (now - slot.ready) * 1000.0;
    slot.state = SlotState::Reading;
    g_handed = slot.serial;
    *frame = lmvr_visionos_window_frame{};
    frame->serial = slot.serial;
    frame->generation = g_surfaceGeneration.load();
    frame->final = slot.final.surface;
    frame->base = slot.hasScene && slot.hasBase ? slot.base.surface : nullptr;
    frame->scene = frame->base;
    frame->event = slot.event;
    frame->value = slot.value;
    frame->tan_half_x = slot.scene.tan_half_x;
    frame->tan_half_y = slot.scene.tan_half_y;
    frame->focus = slot.scene.focus;
    frame->game_frame = slot.gameFrame;
    frame->cinematic = slot.hasScene && slot.scene.cinematic;
    frame->flat_2d = slot.flat2d;
    frame->capture = slot.capture;
    frame->exact = frame->base != nullptr && slot.hasExact && slot.exactValid ? slot.exact.surface : nullptr;
    frame->ui = slot.hasUi ? slot.ui.surface : nullptr;
    frame->capture_arrow = slot.hasScene && slot.scene.capture;
    for (int k = 0; k < 3; ++k) {
        frame->capture_luigi[k] = slot.scene.capture_luigi[k];
        frame->capture_ghost[k] = slot.scene.capture_ghost[k];
    }
    frame->set_scale = slot.scene.scale;
    return true;
}

void lmvr_visionos_window_release(uint64_t serial) {
    std::lock_guard lock(g_mutex);
    for (Slot& slot : g_slots) {
        if (slot.state == SlotState::Reading && slot.serial == serial) {
            slot.state = SlotState::Free;
        }
    }
}

void lmvr_visionos_set_mirror_enabled(bool enabled) {
    aurora::mirror::set_enabled(enabled);
}

void lmvr_visionos_set_mirror_front_allowance(float widths, float min_view_tangent) {
    aurora::mirror::set_front_allowance(widths, min_view_tangent);
}

static_assert(sizeof(lmvr_visionos_mirror_vertex) == sizeof(aurora::mirror::Vertex));
static_assert(sizeof(lmvr_visionos_mirror_part) == sizeof(aurora::mirror::Part));
static_assert(offsetof(lmvr_visionos_mirror_part, wrap_s) == offsetof(aurora::mirror::Part, wrapS));
static_assert(LMVR_VISIONOS_MIRROR_WORLD == aurora::mirror::PartWorld);

bool lmvr_visionos_mirror_acquire(lmvr_visionos_mirror_frame* frame, uint32_t up_to) {
    static std::vector<lmvr_visionos_mirror_texture> textures;
    aurora::mirror::Frame source;
    const bool ok = aurora::mirror::acquire(source, up_to);
    textures.clear();
    for (uint32_t i = 0; i < source.textureCount; ++i) {
        const auto& t = source.textures[i];
        textures.push_back({t.id, t.rgba, t.width, t.height, t.mipmapped, t.cutout});
    }
    *frame = {};
    frame->serial = source.serial;
    frame->game_frame = source.tag;
    frame->scene = source.scene;
    frame->vertices = reinterpret_cast<const lmvr_visionos_mirror_vertex*>(source.vertices);
    frame->vertex_count = source.vertexCount;
    frame->indices = source.indices;
    frame->index_count = source.indexCount;
    frame->parts = reinterpret_cast<const lmvr_visionos_mirror_part*>(source.parts);
    frame->part_count = source.partCount;
    std::copy(source.boundsMin, source.boundsMin + 3, frame->bounds_min);
    std::copy(source.boundsMax, source.boundsMax + 3, frame->bounds_max);
    frame->tan_half_x = source.tanHalfX;
    frame->tan_half_y = source.tanHalfY;
    frame->focus = source.focus;
    frame->plane = source.plane;
    frame->textures = textures.data();
    frame->texture_count = static_cast<uint32_t>(textures.size());
    frame->removed = source.removed;
    frame->removed_count = source.removedCount;
    // Its game frame's placement in the volume (the set).
    if (ok) {
        std::lock_guard lock(g_mutex);
        const FrameScene& entry = g_scenes[source.tag % g_scenes.size()];
        if (entry.gameFrame == source.tag && entry.scene.placed) {
            std::copy(entry.scene.volume_from_view, entry.scene.volume_from_view + 16, frame->volume_from_view);
            frame->placed = true;
            frame->set_serial = entry.scene.set_serial;
            frame->fit_serial = entry.scene.fit_serial;
            frame->cinematic = entry.scene.cinematic;
            frame->set_scale = entry.scene.scale;
        }
    }
    return ok;
}

}  // extern "C"
