#pragma once

// The scene mirror: a frame's 3D draws, decoded on the CPU into plain triangles, textures and
// materials a host renderer can draw again from any viewpoint (the visionOS port's shared-space
// window gives them to RealityKit, which renders them from the viewer's real eyes).
//
// The game marks where its 3D scene starts and ends in the GX stream (GXAuroraMirrorMark); every
// perspective draw between the marks into the main framebuffer is recorded as aurora processes
// it. Each vertex is transformed, lit and given its texture coordinate as GX would, and the TEV
// combine is reduced to an affine function of one texture's sample (lib/gx/mirror.cpp).

#include <cstdint>

namespace aurora::mirror {

// One vertex: its position in the frame's view space (GX's: right-handed, the camera at the
// origin looking down -z), its primary texture's coordinate, and the TEV result as an affine
// function of that texture's sample T, in the game's (gamma) space:
//   colour = T.rgb * mul.rgb + add.rgb, alpha = T.a * mul.a + add.a.
// mul and add are half floats.
struct Vertex {
  float position[3];
  float uv[2];
  uint16_t mul[4];
  uint16_t add[4];
};
static_assert(sizeof(Vertex) == 36, "packed");

enum PartKind : uint32_t {
  PartOpaque = 0,
  PartCutout = 1, // alpha below `cutoff` is discarded
  PartBlend = 2,  // see Part's weights
};

enum PartFlags : uint32_t {
  PartCullBack = 1u << 0, // GX's back faces culled (front faces wind counter-clockwise, as RealityKit's)
  PartCullFront = 1u << 1,
  PartDepthWrite = 1u << 2,
  PartDepthTest = 1u << 3,
  PartBackground = 1u << 4, // drawn behind everything (the sky): pushed past the scene's farthest point
  // The Luigi's Mansion port (MarkWorld): the room's own shell (its static model: walls, floor,
  // ceiling), not its furniture or characters. A volume can keep it as it stood in play while a
  // cutscene's camera, which the game hides and shows walls for, looks from elsewhere.
  PartWorld = 1u << 5,
};

enum WrapMode : uint8_t {
  WrapClamp = 0,
  WrapRepeat = 1,
  WrapMirror = 2,
};

// A run of triangles with one material.
struct Part {
  uint32_t firstIndex;
  uint32_t indexCount;
  uint64_t texture; // 0: none (white)
  uint32_t kind;    // PartKind
  uint32_t flags;   // PartFlags
  float cutoff;
  // PartBlend, premultiplied (result = colour + destination * (1 - opacity)):
  //   colour  = rgb * (colourBase + colourAlpha * a)
  //   opacity = opacityBase + opacityAlpha * a + opacityLuma * luma(rgb)
  float colourBase, colourAlpha, opacityBase, opacityAlpha, opacityLuma;
  uint8_t wrapS, wrapT; // WrapMode
  uint8_t pad[6];
};

// A texture the frame's parts use, sent once: RGBA8, the top row first.
struct Texture {
  uint64_t id;
  const uint8_t* rgba;
  uint32_t width, height;
  bool mipmapped; // the game samples it with mipmaps
  bool cutout;    // its alpha is all but on or off (foliage, fences)
};

struct Frame {
  uint64_t serial;
  uint32_t tag; // the game's number for the frame (the BEGIN mark's value, 24 bits)
  bool scene;   // false: the frame had no 3D scene
  const Vertex* vertices;
  uint32_t vertexCount;
  const uint32_t* indices;
  uint32_t indexCount;
  const Part* parts;
  uint32_t partCount;
  float boundsMin[3], boundsMax[3];
  float tanHalfX, tanHalfY; // the scene projection's half-view tangents
  float focus;              // the camera's distance to what it looks at (view units)
  // Where a window showing the frame should put its glass (view units): a little short of the
  // focus, and no farther than the sill (0111: MarkSill), following it over a second. What's
  // nearer comes out in front of it as the shape it is, and what would come out past the window's
  // allowance (set_front_allowance) is cut there, as a near plane cuts.
  float plane;
  // Textures new since the last acquire, and ones no longer used. A texture whose id was sent
  // before is new pixels for it (a shadow, read back from the GPU each frame): replace it in place.
  const Texture* textures;
  uint32_t textureCount;
  const uint64_t* removed;
  uint32_t removedCount;
};

// The view-scale mark (GXAuroraMirrorMark; GXAurora.h has 1 to 3): how much wider than its picture
// the frame is drawn (value: the scale), so the tangents reported are the picture's.
inline constexpr uint32_t MarkViewScale = 4u;
// 0111: the sill mark, before END: the view depth where the line of sight through the middle of the
// window's foot meets the ground the player stands on (the window's re-aimed view; 0 for none). The
// glass comes no farther than it (finish), so the ground there lies on the glass, never in front.
inline constexpr uint32_t MarkSill = 5u;
// 0111: the subject mark, before END: the nearest view depth of the player (0 for none), whom the
// window keeps whole: the glass comes to him when he's nearer than its cut would keep (finish).
inline constexpr uint32_t MarkSubject = 6u;
// 0111: the framed mark, before END: 1 when the frame's camera frames its own subject (an event's: a
// close-up whose focus can lie far behind the face it frames), 0 otherwise. The window then keeps
// whole what lies nearest in the middle of the picture, as it keeps the player.
inline constexpr uint32_t MarkFramed = 7u;
// 0111: the cut mark, before END: 1 when the camera cut since the frame before (it looks elsewhere,
// or its eye jumped), 0 otherwise. The glass then goes where it's going at once (finish).
inline constexpr uint32_t MarkCut = 8u;
// The Luigi's Mansion port. The ghost pass mark (value 1 at its start, 0 at its end): the game draws
// its ghosts into a layer of their own (the scene copied away, the framebuffer cleared, each ghost
// drawn solid with its fade level as destination alpha, the scene put back) and adds that layer
// over the scene, weighted by its alpha. The mirror records them as that sum: each ghost additive,
// its colour times its fade level, so it glows and never hides what's behind it.
inline constexpr uint32_t MarkGhostPass = 9u;
// The world mark (1 at the start of a room's static model, 0 at its end): its draws' parts are
// PartWorld.
inline constexpr uint32_t MarkWorld = 10u;
// The sky mark (1 at the start of the game's sky, 0 at its end): its draws are left out. The sky is
// a ball around the scene, seen from inside by the game's camera; from outside, on the port's
// volume, it is a shell that hides an outdoor room or a boss's arena.
inline constexpr uint32_t MarkSky = 11u;

// Whether to record (any thread). Turning it on starts the textures over: each is sent again.
void set_enabled(bool enabled) noexcept;
bool enabled() noexcept;

// How far in front of its glass the window showing the frames lets them come out, in window widths
// (any thread; 0, the default: no limit), and how far in front of the glass the window puts the
// camera, in window widths (0: where the view puts it, 1 / (2 tan); further out, depths are scaled
// so the picture from there is the same, which the window's scale of depth has to match). What
// would come out past the allowance is cut, as a near plane cuts (0111; AURORA_MIRROR_NEAR=ease
// eases nearer geometry into it from half of it instead, as before).
void set_front_allowance(float widths, float cameraWidths) noexcept;

// The Luigi's Mansion port's volume: nothing stands behind the scene there (no sky, no backdrop), so
// a solid surface drawn from a copy of the framebuffer (a window showing the night through a
// refraction) stays solid, the copy read as black, instead of turning see-through over nothing
// (any thread; off by default, as the window's mirror has the sky behind).
void set_solid_refraction(bool on) noexcept;

// The newest recorded frame whose tag is no later than `upTo` (the game frame the caller shows
// alongside, so the two match), or false before the first. Its pointers stay valid until the next
// call. One consumer thread.
bool acquire(Frame& frame, uint32_t upTo) noexcept;

// The Luigi's Mansion port: what the mirror did since the last call (any thread), for its timing log.
// A frame is skipped when the mirror is still busy with an earlier one as it begins, or the scene
// before cost the FIFO thread too much (nothing of it is recorded, so the game never waits for the
// mirror), dropped when it was recorded but the worker was still busy as it ended.
struct Stats {
  uint64_t recorded = 0, skipped = 0, dropped = 0, finished = 0;
  // Totals: the scene's wall time on the FIFO thread (BEGIN to END, mostly the game issuing its
  // draws), the worker's finishing, and the FIFO thread's CPU time over the scene.
  double recordMs = 0, finishMs = 0, sceneCpuMs = 0;
};
void take_stats(Stats& out) noexcept;

} // namespace aurora::mirror
