#pragma once

// The game's side of the window mode: each finished frame goes to the app (GameWindowView.swift)
// as images in IOSurfaces, and the window paces the game. Engine-independent; generalised from
// the TPVR port's (src/dusk/visionos/visionos_window.cpp, which does the copies with Dawn).
//
// Per frame (begin_frame and claim on the game's thread; the slot's writes, publish or drop may
// happen later on the render thread, as TPVR's did from its render worker):
//   begin_frame()           top of the frame: waits for the window's tick (at most 100 ms),
//                           returns the frame's number (give it to scene_mirror::begin_frame too)
//   claim()                 this frame's slot, or nullptr (the app holds them all: skip the frame)
//   surface(slot, image..)  an image's IOSurface, (re)made at its size; write it on the GPU
//                           (window_frames::metal below, or import it into your API) or the CPU
//   set_camera(slot, ...)   the scene camera, when a 3D scene was drawn
//   publish(slot, ...)      once the writes are submitted, with the MTLSharedEvent they signal
//
// The images, and where in the frame to take them:
//   Base      the 3D world, before the screen effects (bloom, mist, fades, letterbox bars): with
//             the scene mirror, where its frame ends (scene_mirror::end_frame)
//   Scene     the picture just before the HUD/2D pass, effects included,
//   Distance  and its depth buffer as distances (copy_distance)
//   Final     the finished frame, HUD and all (the only one a frame without 3D needs)
//   Ui        an overlay UI the engine draws outside the frame (premultiplied), if any
// The window shows the mirror's 3D, the screen effects as a layer (Scene against Base), and the HUD
// (Final against Scene) flat on its glass.

#include "lmvr_visionos.h"

#include <IOSurface/IOSurfaceRef.h>

#include <cstdint>

namespace window_frames {

enum class Image { Base, Scene, Distance, Final, Ui };

struct Slot;

// Window mode for this run (the bridge sets it before the game starts).
void set_enabled(bool enabled);
bool enabled();

// The window's update ticked (lmvr_visionos_window_tick).
void tick();

// Top of the game's frame: waits for the window's next update (at most 100 ms, so a hidden window
// doesn't stop the game dead), then forgets any that piled up, so the game never races to catch up.
// Returns the frame's number (24 bits). Without window mode: returns at once.
uint32_t begin_frame();
// The number begin_frame() last returned.
uint32_t game_frame();

// This frame's slot (the same one each call within a frame), or nullptr. It carries this frame's
// number. Every slot claimed must be published or dropped exactly once, from any thread.
Slot* claim();

// The slot's IOSurface for `image`, made or remade at this size: BGRA8 (Distance: RGBA16F, depth
// along the view axis in .r, game units: not the distance to each pixel, which the window's layout
// doesn't expect). Asking for it marks the image as written this frame.
IOSurfaceRef surface(Slot* slot, Image image, uint32_t width, uint32_t height);
// Unmarks an image whose write failed.
void forget(Slot* slot, Image image);

void set_camera(Slot* slot, float tan_half_x, float tan_half_y, float focus);

// Hands the slot to the app: its reads wait for `event` (an MTLSharedEvent, or nullptr when the
// writes were on the CPU and are done) to reach `value`. A slot without Final is given back instead.
void publish(Slot* slot, void* event, uint64_t value);
// Gives the slot back unpublished (the frame was skipped).
void drop(Slot* slot);

// The bridge's.
bool acquire(lmvr_visionos_window_frame* frame);
void release(uint64_t serial);

}  // namespace window_frames

#ifdef __OBJC__
#import <Metal/Metal.h>

// Copies on the GPU, for an engine with Metal textures in hand: a Metal renderer, MoltenVK
// (vkExportMetalObjectsEXT), Dawn (its Metal texture, or SharedTextureMemory as TPVR does), ANGLE.
// Encode them into the engine's own command buffer after it drew what they copy.
namespace window_frames::metal {

// How the depth buffer stores depth: 0 at the near plane and 1 at the far (Metal, D3D), reversed
// (1 near, 0 far: CompositorServices', aurora's), or OpenGL's (-1..1 in clip space, 0..1 stored).
enum class Depth { ZeroToOne, Reversed, OpenGL };

// `source` (any colour format, MSAA too) into the slot's image, texel for texel: the image stays
// BGRA8 with the same encoding (an sRGB or float source is written sRGB-encoded).
bool copy_colour(id<MTLCommandBuffer> commands, id<MTLTexture> source, Slot* slot, Image image);
// A depth texture into the slot's Distance image, as depth along the view axis (game units).
// `far_plane` <= 0 or infinite: an infinite projection.
bool copy_distance(id<MTLCommandBuffer> commands, id<MTLTexture> depth, Slot* slot, float near_plane,
                   float far_plane, Depth convention);
// Signals the helpers' event at the end of `commands` and publishes the slot with it. Call before
// committing `commands`.
void publish(id<MTLCommandBuffer> commands, Slot* slot);

}  // namespace window_frames::metal
#endif
