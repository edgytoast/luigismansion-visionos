#pragma once

// The scene mirror's frame builder, for the window mode (GameWindowView.swift, MirrorScene.swift).
//
// The window can't show the game's finished picture with real depth (a picture smears from the
// side), so it draws the game's 3D scene again itself: RealityKit renders the same triangles from
// the viewer's real eyes, with correct depth from any angle. This file is the engine-independent
// half of that, generalised from the TPVR port's (extern/aurora/lib/gx/mirror.cpp): the engine's
// capture hands over each 3D draw already decoded (view-space triangles, one texture, a per-vertex
// colour formula, a material), and this
//   - clips them as the GPU does (near plane; the view's sides with a margin for leaning),
//   - nudges coplanar draws apart in the game's order (RealityKit can't settle depth ties the way
//     a LEQUAL test does), lifts decals a capped amount, drops runaway vertices,
//   - merges solid draws by material and keeps blended runs in the game's order,
//   - on a worker thread: pushes the sky past the level, eases what is nearer than the window's
//     glass into the room the window has in front of it (subdividing first), lifts decals,
//     computes bounds, orders the parts,
//   - keeps the last few finished frames for the window to pair with its own frame (by the game's
//     frame number), and sends each texture once.
//
// What stays the engine's (the capture; see port-build/references/windowed-mode.md):
//   - which draws are the 3D world: between the top of the frame and the end of the 3D pass,
//     perspective, into the main colour target, writing colour, with the scene's projection;
//   - each vertex in the camera's view space (right-handed: camera at the origin looking down -z,
//     +y up), in the game's own units (set_units_per_metre);
//   - the material reduced to "one texture T, colour = T.rgb * mul + add, alpha = T.a * mul.a +
//     add.a, in the game's gamma space", per vertex (lighting and vertex colours folded into mul
//     and add), and the blend/alpha-test/cull/depth state;
//   - each texture as RGBA8, top row first, its id from its decoded pixels (texture_id), never an
//     engine's object id or a decode counter (TPVR's game re-made its texture objects every frame:
//     thousands of "new" textures a minute; a texture decoded again under a new id set the level's
//     material list again, and the window flashed for minutes).
//
// Threads: begin_frame, set_projection, add_draw, add_texture, remove_texture and end_frame on one
// thread (the engine's render or command thread), in that order per frame. acquire on the app's
// main thread (through the bridge). set_enabled from any.

#include "lmvr_visionos.h"

#include <cstdint>

namespace scene_mirror {

// A vertex as the engine hands it over (full floats; stored as halves).
struct Vertex {
    float position[3];  // view space, game units
    float uv[2];        // the part's texture coordinate, v down from the top row
    float mul[4];       // colour = T.rgb * mul.rgb + add.rgb, alpha = T.a * mul.a + add.a,
    float add[4];       // in the game's gamma space; no texture (0): T is white
};

// A draw's material: the bridge's part without its index range. Kinds and flags are
// LMVR_VISIONOS_MIRROR_PART_* and LMVR_VISIONOS_MIRROR_* (the bridge header).
struct Material {
    uint64_t texture = 0;  // 0: none (white)
    uint32_t kind = LMVR_VISIONOS_MIRROR_PART_OPAQUE;
    uint32_t flags = LMVR_VISIONOS_MIRROR_DEPTH_TEST | LMVR_VISIONOS_MIRROR_DEPTH_WRITE;
    float cutoff = 0.5f;  // cut-out: alpha below it is discarded
    // Blend, premultiplied: colour = rgb * (colour_base + colour_alpha * a);
    // opacity = opacity_base + opacity_alpha * a + opacity_luma * luma(rgb).
    // Normal alpha blending: 0, 1, 0, 1, 0. Additive (one, one): 1, 0, 0, 0, 0.
    float colour_base = 0, colour_alpha = 1, opacity_base = 0, opacity_alpha = 1, opacity_luma = 0;
    uint8_t wrap_s = LMVR_VISIONOS_MIRROR_WRAP_REPEAT, wrap_t = LMVR_VISIONOS_MIRROR_WRAP_REPEAT;
};

struct Draw {
    const Vertex* vertices = nullptr;
    uint32_t vertex_count = 0;
    // A triangle list into `vertices`; null: the vertices in threes. Convert strips and fans first.
    const uint32_t* indices = nullptr;
    uint32_t index_count = 0;
    Material material;
    // RealityKit's front faces wind counter-clockwise as seen. True when the engine's front faces
    // wind clockwise (GX, D3D's default): each triangle is turned round.
    bool clockwise_front = false;
    // A decal: blended, depth-tested, not writing depth (paths, stains, blob shadows). Lifted off
    // what's under it once depths are final: 3 cm along `decal_normal` when the engine knows it (a
    // blob shadow's ground normal, view space), the same from any angle; else towards the camera by
    // 0.4% of its distance, at most 2 cm, which slides against the ground as the viewer leans (SHAR).
    bool decal = false;
    float decal_normal[3] = {0, 0, 0};
    // "This mesh at this place" (e.g. a hash of its first vertex bytes, its count and its model
    // matrix's translation), or 0. A mesh drawn again where it was just drawn (a second pass) is
    // nudged over the first.
    uint64_t pass_key = 0;
};

// Whether the window wants the mirror. Turning it on forgets every texture: knows_texture() is
// false for each until it is sent again.
void set_enabled(bool enabled);
bool enabled();

// The game's units: how many make a metre (1 for metres, ~100 for TP's). Decal lifts, the glass's
// range and runaway vertices are in metres.
void set_units_per_metre(float units);

// How far in front of its glass the window lets the scene come out (window widths; 0, the default:
// unlimited), and the narrowest half-view tangent it lays out as is (MirrorScene.minViewTangent).
// The window sends it when its size changes (lmvr_visionos_set_mirror_front_allowance): visionOS cuts
// a plain window's content off ~0.47 m in front of its face, whatever its size (TPVR, measured).
// Any thread.
void set_front_allowance(float widths, float min_view_tangent);

// The top of the game's frame. `game_frame` is the frame's number (24 bits; the window frame of the
// same game frame carries the same number, so the window shows the two together).
void begin_frame(uint32_t game_frame);

// The 3D scene's projection: half-view tangents and the near plane (game units). Call before the
// frame's first draw. Draws with another projection aren't the world (a HUD's 3D model): skip them.
void set_projection(float tan_half_x, float tan_half_y, float near_plane);

// One draw of the 3D world, in the game's order. Copies what it needs.
void add_draw(const Draw& draw);

// The 3D world is drawn (before the screen effects: bloom, fades, the HUD). `scene` false: there
// was none this frame (the window then shows the game's picture flat). `focus`: the camera's
// distance to what it looks at (game units); the window's glass goes a little short of it.
void end_frame(bool scene, float focus);
// A frame that won't end (the engine skipped it).
void discard_frame();

// Textures: RGBA8, the top row first, `id` never 0. `mipmapped`: the game samples it with mipmaps;
// `cutout`: its alpha is all but on or off (foliage, fences; it gets coverage-preserving mipmaps).
// Sending an id again replaces its pixels (a texture the game renders into, read back each frame,
// not mipmapped); a mipmapped id is its pixels, so the app keeps what it made. Unused for 15 s, a
// texture is forgotten: knows_texture() is false again, and the app parks it (up to 48 MB) with
// its materials, so the same id coming back draws at once, with no new material list.
bool knows_texture(uint64_t id);
// A texture's id from its decoded pixels and size: the same texture decoded again gets the same id,
// and only it does. Keep a map from the engine's source (pointer, format, size) to it, so a frame
// needn't decode to ask knows_texture().
uint64_t texture_id(const uint8_t* rgba, uint32_t width, uint32_t height);
void add_texture(uint64_t id, const uint8_t* rgba, uint32_t width, uint32_t height, bool mipmapped, bool cutout);
void remove_texture(uint64_t id);

// The bridge's: the newest finished frame no later than game frame `up_to`, or false before the
// first. Its pointers stay valid until the next call.
bool acquire(lmvr_visionos_mirror_frame* frame, uint32_t up_to);

}  // namespace scene_mirror
