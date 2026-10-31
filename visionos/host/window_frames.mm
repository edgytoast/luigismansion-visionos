// The game's side of the window mode: see window_frames.h. Objective-C++ with ARC.

#include "window_frames.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>

#include <array>
#include <atomic>
#include <map>
#include <mutex>
#include <string>

namespace window_frames {

constexpr size_t kImages = 5;

enum class SlotState { Free, Writing, Ready, Reading };

struct Surface {
    IOSurfaceRef surface = nullptr;
    uint32_t width = 0, height = 0;
    // Metal views of it (window_frames::metal): BGRA8, BGRA8 sRGB, RGBA16F.
    id<MTLTexture> views[3] = {nil, nil, nil};
};

// A frame on its way to the app. Writing: the game's; Ready and Reading: the app's.
struct Slot {
    SlotState state = SlotState::Free;
    std::array<Surface, kImages> images;
    std::array<bool, kImages> has{};
    uint64_t serial = 0;
    uint32_t gameFrame = 0;
    float tanX = 0, tanY = 0, focus = 0;
    void* event = nullptr;
    uint64_t value = 0;
};

namespace {

constexpr size_t kSlots = 4;

std::atomic_bool g_enabled{false};
dispatch_semaphore_t g_tick = dispatch_semaphore_create(0);
// Bumped whenever any slot's surfaces are remade: the app drops its textures over them.
std::atomic<uint64_t> g_generation{1};
std::atomic<uint32_t> g_gameFrame{0};

std::mutex g_mutex;  // slot states, serials, g_latest, g_handed
std::array<Slot, kSlots> g_slots;
uint64_t g_nextSerial = 1;
int g_latest = -1;
uint64_t g_handed = 0;
// The game thread's: this frame's slot (-1 none claimed yet, -2 none free: the frame is skipped).
int g_frameSlot = -1;

size_t Index(Image image) {
    return static_cast<size_t>(image);
}

IOSurfaceRef CreateSurface(uint32_t width, uint32_t height, uint32_t fourcc, int bytesPerElement) {
    CFMutableDictionaryRef properties = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    const auto set = [&](CFStringRef key, int value) {
        CFNumberRef number = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &value);
        CFDictionarySetValue(properties, key, number);
        CFRelease(number);
    };
    set(kIOSurfaceWidth, static_cast<int>(width));
    set(kIOSurfaceHeight, static_cast<int>(height));
    set(kIOSurfaceBytesPerElement, bytesPerElement);
    set(kIOSurfacePixelFormat, static_cast<int>(fourcc));
    IOSurfaceRef surface = IOSurfaceCreate(properties);
    CFRelease(properties);
    return surface;
}

}  // namespace

void set_enabled(bool enabled) {
    g_enabled.store(enabled);
}

bool enabled() {
    return g_enabled.load(std::memory_order_relaxed);
}

void tick() {
    dispatch_semaphore_signal(g_tick);
}

uint32_t begin_frame() {
    const uint32_t frame = (g_gameFrame.load() + 1) & 0xFFFFFFu;
    g_gameFrame.store(frame);
    if (!enabled()) {
        return frame;
    }
    // Last frame's slot is publish()'s or drop()'s now, on whichever thread finishes it.
    g_frameSlot = -1;
    dispatch_semaphore_wait(g_tick, dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC));
    while (dispatch_semaphore_wait(g_tick, DISPATCH_TIME_NOW) == 0) {
    }
    return frame;
}

uint32_t game_frame() {
    return g_gameFrame.load();
}

Slot* claim() {
    if (!enabled() || g_frameSlot == -2) {
        return nullptr;
    }
    if (g_frameSlot >= 0) {
        return &g_slots[static_cast<size_t>(g_frameSlot)];
    }
    std::lock_guard lock(g_mutex);
    for (size_t i = 0; i < kSlots; ++i) {
        Slot& slot = g_slots[i];
        if (slot.state == SlotState::Free) {
            slot.state = SlotState::Writing;
            slot.has.fill(false);  // every claim: a slot reused for a 2D frame must not show its old scene
            slot.tanX = slot.tanY = slot.focus = 0;
            slot.gameFrame = g_gameFrame.load();  // the frame that claimed it, wherever it's published
            g_frameSlot = static_cast<int>(i);
            return &slot;
        }
    }
    g_frameSlot = -2;  // the app holds them all: this frame is skipped
    return nullptr;
}

IOSurfaceRef surface(Slot* slot, Image image, uint32_t width, uint32_t height) {
    if (slot == nullptr || width == 0 || height == 0) {
        return nullptr;
    }
    Surface& s = slot->images[Index(image)];
    if (s.surface == nullptr || s.width != width || s.height != height) {
        if (s.surface != nullptr) {
            CFRelease(s.surface);  // the app's textures over it keep it alive until they go
        }
        s = Surface{};
        const bool distance = image == Image::Distance;
        s.surface = distance ? CreateSurface(width, height, 'RGhA', 8) : CreateSurface(width, height, 'BGRA', 4);
        if (s.surface == nullptr) {
            NSLog(@"[LMVR] window: IOSurfaceCreate %ux%u failed", width, height);
            slot->has[Index(image)] = false;
            return nullptr;
        }
        s.width = width;
        s.height = height;
        g_generation.fetch_add(1);
    }
    slot->has[Index(image)] = true;
    return s.surface;
}

void forget(Slot* slot, Image image) {
    if (slot != nullptr) {
        slot->has[Index(image)] = false;
    }
}

void set_camera(Slot* slot, float tan_half_x, float tan_half_y, float focus) {
    if (slot != nullptr) {
        slot->tanX = tan_half_x;
        slot->tanY = tan_half_y;
        slot->focus = focus;
    }
}

void publish(Slot* slot, void* event, uint64_t value) {
    if (slot == nullptr) {
        return;
    }
    std::lock_guard lock(g_mutex);
    if (slot->state != SlotState::Writing) {
        return;  // dropped already
    }
    if (!slot->has[Index(Image::Final)]) {
        slot->state = SlotState::Free;
        return;
    }
    slot->event = event;
    slot->value = value;
    slot->serial = g_nextSerial++;
    const int index = static_cast<int>(slot - g_slots.data());
    if (g_latest >= 0 && g_latest != index && g_slots[static_cast<size_t>(g_latest)].state == SlotState::Ready) {
        g_slots[static_cast<size_t>(g_latest)].state = SlotState::Free;  // never shown: a newer one replaces it
    }
    slot->state = SlotState::Ready;
    g_latest = index;
}

void drop(Slot* slot) {
    if (slot == nullptr) {
        return;
    }
    std::lock_guard lock(g_mutex);
    if (slot->state == SlotState::Writing) {
        slot->state = SlotState::Free;
    }
}

bool acquire(lmvr_visionos_window_frame* frame) {
    if (frame == nullptr) {
        return false;
    }
    std::lock_guard lock(g_mutex);
    if (g_latest < 0) {
        return false;
    }
    Slot& slot = g_slots[static_cast<size_t>(g_latest)];
    if (slot.state != SlotState::Ready || slot.serial == g_handed) {
        return false;
    }
    slot.state = SlotState::Reading;
    g_handed = slot.serial;
    const auto image = [&](Image which) {
        return slot.has[Index(which)] ? static_cast<void*>(slot.images[Index(which)].surface) : nullptr;
    };
    const bool scene = image(Image::Scene) != nullptr && image(Image::Distance) != nullptr && slot.tanX > 0;
    *frame = lmvr_visionos_window_frame{};
    frame->serial = slot.serial;
    frame->generation = g_generation.load();
    frame->scene = scene ? image(Image::Scene) : nullptr;
    frame->distance = scene ? image(Image::Distance) : nullptr;
    frame->final = image(Image::Final);
    frame->ui = image(Image::Ui);
    frame->base = scene ? image(Image::Base) : nullptr;
    frame->event = slot.event;
    frame->value = slot.value;
    frame->game_frame = slot.gameFrame;
    frame->tan_half_x = slot.tanX;
    frame->tan_half_y = slot.tanY;
    frame->focus = slot.focus;
    return true;
}

void release(uint64_t serial) {
    std::lock_guard lock(g_mutex);
    for (Slot& slot : g_slots) {
        if (slot.state == SlotState::Reading && slot.serial == serial) {
            slot.state = SlotState::Free;
        }
    }
}

}  // namespace window_frames

// --- Metal copies.

namespace window_frames::metal {
namespace {

const char* const kShader = R"(
#include <metal_stdlib>
using namespace metal;

struct Out { float4 position [[position]]; };

vertex Out vs(uint id [[vertex_id]]) {
    const float2 p = float2(float((id << 1) & 2), float(id & 2));
    Out out;
    out.position = float4(p * 2.0 - 1.0, 0.0, 1.0);
    return out;
}

fragment float4 fs_copy(Out in [[stage_in]], texture2d<float, access::read> source [[texture(0)]]) {
    return source.read(uint2(in.position.xy));
}

fragment float4 fs_copy_ms(Out in [[stage_in]], texture2d_ms<float, access::read> source [[texture(0)]]) {
    return source.read(uint2(in.position.xy), 0);
}

struct DepthParams { float nearZ, farZ; uint convention; uint unused; };

// Depth to the distance from the camera along the view axis. 0: 0 near, 1 far; 1: reversed;
// 2: OpenGL (stored 0..1 from clip -1..1). An infinite projection when far isn't beyond near.
static float Distance(float z, constant DepthParams& p) {
    const float n = p.nearZ, f = p.farZ;
    const bool infinite = !(f > n) || isinf(f);
    if (p.convention == 1) {
        return infinite ? n / max(z, 1e-7) : n * f / max(n + z * (f - n), 1e-7);
    }
    if (p.convention == 2) {
        const float ndc = z * 2.0 - 1.0;
        return infinite ? 2.0 * n / max(1.0 - ndc, 1e-7) : 2.0 * f * n / max((f + n) - ndc * (f - n), 1e-7);
    }
    return infinite ? n / max(1.0 - z, 1e-7) : n * f / max(f - z * (f - n), 1e-7);
}

// Half floats end at 65504.
fragment float4 fs_distance(Out in [[stage_in]], depth2d<float, access::read> depth [[texture(0)]],
                            constant DepthParams& p [[buffer(0)]]) {
    const float z = depth.read(uint2(in.position.xy));
    return float4(min(Distance(z, p), 60000.0), z, 0.0, 1.0);
}

fragment float4 fs_distance_ms(Out in [[stage_in]], depth2d_ms<float, access::read> depth [[texture(0)]],
                               constant DepthParams& p [[buffer(0)]]) {
    const float z = depth.read(uint2(in.position.xy), 0);
    return float4(min(Distance(z, p), 60000.0), z, 0.0, 1.0);
}
)";

struct DepthParams {
    float nearZ, farZ;
    uint32_t convention, unused;
};

id<MTLDevice> g_device = nil;
id<MTLLibrary> g_library = nil;
std::map<std::string, id<MTLRenderPipelineState>> g_pipelines;
id<MTLSharedEvent> g_event = nil;
uint64_t g_eventValue = 0;
bool g_failed = false;

bool Ready(id<MTLDevice> device) {
    if (g_library != nil && device == g_device) {
        return true;
    }
    if (g_failed) {
        return false;
    }
    NSError* error = nil;
    g_library = [device newLibraryWithSource:[NSString stringWithUTF8String:kShader] options:nil error:&error];
    if (g_library == nil) {
        NSLog(@"[LMVR] window: the copy shaders failed: %@", error);
        g_failed = true;
        return false;
    }
    g_device = device;
    g_pipelines.clear();
    g_event = [device newSharedEvent];
    return true;
}

id<MTLRenderPipelineState> Pipeline(NSString* fragment, MTLPixelFormat format) {
    const std::string key = std::string(fragment.UTF8String) + "/" + std::to_string(static_cast<int>(format));
    if (auto found = g_pipelines.find(key); found != g_pipelines.end()) {
        return found->second;
    }
    MTLRenderPipelineDescriptor* descriptor = [MTLRenderPipelineDescriptor new];
    descriptor.label = @"Luigi's Mansion VR window copy";
    descriptor.vertexFunction = [g_library newFunctionWithName:@"vs"];
    descriptor.fragmentFunction = [g_library newFunctionWithName:fragment];
    descriptor.colorAttachments[0].pixelFormat = format;
    NSError* error = nil;
    id<MTLRenderPipelineState> pipeline = [g_device newRenderPipelineStateWithDescriptor:descriptor error:&error];
    if (pipeline == nil) {
        NSLog(@"[LMVR] window: the %@ pipeline failed: %@", fragment, error);
    }
    g_pipelines[key] = pipeline;
    return pipeline;
}

// A Metal view of the slot's IOSurface for `image`, in `format`.
id<MTLTexture> View(Slot* slot, Image image, uint32_t width, uint32_t height, MTLPixelFormat format) {
    IOSurfaceRef surface = window_frames::surface(slot, image, width, height);
    if (surface == nullptr) {
        return nil;
    }
    const int kind = format == MTLPixelFormatBGRA8Unorm ? 0 : format == MTLPixelFormatBGRA8Unorm_sRGB ? 1 : 2;
    Surface& s = slot->images[static_cast<size_t>(image)];
    if (s.views[kind] == nil) {
        MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                                                              width:width
                                                                                             height:height
                                                                                          mipmapped:NO];
        descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        descriptor.storageMode = MTLStorageModeShared;
        s.views[kind] = [g_device newTextureWithDescriptor:descriptor iosurface:surface plane:0];
    }
    if (s.views[kind] == nil) {
        window_frames::forget(slot, image);
    }
    return s.views[kind];
}

bool IsSrgbOrFloat(MTLPixelFormat format) {
    switch (format) {
    case MTLPixelFormatBGRA8Unorm_sRGB:
    case MTLPixelFormatRGBA8Unorm_sRGB:
    case MTLPixelFormatRGBA16Float:
    case MTLPixelFormatRGBA32Float:
    case MTLPixelFormatRG11B10Float:
    case MTLPixelFormatRGB9E5Float:
    case MTLPixelFormatBGR10_XR_sRGB:
    case MTLPixelFormatBGRA10_XR_sRGB:
        return true;
    default:
        return false;
    }
}

void Draw(id<MTLCommandBuffer> commands, id<MTLRenderPipelineState> pipeline, id<MTLTexture> source,
          id<MTLTexture> target, const DepthParams* params) {
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
    encoder.label = @"Luigi's Mansion VR window copy";
    [encoder setRenderPipelineState:pipeline];
    [encoder setFragmentTexture:source atIndex:0];
    if (params != nullptr) {
        [encoder setFragmentBytes:params length:sizeof(DepthParams) atIndex:0];
    }
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
}

}  // namespace

bool copy_colour(id<MTLCommandBuffer> commands, id<MTLTexture> source, Slot* slot, Image image) {
    if (commands == nil || source == nil || slot == nullptr || image == Image::Distance || !Ready(commands.device)) {
        return false;
    }
    const auto width = static_cast<uint32_t>(source.width), height = static_cast<uint32_t>(source.height);
    // A BGRA8 source is blitted (it needn't be shader-readable); anything else is drawn.
    const MTLPixelFormat format = source.pixelFormat;
    if ((format == MTLPixelFormatBGRA8Unorm || format == MTLPixelFormatBGRA8Unorm_sRGB) && source.sampleCount == 1 &&
        !source.framebufferOnly) {
        id<MTLTexture> target = View(slot, image, width, height, format);
        if (target == nil) {
            return false;
        }
        id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
        [blit copyFromTexture:source toTexture:target];
        [blit endEncoding];
        return true;
    }
    if ((source.usage & MTLTextureUsageShaderRead) == 0) {
        NSLog(@"[LMVR] window: a %lu texture without shader read can't be copied", (unsigned long)format);
        window_frames::forget(slot, image);
        return false;
    }
    const MTLPixelFormat targetFormat = IsSrgbOrFloat(format) ? MTLPixelFormatBGRA8Unorm_sRGB : MTLPixelFormatBGRA8Unorm;
    id<MTLTexture> target = View(slot, image, width, height, targetFormat);
    id<MTLRenderPipelineState> pipeline = Pipeline(source.sampleCount > 1 ? @"fs_copy_ms" : @"fs_copy", targetFormat);
    if (target == nil || pipeline == nil) {
        window_frames::forget(slot, image);
        return false;
    }
    Draw(commands, pipeline, source, target, nullptr);
    return true;
}

bool copy_distance(id<MTLCommandBuffer> commands, id<MTLTexture> depth, Slot* slot, float near_plane, float far_plane,
                   Depth convention) {
    if (commands == nil || depth == nil || slot == nullptr || !Ready(commands.device)) {
        return false;
    }
    if ((depth.usage & MTLTextureUsageShaderRead) == 0) {
        NSLog(@"[LMVR] window: the depth texture needs shader read to be copied");
        window_frames::forget(slot, Image::Distance);
        return false;
    }
    id<MTLTexture> target = View(slot, Image::Distance, static_cast<uint32_t>(depth.width),
                                 static_cast<uint32_t>(depth.height), MTLPixelFormatRGBA16Float);
    id<MTLRenderPipelineState> pipeline =
        Pipeline(depth.sampleCount > 1 ? @"fs_distance_ms" : @"fs_distance", MTLPixelFormatRGBA16Float);
    if (target == nil || pipeline == nil) {
        window_frames::forget(slot, Image::Distance);
        return false;
    }
    const DepthParams params{near_plane, far_plane > 0 ? far_plane : 0.f, static_cast<uint32_t>(convention), 0};
    Draw(commands, pipeline, depth, target, &params);
    return true;
}

void publish(id<MTLCommandBuffer> commands, Slot* slot) {
    if (slot == nullptr) {
        return;
    }
    if (commands == nil || !Ready(commands.device)) {
        window_frames::drop(slot);
        return;
    }
    [commands encodeSignalEvent:g_event value:++g_eventValue];
    window_frames::publish(slot, (__bridge void*)g_event, g_eventValue);
}

}  // namespace window_frames::metal
