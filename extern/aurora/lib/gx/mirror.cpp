// The scene mirror (aurora/mirror.h): every perspective draw of a frame's 3D scene, decoded on the
// CPU as the generated shaders (shader.cpp) would run it, so a host renderer can draw the level
// again from any viewpoint.
//
// Per draw, on the FIFO thread, after the command processor has pushed it:
//   - which draws: between the game's BEGIN and END marks, perspective, into the main framebuffer
//     (no offscreen pass, a full-size viewport), writing colour, with the frame's projection;
//   - each vertex as the vertex shader has it: its attributes (direct or from the arrays, big- or
//     little-endian), the position and normal matrices, both colour channels lit by GX's lights,
//     and the texture coordinate of the one texture the mirror keeps (the "primary": the first
//     a TEV stage samples that has pixels to copy);
//   - the TEV stages, run per vertex with that texture's sample T left as an unknown, so their
//     result is an affine function of it: colour = T.rgb * mul + add. Other textures stand in as
//     their average colour; where a stage would need T twice, the second use is its average too.
//     Fog is applied per vertex;
//   - the material: blend mode as premultiplied weights, alpha test as a cut-out, cull mode, depth
//     test and write. Opaque draws that neither test nor write depth (the sky) are the background,
//     pushed past the scene's farthest point so that from any viewpoint they stay behind it.
// Triangles are grouped by material: solid groups merged, blended ones kept in the game's order.
// Textures are decoded once (texture_convert) and sent with the frame that first uses them.

#include "mirror.hpp"

#include "aurora/mirror.h"
#include "../gfx/hash.hpp"
#include "../gfx/texture_convert.hpp"
#include "../internal.hpp"
#include "../gfx/recording.hpp"
#include "shader_info.hpp"
#include "texture.hpp"

#include <absl/container/flat_hash_map.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <string>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(__APPLE__)
#include <dispatch/dispatch.h>
#include <pthread/qos.h>
#endif

namespace aurora::gx::mirror {
namespace {
constexpr Module Log{"aurora::gx::mirror"};

using MirrorVertex = aurora::mirror::Vertex;
using MirrorPart = aurora::mirror::Part;

struct F3 {
  float x = 0, y = 0, z = 0;
};
struct F4 {
  float v[4] = {0, 0, 0, 0};
  float& operator[](int i) { return v[i]; }
  float operator[](int i) const { return v[i]; }
};

F3 operator-(F3 a, F3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
float dot(F3 a, F3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
F3 normalized(F3 a) {
  const float l = std::sqrt(dot(a, a));
  return l > 1e-10f ? F3{a.x / l, a.y / l, a.z / l} : a;
}
F3 xyz(const Vec4<float>& v) { return {v[0], v[1], v[2]}; }
F4 f4(const Vec4<float>& v) { return {{v[0], v[1], v[2], v[3]}}; }

// A GX 3x4 matrix (rows) times (x, y, z, w).
F3 transform(const Mat3x4<float>& m, float x, float y, float z, float w) {
  return {m.m0[0] * x + m.m0[1] * y + m.m0[2] * z + m.m0[3] * w, m.m1[0] * x + m.m1[1] * y + m.m1[2] * z + m.m1[3] * w,
          m.m2[0] * x + m.m2[1] * y + m.m2[2] * z + m.m2[3] * w};
}

// IEEE half floats: the CPU's own conversion on ARM64 (a few hundred thousand a frame), otherwise
// without a compiler's half type (MSVC has none): round to nearest, clamped to the finite range,
// small values flushed to zero.
#if defined(__aarch64__) && defined(__clang__)
uint16_t half(float value) {
  const __fp16 h = static_cast<__fp16>(std::clamp(std::isfinite(value) ? value : 0.f, -65504.f, 65504.f));
  uint16_t bits;
  std::memcpy(&bits, &h, sizeof bits);
  return bits;
}
float from_half(uint16_t bits) {
  __fp16 h;
  std::memcpy(&h, &bits, sizeof h);
  return static_cast<float>(h);
}
#else
uint16_t half(float value) {
  value = std::clamp(std::isfinite(value) ? value : 0.f, -65504.f, 65504.f);
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof bits);
  const uint32_t sign = (bits >> 16) & 0x8000u;
  const int exponent = static_cast<int>((bits >> 23) & 0xFFu) - 127 + 15;
  uint32_t mantissa = bits & 0x7FFFFFu;
  if (exponent <= 0) {
    return static_cast<uint16_t>(sign);
  }
  uint32_t result = sign | static_cast<uint32_t>(exponent) << 10 | mantissa >> 13;
  if ((mantissa & 0x1FFFu) > 0x1000u || ((mantissa & 0x1FFFu) == 0x1000u && (result & 1u))) {
    ++result; // may carry into the exponent, as rounding should
  }
  return static_cast<uint16_t>(std::min<uint32_t>(result, sign | 0x7BFFu));
}
float from_half(uint16_t h) {
  const uint32_t sign = static_cast<uint32_t>(h & 0x8000u) << 16;
  const uint32_t exponent = (h >> 10) & 0x1Fu, mantissa = h & 0x3FFu;
  uint32_t bits;
  if (exponent == 0) {
    if (mantissa == 0) {
      bits = sign;
    } else {
      // Subnormal: normalise it.
      int e = -1;
      uint32_t m = mantissa;
      do {
        ++e;
        m <<= 1;
      } while ((m & 0x400u) == 0);
      bits = sign | static_cast<uint32_t>(127 - 15 - e) << 23 | (m & 0x3FFu) << 13;
    }
  } else {
    bits = sign | (exponent + 127 - 15) << 23 | mantissa << 13;
  }
  float value;
  std::memcpy(&value, &bits, sizeof value);
  return value;
}
#endif

// --- Symbolic TEV values: k * T[sel] + c, T the primary texture's sample (sel -1: a constant).

// TEV values as k * T[sel] + kb * B[bsel] + c: T the primary texture's sample, B a framebuffer
// copy's (what's behind the surface, for a refracting one like water), either absent (-1).
struct Sym {
  int8_t sel = -1;
  int8_t bsel = -1;
  float k = 0.f;
  float kb = 0.f;
  float c = 0.f;
};

struct TevContext {
  std::array<float, 4> primaryAverage{0.5f, 0.5f, 0.5f, 1.f};
};

// What's behind a surface, on average.
constexpr float kBehindAverage = 0.5f;

Sym constant(float value) { return {-1, -1, 0.f, 0.f, value}; }
bool is_constant(const Sym& a) { return a.sel < 0 && a.bsel < 0; }
Sym scaled(Sym a, float f) { return {a.sel, a.bsel, a.k * f, a.kb * f, a.c * f}; }
float average_of(const TevContext& ctx, const Sym& a) {
  return a.c + (a.sel >= 0 ? a.k * ctx.primaryAverage[a.sel] : 0.f) + (a.bsel >= 0 ? a.kb * kBehindAverage : 0.f);
}
// A second, different channel of T (or of B) counts as its average.
Sym add(const TevContext& ctx, Sym a, const Sym& b) {
  a.c += b.c;
  if (b.sel >= 0) {
    if (a.sel < 0) {
      a.sel = b.sel;
      a.k = b.k;
    } else if (a.sel == b.sel) {
      a.k += b.k;
    } else {
      a.c += b.k * ctx.primaryAverage[b.sel];
    }
  }
  if (b.bsel >= 0) {
    if (a.bsel < 0) {
      a.bsel = b.bsel;
      a.kb = b.kb;
    } else if (a.bsel == b.bsel) {
      a.kb += b.kb;
    } else {
      a.c += b.kb * kBehindAverage;
    }
  }
  return a;
}
// A product of two unknowns keeps the first; the second counts as its average.
Sym multiply(const TevContext& ctx, const Sym& a, const Sym& b) {
  if (is_constant(a)) {
    return scaled(b, a.c);
  }
  if (is_constant(b)) {
    return scaled(a, b.c);
  }
  return scaled(a, average_of(ctx, b));
}
// Clamped over the unknowns from 0 to 1: the line through the clamped ends.
Sym clamped(Sym a, float low, float high) {
  if (is_constant(a)) {
    return constant(std::clamp(a.c, low, high));
  }
  const float k = a.sel >= 0 ? a.k : 0.f, kb = a.bsel >= 0 ? a.kb : 0.f, total = k + kb;
  const float at0 = std::clamp(a.c, low, high), at1 = std::clamp(a.c + total, low, high);
  const float f = std::fabs(total) > 1e-6f ? (at1 - at0) / total : 0.f;
  a.c = at0;
  a.k = k * f;
  a.kb = kb * f;
  return a;
}

float tev_scale(GXTevScale scale) {
  switch (scale) {
  case GX_CS_SCALE_2:
    return 2.f;
  case GX_CS_SCALE_4:
    return 4.f;
  case GX_CS_DIVIDE_2:
    return 0.5f;
  default:
    return 1.f;
  }
}
float tev_bias(GXTevBias bias) {
  switch (bias) {
  case GX_TB_ADDHALF:
    return 0.5f;
  case GX_TB_SUBHALF:
    return -0.5f;
  default:
    return 0.f;
  }
}

// One channel of a stage: (d +- mix(a, b, c) + bias) * scale, clamped. The comparison ops are d.
Sym tev(const TevContext& ctx, const TevOp& op, const Sym& a, const Sym& b, const Sym& c, const Sym& d) {
  Sym result = d;
  if (op.op == GX_TEV_ADD || op.op == GX_TEV_SUB) {
    const Sym mixed = add(ctx, a, multiply(ctx, add(ctx, b, scaled(a, -1.f)), c));
    result = add(ctx, d, op.op == GX_TEV_SUB ? scaled(mixed, -1.f) : mixed);
    result.c += tev_bias(op.bias);
    result = scaled(result, tev_scale(op.scale));
  }
  return op.clamp ? clamped(result, 0.f, 1.f) : clamped(result, -4.f, 4.f);
}

float konst_colour(GXTevKColorSel sel, int channel) {
  const auto& k = g_gxState.kcolors;
  switch (sel) {
  case GX_TEV_KCSEL_8_8:
    return 1.f;
  case GX_TEV_KCSEL_7_8:
    return 7.f / 8.f;
  case GX_TEV_KCSEL_6_8:
    return 6.f / 8.f;
  case GX_TEV_KCSEL_5_8:
    return 5.f / 8.f;
  case GX_TEV_KCSEL_4_8:
    return 4.f / 8.f;
  case GX_TEV_KCSEL_3_8:
    return 3.f / 8.f;
  case GX_TEV_KCSEL_2_8:
    return 2.f / 8.f;
  case GX_TEV_KCSEL_1_8:
    return 1.f / 8.f;
  case GX_TEV_KCSEL_K0:
  case GX_TEV_KCSEL_K1:
  case GX_TEV_KCSEL_K2:
  case GX_TEV_KCSEL_K3:
    return k[sel - GX_TEV_KCSEL_K0][channel];
  case GX_TEV_KCSEL_K0_R:
  case GX_TEV_KCSEL_K1_R:
  case GX_TEV_KCSEL_K2_R:
  case GX_TEV_KCSEL_K3_R:
    return k[sel - GX_TEV_KCSEL_K0_R][0];
  case GX_TEV_KCSEL_K0_G:
  case GX_TEV_KCSEL_K1_G:
  case GX_TEV_KCSEL_K2_G:
  case GX_TEV_KCSEL_K3_G:
    return k[sel - GX_TEV_KCSEL_K0_G][1];
  case GX_TEV_KCSEL_K0_B:
  case GX_TEV_KCSEL_K1_B:
  case GX_TEV_KCSEL_K2_B:
  case GX_TEV_KCSEL_K3_B:
    return k[sel - GX_TEV_KCSEL_K0_B][2];
  case GX_TEV_KCSEL_K0_A:
  case GX_TEV_KCSEL_K1_A:
  case GX_TEV_KCSEL_K2_A:
  case GX_TEV_KCSEL_K3_A:
    return k[sel - GX_TEV_KCSEL_K0_A][3];
  default:
    return 1.f;
  }
}

float konst_alpha(GXTevKAlphaSel sel) {
  const auto& k = g_gxState.kcolors;
  switch (sel) {
  case GX_TEV_KASEL_8_8:
    return 1.f;
  case GX_TEV_KASEL_7_8:
    return 7.f / 8.f;
  case GX_TEV_KASEL_6_8:
    return 6.f / 8.f;
  case GX_TEV_KASEL_5_8:
    return 5.f / 8.f;
  case GX_TEV_KASEL_4_8:
    return 4.f / 8.f;
  case GX_TEV_KASEL_3_8:
    return 3.f / 8.f;
  case GX_TEV_KASEL_2_8:
    return 2.f / 8.f;
  case GX_TEV_KASEL_1_8:
    return 1.f / 8.f;
  case GX_TEV_KASEL_K0_R:
  case GX_TEV_KASEL_K1_R:
  case GX_TEV_KASEL_K2_R:
  case GX_TEV_KASEL_K3_R:
    return k[sel - GX_TEV_KASEL_K0_R][0];
  case GX_TEV_KASEL_K0_G:
  case GX_TEV_KASEL_K1_G:
  case GX_TEV_KASEL_K2_G:
  case GX_TEV_KASEL_K3_G:
    return k[sel - GX_TEV_KASEL_K0_G][1];
  case GX_TEV_KASEL_K0_B:
  case GX_TEV_KASEL_K1_B:
  case GX_TEV_KASEL_K2_B:
  case GX_TEV_KASEL_K3_B:
    return k[sel - GX_TEV_KASEL_K0_B][2];
  case GX_TEV_KASEL_K0_A:
  case GX_TEV_KASEL_K1_A:
  case GX_TEV_KASEL_K2_A:
  case GX_TEV_KASEL_K3_A:
    return k[sel - GX_TEV_KASEL_K0_A][3];
  default:
    return 1.f;
  }
}

int swap_channel(GXTevColorChan chan) { return static_cast<int>(chan); }

bool samples_texture(const TevStage& stage) noexcept {
  if (stage.texMapId == GX_TEXMAP_NULL) {
    return false;
  }
  const auto& c = stage.colorPass;
  const auto& a = stage.alphaPass;
  return c.a == GX_CC_TEXC || c.a == GX_CC_TEXA || c.b == GX_CC_TEXC || c.b == GX_CC_TEXA || c.c == GX_CC_TEXC ||
         c.c == GX_CC_TEXA || c.d == GX_CC_TEXC || c.d == GX_CC_TEXA || a.a == GX_CA_TEXA || a.b == GX_CA_TEXA ||
         a.c == GX_CA_TEXA || a.d == GX_CA_TEXA;
}

bool is_bump_channel(GXChannelID id) { return id == GX_ALPHA_BUMP || id == GX_ALPHA_BUMPN; }

// --- Textures.

constexpr uint8_t kIdentitySwizzle = 0xE4; // r g b a: 0 1 2 3, two bits each

struct TexEntry {
  uint64_t id = 0;
  bool ok = false; // decoded: otherwise drawn white
  bool captured = false; // a framebuffer copy read back from the GPU each frame (capture_entry)
  std::array<float, 4> average{0.5f, 0.5f, 0.5f, 1.f};
  bool cutout = false;
  bool mipmapped = false;
  uint32_t lastFrame = 0;
  std::vector<uint64_t> variants; // swizzled copies' ids, removed with it
  uint64_t hash = 0; // of the decoded pixels: a swept id comes back only for the same ones
};

struct OwnedTexture {
  uint64_t id;
  std::vector<uint8_t> rgba;
  uint32_t width, height;
  bool mipmapped, cutout;
};

struct Group {
  uint64_t key;
  MirrorPart part{};
  std::vector<uint32_t> indices;
  bool blended = false;
};

struct Built {
  uint64_t serial = 0;
  uint32_t tag = 0; // the game's frame number (the BEGIN mark's value)
  bool scene = false;
  std::vector<MirrorVertex> vertices;
  std::vector<uint32_t> indices;
  std::vector<MirrorPart> parts;
  float boundsMin[3]{}, boundsMax[3]{};
  float tanHalfX = 1, tanHalfY = 1, focus = 0, plane = 1;
};

struct Stats {
  uint64_t draws = 0, recorded = 0, merged = 0, lines = 0, screen = 0, noColour = 0, viewport = 0, projection = 0, blend = 0,
           copyOnly = 0, alphaNever = 0, vertices = 0, triangles = 0, background = 0, behind = 0, frames = 0,
           bakes = 0, bakeTexels = 0, alphaOps = 0;
  double seconds = 0;
};

std::atomic_bool g_enabled{false};
std::atomic_bool g_reset{false};

// FIFO thread.
bool g_recording = false;
int g_offscreen = 0;
// 0109: how much wider the game draws than the picture the window shows (its view scale mark, right
// after BEGIN): the frame's view tangents are the picture's, so the window lays the scene out as the
// 2D picture is framed, and what the wider view adds lies past the picture's edges.
constexpr u32 kMarkViewScale = ::aurora::mirror::MarkViewScale; // visionos/host/wwvr_window.cpp sends it
constexpr u32 kMarkSill = ::aurora::mirror::MarkSill;           // 0111: and these
constexpr u32 kMarkSubject = ::aurora::mirror::MarkSubject;
constexpr u32 kMarkFramed = ::aurora::mirror::MarkFramed;
constexpr u32 kMarkCut = ::aurora::mirror::MarkCut;
float g_viewScale = 1.f;            // this frame's (FIFO thread)
float g_sill = 0.f;                 // 0111: this frame's sill (kMarkSill; 0: none), likewise
float g_subject = 0.f;              // 0111: and the player's nearest depth (kMarkSubject; 0: none)
bool g_framed = false;              // 0111: and whether an event's camera frames it (kMarkFramed)
bool g_cameraCut = false;           // 0111: and whether the camera cut (kMarkCut)
std::atomic_bool g_solidRefraction{false}; // set_solid_refraction
bool g_ghostPass = false;           // the Luigi's Mansion port: inside the ghost pass (MarkGhostPass)
bool g_world = false;               // and inside a room's static model (MarkWorld)
bool g_sky = false;                 // and inside the game's sky (MarkSky): left out
long g_frameDraws = 0;              // this frame's draws so far (AURORA_MIRROR_SKIP)
bool g_haveProjection = false;
float g_p0 = 1, g_p2 = 1;
bool g_sawDepthWrite = false;
float g_farthest = 0, g_nearestBackground = INFINITY;
std::vector<std::pair<uint32_t, uint32_t>> g_backgroundRanges;
std::vector<std::pair<uint32_t, uint32_t>> g_decalRanges; // lifted in finish, once depths are final
std::vector<Group> g_groups;
// 0106: index lists of earlier frames' groups, emptied but keeping their room, for this frame's
// (made and freed each frame, a busy frame's large lists went back to the system and were asked
// for again, every frame). Only modest ones, up to 16 MB in all: a list keeps the room it grew to,
// and the near subdivision's big lists, passed from group to group, would otherwise all end up as
// big as the biggest and stay so.
std::vector<std::vector<uint32_t>> g_spareIndices;
size_t g_spareBytes = 0;
constexpr size_t kSpareListMax = size_t{1} << 16; // indices
constexpr size_t kSpareBytesMax = size_t{16} << 20;
std::unordered_map<uint64_t, size_t> g_solidGroups;
int g_lastBlended = -1;
Built g_building;
uint32_t g_frame = 0;
uint32_t g_frameTag = 0; // the game's number for the frame being recorded (the BEGIN mark's value)
std::unordered_map<uint64_t, TexEntry> g_textures;  // by source key
std::unordered_map<uint64_t, uint64_t> g_variants; // (base id << 8 | swizzle) -> id
// The ids of textures swept away (by source key, and the variants by their key), so one that comes
// back has the id it had: the window keeps materials by texture id, and a new one for the same
// texture grew the level's material list each time an area's textures returned, setting it again.
// (A texture's with the hash of its pixels: the source key samples them only, and memory a level
// freed can hold another texture of the same shape whose sampled bytes match.)
struct Retired {
  uint64_t id, hash;
};
std::unordered_map<uint64_t, Retired> g_retiredTextures;
std::unordered_map<uint64_t, uint64_t> g_retiredVariants;
uint64_t g_nextTextureId = 1;

// Framebuffer copies read back (capture_entry): one per revision of a copy destination used this
// frame and per channel arrangement it's read with. TP draws its shadows into one copy, four at a
// time, one per channel, and a ground patch picks its character's channel with the swap table;
// and it copies again for the next four. Each is read back right after the draw that uses it, under
// an id that stays the same from frame to frame (the n-th copy, read this way).
struct Capture {
  uint64_t id;
  gfx::TextureHandle handle;
  uint8_t swizzle; // the channels as the TEV reads them (a swap table's red as alpha, for a shadow)
  // 0107: a mask: its alpha read as 1 above `threshold` and 0 at or below it (where the draw passes
  // its alpha test: Wind Waker's real-time shadows). The threshold changes as a shadow fades; the
  // copy keeps one id (one texture in the window) all the same.
  bool mask = false;
  uint8_t threshold = 0;
};
struct CaptureVariant {
  uint64_t id = 0;
  uint64_t frame = UINT64_MAX;
  uint32_t revision = 0;
};
struct CopySlot {
  uint64_t frame = UINT64_MAX;
  uint32_t revision = 0;
  size_t occurrence = 0;
  std::deque<TexEntry> entries; // per occurrence (the TEV's view of it); a deque, so they stay put
  std::deque<std::unordered_map<uint16_t, CaptureVariant>> variants; // per occurrence, by swizzle | mask << 8
};
std::unordered_map<const void*, CopySlot> g_copySlots; // FIFO thread
constexpr size_t kCaptureRing = 256;
std::mutex g_captureMutex;
std::array<Capture, kCaptureRing> g_captureRing; // FIFO thread writes, render worker reads (the mutex)
uint32_t g_captureNext = 0;                       // FIFO thread
std::vector<OwnedTexture> g_newTextures;
std::vector<uint64_t> g_removedTextures;
Stats g_stats;

// Shared with the consumer.
std::mutex g_mutex;
// The finished frames not yet taken, oldest first (a few: the window takes the one matching the
// game frame it shows, which may be a frame or two behind), and spent ones to reuse. The port's
// volume shows each frame on an even beat, up to a couple of frames late: six kept.
constexpr size_t kKeptFrames = 6;
std::deque<Built> g_finishedFrames;
std::vector<Built> g_spareFrames;
Built g_acquired;
uint64_t g_serial = 0;

// Frame numbers are 24 bits (they travel as a float): whether `a` is `b` or before it.
bool not_after(uint32_t a, uint32_t b) { return ((b - a) & 0xFFFFFFu) < 0x800000u; }
std::vector<OwnedTexture> g_pendingTextures, g_acquiredTextures;
std::vector<uint64_t> g_pendingRemoved, g_acquiredRemoved;
std::vector<aurora::mirror::Texture> g_textureViews;

uint64_t texture_key(const GXTexObj_& obj, const GXTlutObj_* tlut) {
  struct Key {
    const void* data;
    const void* tlutData;
    // Not the object id: GXInitTexObj gives a new one each time, and J3D initialises its
    // materials' texture objects every frame.
    uint32_t format, width, height, version;
    uint32_t tlutFormat, tlutEntries, tlutVersion;
  } key{};
  key.data = obj.data;
  key.format = obj.format();
  key.width = obj.width();
  key.height = obj.height();
  key.version = obj.texDataVersion;
  if (tlut != nullptr) {
    key.tlutData = tlut->data;
    key.tlutFormat = tlut->format;
    key.tlutEntries = tlut->numEntries;
    key.tlutVersion = tlut->tlutDataVersion;
  }
  // A sample of the pixels too, as memory a level freed may hold another texture of the same shape.
  const auto* bytes = static_cast<const uint8_t*>(obj.data);
  const size_t size = texture::texture_source_size(key.format, key.width, key.height, 1);
  const uint64_t sample = xxh3_hash_s(bytes, std::min<size_t>(size, 64)) ^
                          (size > 128 ? xxh3_hash_s(bytes + size - 64, 64) : 0);
  return xxh3_hash_s(&key, sizeof key, sample);
}

const GXTlutObj_* palette_of(const GXTexObj_& obj) {
  if (!is_palette_format(obj.format())) {
    return nullptr;
  }
  const auto index = static_cast<size_t>(obj.tlut);
  if (index >= g_gxState.loadedTluts.size() || g_gxState.loadedTluts[index].data == nullptr) {
    return nullptr;
  }
  return &g_gxState.loadedTluts[index];
}

// The texture's level-zero pixels as RGBA8, or empty.
std::vector<uint8_t> decode(const GXTexObj_& obj, const GXTlutObj_* tlut) {
  const u32 format = obj.format(), width = obj.width(), height = obj.height();
  if (width == 0 || height == 0 || width > 4096 || height > 4096) {
    return {};
  }
  const auto* data = static_cast<const uint8_t*>(obj.data);
  const size_t size = texture::texture_source_size(format, width, height, 1);
  if (format == GX_TF_RGBA8_PC) {
    return {data, data + static_cast<size_t>(width) * height * 4};
  }
  if (gfx::is_pc_texture_format(format) && format != GX_TF_BC1_PC) {
    return {};
  }
  gfx::ConvertedTexture converted;
  if (is_palette_format(format)) {
    if (tlut == nullptr) {
      return {};
    }
    converted = gfx::convert_texture_palette(format, width, height, 1, {data, size}, tlut->format, tlut->numEntries,
                                             {static_cast<const uint8_t*>(tlut->data), texture::tlut_source_size(tlut->numEntries)});
  } else {
    converted = gfx::convert_texture(format, width, height, 1, {data, size});
  }
  if (converted.format != wgpu::TextureFormat::RGBA8Unorm ||
      converted.data.size() < static_cast<size_t>(width) * height * 4) {
    return {};
  }
  const auto* pixels = converted.data.data();
  return {pixels, pixels + static_cast<size_t>(width) * height * 4};
}

// A texture's alpha: all opaque, all but on or off (foliage, cut-out trim), or translucent.
bool binary_alpha(const std::vector<uint8_t>& rgba) {
  size_t translucent = 0;
  uint8_t lowest = 255;
  for (size_t i = 3; i < rgba.size(); i += 4) {
    lowest = std::min(lowest, rgba[i]);
    translucent += rgba[i] > 25 && rgba[i] < 230;
  }
  return lowest < 250 && translucent * 10 <= rgba.size() / 4;
}

TexEntry* texture_entry(const GXTexObj_& obj) {
  if (!obj.has_data() || g_gxState.copyTextures.contains(obj.data)) {
    return nullptr;
  }
  const GXTlutObj_* tlut = palette_of(obj);
  const uint64_t key = texture_key(obj, tlut);
  auto found = g_textures.find(key);
  if (found != g_textures.end()) {
    found->second.lastFrame = g_frame;
    return &found->second;
  }
  TexEntry entry;
  entry.lastFrame = g_frame;
  entry.mipmapped = obj.has_mips();
  auto rgba = decode(obj, tlut);
  if (!rgba.empty()) {
    entry.ok = true;
    entry.hash = xxh3_hash_s(rgba.data(), rgba.size());
    entry.id = 0;
    if (auto retired = g_retiredTextures.find(key); retired != g_retiredTextures.end()) {
      if (retired->second.hash == entry.hash) {
        entry.id = retired->second.id;
      }
      g_retiredTextures.erase(retired);
    }
    if (entry.id == 0) {
      entry.id = g_nextTextureId++;
    }
    double sum[4] = {0, 0, 0, 0};
    for (size_t i = 0; i < rgba.size(); i += 4) {
      for (int c = 0; c < 4; ++c) {
        sum[c] += rgba[i + c];
      }
    }
    const double texels = static_cast<double>(rgba.size() / 4);
    for (int c = 0; c < 4; ++c) {
      entry.average[c] = static_cast<float>(sum[c] / texels / 255.0);
    }
    entry.cutout = binary_alpha(rgba);
    g_newTextures.push_back({entry.id, std::move(rgba), obj.width(), obj.height(), entry.mipmapped, entry.cutout});
  }
  return &g_textures.emplace(key, std::move(entry)).first->second;
}

// The texture with its channels rearranged, as the TEV reads them (an intensity texture's alpha
// as colour, a swap table's red as everything...).
uint64_t texture_variant(TexEntry& entry, const GXTexObj_& obj, uint8_t swizzle) {
  if (!entry.ok || swizzle == kIdentitySwizzle) {
    return entry.ok ? entry.id : 0;
  }
  const uint64_t key = entry.id << 8 | swizzle;
  if (auto found = g_variants.find(key); found != g_variants.end()) {
    return found->second;
  }
  // From the pixels decoded this frame, if they're still here; otherwise decoded again.
  std::vector<uint8_t> rgba;
  for (const auto& texture : g_newTextures) {
    if (texture.id == entry.id) {
      rgba = texture.rgba;
      break;
    }
  }
  if (rgba.empty()) {
    rgba = decode(obj, palette_of(obj));
  }
  if (rgba.empty()) {
    return entry.id;
  }
  for (size_t i = 0; i < rgba.size(); i += 4) {
    const uint8_t texel[4] = {rgba[i], rgba[i + 1], rgba[i + 2], rgba[i + 3]};
    for (int c = 0; c < 4; ++c) {
      rgba[i + c] = texel[(swizzle >> (2 * c)) & 3];
    }
  }
  uint64_t id;
  if (auto retired = g_retiredVariants.find(key); retired != g_retiredVariants.end()) {
    id = retired->second;
    g_retiredVariants.erase(retired);
  } else {
    id = g_nextTextureId++;
  }
  const bool cutout = binary_alpha(rgba);
  g_newTextures.push_back({id, std::move(rgba), obj.width(), obj.height(), entry.mipmapped, cutout});
  g_variants.emplace(key, id);
  entry.variants.push_back(id);
  return id;
}

// visionos/patches/aurora/0104: textures the vertices read themselves.
//
// A texture sampled with coordinates made from a lit colour channel (Wind Waker's toon ramps: the
// light's intensity, as a coordinate, picks the shade) is a lookup: its pixels are kept here and
// read at each vertex, where an average would give every vertex the same half-lit shade. Small
// ones only (they're read on the CPU); a few hundred at most, by texture id.
struct LookupPixels {
  std::vector<uint8_t> rgba;
  uint32_t width = 0, height = 0;
};
// (0106: owned here, read by the frame's jobs through plain pointers; let go of only between frames,
// in sweep_textures and forget_textures, when no job is in flight.)
std::unordered_map<uint64_t, std::unique_ptr<LookupPixels>> g_lookupPixels; // FIFO thread
constexpr uint32_t kLookupMaxTexels = 256 * 256;

// 0105: two textures a draw multiplies (an eye: its white and pupil, scrolled by a texture matrix, times
// its outline and shape), baked into one at the base's size, under an id per draw of the frame (the
// n-th of these two textures): baked again, in place, only when the scroll or the TEV's constants
// change.
struct Baked {
  uint64_t id = 0;
  uint64_t key = 0; // what it was baked from
  uint32_t lastFrame = 0;
  uint32_t bakedFrame = 0; // 0108: when it was last baked (a bake is never more than a few frames old)
};
std::unordered_map<uint64_t, Baked> g_baked;           // FIFO thread
std::unordered_map<uint64_t, unsigned> g_bakeOccurrences; // this frame's, by texture pair
uint64_t g_bakeTexels = 0;                          // 0108: this frame's
std::unordered_map<uint64_t, uint64_t> g_bakeKeys; // 0108: this frame's: what was baked -> its texture

void forget_textures() {
  g_lookupPixels.clear();
  g_baked.clear();
  g_textures.clear();
  g_copySlots.clear();
  g_variants.clear();
  g_retiredTextures.clear();
  g_retiredVariants.clear();
  g_newTextures.clear();
  g_removedTextures.clear();
  std::lock_guard lock(g_mutex);
  g_pendingTextures.clear();
  g_pendingRemoved.clear();
}

// Every few seconds: textures unused for fifteen go. (`all`: every one, for test runs.)
void sweep_textures(bool all = false) {
  // Lookups' pixels: a few hundred at most (between frames: no job reads them now).
  if (g_lookupPixels.size() > 512) {
    g_lookupPixels.clear();
  }
  // Ids kept for textures that never came back (memory a level freed, reused): a long session's worth
  // at most.
  if (g_retiredTextures.size() > 65536 || g_retiredVariants.size() > 65536) {
    g_retiredTextures.clear();
    g_retiredVariants.clear();
  }
  for (auto it = g_textures.begin(); it != g_textures.end();) {
    if (all || g_frame - it->second.lastFrame > 900) {
      if (it->second.ok) {
        g_removedTextures.push_back(it->second.id);
        g_removedTextures.insert(g_removedTextures.end(), it->second.variants.begin(), it->second.variants.end());
        const uint64_t id = it->second.id;
        g_retiredTextures[it->first] = {id, it->second.hash};
        std::erase_if(g_variants, [id](const auto& v) {
          if ((v.first >> 8) != id) {
            return false;
          }
          g_retiredVariants[v.first] = v.second;
          return true;
        });
      }
      g_lookupPixels.erase(it->second.id);
      it = g_textures.erase(it);
    } else {
      ++it;
    }
  }
  for (auto it = g_baked.begin(); it != g_baked.end();) {
    if (all || g_frame - it->second.lastFrame > 900) {
      if (it->second.id != 0) {
        g_removedTextures.push_back(it->second.id);
      }
      it = g_baked.erase(it);
    } else {
      ++it;
    }
  }
}

// A lookup's pixels (decoded once, then kept), or null for one too big to read on the CPU.
const LookupPixels* lookup_pixels(const TexEntry& entry, const GXTexObj_& obj) {
  if (!entry.ok || entry.captured || obj.width() * obj.height() > kLookupMaxTexels) {
    return nullptr;
  }
  if (auto found = g_lookupPixels.find(entry.id); found != g_lookupPixels.end()) {
    return found->second.get();
  }
  auto pixels = std::make_unique<LookupPixels>();
  pixels->rgba = decode(obj, palette_of(obj));
  pixels->width = obj.width();
  pixels->height = obj.height();
  if (pixels->rgba.size() < static_cast<size_t>(pixels->width) * pixels->height * 4) {
    return nullptr;
  }
  return g_lookupPixels.emplace(entry.id, std::move(pixels)).first->second.get();
}

// GX's wrap of texel index i in n texels.
int wrap_texel(int i, int n, uint8_t mode) {
  if (mode == GX_REPEAT) {
    i %= n;
    return i < 0 ? i + n : i;
  }
  if (mode == GX_MIRROR) {
    const int period = 2 * n;
    i %= period;
    if (i < 0) {
      i += period;
    }
    return i < n ? i : period - 1 - i;
  }
  return std::clamp(i, 0, n - 1);
}

// The texture at (u, v), filtered bilinearly as the GPU would, 0..1 per channel.
std::array<float, 4> sample_lookup(const LookupPixels& p, float u, float v, uint8_t wrapS, uint8_t wrapT) {
  if (!std::isfinite(u) || !std::isfinite(v) || p.width == 0 || p.height == 0) {
    return {0.f, 0.f, 0.f, 0.f};
  }
  const float x = u * static_cast<float>(p.width) - 0.5f, y = v * static_cast<float>(p.height) - 0.5f;
  const float fx0 = std::floor(x), fy0 = std::floor(y);
  const float fx = x - fx0, fy = y - fy0;
  const int x0 = static_cast<int>(std::clamp(fx0, -1e6f, 1e6f)), y0 = static_cast<int>(std::clamp(fy0, -1e6f, 1e6f));
  const int w = static_cast<int>(p.width), h = static_cast<int>(p.height);
  const int xs[2] = {wrap_texel(x0, w, wrapS), wrap_texel(x0 + 1, w, wrapS)};
  const int ys[2] = {wrap_texel(y0, h, wrapT), wrap_texel(y0 + 1, h, wrapT)};
  const float wx[2] = {1.f - fx, fx}, wy[2] = {1.f - fy, fy};
  std::array<float, 4> out{0.f, 0.f, 0.f, 0.f};
  for (int j = 0; j < 2; ++j) {
    for (int i = 0; i < 2; ++i) {
      const uint8_t* texel = &p.rgba[(static_cast<size_t>(ys[j]) * p.width + xs[i]) * 4];
      const float weight = wx[i] * wy[j] / 255.f;
      for (int c = 0; c < 4; ++c) {
        out[c] += weight * texel[c];
      }
    }
  }
  return out;
}

// --- Vertices.

uint32_t read16(const uint8_t* p, bool le) { return le ? (p[0] | p[1] << 8) : (p[0] << 8 | p[1]); }
uint32_t read32(const uint8_t* p, bool le) {
  return le ? (p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24)
            : (static_cast<uint32_t>(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]);
}

// 0106: a monotonic clock in nanoseconds that doesn't cost a system call (the recording timed each
// of a frame's thousands of draws with steady_clock, whose clock_gettime did).
uint64_t clock_ns() {
#if defined(__APPLE__)
  return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#else
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
}

// What a draw's vertices read of the GX state, copied when the draw is recorded: the vertices are
// decoded on worker threads while the FIFO thread moves on.
struct DrawState {
  std::array<PnMtx, MaxPnMtx> pnMtx;
  std::array<Mat3x4<float>, MaxTexMtx> texMtxs;
  std::array<Mat3x4<float>, MaxPTTexMtx> ptTexMtxs;
  std::array<Light, GX::MaxLights> lights;
  std::array<ColorChannelConfig, MaxColorChannels> channelConfig;
  std::array<ColorChannelState, MaxColorChannels> channelState;
  std::array<Vec4<float>, MaxTevRegs> colorRegs;
  std::array<AttrArray, MaxVtxAttr> arrays;
  FogState fog;
  Mat4x4<float> proj;
  u32 currentPnMtx = 0;

  // 0106: only what this draw's vertices read (a frame has thousands of draws, and copying all of
  // it each time was a good part of recording one): the current matrix unless the vertices or their
  // texture coordinates pick others, the texture matrices and lights only if used, and the arrays
  // of the indexed attributes. What isn't copied isn't read (decode_vertex, tex_coord, light).
  void capture(const ShaderConfig& config) {
    const auto& st = g_gxState;
    currentPnMtx = st.currentPnMtx;
    bool allPn = config.attrs[GX_VA_PNMTXIDX].attrType != GX_NONE;
    bool texMtx = false, postMtx = false;
    for (u32 i = 0; i < MaxTexCoord; ++i) {
      const TcgConfig& tcg = config.tcgs[i];
      if (config.attrs[GX_VA_TEX0MTXIDX + i].attrType != GX_NONE) {
        allPn = true;
        texMtx = true;
      }
      if (tcg.src == GX_MAX_TEXGENSRC) {
        continue;
      }
      if (tcg.mtx != GX_IDENTITY) {
        (tcg.mtx < GX_TEXMTX0 ? allPn : texMtx) = true;
      }
      postMtx = postMtx || tcg.postMtx != GX_PTIDENTITY;
    }
    if (allPn) {
      pnMtx = st.pnMtx;
    } else {
      const u32 current = std::min<u32>(currentPnMtx, MaxPnMtx - 1);
      pnMtx[current] = st.pnMtx[current];
    }
    if (texMtx) {
      texMtxs = st.texMtxs;
    }
    if (postMtx) {
      ptTexMtxs = st.ptTexMtxs;
    }
    channelConfig = st.colorChannelConfig;
    channelState = st.colorChannelState;
    for (const auto& channel : channelConfig) {
      if (channel.lightingEnabled) {
        lights = st.lights;
        break;
      }
    }
    colorRegs = st.colorRegs;
    for (u32 a = 0; a < MaxVtxAttr; ++a) {
      if (config.attrs[a].attrType == GX_INDEX8 || config.attrs[a].attrType == GX_INDEX16) {
        arrays[a] = st.arrays[a];
      }
    }
    fog = st.fog;
    proj = st.proj;
  }
};

struct Decoder {
  const DrawState& st;
  const ShaderConfig& config;
  const uint8_t* vertices;
  size_t bytes;

  // Where an attribute's data for vertex `v` is, and its endianness; null if out of range.
  const uint8_t* locate(GXAttr attr, uint32_t v, size_t size, bool* le) const {
    const AttrConfig& m = config.attrs[attr];
    const size_t at = static_cast<size_t>(v) * config.vtxStride + m.offset;
    if (m.attrType == GX_DIRECT) {
      *le = false;
      return at + size <= bytes ? vertices + at : nullptr;
    }
    const auto& array = st.arrays[attr];
    size_t index;
    if (m.attrType == GX_INDEX8) {
      if (at + 1 > bytes) {
        return nullptr;
      }
      index = vertices[at];
    } else {
      if (at + 2 > bytes) {
        return nullptr;
      }
      index = read16(vertices + at, false);
    }
    *le = array.le;
    const size_t offset = index * (m.stride != 0 ? m.stride : array.stride);
    // (The Luigi's Mansion port: an array in guest memory has no size, 0, as its command processor's
    // own decoding takes it.)
    if (array.data == nullptr || (array.size != 0 && offset + size > array.size)) {
      return nullptr;
    }
    return static_cast<const uint8_t*>(array.data) + offset;
  }

  uint32_t u8(GXAttr attr, uint32_t v, uint32_t fallback) const {
    if (config.attrs[attr].attrType == GX_NONE) {
      return fallback;
    }
    bool le;
    const uint8_t* p = locate(attr, v, 1, &le);
    return p != nullptr ? *p : fallback;
  }

  // Up to four components of a position, normal or texture coordinate.
  bool numbers(GXAttr attr, uint32_t v, int count, float* out) const {
    const AttrConfig& m = config.attrs[attr];
    if (m.attrType == GX_NONE) {
      return false;
    }
    const int size = m.compType == GX_F32 ? 4 : m.compType >= GX_U16 ? 2 : 1;
    bool le;
    const uint8_t* p = locate(attr, v, static_cast<size_t>(size) * count, &le);
    if (p == nullptr) {
      return false;
    }
    const float scale = 1.f / static_cast<float>(1u << m.frac);
    for (int i = 0; i < count; ++i, p += size) {
      switch (m.compType) {
      case GX_U8:
        out[i] = static_cast<float>(p[0]) * scale;
        break;
      case GX_S8:
        out[i] = static_cast<float>(static_cast<int8_t>(p[0])) * scale;
        break;
      case GX_U16:
        out[i] = static_cast<float>(read16(p, le)) * scale;
        break;
      case GX_S16:
        out[i] = static_cast<float>(static_cast<int16_t>(read16(p, le))) * scale;
        break;
      default: {
        const uint32_t bits = read32(p, le);
        std::memcpy(&out[i], &bits, sizeof bits);
        if (!std::isfinite(out[i])) {
          out[i] = 0;
        }
        break;
      }
      }
    }
    return true;
  }

  F4 colour(GXAttr attr, uint32_t v) const {
    const AttrConfig& m = config.attrs[attr];
    if (m.attrType == GX_NONE) {
      return {};
    }
    static constexpr int kSizes[] = {2, 3, 4, 2, 3, 4}; // RGB565 RGB8 RGBX8 RGBA4 RGBA6 RGBA8
    const int size = m.compType < 6 ? kSizes[m.compType] : 4;
    bool le;
    const uint8_t* p = locate(attr, v, size, &le);
    if (p == nullptr) {
      return {};
    }
    switch (m.compType) {
    case GX_RGB565: {
      const uint32_t c = read16(p, le);
      return {{static_cast<float>(c >> 11 & 31) / 31.f, static_cast<float>(c >> 5 & 63) / 63.f,
               static_cast<float>(c & 31) / 31.f, 1.f}};
    }
    case GX_RGB8:
    case GX_RGBX8:
      return {{p[0] / 255.f, p[1] / 255.f, p[2] / 255.f, 1.f}};
    case GX_RGBA4: {
      const uint32_t c = read16(p, le);
      return {{static_cast<float>(c >> 12 & 15) / 15.f, static_cast<float>(c >> 8 & 15) / 15.f,
               static_cast<float>(c >> 4 & 15) / 15.f, static_cast<float>(c & 15) / 15.f}};
    }
    case GX_RGBA6: {
      const uint32_t c = le ? (p[0] | p[1] << 8 | p[2] << 16) : (p[0] << 16 | p[1] << 8 | p[2]);
      return {{static_cast<float>(c >> 18 & 63) / 63.f, static_cast<float>(c >> 12 & 63) / 63.f,
               static_cast<float>(c >> 6 & 63) / 63.f, static_cast<float>(c & 63) / 63.f}};
    }
    default:
      return {{p[0] / 255.f, p[1] / 255.f, p[2] / 255.f, p[3] / 255.f}};
    }
  }
};

const Mat3x4<float>& postex_matrix(const DrawState& st, uint32_t index) {
  return index < MaxPnMtx ? st.pnMtx[index].pos : st.texMtxs[std::min(index - MaxPnMtx, MaxTexMtx - 1)];
}

F4 light_channel(const DrawState& st, const ColorChannelConfig& cc, const ColorChannelState& state,
                 const F4& vertexColour, F3 position, F3 normal) {
  const F4 material = cc.matSrc == GX_SRC_VTX ? vertexColour : f4(state.matColor);
  if (!cc.lightingEnabled) {
    return material;
  }
  F4 lighting = cc.ambSrc == GX_SRC_VTX ? vertexColour : f4(state.ambColor);
  for (u32 l = 0; l < GX::MaxLights; ++l) {
    if (!state.lightMask.test(l)) {
      continue;
    }
    const Light& light = st.lights[l];
    F3 direction = xyz(light.pos) - position;
    const float distance2 = dot(direction, direction);
    const float distance = std::sqrt(distance2);
    if (distance > 1e-10f) {
      direction = {direction.x / distance, direction.y / distance, direction.z / distance};
    }
    float attenuation = 1.f;
    if (cc.attnFn == GX_AF_SPOT) {
      const float cosine = std::max(0.f, dot(direction, xyz(light.dir)));
      const float cosAtt = light.cosAtt[0] + light.cosAtt[1] * cosine + light.cosAtt[2] * cosine * cosine;
      const float distAtt = light.distAtt[0] + light.distAtt[1] * distance + light.distAtt[2] * distance2;
      attenuation = distAtt > 1e-10f ? std::max(0.f, cosAtt / distAtt) : 0.f;
    } else if (cc.attnFn == GX_AF_SPEC) {
      float a = dot(normal, direction) >= 0.f ? std::max(0.f, dot(normal, xyz(light.dir))) : 0.f;
      const float cosAtt = light.cosAtt[0] + light.cosAtt[1] * a + light.cosAtt[2] * a * a;
      F3 k = xyz(light.distAtt);
      if (cc.diffFn != GX_DF_NONE) {
        k = normalized(k);
      }
      const float distAtt = std::max(0.f, k.x + k.y * a + k.z * a * a);
      attenuation = distAtt > 1e-10f ? std::max(0.f, cosAtt / distAtt) : 0.f;
    }
    float diffuse = 1.f;
    if (cc.diffFn == GX_DF_SIGN) {
      diffuse = dot(direction, normal);
    } else if (cc.diffFn == GX_DF_CLAMP) {
      diffuse = std::max(0.f, dot(direction, normal));
    }
    for (int c = 0; c < 4; ++c) {
      lighting[c] += attenuation * diffuse * light.color[c];
    }
  }
  F4 out;
  for (int c = 0; c < 4; ++c) {
    out[c] = material[c] * std::clamp(lighting[c], 0.f, 1.f);
  }
  return out;
}

// --- Per draw.

// --- Framebuffer copies a blended draw samples, read back from the GPU: masks (character shadows:
// the shadow is drawn into the EFB from the light, copied, and projected onto the ground through
// the copy's alpha) and pictures of the screen (water's reflection layer, which samples a copy of
// the scene through a projection). The FIFO thread notes each and, right after the draw, pushes a
// GPU task that renders the copy, small, into a readable texture; the pixels reach the window a
// frame later, as a texture updated in place. (Screen effects after the 3D world don't come
// here: the window lays them on its glass.)


// A copy the draw samples, as a texture to capture; null if it isn't one.
// record() picks the id and pushes the capture once it knows the swizzle.
TexEntry* capture_entry(const ShaderConfig& config, int texMap, gfx::TextureHandle* handle, CopySlot** slotOut,
                        size_t* occurrence, uint32_t* revision) {
  // AURORA_MIRROR_COPY_COLOUR=0 (a test switch): only masks, as before the screen effects moved
  // to the window's glass.
  static const bool colour = [] {
    const char* value = std::getenv("AURORA_MIRROR_COPY_COLOUR");
    return value == nullptr || std::strcmp(value, "0") != 0;
  }();
  for (u32 i = 0; !colour && i < config.tevStageCount; ++i) {
    const TevStage& stage = config.tevStages[i];
    if (stage.texMapId != texMap) {
      continue;
    }
    const auto& c = stage.colorPass;
    if (c.a == GX_CC_TEXC || c.b == GX_CC_TEXC || c.c == GX_CC_TEXC || c.d == GX_CC_TEXC) {
      return nullptr; // its colour: a picture of the screen, not a mask
    }
  }
  const auto& obj = g_gxState.loadedTextures[texMap];
  const auto copy = g_gxState.copyTextures.find(obj.data);
  if (copy == g_gxState.copyTextures.end() || !copy->second.handle) {
    return nullptr;
  }
  CopySlot& slot = g_copySlots[obj.data];
  if (slot.frame != g_frame) {
    slot.frame = g_frame;
    slot.occurrence = 0;
    slot.revision = copy->second.revision;
  } else if (slot.revision != copy->second.revision) {
    ++slot.occurrence;
    slot.revision = copy->second.revision;
  }
  while (slot.entries.size() <= slot.occurrence) {
    TexEntry& entry = slot.entries.emplace_back();
    entry.ok = true;
    entry.captured = true;
    entry.average = {0.f, 0.f, 0.f, 0.5f};
    slot.variants.emplace_back();
  }
  *handle = copy->second.handle;
  *slotOut = &slot;
  *occurrence = slot.occurrence;
  *revision = slot.revision;
  return &slot.entries[slot.occurrence];
}

// The GPU side, on the render worker: each copy drawn into a 256 x 256 RGBA8 target (a box
// filter over the texels it covers), copied to a buffer, mapped after the frame is submitted.
constexpr uint32_t kCaptureSize = 256;
constexpr uint64_t kCaptureBytes = uint64_t{kCaptureSize} * kCaptureSize * 4;
constexpr char kCaptureShader[] = R"(
@group(0) @binding(0) var source: texture_2d<f32>;

struct VertexOut {
  @builtin(position) position: vec4f,
};

@vertex
fn vs(@builtin(vertex_index) index: u32) -> VertexOut {
  let p = vec2f(f32((index << 1u) & 2u), f32(index & 2u));
  var out: VertexOut;
  out.position = vec4f(p * 2.0 - 1.0, 0.0, 1.0);
  return out;
}

@fragment
fn fs(in: VertexOut) -> @location(0) vec4f {
  let size = vec2i(textureDimensions(source));
  let scale = vec2f(size) / 256.0;
  let first = vec2i(floor((in.position.xy - 0.5) * scale));
  let span = max(vec2i(ceil(scale)), vec2i(1));
  var sum = vec4f(0.0);
  for (var y = 0; y < span.y; y++) {
    for (var x = 0; x < span.x; x++) {
      sum += textureLoad(source, clamp(first + vec2i(x, y), vec2i(0), size - 1), 0);
    }
  }
  return sum / f32(span.x * span.y);
}
)";

struct Readback {
  uint64_t id = 0;
  uint8_t swizzle = kIdentitySwizzle;
  bool mask = false;
  uint8_t threshold = 0;
  wgpu::Texture target;
  wgpu::Buffer buffer;
  std::atomic<bool> busy{false};
};

std::vector<std::unique_ptr<Readback>> g_readbacks; // render worker
wgpu::RenderPipeline g_capturePipeline;
wgpu::BindGroupLayout g_captureLayout;
bool g_captureFailed = false;
gfx::EncoderTaskId g_captureTask = gfx::InvalidEncoderTask;
std::array<Readback*, kCaptureRing> g_captureEncoded{}; // render worker: each ring slot's readback

bool ensure_capture_pipeline(const wgpu::Device& device) {
  if (g_capturePipeline || g_captureFailed) {
    return !g_captureFailed;
  }
  wgpu::ShaderSourceWGSL wgsl{};
  wgsl.code = kCaptureShader;
  wgpu::ShaderModuleDescriptor moduleDesc{};
  moduleDesc.nextInChain = &wgsl;
  moduleDesc.label = "Mirror capture";
  wgpu::ShaderModule module = device.CreateShaderModule(&moduleDesc);
  wgpu::BindGroupLayoutEntry entry{};
  entry.binding = 0;
  entry.visibility = wgpu::ShaderStage::Fragment;
  entry.texture.sampleType = wgpu::TextureSampleType::UnfilterableFloat;
  entry.texture.viewDimension = wgpu::TextureViewDimension::e2D;
  wgpu::BindGroupLayoutDescriptor layoutDesc{};
  layoutDesc.entryCount = 1;
  layoutDesc.entries = &entry;
  g_captureLayout = device.CreateBindGroupLayout(&layoutDesc);
  wgpu::PipelineLayoutDescriptor pipelineLayoutDesc{};
  pipelineLayoutDesc.bindGroupLayoutCount = 1;
  pipelineLayoutDesc.bindGroupLayouts = &g_captureLayout;
  wgpu::PipelineLayout layout = device.CreatePipelineLayout(&pipelineLayoutDesc);
  wgpu::ColorTargetState target{};
  target.format = wgpu::TextureFormat::RGBA8Unorm;
  wgpu::FragmentState fs{};
  fs.module = module;
  fs.entryPoint = "fs";
  fs.targetCount = 1;
  fs.targets = &target;
  wgpu::RenderPipelineDescriptor desc{};
  desc.label = "Mirror capture";
  desc.layout = layout;
  desc.vertex.module = module;
  desc.vertex.entryPoint = "vs";
  desc.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
  desc.fragment = &fs;
  g_capturePipeline = device.CreateRenderPipeline(&desc);
  g_captureFailed = !g_capturePipeline;
  if (g_captureFailed) {
    Log.warn("mirror: the capture pipeline failed; shadows won't show");
  }
  return !g_captureFailed;
}

Readback* readback_for(const wgpu::Device& device, uint64_t id) {
  for (auto& readback : g_readbacks) {
    if (readback->id == id) {
      return readback.get();
    }
  }
  auto readback = std::make_unique<Readback>();
  readback->id = id;
  wgpu::TextureDescriptor textureDesc{};
  textureDesc.label = "Mirror capture";
  textureDesc.size = {kCaptureSize, kCaptureSize, 1};
  textureDesc.format = wgpu::TextureFormat::RGBA8Unorm;
  textureDesc.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc;
  readback->target = device.CreateTexture(&textureDesc);
  wgpu::BufferDescriptor bufferDesc{};
  bufferDesc.label = "Mirror capture readback";
  bufferDesc.size = kCaptureBytes;
  bufferDesc.usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst;
  readback->buffer = device.CreateBuffer(&bufferDesc);
  if (!readback->target || !readback->buffer) {
    return nullptr;
  }
  g_readbacks.push_back(std::move(readback));
  return g_readbacks.back().get();
}

bool capturable_format(wgpu::TextureFormat format) {
  switch (format) {
  case wgpu::TextureFormat::RGBA8Unorm:
  case wgpu::TextureFormat::BGRA8Unorm:
  case wgpu::TextureFormat::R8Unorm:
  case wgpu::TextureFormat::RG8Unorm:
  case wgpu::TextureFormat::RGBA16Float:
    return true;
  default:
    return false;
  }
}

void capture_task(const gfx::EncoderTaskContext& ctx, const wgpu::CommandEncoder& cmd, const void* payload, size_t,
                  void*) {
  const uint32_t slot = *static_cast<const uint32_t*>(payload) % kCaptureRing;
  Capture capture;
  {
    std::lock_guard lock(g_captureMutex);
    capture = g_captureRing[slot];
  }
  g_captureEncoded[slot] = nullptr;
  const auto& ref = capture.handle;
  if (!ref || !ref->sampleTextureView || !capturable_format(ref->format) || !ensure_capture_pipeline(ctx.device)) {
    return;
  }
  Readback* readback = readback_for(ctx.device, capture.id);
  if (readback == nullptr || readback->busy) {
    return; // the last one is still on its way: this one is skipped
  }
  wgpu::CommandEncoder encoder = cmd;
  wgpu::BindGroupEntry entry{};
  entry.binding = 0;
  entry.textureView = ref->sampleTextureView;
  wgpu::BindGroupDescriptor groupDesc{};
  groupDesc.layout = g_captureLayout;
  groupDesc.entryCount = 1;
  groupDesc.entries = &entry;
  wgpu::BindGroup group = ctx.device.CreateBindGroup(&groupDesc);
  wgpu::RenderPassColorAttachment colour{};
  colour.view = readback->target.CreateView();
  colour.loadOp = wgpu::LoadOp::Clear;
  colour.storeOp = wgpu::StoreOp::Store;
  colour.clearValue = {0, 0, 0, 0};
  wgpu::RenderPassDescriptor passDesc{};
  passDesc.label = "Mirror capture";
  passDesc.colorAttachmentCount = 1;
  passDesc.colorAttachments = &colour;
  wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&passDesc);
  pass.SetPipeline(g_capturePipeline);
  pass.SetBindGroup(0, group);
  pass.Draw(3);
  pass.End();
  wgpu::TexelCopyTextureInfo source{};
  source.texture = readback->target;
  wgpu::TexelCopyBufferInfo destination{};
  destination.buffer = readback->buffer;
  destination.layout.bytesPerRow = kCaptureSize * 4;
  destination.layout.rowsPerImage = kCaptureSize;
  const wgpu::Extent3D size{kCaptureSize, kCaptureSize, 1};
  encoder.CopyTextureToBuffer(&source, &destination, &size);
  readback->swizzle = capture.swizzle;
  readback->mask = capture.mask;
  readback->threshold = capture.threshold;
  readback->busy = true;
  g_captureEncoded[slot] = readback;
}

// After the frame's submission: the readbacks mapped; their pixels go to the window as updates.
void capture_submitted(const gfx::EncoderTaskCompletionContext&, const void* payload, size_t, void*) {
  const uint32_t slot = *static_cast<const uint32_t*>(payload) % kCaptureRing;
  if (Readback* readback = std::exchange(g_captureEncoded[slot], nullptr)) {
    readback->buffer.MapAsync(
        wgpu::MapMode::Read, 0, kCaptureBytes, wgpu::CallbackMode::AllowSpontaneous,
        [readback](wgpu::MapAsyncStatus status, wgpu::StringView) {
          if (status == wgpu::MapAsyncStatus::Success) {
            const auto* data = static_cast<const uint8_t*>(readback->buffer.GetConstMappedRange(0, kCaptureBytes));
            std::vector<uint8_t> rgba(data, data + kCaptureBytes);
            readback->buffer.Unmap();
            if (const uint8_t swizzle = readback->swizzle; swizzle != kIdentitySwizzle) {
              for (size_t i = 0; i < rgba.size(); i += 4) {
                const uint8_t texel[4] = {rgba[i], rgba[i + 1], rgba[i + 2], rgba[i + 3]};
                for (int c = 0; c < 4; ++c) {
                  rgba[i + c] = texel[(swizzle >> (2 * c)) & 3];
                }
              }
            }
            if (readback->mask) {
              const uint8_t threshold = readback->threshold;
              for (size_t i = 3; i < rgba.size(); i += 4) {
                rgba[i] = rgba[i] > threshold ? 255 : 0;
              }
            }
            // Test runs: AURORA_MIRROR_CAPTURE_LOG=1 logs each read-back mask's alpha (a shadow's
            // darkness): a mask almost all opaque draws as a dark square under its actor.
            static const bool s_captureLog = [] {
              const char* value = std::getenv("AURORA_MIRROR_CAPTURE_LOG");
              return value != nullptr && std::strcmp(value, "1") == 0;
            }();
            if (s_captureLog) {
              static std::atomic<uint32_t> logged{0};
              double sum = 0;
              size_t opaque = 0, clear = 0;
              for (size_t i = 3; i < rgba.size(); i += 4) {
                sum += rgba[i];
                opaque += rgba[i] > 230;
                clear += rgba[i] < 25;
              }
              const double texels = static_cast<double>(rgba.size() / 4);
              const bool square = static_cast<double>(opaque) / texels > 0.9;
              if (square || logged.fetch_add(1) % 120 == 0) {
                Log.info("mirror: capture {} swizzle {:#04x}: alpha mean {:.2f}, {:.0f}% opaque, {:.0f}% clear{}",
                         readback->id, readback->swizzle, sum / texels / 255.0, 100.0 * opaque / texels,
                         100.0 * clear / texels, square ? "  <<< almost all opaque" : "");
              }
            }
            std::lock_guard lock(g_mutex);
            // Only the newest: an older one still waiting for the window is replaced.
            std::erase_if(g_pendingTextures, [&](const OwnedTexture& t) { return t.id == readback->id; });
            g_pendingTextures.push_back({readback->id, std::move(rgba), kCaptureSize, kCaptureSize, false, false});
          }
          readback->busy = false;
        });
  }
}

struct DrawPlan {
  int primaryMap = -1;
  int primaryCoord = -1;
  TexEntry* primary = nullptr;
  std::array<std::array<float, 4>, MaxTextures> average{};
  bool rasterUsed[2] = {false, false};
  bool alphaAsColour[2] = {false, false};
  // A framebuffer copy it samples (refraction, water): the unknown B, what's behind the surface.
  int behindMap = -1;
  // The primary texture is a mask read back from a framebuffer copy (a shadow): which copy, which
  // time this frame it was made, and its revision.
  gfx::TextureHandle captureHandle;
  CopySlot* captureSlot = nullptr;
  size_t captureOccurrence = 0;
  uint32_t captureRevision = 0;
  TevContext ctx;
  // Per stage: its konst colour and alpha.
  std::array<std::array<float, 4>, MaxTevStages> konst{};
  // 0104: textures read per vertex (lookups, by map), the coordinate each is read with, its wraps.
  struct Lookup {
    const LookupPixels* pixels = nullptr;
    int coord = -1;
    uint8_t wrapS = 0, wrapT = 0;
  };
  std::array<Lookup, MaxTextures> lookups{};
  bool anyLookup = false;
  // A texture coordinate the vertices need comes from a colour channel (GX's COLOR0/1 texgen
  // sources are the channels' lit output).
  bool colourCoords = false;
};

// 0104 test switch: AURORA_MIRROR_LOOKUPS=0 averages every texture but the kept one again (no
// toon shading), for the before pictures.
bool switch_on(const char* name) {
  const char* value = std::getenv(name);
  return value == nullptr || std::strcmp(value, "0") != 0;
}
const bool kLookups = switch_on("AURORA_MIRROR_LOOKUPS");
// 0105 test switch: AURORA_MIRROR_OVER_HAIR=0 reads the eyes' blend as before (additive), with no
// bake.
const bool kOverHair = switch_on("AURORA_MIRROR_OVER_HAIR");
// 0107 test switch: AURORA_MIRROR_REAL_SHADOWS=0 reads a real-time shadow's blend as before
// (additive: no shadow).
const bool kRealShadows = switch_on("AURORA_MIRROR_REAL_SHADOWS");
// 0108 test switch: AURORA_MIRROR_BAKE=0 bakes two textures into one only for the eyes over the
// hair, as 0105 did.
const bool kBakePairs = switch_on("AURORA_MIRROR_BAKE");

bool colour_coord(const ShaderConfig& config, int coord) {
  if (coord < 0 || coord >= static_cast<int>(MaxTexCoord)) {
    return false;
  }
  const TcgConfig& tcg = config.tcgs[coord];
  return tcg.src == GX_TG_COLOR0 || tcg.src == GX_TG_COLOR1;
}

// Which texture the mirror keeps, the others' averages, and a framebuffer copy it samples.
// How much a texture shows of what a draw looks like, to pick the one the mirror keeps: a colour
// picture over an intensity mask, mapped by the model's own coordinates over a projection.
int texture_rank(const ShaderConfig& config, const TevStage& stage, const GXTexObj_& obj) {
  const u32 format = obj.format();
  const bool colour = !(format == GX_TF_I4 || format == GX_TF_I8 || format == GX_TF_IA4 || format == GX_TF_IA8);
  bool mapped = false;
  if (stage.texCoordId < MaxTexCoord) {
    const TcgConfig& tcg = config.tcgs[stage.texCoordId];
    mapped = tcg.src >= GX_TG_TEX0 && tcg.src <= GX_TG_TEX7 && (tcg.type == GX_TG_MTX2x4 || tcg.type == GX_TG_MTX3x4);
  }
  return (colour ? 2 : 0) + (mapped ? 1 : 0);
}

bool plan_draw(const ShaderConfig& config, DrawPlan& plan, bool blended) {
  for (auto& average : plan.average) {
    average = {0.5f, 0.5f, 0.5f, 1.f};
  }
  // Each map's entry, looked up once (0104: a draw's stages often sample one map twice, and each
  // look-up hashes the texture).
  std::array<TexEntry*, MaxTextures> entries{};
  std::array<bool, MaxTextures> looked{};
  const auto entry_of = [&](u32 map) {
    if (!looked[map]) {
      looked[map] = true;
      entries[map] = texture_entry(g_gxState.loadedTextures[map]);
    }
    return entries[map];
  };
  // The texture to keep: the best ranked, the earliest of those. (TP's eyes sample a grey mask
  // first and the eye's own picture, iris and all, second: kept first, the irises vanished.)
  int keep = -1, best = -1;
  for (u32 i = 0; i < config.tevStageCount; ++i) {
    const TevStage& stage = config.tevStages[i];
    if (!samples_texture(stage) || stage.texMapId >= MaxTextures) {
      continue;
    }
    const auto& obj = g_gxState.loadedTextures[stage.texMapId];
    // A blend sampling a screen copy first keeps the copy, read back (a heat shimmer's warped
    // picture; kept as a mask's, it drew a flat grey smudge in the mask's shape).
    if (blended && best < 0 && obj.has_data() && g_gxState.copyTextures.contains(obj.data)) {
      break;
    }
    const TexEntry* entry = entry_of(stage.texMapId);
    if (entry == nullptr || !entry->ok) {
      continue;
    }
    const int rank = texture_rank(config, stage, obj);
    if (rank > best) {
      best = rank;
      keep = stage.texMapId;
    }
  }
  for (u32 i = 0; i < config.tevStageCount; ++i) {
    const TevStage& stage = config.tevStages[i];
    if (stage.channelId != GX_COLOR_NULL && stage.channelId != GX_COLOR_ZERO && !is_bump_channel(stage.channelId)) {
      plan.rasterUsed[color_channel(stage.channelId) & 1] = true;
    }
    if (!samples_texture(stage) || stage.texMapId >= MaxTextures) {
      continue;
    }
    TexEntry* entry = entry_of(stage.texMapId);
    if (entry == nullptr && blended && plan.primary == nullptr && keep < 0) {
      entry = capture_entry(config, stage.texMapId, &plan.captureHandle, &plan.captureSlot, &plan.captureOccurrence,
                            &plan.captureRevision);
    }
    if (entry == nullptr || !entry->ok) {
      if (entry == nullptr && plan.behindMap < 0) {
        plan.behindMap = stage.texMapId;
      }
      continue;
    }
    plan.average[stage.texMapId] = entry->average;
    if (plan.primary == nullptr && (keep < 0 || stage.texMapId == keep)) {
      plan.primary = entry;
      plan.primaryMap = stage.texMapId;
      plan.primaryCoord = stage.texCoordId != GX_TEXCOORD_NULL ? stage.texCoordId : -1;
    }
  }
  if (plan.primary != nullptr) {
    plan.ctx.primaryAverage = plan.primary->average;
  }
  // 0104: the other textures read with coordinates from a lit colour channel (toon ramps) are
  // lookups, read at each vertex; the channels they read are lit.
  for (u32 i = 0; i < config.tevStageCount && kLookups; ++i) {
    const TevStage& stage = config.tevStages[i];
    if (!samples_texture(stage) || stage.texMapId >= MaxTextures ||
        static_cast<int>(stage.texMapId) == plan.primaryMap || !colour_coord(config, stage.texCoordId)) {
      continue;
    }
    auto& lookup = plan.lookups[stage.texMapId];
    TexEntry* entry = looked[stage.texMapId] ? entries[stage.texMapId] : nullptr;
    if (lookup.pixels || entry == nullptr || !entry->ok) {
      continue;
    }
    const auto& obj = g_gxState.loadedTextures[stage.texMapId];
    lookup.pixels = lookup_pixels(*entry, obj);
    if (!lookup.pixels) {
      continue;
    }
    lookup.coord = stage.texCoordId;
    lookup.wrapS = static_cast<uint8_t>(obj.wrap_s());
    lookup.wrapT = static_cast<uint8_t>(obj.wrap_t());
    plan.anyLookup = true;
    plan.colourCoords = true;
    plan.rasterUsed[config.tcgs[stage.texCoordId].src == GX_TG_COLOR1 ? 1 : 0] = true;
  }
  if (plan.primary != nullptr && colour_coord(config, plan.primaryCoord) && kLookups) {
    plan.colourCoords = true;
    plan.rasterUsed[config.tcgs[plan.primaryCoord].src == GX_TG_COLOR1 ? 1 : 0] = true;
  }
  for (u32 i = 0; i < std::min<u32>(config.tevStageCount, MaxTevStages); ++i) {
    const TevStage& stage = config.tevStages[i];
    for (int c = 0; c < 3; ++c) {
      plan.konst[i][c] = konst_colour(stage.kcSel, c);
    }
    plan.konst[i][3] = konst_alpha(stage.kaSel);
  }
  for (int c = 0; c < 2; ++c) {
    const auto& colour = g_gxState.colorChannelConfig[c];
    const auto& alpha = g_gxState.colorChannelConfig[c + GX_ALPHA0];
    plan.alphaAsColour[c] = colour == alpha && g_gxState.colorChannelState[c].lightMask ==
                                                   g_gxState.colorChannelState[c + GX_ALPHA0].lightMask;
  }
  return true;
}

// GX's alpha test as a threshold: the least alpha it passes, when it passes everything above
// that; -1 when it passes everything (or isn't a threshold), 2 when it passes nothing.
float alpha_threshold(const AlphaCompare& compare) {
  if (!compare) {
    return -1.f;
  }
  const auto test = [](GXCompare comp, u32 ref, u32 a) {
    switch (comp) {
    case GX_NEVER:
      return false;
    case GX_LESS:
      return a < ref;
    case GX_EQUAL:
      return a == ref;
    case GX_LEQUAL:
      return a <= ref;
    case GX_GREATER:
      return a > ref;
    case GX_NEQUAL:
      return a != ref;
    case GX_GEQUAL:
      return a >= ref;
    default:
      return true;
    }
  };
  const auto passes = [&](u32 a) {
    const bool p0 = test(compare.comp0, compare.ref0, a), p1 = test(compare.comp1, compare.ref1, a);
    switch (compare.op) {
    case GX_AOP_AND:
      return p0 && p1;
    case GX_AOP_OR:
      return p0 || p1;
    case GX_AOP_XOR:
      return p0 != p1;
    default:
      return p0 == p1;
    }
  };
  int first = -1;
  for (u32 a = 0; a < 256; ++a) {
    if (passes(a)) {
      if (first < 0) {
        first = static_cast<int>(a);
      }
    } else if (first >= 0) {
      return -1.f; // not a threshold
    }
  }
  if (first < 0) {
    return 2.f;
  }
  return first == 0 ? -1.f : static_cast<float>(first) / 255.f;
}

// Blend weights from GX's factors: colour = rgb * (base + alpha * a); opacity = 1 - dst factor.
bool blend_weights(MirrorPart& part) {
  const auto& st = g_gxState;
  if (st.blendMode == GX_BM_NONE || (st.blendMode == GX_BM_LOGIC && st.blendOp == GX_LO_COPY)) {
    return true;
  }
  if (st.blendMode != GX_BM_BLEND) {
    return false; // subtractive and other logic ops
  }
  part.kind = aurora::mirror::PartBlend;
  float colourBase = 0, colourAlpha = 0, opacityBase = 1, opacityAlpha = 0, opacityLuma = 0;
  switch (st.blendFacSrc) {
  case GX_BL_ZERO:
    break;
  case GX_BL_ONE:
    colourBase = 1;
    break;
  case GX_BL_SRCALPHA:
    colourAlpha = 1;
    break;
  case GX_BL_INVSRCALPHA:
    colourBase = 1;
    colourAlpha = -1;
    break;
  case GX_BL_DSTCLR: // src * dst: a darkening by src
    part.colourBase = 0;
    part.colourAlpha = 0;
    part.opacityBase = 1;
    part.opacityAlpha = 0;
    part.opacityLuma = -1;
    return st.blendFacDst == GX_BL_ZERO;
  default:
    colourBase = 1;
    break;
  }
  switch (st.blendFacDst) {
  case GX_BL_ZERO:
    opacityBase = 1;
    break;
  case GX_BL_ONE:
    opacityBase = 0;
    break;
  case GX_BL_SRCALPHA:
    opacityBase = 1;
    opacityAlpha = -1;
    break;
  case GX_BL_INVSRCALPHA:
    opacityBase = 0;
    opacityAlpha = 1;
    break;
  case GX_BL_SRCCLR:
    opacityBase = 1;
    opacityLuma = -1;
    break;
  case GX_BL_INVSRCCLR:
    opacityBase = 0;
    opacityLuma = 1;
    break;
  default:
    opacityBase = 0;
    break;
  }
  part.colourBase = colourBase;
  part.colourAlpha = colourAlpha;
  part.opacityBase = opacityBase;
  part.opacityAlpha = opacityAlpha;
  part.opacityLuma = opacityLuma;
  // A blend that's opaque after all (src one, dst zero).
  if (colourBase == 1 && colourAlpha == 0 && opacityBase == 1 && opacityAlpha == 0 && opacityLuma == 0) {
    part.kind = aurora::mirror::PartOpaque;
  }
  return true;
}

uint64_t part_key(const MirrorPart& part) {
  struct Key {
    uint64_t texture;
    uint32_t kind, flags;
    int32_t cutoff, weights[5];
    uint8_t wrapS, wrapT;
  } key{};
  key.texture = part.texture;
  key.kind = part.kind;
  key.flags = part.flags;
  key.cutoff = static_cast<int32_t>(std::lround(part.cutoff * 255.f));
  const float weights[5] = {part.colourBase, part.colourAlpha, part.opacityBase, part.opacityAlpha, part.opacityLuma};
  for (int i = 0; i < 5; ++i) {
    key.weights[i] = static_cast<int32_t>(std::lround(weights[i] * 16.f));
  }
  key.wrapS = part.wrapS;
  key.wrapT = part.wrapT;
  return xxh3_hash_s(&key, sizeof key);
}

std::vector<uint32_t> take_spare_indices() {
  if (g_spareIndices.empty()) {
    return {};
  }
  std::vector<uint32_t> indices = std::move(g_spareIndices.back());
  g_spareIndices.pop_back();
  g_spareBytes -= indices.capacity() * sizeof(uint32_t);
  return indices;
}

Group& group_for(const MirrorPart& part) {
  const uint64_t key = part_key(part);
  if (part.kind != aurora::mirror::PartBlend) {
    if (auto found = g_solidGroups.find(key); found != g_solidGroups.end()) {
      return g_groups[found->second];
    }
    g_solidGroups.emplace(key, g_groups.size());
    g_groups.push_back({key, part, take_spare_indices(), false});
    return g_groups.back();
  }
  // Blended draws keep the game's order: a run of one material is one group.
  if (g_lastBlended >= 0 && g_groups[g_lastBlended].key == key) {
    return g_groups[g_lastBlended];
  }
  g_lastBlended = static_cast<int>(g_groups.size());
  g_groups.push_back({key, part, take_spare_indices(), true});
  return g_groups.back();
}

// The texture coordinate GX generates for `coord` at vertex `v`.
std::array<float, 2> tex_coord(const DrawState& st, const Decoder& decoder, const ShaderConfig& config, int coord,
                               uint32_t v, const float position[3], F3 normal, const F4 colours[2], int depth = 0) {
  if (coord < 0 || coord >= static_cast<int>(MaxTexCoord) || depth > 2) {
    return {0, 0};
  }
  const TcgConfig& tcg = config.tcgs[coord];
  if (tcg.type >= GX_TG_BUMP0 && tcg.type <= GX_TG_BUMP7) {
    return tex_coord(st, decoder, config, tcg.embossSrc, v, position, normal, colours, depth + 1);
  }
  float src[4] = {0, 0, 1, 1};
  if (tcg.src >= GX_TG_TEX0 && tcg.src <= GX_TG_TEX7) {
    float uv[2] = {0, 0};
    decoder.numbers(static_cast<GXAttr>(GX_VA_TEX0 + (tcg.src - GX_TG_TEX0)), v,
                    std::min<int>(config.attrs[GX_VA_TEX0 + (tcg.src - GX_TG_TEX0)].cnt, 2), uv);
    src[0] = uv[0];
    src[1] = uv[1];
  } else if (tcg.src == GX_TG_POS) {
    src[0] = position[0];
    src[1] = position[1];
    src[2] = position[2];
  } else if (tcg.src == GX_TG_NRM) {
    src[0] = normal.x;
    src[1] = normal.y;
    src[2] = normal.z;
  } else if (tcg.src == GX_TG_COLOR0 || tcg.src == GX_TG_COLOR1) {
    const F4& c = colours[tcg.src == GX_TG_COLOR1];
    for (int i = 0; i < 4; ++i) {
      src[i] = c[i];
    }
  }
  F3 tmp{src[0], src[1], src[2]};
  if (tcg.type == GX_TG_MTX2x4 || tcg.type == GX_TG_MTX3x4) {
    const GXAttr mtxAttr = static_cast<GXAttr>(GX_VA_TEX0MTXIDX + coord);
    if (config.attrs[mtxAttr].attrType != GX_NONE) {
      tmp = transform(postex_matrix(st, decoder.u8(mtxAttr, v, 0) / 3), src[0], src[1], src[2], src[3]);
    } else if (tcg.mtx != GX_IDENTITY) {
      tmp = transform(postex_matrix(st, tcg.mtx / 3), src[0], src[1], src[2], src[3]);
    }
    if (tcg.type == GX_TG_MTX2x4) {
      tmp.z = 1.f;
    }
  } else {
    tmp.z = 1.f;
  }
  if (tcg.normalize) {
    tmp = normalized(tmp);
  }
  if (tcg.postMtx != GX_PTIDENTITY) {
    const u32 index = std::min<u32>((tcg.postMtx - GX_PTTEXMTX0) / 3, MaxPTTexMtx - 1);
    tmp = transform(st.ptTexMtxs[index], tmp.x, tmp.y, tmp.z, 1.f);
  }
  if (tcg.type == GX_TG_MTX3x4 && std::fabs(tmp.z) > 1e-6f) {
    return {tmp.x / tmp.z, tmp.y / tmp.z};
  }
  return {tmp.x, tmp.y};
}

// The fog factor at a view-space point, as the fragment shader computes it from its depth.
float fog_at(const DrawState& st, F3 view) {
  const auto& fog = st.fog;
  if (fog.type == GX_FOG_NONE) {
    return 0.f;
  }
  const auto& p = st.proj;
  float depth;
  if ((fog.type & 0x08) != 0) {
    depth = p.m2[2] * view.z + p.m2[3]; // orthographic: z' directly
    depth = 1.f - depth;
  } else {
    // GX's depth: 1 - (p4 z + p5) / z (0 at the near plane, 1 at the far one).
    depth = std::fabs(view.z) > 1e-6f ? 1.f - (p.m2[2] * view.z + p.m2[3]) / view.z : 0.f;
  }
  float base = (fog.type & 0x08) != 0 ? fog.a * depth : (std::fabs(fog.b - depth) > 1e-9f ? fog.a / (fog.b - depth) : 0.f);
  float f = std::clamp(base - fog.c, 0.f, 1.f);
  float z;
  switch (fog.type) {
  case GX_FOG_PERSP_EXP:
  case GX_FOG_ORTHO_EXP:
    z = 1.f - std::exp2(-8.f * f);
    break;
  case GX_FOG_PERSP_EXP2:
  case GX_FOG_ORTHO_EXP2:
    z = 1.f - std::exp2(-8.f * f * f);
    break;
  case GX_FOG_PERSP_REVEXP:
  case GX_FOG_ORTHO_REVEXP:
    z = std::exp2(-8.f * (1.f - f));
    break;
  case GX_FOG_PERSP_REVEXP2:
  case GX_FOG_ORTHO_REVEXP2:
    f = 1.f - f;
    z = std::exp2(-8.f * f * f);
    break;
  default:
    z = f;
    break;
  }
  return std::clamp(z, 0.f, 1.f);
}

// 0107 test switch: AURORA_MIRROR_FOG_SPLIT=0 fogs at the vertices only, as before.
const bool kFogSplit = switch_on("AURORA_MIRROR_FOG_SPLIT");
// The most a fog factor may change along a triangle's edge before the edge is split, and how many
// times a triangle may be split (each halving its edges): a triangle from under the camera to the
// horizon becomes at most a few hundred.
constexpr float kFogStep = 1.f / 8.f;
constexpr int kFogSplits = 5;

// --- 0110: the framebuffer's alpha, followed through a frame.
//
// Wind Waker lights and darkens parts of its picture through the framebuffer's alpha. A torch's or
// a lantern's glow, a light tag's, a searchlight's beam (dDlst_alphaModel_c): spheres (or a cube, a
// beam) drawn into the alpha only, from behind with the depth test reversed, from the front, and
// from the front again subtracted, which leaves the sphere's alpha where the scene's surface lies
// inside it; then a quad over the whole picture adds a colour by that alpha (drawAlphaBuffer). A
// dark room (the spot model): the alpha set over the whole picture, Link's light added in two
// spheres around him, doubled, and the picture multiplied by it (drawSpot). The mirror left all of
// it out: the alpha-only draws write no colour, and the quads are flat.
//
// Now each alpha-only draw without a texture is kept as an operation on the alpha: a volume (its
// triangles, decoded on the workers, as the planes of a convex solid: which faces it draws, its
// depth test, its blend on the alpha, its alpha) or the whole picture (a flat quad). A flat quad
// drawn by the alpha is a layer, worked into what was drawn before it (build_alpha_layers): the
// alpha at a point found by playing the operations there (the ray from the camera through it: where
// it enters and leaves each volume, against the surface's depth), the ground cut along a light's
// volume where they meet. Built by the finishing worker.
struct AlphaOp {
  bool screen = false;             // the whole picture (a flat quad); else a volume
  bool front = true, back = true;  // the faces it draws (its cull mode)
  bool depthTest = false;
  GXCompare depthFunc = GX_ALWAYS;
  GXBlendMode mode = GX_BM_NONE;
  GXBlendFactor src = GX_BL_ONE, dst = GX_BL_ZERO;
  float alpha = 0.f;               // the TEV's alpha
  bool constant = false;           // GXSetDstAlpha: this alpha is written whatever the blend
  float constantAlpha = 0.f;
  // A volume's planes (inside where n.x <= d) and bounding sphere, from its draw (merge_jobs).
  std::vector<std::array<float, 4>> planes;
  float centre[3] = {0.f, 0.f, 0.f};
  float radius = 0.f;
  // A flat quad setting the alpha over the whole picture: the frame's draws before it (and a group
  // made in its place, which keeps the draws on either side apart), for a layer multiplying by the
  // alpha it sets up (see AlphaLayer).
  size_t jobs = 0;
  int group = -1;
};
struct AlphaLayer {
  size_t ops = 0;          // the operations before it
  uint32_t vertexEnd = 0;  // the frame's vertices drawn before it (merge_jobs)
  size_t group = 0;        // its place in the frame's groups (it draws nothing of its own)
  float colour[3] = {0.f, 0.f, 0.f};
  float alpha = 0.f;       // the quad's TEV alpha
  GXBlendFactor src = GX_BL_ONE, dst = GX_BL_ZERO;
  // A layer that multiplies (a dark room's) reads the alpha set up by the flat quad clearing it (job
  // `fromJob`): the translucent draws before it, and those after it that don't write the alpha
  // (particles do: as bright in the dark as out of it), are darkened in place, their vertices'
  // colours (vertex ranges, merge_jobs).
  size_t fromJob = SIZE_MAX, toJob = 0;
  std::vector<std::pair<uint32_t, uint32_t>> darken;
};
std::vector<AlphaOp> g_alphaOps;       // this frame's (FIFO thread)
std::vector<AlphaLayer> g_alphaLayers; // this frame's
float g_alphaStart = 0.f;              // the alpha the frame starts with (the copy's clear colour)
// 0110 test switch: AURORA_MIRROR_ALPHA_BUFFER=0 leaves the framebuffer's alpha out, as before.
const bool kAlphaBuffer = switch_on("AURORA_MIRROR_ALPHA_BUFFER");

// A blend factor's value on the alpha (src: the source's slot, where 2 and 3 are the destination's
// colour, as in the destination's slot they're the source's).
float alpha_factor(GXBlendFactor factor, bool srcSlot, float s, float d) {
  switch (factor) {
  case GX_BL_ZERO:
    return 0.f;
  case GX_BL_ONE:
    return 1.f;
  case GX_BL_SRCCLR:
    return srcSlot ? d : s;
  case GX_BL_INVSRCCLR:
    return srcSlot ? 1.f - d : 1.f - s;
  case GX_BL_SRCALPHA:
    return s;
  case GX_BL_INVSRCALPHA:
    return 1.f - s;
  case GX_BL_DSTALPHA:
    return d;
  case GX_BL_INVDSTALPHA:
    return 1.f - d;
  default:
    return 1.f;
  }
}

// What an operation leaves in the alpha over `d`.
float alpha_blend(const AlphaOp& op, float d) {
  if (op.constant) {
    return op.constantAlpha;
  }
  const float s = op.alpha;
  switch (op.mode) {
  case GX_BM_NONE:
    return s;
  case GX_BM_SUBTRACT:
    return std::clamp(d - s, 0.f, 1.f);
  case GX_BM_BLEND:
    return std::clamp(s * alpha_factor(op.src, true, s, d) + d * alpha_factor(op.dst, false, s, d), 0.f, 1.f);
  default:
    return d; // a logic op: left as it was
  }
}

bool depth_passes(GXCompare func, float fragment, float surface) {
  const float eps = 1e-4f * surface;
  switch (func) {
  case GX_NEVER:
    return false;
  case GX_LESS:
    return fragment < surface - eps;
  case GX_EQUAL:
    return std::fabs(fragment - surface) <= eps;
  case GX_LEQUAL:
    return fragment <= surface + eps;
  case GX_GREATER:
    return fragment > surface + eps;
  case GX_NEQUAL:
    return std::fabs(fragment - surface) > eps;
  case GX_GEQUAL:
    return fragment >= surface - eps;
  default:
    return true;
  }
}

// The alpha at a surface point p (view space) after the first `count` operations: each volume met
// by the ray from the camera through p (p at t = 1) where it enters (its front faces) and leaves
// (its back faces), each tested against the surface's depth. With `volumes` false, as if no volume
// covered p (the alpha away from them all).
float alpha_at(const std::vector<AlphaOp>& ops, size_t count, float start, const float p[3], bool volumes = true,
               const std::vector<uint8_t>* live = nullptr) {
  float a = start;
  const float pp = p[0] * p[0] + p[1] * p[1] + p[2] * p[2];
  // A volume is drawn two or three times running (from behind, from the front, taken off): where the
  // ray meets it, worked out once.
  const AlphaOp* last = nullptr;
  bool lastHit = false;
  float lastIn = 0.f, lastOut = 0.f;
  for (size_t i = 0; i < count && i < ops.size(); ++i) {
    if (live != nullptr && i < live->size() && !(*live)[i]) {
      continue;
    }
    const AlphaOp& op = ops[i];
    if (op.screen) {
      a = alpha_blend(op, a);
      continue;
    }
    if (!volumes || op.planes.empty() || pp <= 0.f) {
      continue;
    }
    float tIn = -INFINITY, tOut = INFINITY;
    bool hit = true;
    if (last != nullptr && last->radius == op.radius && last->centre[0] == op.centre[0] && last->centre[1] == op.centre[1] &&
        last->centre[2] == op.centre[2] && last->planes.size() == op.planes.size()) {
      hit = lastHit;
      tIn = lastIn;
      tOut = lastOut;
    } else {
      // The ray past the bounding sphere: nothing.
      const float cp = op.centre[0] * p[0] + op.centre[1] * p[1] + op.centre[2] * p[2];
      const float tc = cp / pp;
      float closest = 0.f;
      for (int c = 0; c < 3; ++c) {
        const float v = op.centre[c] - (tc > 0.f ? tc : 0.f) * p[c];
        closest += v * v;
      }
      if (closest > op.radius * op.radius) {
        hit = false;
      } else {
        for (const auto& plane : op.planes) {
          const float np = plane[0] * p[0] + plane[1] * p[1] + plane[2] * p[2];
          if (std::fabs(np) < 1e-12f) {
            if (plane[3] < 0.f) {
              hit = false; // the camera outside this plane, the ray along it
              break;
            }
            continue;
          }
          const float t = plane[3] / np;
          if (np > 0.f) {
            tOut = std::min(tOut, t);
          } else {
            tIn = std::max(tIn, t);
          }
        }
      }
      last = &op;
      lastHit = hit;
      lastIn = tIn;
      lastOut = tOut;
    }
    if (!hit || !(tIn <= tOut)) {
      continue;
    }
    constexpr float kNear = 1e-4f;
    if (op.back && tOut > kNear && (!op.depthTest || depth_passes(op.depthFunc, tOut, 1.f))) {
      a = alpha_blend(op, a);
    }
    if (op.front && tIn > kNear && (!op.depthTest || depth_passes(op.depthFunc, tIn, 1.f))) {
      a = alpha_blend(op, a);
    }
  }
  return a;
}

// --- Draws, decoded on worker threads. The FIFO thread snapshots what a draw's vertices read and
// moves on; the frame's END waits for them all and merges them in the game's order. The game's
// vertex arrays stay put until then: the game thread waits for the FIFO before its next frame.

struct DrawJob {
  DrawState state;
  ShaderConfig config;
  DrawPlan plan;
  GXPrimitive prim = GX_TRIANGLES;
  uint32_t vtxCount = 0;
  std::vector<u8> bytes;
  std::vector<u16> indices;
  std::array<int8_t, 4> channelSel{};
  bool behind = false;
  bool background = false;
  bool perVertex = true;
  std::array<Sym, 4> constantResult{};
  float nearClip = 1, kx = 1, ky = 1;
  size_t group = 0;
  // How much nearer the camera to draw it (scaling about the camera keeps the picture): RealityKit
  // can't settle coplanar surfaces as GX's LEQUAL test does, in favour of the later draw.
  float nearer = 1.f;
  // A decal: lifted towards the camera too, by 0.4% of its distance but at most kDecalLift.
  bool decal = false;
  // 0105: drawn this much nearer the camera (game units: an eye over the hair), and the texture is
  // a bake of the TEV's whole result (its colour and alpha as they are).
  float pull = 0.f;
  bool baked = false;
  // 0109: a bake_reads bake that also reads the vertex colour: per channel, a constant times each of
  // the two lit channels' rgba added, or (>= 0) the one that multiplies the bake.
  bool bakeRaster = false;
  float bakeAlpha = 1.f; // the bake's alpha times this (a blob shadow's darkness)
  float bakeUV[2] = {1.f, 1.f}; // the base coordinate's scale onto the bake (1 / its periods: bake_reads)
  int bakeRaw = -1; // bake_reads: the vertices' own coordinate (TEX0 + n) the bake is laid out in
  std::array<std::array<float, 8>, 4> bakeAdded{};
  std::array<int8_t, 4> bakeMultiplied{-1, -1, -1, -1};
  // 0107: each vertex's colour before its fog, and its fog: a draw whose fog changes much across
  // a triangle has that triangle split (build_triangles), each new vertex fogged where it is.
  // GX fogs each pixel by its depth; fogged at the vertices only, a big polygon (the sea's plane,
  // from under the camera to the horizon) took the horizon's haze far into the near sea.
  struct Prefog {
    float mul[4], add[4];
    float transmitted, fog;
  };
  bool fogged = false;
  std::vector<Prefog> prefog;
  // 0106: draws the command processor merged into this one (no GX state changed between them): each
  // primitive's first vertex and count in `bytes`. Empty: one primitive, `prim`, of every vertex.
  struct Segment {
    GXPrimitive prim;
    uint32_t first, count;
  };
  std::vector<Segment> segments;
  // 0110: an alpha-only volume (its operation's index: decoded, not drawn), or an alpha layer's place.
  int alphaOp = -1;
  int alphaLayer = -1;
  bool writesAlpha = false; // its pixels' alpha goes into the framebuffer's (a particle's)
  // Out: its vertices (clipping adds some) and triangles into them, wound for RealityKit.
  std::vector<MirrorVertex> vertices;
  std::vector<uint32_t> triangles;
  float nearest = INFINITY, farthest = 0;
};

std::vector<std::unique_ptr<DrawJob>> g_jobs;
size_t g_jobCount = 0;
// How many times this frame each (mesh, place) has been drawn.
absl::flat_hash_map<uint64_t, unsigned> g_passes; // (0106: a flat map, given last frame's room)
std::atomic<uint64_t> g_decodeNanos{0};
// The FIFO thread's CPU time over each scene (BEGIN to END: the command processor's own work and
// the mirror's recording and merging), and the finishing worker's: thread CPU time, which leaves
// out what other processes take of the cores (a Simulator on a busy Mac).
uint64_t thread_cpu_ns() {
#if defined(__APPLE__)
  return clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID);
#else
  struct timespec ts {};
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
#endif
}
std::atomic<uint64_t> g_sceneCpuNanos{0}, g_finishCpuNanos{0}, g_scenes{0};
uint64_t g_sceneCpuStart = 0;
#if defined(__APPLE__)
dispatch_group_t g_jobGroup = dispatch_group_create();
#endif

void light(const DrawJob& job, const F4 colours[2], F3 view, F3 normal, F4 raster[2]) {
  const auto& st = job.state;
  for (int c = 0; c < 2; ++c) {
    if (!job.plan.rasterUsed[c]) {
      continue;
    }
    const F4 rgb = light_channel(st, st.channelConfig[c], st.channelState[c], colours[c], view, normal);
    // The alpha channel, lit the same way as the colour (the usual case), is the colour's alpha.
    const F4 alpha = job.plan.alphaAsColour[c] ? rgb
                                               : light_channel(st, st.channelConfig[c + GX_ALPHA0],
                                                               st.channelState[c + GX_ALPHA0], colours[c], view, normal);
    raster[c] = {{rgb[0], rgb[1], rgb[2], alpha[3]}};
  }
}

// The TEV stages with T (and B) unknown, from a vertex's colour channels.
// lookups (0104): per map, its sample at this vertex (plan.lookups); null: averages. With
// `allKnown` (0105), the primary map's too (the stages are then all constants: baking).
std::array<Sym, 4> run_tev(const DrawJob& job, const F4 raster[2], const F4* lookups = nullptr, bool allKnown = false) {
  const auto& config = job.config;
  const auto& plan = job.plan;
  const auto& stages = config.tevStages;
  const u32 stageCount = std::min<u32>(config.tevStageCount, MaxTevStages);
  std::array<std::array<Sym, 4>, MaxTevRegs> regs;
  for (u32 r = 0; r < MaxTevRegs; ++r) {
    for (int c = 0; c < 4; ++c) {
      regs[r][c] = constant(job.state.colorRegs[r][c]);
    }
  }
  for (u32 s = 0; s < stageCount; ++s) {
    const TevStage& stage = stages[s];
    const bool primaryStage = plan.primary != nullptr && stage.texMapId == plan.primaryMap;
    const bool behindStage = plan.behindMap >= 0 && stage.texMapId == plan.behindMap;
    const auto& texSwap = config.tevSwapTable[stage.tevSwapTex];
    const auto& rasSwap = config.tevSwapTable[stage.tevSwapRas];
    const int texChannel[4] = {swap_channel(texSwap.red), swap_channel(texSwap.green), swap_channel(texSwap.blue),
                               swap_channel(texSwap.alpha)};
    const int rasChannel[4] = {swap_channel(rasSwap.red), swap_channel(rasSwap.green), swap_channel(rasSwap.blue),
                               swap_channel(rasSwap.alpha)};
    const auto texture = [&](int channel) -> Sym {
      if (stage.texMapId == GX_TEXMAP_NULL) {
        return constant(1.f);
      }
      const int from = texChannel[channel];
      if (lookups != nullptr && stage.texMapId < MaxTextures &&
          (allKnown || (!primaryStage && plan.lookups[stage.texMapId].pixels &&
                        plan.lookups[stage.texMapId].coord == static_cast<int>(stage.texCoordId)))) {
        return constant(lookups[stage.texMapId][from]);
      }
      if (primaryStage) {
        return {static_cast<int8_t>(from), -1, 1.f, 0.f, 0.f};
      }
      if (behindStage) {
        return {-1, static_cast<int8_t>(from), 0.f, 1.f, 0.f};
      }
      return constant(stage.texMapId < MaxTextures ? plan.average[stage.texMapId][from] : 0.5f);
    };
    const auto raster_value = [&](int channel) -> Sym {
      if (stage.channelId == GX_COLOR_NULL || stage.channelId == GX_COLOR_ZERO || is_bump_channel(stage.channelId)) {
        return constant(0.f);
      }
      return constant(raster[color_channel(stage.channelId) & 1][rasChannel[channel]]);
    };
    const auto colour_arg = [&](GXTevColorArg arg, int c) -> Sym {
      switch (arg) {
      case GX_CC_CPREV:
        return regs[0][c];
      case GX_CC_APREV:
        return regs[0][3];
      case GX_CC_C0:
        return regs[1][c];
      case GX_CC_A0:
        return regs[1][3];
      case GX_CC_C1:
        return regs[2][c];
      case GX_CC_A1:
        return regs[2][3];
      case GX_CC_C2:
        return regs[3][c];
      case GX_CC_A2:
        return regs[3][3];
      case GX_CC_TEXC:
        return texture(c);
      case GX_CC_TEXA:
        return texture(3);
      case GX_CC_RASC:
        return raster_value(c);
      case GX_CC_RASA:
        return raster_value(3);
      case GX_CC_ONE:
        return constant(1.f);
      case GX_CC_HALF:
        return constant(0.5f);
      case GX_CC_KONST:
        return constant(plan.konst[s][c]);
      default:
        return constant(0.f);
      }
    };
    const auto alpha_arg = [&](GXTevAlphaArg arg) -> Sym {
      switch (arg) {
      case GX_CA_APREV:
        return regs[0][3];
      case GX_CA_A0:
        return regs[1][3];
      case GX_CA_A1:
        return regs[2][3];
      case GX_CA_A2:
        return regs[3][3];
      case GX_CA_TEXA:
        return texture(3);
      case GX_CA_RASA:
        return raster_value(3);
      case GX_CA_KONST:
        return constant(plan.konst[s][3]);
      default:
        return constant(0.f);
      }
    };
    Sym colour[3];
    for (int c = 0; c < 3; ++c) {
      colour[c] = tev(plan.ctx, stage.colorOp, colour_arg(stage.colorPass.a, c), colour_arg(stage.colorPass.b, c),
                      colour_arg(stage.colorPass.c, c), colour_arg(stage.colorPass.d, c));
    }
    const Sym alpha = tev(plan.ctx, stage.alphaOp, alpha_arg(stage.alphaPass.a), alpha_arg(stage.alphaPass.b),
                          alpha_arg(stage.alphaPass.c), alpha_arg(stage.alphaPass.d));
    for (int c = 0; c < 3; ++c) {
      regs[std::min<u32>(stage.colorOp.outReg, MaxTevRegs - 1)][c] = colour[c];
    }
    regs[std::min<u32>(stage.alphaOp.outReg, MaxTevRegs - 1)][3] = alpha;
  }
  std::array<Sym, 4> result = regs[0];
  if (stageCount > 0) {
    const TevStage& last = stages[stageCount - 1];
    for (int c = 0; c < 3; ++c) {
      result[c] = regs[std::min<u32>(last.colorOp.outReg, MaxTevRegs - 1)][c];
    }
    result[3] = regs[std::min<u32>(last.alphaOp.outReg, MaxTevRegs - 1)][3];
  }
  return result;
}

// A vertex's colour from its colour before fog and its fog (GX's: towards the fog's colour).
void apply_fog(const DrawJob& job, const DrawJob::Prefog& pre, MirrorVertex& out) {
  const float fog = pre.fog;
  for (int c = 0; c < 4; ++c) {
    float mul = pre.mul[c], addend = pre.add[c];
    if (c < 3 && fog > 0.f) {
      mul *= 1.f - fog;
      addend = addend * (1.f - fog) + fog * job.state.fog.color[c];
    }
    if (c == 3 && job.behind && !job.baked) {
      mul = 0.f;
      addend = 1.f - std::clamp(pre.transmitted, 0.f, 1.f) * (1.f - fog);
    }
    out.mul[c] = half(mul);
    out.add[c] = half(addend);
  }
}

// Vertex v into job.vertices[v]; its distance from the camera back.
float decode_vertex(DrawJob& job, const Decoder& decoder, uint32_t v) {
  const auto& st = job.state;
  const auto& config = job.config;
  const auto& plan = job.plan;
  float position[3] = {0, 0, 0};
  decoder.numbers(GX_VA_POS, v, config.attrs[GX_VA_POS].cnt == 2 ? 2 : 3, position);
  const uint32_t matrix = std::min<uint32_t>(decoder.u8(GX_VA_PNMTXIDX, v, st.currentPnMtx * 3) / 3, MaxPnMtx - 1);
  const F3 view = transform(st.pnMtx[matrix].pos, position[0], position[1], position[2], 1.f);
  float rawNormal[3] = {1, 0, 0};
  if (config.attrs[GX_VA_NRM].attrType != GX_NONE) {
    decoder.numbers(GX_VA_NRM, v, 3, rawNormal);
  }
  const F3 normal = normalized(transform(st.pnMtx[matrix].nrm, rawNormal[0], rawNormal[1], rawNormal[2], 0.f));
  const F4 colours[2] = {decoder.colour(GX_VA_CLR0, v), decoder.colour(GX_VA_CLR1, v)};
  std::array<Sym, 4> result = job.constantResult;
  // The lit channels: the TEV's raster colours, and (0104) the source of a texture coordinate made
  // from a colour channel.
  F4 raster[2];
  if (job.perVertex || plan.colourCoords || job.bakeRaster) {
    light(job, colours, view, normal, raster);
  }
  // 0104: each lookup read where this vertex's coordinate for it falls.
  F4 lookups[MaxTextures];
  if (plan.anyLookup) {
    for (u32 m = 0; m < MaxTextures; ++m) {
      const auto& lookup = plan.lookups[m];
      if (lookup.pixels) {
        const auto uv = tex_coord(st, decoder, config, lookup.coord, v, position,
                                  F3{rawNormal[0], rawNormal[1], rawNormal[2]}, raster);
        const auto texel = sample_lookup(*lookup.pixels, uv[0], uv[1], lookup.wrapS, lookup.wrapT);
        lookups[m] = {{texel[0], texel[1], texel[2], texel[3]}};
      }
    }
  }
  if (job.perVertex) {
    result = run_tev(job, raster, plan.anyLookup ? lookups : nullptr);
  }
  const float fog = fog_at(st, view);

  MirrorVertex& out = job.vertices[v];
  out = {};
  out.position[0] = view.x;
  out.position[1] = view.y;
  out.position[2] = view.z;
  if (job.pull > 0.f) {
    // 0105: drawn over the hair (an eye): this much nearer the camera along its line of sight.
    const float distance = std::sqrt(dot(view, view));
    const float scale = distance > 1e-3f ? std::max(0.25f, 1.f - job.pull / distance) : 1.f;
    for (float& c : out.position) {
      c *= scale;
    }
  }
  if (plan.primary != nullptr || job.baked) {
    auto uv = tex_coord(st, decoder, config, plan.primaryCoord, v, position,
                        F3{rawNormal[0], rawNormal[1], rawNormal[2]}, plan.colourCoords ? raster : colours);
    if (job.baked && job.bakeRaw >= 0) {
      // (0109: bake_reads lays its bake out in the vertices' own coordinate.)
      float raw[2] = {0.f, 0.f};
      const auto attr = static_cast<GXAttr>(GX_VA_TEX0 + job.bakeRaw);
      decoder.numbers(attr, v, std::min<int>(config.attrs[attr].cnt, 2), raw);
      uv = {raw[0], raw[1]};
    }
    out.uv[0] = std::isfinite(uv[0]) ? uv[0] * (job.baked ? job.bakeUV[0] : 1.f) : 0.f;
    out.uv[1] = std::isfinite(uv[1]) ? uv[1] * (job.baked ? job.bakeUV[1] : 1.f) : 0.f;
  }
  DrawJob::Prefog pre{};
  pre.fog = fog;
  if (job.baked) {
    // 0105: the baked texture is the TEV's whole result; only the fog is per vertex (0109: and the
    // part of the vertex colour in it, bake_reads).
    for (int c = 0; c < 4; ++c) {
      pre.mul[c] = 1.f;
      pre.add[c] = 0.f;
    }
    pre.mul[3] = job.bakeAlpha;
    if (job.bakeRaster) {
      const float lit[8] = {raster[0][0], raster[0][1], raster[0][2], raster[0][3],
                            raster[1][0], raster[1][1], raster[1][2], raster[1][3]};
      for (int c = 0; c < 4; ++c) {
        if (job.bakeMultiplied[c] >= 0) {
          pre.mul[c] = lit[job.bakeMultiplied[c]];
        } else {
          for (int j = 0; j < 8; ++j) {
            pre.add[c] += job.bakeAdded[c][j] * lit[j];
          }
        }
      }
    }
    apply_fog(job, pre, out);
    if (job.fogged) {
      job.prefog[v] = pre;
    }
    return std::sqrt(dot(view, view));
  }
  float transmitted = 0.f; // B's mean weight (a refracting surface's transparency)
  for (int c = 0; c < 4; ++c) {
    Sym s = result[c];
    // T: only the channel the texture holds there (the swizzled copy), the rest as its average.
    if (s.sel >= 0 && s.sel != job.channelSel[c]) {
      if (job.channelSel[c] >= 0) {
        s.c += s.k * plan.ctx.primaryAverage[s.sel];
        s.k = 0.f;
        s.sel = job.channelSel[c];
      } else {
        s.c += s.k * plan.ctx.primaryAverage[s.sel];
        s.k = 0.f;
        s.sel = -1;
      }
    }
    if (s.bsel >= 0) {
      if (job.behind && c < 3) {
        transmitted += s.kb / 3.f;
      } else {
        s.c += s.kb * kBehindAverage;
      }
    }
    pre.mul[c] = s.sel >= 0 ? s.k : 0.f;
    pre.add[c] = s.c;
  }
  pre.transmitted = transmitted;
  apply_fog(job, pre, out);
  if (job.fogged) {
    job.prefog[v] = pre;
  }
  return std::sqrt(dot(view, view));
}

// The draw's triangles, clipped as the GPU clips them and turned round (GX's front faces wind
// clockwise on screen, RealityKit's counter-clockwise). Clipping adds vertices.
void build_triangles(DrawJob& job) {
  auto& built = job.vertices;
  const uint32_t vtxCount = job.vtxCount;
  // Inside where >= 0: near, left, right, bottom, top.
  const auto inside = [&](int plane, uint32_t i) {
    const float* p = built[i].position;
    const float d = -p[2];
    switch (plane) {
    case 0:
      return d - job.nearClip;
    case 1:
      return job.kx * d + p[0];
    case 2:
      return job.kx * d - p[0];
    case 3:
      return job.ky * d + p[1];
    default:
      return job.ky * d - p[1];
    }
  };
  const auto between = [&](uint32_t p, uint32_t q, float fp, float fq) {
    const MirrorVertex a = built[p], b = built[q];
    const float t = fp / (fp - fq);
    MirrorVertex out{};
    for (int i = 0; i < 3; ++i) {
      out.position[i] = a.position[i] + (b.position[i] - a.position[i]) * t;
    }
    for (int i = 0; i < 2; ++i) {
      out.uv[i] = a.uv[i] + (b.uv[i] - a.uv[i]) * t;
    }
    for (int i = 0; i < 4; ++i) {
      const float ma = from_half(a.mul[i]), mb = from_half(b.mul[i]);
      const float aa = from_half(a.add[i]), ab = from_half(b.add[i]);
      out.mul[i] = half(ma + (mb - ma) * t);
      out.add[i] = half(aa + (ab - aa) * t);
    }
    built.push_back(out);
    return static_cast<uint32_t>(built.size() - 1);
  };
  const auto clipped = [&](uint32_t a, uint32_t b, uint32_t c) {
    uint32_t polygon[9] = {a, b, c};
    int count = 3;
    // The sky only at the near plane: it goes on past the view's sides, for a viewer leaning in.
    // (0110: an alpha volume not at all: it's the solid, not a picture of it.)
    // The Luigi's Mansion port's volume (AURORA_MIRROR_CLIP=none): nothing clipped at all, as the
    // set is seen from all round, the room behind the game's camera too.
    static const bool s_noClip = [] {
      const char* value = std::getenv("AURORA_MIRROR_CLIP");
      return value != nullptr && std::strcmp(value, "none") == 0;
    }();
    const int planes = s_noClip || job.alphaOp >= 0 ? 0 : job.background ? 1 : 5;
    for (int plane = 0; plane < planes && count >= 3; ++plane) {
      float values[9];
      int outside = 0;
      for (int i = 0; i < count; ++i) {
        values[i] = inside(plane, polygon[i]);
        outside += values[i] < 0.f;
      }
      if (outside == 0) {
        continue;
      }
      if (outside == count) {
        return;
      }
      uint32_t next[9];
      int nextCount = 0;
      for (int i = 0; i < count && nextCount < 8; ++i) {
        const int j = (i + 1) % count;
        if (values[i] >= 0.f) {
          next[nextCount++] = polygon[i];
        }
        if ((values[i] >= 0.f) != (values[j] >= 0.f)) {
          next[nextCount++] = between(polygon[i], polygon[j], values[i], values[j]);
        }
      }
      std::copy(next, next + nextCount, polygon);
      count = nextCount;
    }
    for (int i = 1; i + 1 < count; ++i) {
      job.triangles.insert(job.triangles.end(), {polygon[0], polygon[i + 1], polygon[i]});
    }
  };
  // 0107: a fogged draw's triangles split where their fog changes by more than kFogStep along an
  // edge (shared midpoints, so no cracks; children keep their parent's winding), each new vertex
  // fogged at its own depth.
  static thread_local absl::flat_hash_map<uint64_t, uint32_t> midpoints;
  midpoints.clear();
  const auto midpoint = [&](uint32_t i, uint32_t j) {
    const uint64_t key = static_cast<uint64_t>(std::min(i, j)) << 32 | std::max(i, j);
    if (auto found = midpoints.find(key); found != midpoints.end()) {
      return found->second;
    }
    const MirrorVertex a = built[i], b = built[j];
    const DrawJob::Prefog pa = job.prefog[i], pb = job.prefog[j];
    MirrorVertex m{};
    DrawJob::Prefog pm{};
    for (int c = 0; c < 3; ++c) {
      m.position[c] = (a.position[c] + b.position[c]) * 0.5f;
    }
    for (int c = 0; c < 2; ++c) {
      m.uv[c] = (a.uv[c] + b.uv[c]) * 0.5f;
    }
    for (int c = 0; c < 4; ++c) {
      pm.mul[c] = (pa.mul[c] + pb.mul[c]) * 0.5f;
      pm.add[c] = (pa.add[c] + pb.add[c]) * 0.5f;
    }
    pm.transmitted = (pa.transmitted + pb.transmitted) * 0.5f;
    pm.fog = fog_at(job.state, F3{m.position[0], m.position[1], m.position[2]});
    apply_fog(job, pm, m);
    built.push_back(m);
    const auto index = static_cast<uint32_t>(built.size() - 1);
    // Clipping's new vertices have no fog of their own kept (nothing splits them): skipped here.
    job.prefog.resize(built.size());
    job.prefog[index] = pm;
    midpoints.emplace(key, index);
    return index;
  };
  const auto splits = [&](uint32_t i, uint32_t j) { return std::fabs(job.prefog[i].fog - job.prefog[j].fog) > kFogStep; };
  const auto fogSplit = [&](auto& self, uint32_t v0, uint32_t v1, uint32_t v2, int level) -> void {
    const bool s01 = level < kFogSplits && splits(v0, v1), s12 = level < kFogSplits && splits(v1, v2),
               s20 = level < kFogSplits && splits(v2, v0);
    const int count = s01 + s12 + s20;
    if (count == 0) {
      clipped(v0, v1, v2);
      return;
    }
    if (count == 3) {
      const uint32_t m01 = midpoint(v0, v1), m12 = midpoint(v1, v2), m20 = midpoint(v2, v0);
      self(self, v0, m01, m20, level + 1);
      self(self, m01, v1, m12, level + 1);
      self(self, m20, m12, v2, level + 1);
      self(self, m01, m12, m20, level + 1);
      return;
    }
    // Rotated so the split edges come first: (v0 v1), then (v1 v2).
    if (count == 1) {
      if (s12) {
        return self(self, v1, v2, v0, level);
      }
      if (s20) {
        return self(self, v2, v0, v1, level);
      }
      const uint32_t m01 = midpoint(v0, v1);
      self(self, v0, m01, v2, level + 1);
      self(self, m01, v1, v2, level + 1);
      return;
    }
    if (!s01) {
      return self(self, v1, v2, v0, level);
    }
    if (!s12) {
      return self(self, v2, v0, v1, level);
    }
    const uint32_t m01 = midpoint(v0, v1), m12 = midpoint(v1, v2);
    self(self, m01, v1, m12, level + 1);
    self(self, v0, m01, m12, level + 1);
    self(self, v0, m12, v2, level + 1);
  };
  const auto triangle = [&](uint32_t a, uint32_t b, uint32_t c) {
    if (a == b || b == c || a == c || a >= vtxCount || b >= vtxCount || c >= vtxCount) {
      return;
    }
    if (job.fogged) {
      fogSplit(fogSplit, a, b, c, 0);
    } else {
      clipped(a, b, c);
    }
  };
  const auto& indices = job.indices;
  if (!indices.empty()) {
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
      triangle(indices[i], indices[i + 1], indices[i + 2]);
    }
    return;
  }
  // Each primitive (one, or the merged draws' in turn) from its own first vertex.
  const auto primitive = [&](GXPrimitive prim, uint32_t o, uint32_t n) {
    if (prim == GX_TRIANGLES) {
      for (uint32_t i = 0; i + 2 < n; i += 3) {
        triangle(o + i, o + i + 1, o + i + 2);
      }
    } else if (prim == GX_QUADS) {
      uint32_t i = 0;
      for (; i + 3 < n; i += 4) {
        triangle(o + i, o + i + 1, o + i + 2);
        triangle(o + i + 2, o + i + 3, o + i);
      }
      // 0107: three left over are a triangle, as the hardware (and aurora's own draw) has it:
      // Wind Waker draws its stars and the sun's rays as quads of three vertices.
      if (n - i == 3) {
        triangle(o + i, o + i + 1, o + i + 2);
      }
    } else if (prim == GX_TRIANGLESTRIP) {
      for (uint32_t i = 2; i < n; ++i) {
        if (i == 2 || (i & 1) == 0) {
          triangle(o + i - 2, o + i - 1, o + i);
        } else {
          triangle(o + i - 1, o + i - 2, o + i);
        }
      }
    } else if (prim == GX_TRIANGLEFAN) {
      for (uint32_t i = 2; i < n; ++i) {
        triangle(o, o + i - 1, o + i);
      }
    }
  };
  if (job.segments.empty()) {
    primitive(job.prim, 0, vtxCount);
  } else {
    for (const auto& segment : job.segments) {
      primitive(segment.prim, segment.first, std::min(segment.count, vtxCount - std::min(segment.first, vtxCount)));
    }
  }
}

// How far a decal is lifted at most (view units; TP's are about a centimetre). A slide of 0.4% of the
// distance, uncapped, floated a far shadow tens of units off the ground, which a viewer off to the
// side saw move against it (the SHAR port's rule: min(0.4%, 2 cm)).
constexpr float kDecalLift = 3.f;
// How far past the view's sides the draws are kept (times the view's half-width), for a viewer
// leaning to look past the window's edges, or farther from the window than the camera, who sees in
// front of the glass more widely than the camera did (finish's cut at the allowance counts on it). 1.4 left the
// ground beneath the camera out, and the window's foot showed sky.
// The Luigi's Mansion port's volume (a table, seen from all round) keeps more: AURORA_MIRROR_CLIP_MARGIN.
const float kClipMargin = [] {
  const char* value = std::getenv("AURORA_MIRROR_CLIP_MARGIN");
  const float margin = value != nullptr ? std::strtof(value, nullptr) : 2.f;
  return std::isfinite(margin) && margin >= 1.f ? std::min(margin, 16.f) : 2.f;
}();

// 0111: the glass's distance follows its target with this time constant (finish), seconds.
constexpr float kPlaneSeconds = 1.f;

// Farther than this from the camera, a vertex is a runaway (a particle flung to 1e20): its
// triangles go, or the frame's bounds take everything with them (SHAR).
constexpr float kRunaway = 1e6f;

bool runaway(const float position[3]) {
  return !(std::fabs(position[0]) < kRunaway && std::fabs(position[1]) < kRunaway && std::fabs(position[2]) < kRunaway);
}

void process(DrawJob& job) {
  const uint64_t started = clock_ns();
  const Decoder decoder{job.state, job.config, job.bytes.data(), job.bytes.size()};
  job.vertices.resize(job.vtxCount);
  job.triangles.clear();
  job.fogged = kFogSplit && job.state.fog.type != GX_FOG_NONE && job.alphaOp < 0;
  job.prefog.resize(job.fogged ? job.vtxCount : 0);
  job.nearest = INFINITY;
  job.farthest = 0;
  for (uint32_t v = 0; v < job.vtxCount; ++v) {
    const float distance = decode_vertex(job, decoder, v);
    if (runaway(job.vertices[v].position)) {
      continue;
    }
    job.nearest = std::min(job.nearest, distance);
    job.farthest = std::max(job.farthest, distance);
  }
  build_triangles(job);
  g_decodeNanos += clock_ns() - started;
}

#if defined(__APPLE__)
// 0106: draws go to the workers in batches of up to 256 (or 8,192 vertices): a frame has thousands,
// most of a few vertices, and a dispatch each (which woke a worker each time) cost the FIFO thread
// more than recording them. Batches are kept from frame to frame; reused once the group is done.
// The last draw recorded is held back (g_openJob) until the next is, as merged draws join it.
struct Batch {
  std::array<DrawJob*, 256> jobs{};
  uint32_t count = 0;
};
std::vector<std::unique_ptr<Batch>> g_batches;
size_t g_batchCount = 0;
Batch* g_openBatch = nullptr;
uint32_t g_openVertices = 0;

void flush_batch() {
  if (g_openBatch == nullptr) {
    return;
  }
  if (g_openBatch->count > 0) {
    dispatch_group_async_f(g_jobGroup, dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), g_openBatch,
                           [](void* context) {
                             auto* batch = static_cast<Batch*>(context);
                             for (uint32_t i = 0; i < batch->count; ++i) {
                               process(*batch->jobs[i]);
                             }
                           });
  }
  g_openBatch = nullptr;
  g_openVertices = 0;
}

void queue_job(DrawJob& job) {
  if (g_openBatch == nullptr) {
    if (g_batchCount == g_batches.size()) {
      g_batches.push_back(std::make_unique<Batch>());
    }
    g_openBatch = g_batches[g_batchCount++].get();
    g_openBatch->count = 0;
  }
  g_openBatch->jobs[g_openBatch->count++] = &job;
  g_openVertices += job.vtxCount;
  if (g_openBatch->count == g_openBatch->jobs.size() || g_openVertices >= 8192) {
    flush_batch();
  }
}

DrawJob* g_openJob = nullptr;   // recorded, not yet queued: merged draws join it
DrawJob* g_mergeTarget = nullptr; // the last draw's job, if it was recorded (and can take more)
bool g_lastLeftOut = false;       // the last draw was left out (so are draws merged into it)

void queue_open_job() {
  if (g_openJob != nullptr) {
    queue_job(*g_openJob);
    g_openJob = nullptr;
  }
}

// Every batch dispatched, and done.
void wait_jobs() {
  g_mergeTarget = nullptr;
  g_lastLeftOut = false;
  queue_open_job();
  flush_batch();
  dispatch_group_wait(g_jobGroup, DISPATCH_TIME_FOREVER);
  g_batchCount = 0;
}
#else
DrawJob* g_openJob = nullptr;
DrawJob* g_mergeTarget = nullptr;
bool g_lastLeftOut = false;
void queue_open_job() {
  if (g_openJob != nullptr) {
    process(*g_openJob);
    g_openJob = nullptr;
  }
}
void wait_jobs() {
  g_mergeTarget = nullptr;
  g_lastLeftOut = false;
  queue_open_job();
}
#endif

// END: every draw decoded, merged in the game's order into the frame.
void merge_jobs() {
  wait_jobs();
  static std::vector<uint8_t> sane; // FIFO thread: kept, not made again each frame
  for (size_t i = 0; i < g_jobCount; ++i) {
    DrawJob& job = *g_jobs[i];
    // 0110: an alpha volume's planes (oriented away from its centre: a convex solid), and an alpha
    // layer's place among the frame's vertices.
    if (job.alphaOp >= 0) {
      AlphaOp& op = g_alphaOps[static_cast<size_t>(job.alphaOp)];
      double sum[3] = {0, 0, 0};
      size_t used = 0;
      for (const MirrorVertex& v : job.vertices) {
        if (!runaway(v.position)) {
          for (int c = 0; c < 3; ++c) {
            sum[c] += v.position[c];
          }
          ++used;
        }
      }
      if (used == 0) {
        continue;
      }
      for (int c = 0; c < 3; ++c) {
        op.centre[c] = static_cast<float>(sum[c] / static_cast<double>(used));
      }
      float radius = 0.f;
      for (const MirrorVertex& v : job.vertices) {
        if (!runaway(v.position)) {
          float r = 0.f;
          for (int c = 0; c < 3; ++c) {
            r += (v.position[c] - op.centre[c]) * (v.position[c] - op.centre[c]);
          }
          radius = std::max(radius, r);
        }
      }
      op.radius = std::sqrt(radius) * 1.001f + 1e-3f;
      op.planes.clear();
      for (size_t t = 0; t + 2 < job.triangles.size(); t += 3) {
        const float* a = job.vertices[job.triangles[t]].position;
        const float* b = job.vertices[job.triangles[t + 1]].position;
        const float* c = job.vertices[job.triangles[t + 2]].position;
        const F3 e1{b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2{c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        F3 n{e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x};
        const float length = std::sqrt(dot(n, n));
        if (!(length > 1e-6f)) {
          continue;
        }
        n = {n.x / length, n.y / length, n.z / length};
        float d = n.x * a[0] + n.y * a[1] + n.z * a[2];
        if (n.x * op.centre[0] + n.y * op.centre[1] + n.z * op.centre[2] > d) {
          n = {-n.x, -n.y, -n.z};
          d = -d;
        }
        bool same = false;
        for (const auto& q : op.planes) {
          if (q[0] * n.x + q[1] * n.y + q[2] * n.z > 0.9999f && std::fabs(q[3] - d) < 1e-3f * (1.f + std::fabs(d))) {
            same = true;
            break;
          }
        }
        if (!same) {
          op.planes.push_back({n.x, n.y, n.z, d});
        }
      }
      continue;
    }
    if (job.alphaLayer >= 0) {
      g_alphaLayers[static_cast<size_t>(job.alphaLayer)].vertexEnd = static_cast<uint32_t>(g_building.vertices.size());
      continue;
    }
    const auto base = static_cast<uint32_t>(g_building.vertices.size());
    if (job.nearer != 1.f && !job.background) {
      for (MirrorVertex& vertex : job.vertices) {
        for (float& c : vertex.position) {
          c *= job.nearer;
        }
      }
    }
    sane.resize(job.vertices.size());
    for (size_t v = 0; v < job.vertices.size(); ++v) {
      sane[v] = !runaway(job.vertices[v].position);
    }
    g_building.vertices.insert(g_building.vertices.end(), job.vertices.begin(), job.vertices.end());
    // 0110: a translucent draw before a dark room's darkening: darkened in place (build_alpha_layers),
    // as the game's picture is under it; one after its alpha was set up that writes the alpha too (a
    // particle: as bright in the dark as out of it) is left as it is. (Those before the set-up, a
    // cave's misty rim, stayed bright in the dark.)
    // And the background (a cave's backdrop, drawn first and apart): darkened in place too, solid or
    // not (the layer is laid on solid ground only).
    if (job.group < g_groups.size() && (g_groups[job.group].blended || job.background)) {
      for (AlphaLayer& layer : g_alphaLayers) {
        if (layer.fromJob != SIZE_MAX && i < layer.toJob && (i < layer.fromJob || !job.writesAlpha || job.background)) {
          layer.darken.emplace_back(base, static_cast<uint32_t>(g_building.vertices.size()));
        }
      }
    }
    if (job.decal) {
      g_decalRanges.emplace_back(base, static_cast<uint32_t>(g_building.vertices.size()));
    }
    auto& indices = g_groups[job.group].indices;
    // 0106: room grown geometrically. Reserved to the exact size for each draw, a material drawn in
    // thousands of draws (the sea at the title, grass) moved its whole index list each time: the
    // FIFO thread spent most of a title frame (four frames a second) copying and freeing it.
    if (const size_t needed = indices.size() + job.triangles.size(); needed > indices.capacity()) {
      indices.reserve(std::max(needed, indices.capacity() * 2));
    }
    const size_t before = indices.size();
    for (size_t t = 0; t + 2 < job.triangles.size(); t += 3) {
      const uint32_t a = job.triangles[t], b = job.triangles[t + 1], c = job.triangles[t + 2];
      if (sane[a] && sane[b] && sane[c]) {
        indices.push_back(base + a);
        indices.push_back(base + b);
        indices.push_back(base + c);
      }
    }
    g_stats.triangles += (indices.size() - before) / 3;
    if (job.background) {
      g_backgroundRanges.emplace_back(base, static_cast<uint32_t>(g_building.vertices.size()));
      ++g_stats.background;
      g_nearestBackground = std::min(g_nearestBackground, job.nearest);
    } else {
      g_farthest = std::max(g_farthest, job.farthest);
    }
  }
  g_jobCount = 0;
}

void discard_jobs() {
  wait_jobs();
  g_jobCount = 0;
}

// AURORA_MIRROR_GROUPS=1 (a test switch, in a build with DUSK_GFX_DEBUG_GROUPS on): per debug
// group, how many draws the mirror kept and why it skipped the others, every 600 frames.
enum Tally { TallyKept, TallyLines, TallyScreen, TallyNoColour, TallyViewport, TallyProjection, TallyBlend,
             TallyAlphaNever, TallyCopyOnly, TallyOutside, TallyOffscreen, TallyCount };
const bool kGroupLog = std::getenv("AURORA_MIRROR_GROUPS") != nullptr;
std::vector<std::string> g_debugGroups;
std::unordered_map<std::string, std::array<uint32_t, TallyCount>> g_groupTally;

void tally(Tally what) {
  if (!kGroupLog) {
    return;
  }
  std::string key;
  for (const auto& group : g_debugGroups) {
    key += '/';
    key += group;
  }
  ++g_groupTally[key.empty() ? std::string("-") : key][what];
}

void log_tally(double frames) {
  if (!kGroupLog) {
    return;
  }
  std::vector<std::pair<std::string, std::array<uint32_t, TallyCount>>> rows(g_groupTally.begin(), g_groupTally.end());
  std::sort(rows.begin(), rows.end());
  static constexpr const char* kNames[TallyCount] = {"kept", "lines", "screen", "no-colour", "viewport", "projection",
                                                     "blend", "alpha-never", "copy-only", "outside", "offscreen"};
  for (const auto& [group, counts] : rows) {
    std::string text;
    for (int i = 0; i < TallyCount; ++i) {
      if (counts[i] != 0) {
        text += fmt::format(" {} {:.1f}", kNames[i], counts[i] / frames);
      }
    }
    Log.info("mirror group {}:{}", group, text);
  }
  g_groupTally.clear();
}

// AURORA_MIRROR_LOG=1 (a test switch): what the skipped framebuffer-copy draws are, each kind once.
// AURORA_MIRROR_LOG=2 (a test switch): kept draws whose TEV samples more than one texture, each
// distinct one once: what the mirror averages away.
void log_textures(const ShaderConfig& config, const DrawPlan& plan, const MirrorPart& part, u16 vtxCount) {
  // (=3: every kept draw's, one texture or more.)
  static const int level = [] {
    const char* value = std::getenv("AURORA_MIRROR_LOG");
    return value != nullptr ? std::atoi(value) : 0;
  }();
  static std::unordered_map<uint64_t, bool> seen;
  if (level < 2 || seen.size() > 300) {
    return;
  }
  int maps = 0;
  u32 seenMaps = 0;
  for (u32 i = 0; i < config.tevStageCount; ++i) {
    const auto& stage = config.tevStages[i];
    if (samples_texture(stage) && stage.texMapId < MaxTextures && (seenMaps & (1u << stage.texMapId)) == 0) {
      seenMaps |= 1u << stage.texMapId;
      ++maps;
    }
  }
  if (maps < (level >= 3 ? 1 : 2)) {
    return;
  }
  const auto& st = g_gxState;
  std::string group;
  for (const auto& name : g_debugGroups) {
    group += '/';
    group += name;
  }
  std::string channels;
  for (int c = 0; c < 4; ++c) {
    const auto& cc = st.colorChannelConfig[c];
    channels += fmt::format(" ch{}:{}{}{}/{}", c, cc.lightingEnabled ? "lit" : "-", static_cast<int>(cc.matSrc),
                            static_cast<int>(cc.ambSrc), st.colorChannelState[c].lightMask.to_ulong());
  }
  std::string text = fmt::format("{} kind {} depth {}{} func {} blend {} {}->{}{} | {} stages, {} ind:", group, part.kind,
                                 st.depthCompare ? "test" : "-", st.depthUpdate ? "+write" : "",
                                 static_cast<int>(st.depthFunc), static_cast<int>(st.blendMode),
                                 static_cast<int>(st.blendFacSrc), static_cast<int>(st.blendFacDst), channels,
                                 config.tevStageCount, config.numIndStages);
  for (u32 i = 0; i < config.tevStageCount; ++i) {
    const auto& stage = config.tevStages[i];
    const bool sampled = samples_texture(stage) && stage.texMapId < MaxTextures;
    const auto& obj = st.loadedTextures[sampled ? stage.texMapId : 0];
    text += fmt::format(" [c {} {} {} {} op{}b{}s{}>{} a {} {} {} {} op{}b{}s{}>{} k{}/{} ras{} swap{}/{}",
                        static_cast<int>(stage.colorPass.a), static_cast<int>(stage.colorPass.b),
                        static_cast<int>(stage.colorPass.c), static_cast<int>(stage.colorPass.d),
                        static_cast<int>(stage.colorOp.op), static_cast<int>(stage.colorOp.bias),
                        static_cast<int>(stage.colorOp.scale), static_cast<int>(stage.colorOp.outReg),
                        static_cast<int>(stage.alphaPass.a), static_cast<int>(stage.alphaPass.b),
                        static_cast<int>(stage.alphaPass.c), static_cast<int>(stage.alphaPass.d),
                        static_cast<int>(stage.alphaOp.op), static_cast<int>(stage.alphaOp.bias),
                        static_cast<int>(stage.alphaOp.scale), static_cast<int>(stage.alphaOp.outReg),
                        static_cast<int>(stage.kcSel), static_cast<int>(stage.kaSel), static_cast<int>(stage.channelId),
                        static_cast<int>(stage.tevSwapRas), static_cast<int>(stage.tevSwapTex));
    if (sampled) {
      const auto& avg = plan.average[stage.texMapId];
      text += fmt::format(" map{} {} {}x{} coord{}{} avg {:.2f} {:.2f} {:.2f} {:.2f}", static_cast<int>(stage.texMapId),
                          obj.format(), obj.width(), obj.height(), static_cast<int>(stage.texCoordId),
                          plan.primaryMap == static_cast<int>(stage.texMapId) ? " PRIMARY" : "", avg[0], avg[1], avg[2],
                          avg[3]);
      // Its pixels, once, as a PPM (RGB) and PGM (alpha) in TMPDIR.
      static std::unordered_map<const void*, bool> written;
      if (const char* tmp = std::getenv("TMPDIR"); tmp != nullptr && written.emplace(obj.data, true).second) {
        auto rgba = decode(obj, palette_of(obj));
        if (!rgba.empty()) {
          const std::string base = fmt::format("{}/mirror-tex-{}x{}-f{}-{:p}", tmp, obj.width(), obj.height(), obj.format(), obj.data);
          if (FILE* f = std::fopen((base + ".ppm").c_str(), "wb")) {
            std::fprintf(f, "P6 %u %u 255\n", obj.width(), obj.height());
            for (size_t i = 0; i < rgba.size(); i += 4) {
              std::fwrite(&rgba[i], 1, 3, f);
            }
            std::fclose(f);
          }
          if (FILE* f = std::fopen((base + ".pgm").c_str(), "wb")) {
            std::fprintf(f, "P5 %u %u 255\n", obj.width(), obj.height());
            for (size_t i = 3; i < rgba.size(); i += 4) {
              std::fwrite(&rgba[i], 1, 1, f);
            }
            std::fclose(f);
          }
          text += " -> " + base;
        }
      }
    }
    if (stage.indTexMtxId != GX_ITM_OFF) {
      text += fmt::format(" ind{} mtx{}", static_cast<int>(stage.indTexStage), static_cast<int>(stage.indTexMtxId));
    }
    text += "]";
  }
  for (u32 i = 0; i < MaxTexCoord; ++i) {
    if (config.tcgs[i].src != GX_MAX_TEXGENSRC) {
      text += fmt::format(" tcg{} t{} s{} m{} p{}{}", i, static_cast<int>(config.tcgs[i].type),
                          static_cast<int>(config.tcgs[i].src), static_cast<int>(config.tcgs[i].mtx),
                          static_cast<int>(config.tcgs[i].postMtx), config.tcgs[i].normalize ? " n" : "");
    }
  }
  const uint64_t key = xxh3_hash_s(text.data(), text.size());
  if (seen.emplace(key, true).second) {
    Log.info("mirror textures ({} vertices): {}", vtxCount, text);
  }
}

void log_skipped(const ShaderConfig& config, const MirrorPart& part, u16 vtxCount) {
  static const bool enabled = std::getenv("AURORA_MIRROR_LOG") != nullptr;
  static std::unordered_map<uint64_t, bool> seen;
  if (!enabled || seen.size() > 64) {
    return;
  }
  const auto& st = g_gxState;
  std::string text = fmt::format("blend {} {}->{} depth {}{} func {} cull {} kind {} | {} stages:", static_cast<int>(st.blendMode),
                                 static_cast<int>(st.blendFacSrc), static_cast<int>(st.blendFacDst), st.depthCompare ? "test" : "-",
                                 st.depthUpdate ? "+write" : "", static_cast<int>(st.depthFunc), static_cast<int>(st.cullMode),
                                 part.kind, config.tevStageCount);
  for (u32 i = 0; i < config.tevStageCount; ++i) {
    const auto& stage = config.tevStages[i];
    const auto& obj = st.loadedTextures[stage.texMapId < MaxTextures ? stage.texMapId : 0];
    text += fmt::format(" [c {} {} {} {} a {} {} {} {} map {} fmt {} {}x{} copy {} coord {} chan {}]", static_cast<int>(stage.colorPass.a),
                        static_cast<int>(stage.colorPass.b), static_cast<int>(stage.colorPass.c), static_cast<int>(stage.colorPass.d),
                        static_cast<int>(stage.alphaPass.a), static_cast<int>(stage.alphaPass.b), static_cast<int>(stage.alphaPass.c),
                        static_cast<int>(stage.alphaPass.d), static_cast<int>(stage.texMapId), obj.format(), obj.width(), obj.height(),
                        g_gxState.copyTextures.contains(obj.data) ? 1 : 0, static_cast<int>(stage.texCoordId),
                        static_cast<int>(stage.channelId));
  }
  for (u32 i = 0; i < MaxTexCoord; ++i) {
    if (config.tcgs[i].src != GX_MAX_TEXGENSRC) {
      text += fmt::format(" tcg{} type {} src {} mtx {}", i, static_cast<int>(config.tcgs[i].type),
                          static_cast<int>(config.tcgs[i].src), static_cast<int>(config.tcgs[i].mtx));
    }
  }
  const uint64_t key = xxh3_hash_s(text.data(), text.size());
  if (seen.emplace(key, true).second) {
    Log.info("mirror skipped ({} vertices): {}", vtxCount, text);
  }
}

void begin_frame() {
  g_alphaOps.clear();
  g_alphaLayers.clear();
  g_alphaStart = std::clamp(g_gxState.clearColor[3], 0.f, 1.f);
  g_bakeOccurrences.clear();
  g_bakeKeys.clear();
  g_bakeTexels = 0;
  const size_t passes = g_passes.size();
  g_passes.clear();
  g_passes.reserve(passes);
  g_building.vertices.clear();
  for (Group& group : g_groups) {
    const size_t bytes = group.indices.capacity() * sizeof(uint32_t);
    if (group.indices.capacity() > 0 && group.indices.capacity() <= kSpareListMax &&
        g_spareBytes + bytes <= kSpareBytesMax) {
      group.indices.clear();
      g_spareBytes += bytes;
      g_spareIndices.push_back(std::move(group.indices));
    }
  }
  g_groups.clear();
  g_solidGroups.clear();
  g_lastBlended = -1;
  g_backgroundRanges.clear();
  g_decalRanges.clear();
  g_haveProjection = false;
  g_sawDepthWrite = false;
  g_farthest = 0;
  g_nearestBackground = INFINITY;
  g_offscreen = 0;
}

// finish's easing of what's nearer than the glass into the window's allowance: a point at view
// depth d < end moves along its line of sight to depth P - eased(k (P - d)) / k.
struct Easing {
  float plane, k, knee, rest, end;
  // How much a point at `depth` is scaled (about the camera), and how much the easing shortens a
  // small step in depth there (its slope).
  float scale(float depth) const {
    if (!(depth > 1e-3f && depth < end)) {
      return 1.f;
    }
    const float front = k * (plane - depth);
    const float eased = knee + rest * (1.f - std::exp(-(front - knee) / rest));
    return (plane - eased / k) / depth;
  }
  // How far behind the eased curve (eased depth) the eased edge between points at depths a and b
  // lies, where it crosses the line of sight through the edge's middle: there the chord is at the
  // harmonic mean of the ends' scales. (Never in front of the curve: the easing bows towards the
  // camera.)
  float behind(float a, float b) const {
    const float sa = scale(a), sb = scale(b), middle = (a + b) * 0.5f;
    return middle * (2.f / (1.f / sa + 1.f / sb) - scale(middle));
  }
};

// 0111: cuts every group's triangles at view depth `cut`, keeping what's beyond it, as a camera's
// near plane cuts them (TPVR's). An edge crossing it gets one new vertex on it (shared by the two
// triangles on the edge: no cracks), its attributes and mark (`marks`) taken along the edge.
void cut_near(std::vector<MirrorVertex>& vertices, std::vector<Group>& groups, float cut,
              std::vector<uint8_t>& marks) {
  absl::flat_hash_map<uint64_t, uint32_t> crossings;
  const auto depth = [&](uint32_t i) { return -vertices[i].position[2]; };
  const auto crossing = [&](uint32_t i, uint32_t j) {
    const uint64_t key = static_cast<uint64_t>(std::min(i, j)) << 32 | std::max(i, j);
    if (auto found = crossings.find(key); found != crossings.end()) {
      return found->second;
    }
    // From the lower index, so both triangles on the edge get the same vertex.
    const uint32_t from = std::min(i, j), to = std::max(i, j);
    const MirrorVertex a = vertices[from], b = vertices[to];
    const float da = depth(from), db = depth(to);
    const float t = std::clamp((cut - da) / (db - da), 0.f, 1.f);
    MirrorVertex m{};
    for (int c = 0; c < 3; ++c) {
      m.position[c] = a.position[c] + (b.position[c] - a.position[c]) * t;
    }
    for (int c = 0; c < 2; ++c) {
      m.uv[c] = a.uv[c] + (b.uv[c] - a.uv[c]) * t;
    }
    for (int c = 0; c < 4; ++c) {
      const float ma = from_half(a.mul[c]), mb = from_half(b.mul[c]);
      const float aa = from_half(a.add[c]), ab = from_half(b.add[c]);
      m.mul[c] = half(ma + (mb - ma) * t);
      m.add[c] = half(aa + (ab - aa) * t);
    }
    vertices.push_back(m);
    marks.push_back(marks[from] & marks[to]);
    const auto index = static_cast<uint32_t>(vertices.size() - 1);
    crossings.emplace(key, index);
    return index;
  };
  std::vector<uint32_t> out;
  for (Group& group : groups) {
    bool near = false;
    for (uint32_t i : group.indices) {
      if (depth(i) < cut) {
        near = true;
        break;
      }
    }
    if (!near) {
      continue;
    }
    out.clear();
    for (size_t t = 0; t + 2 < group.indices.size(); t += 3) {
      const uint32_t corners[3] = {group.indices[t], group.indices[t + 1], group.indices[t + 2]};
      int inside = 0;
      for (uint32_t i : corners) {
        inside += depth(i) >= cut;
      }
      if (inside == 3) {
        out.insert(out.end(), corners, corners + 3);
        continue;
      }
      if (inside == 0) {
        continue;
      }
      // The polygon beyond the cut, in the triangle's winding (a triangle or a quad).
      uint32_t polygon[4];
      int count = 0;
      for (int c = 0; c < 3; ++c) {
        const uint32_t i = corners[c], j = corners[(c + 1) % 3];
        const bool in = depth(i) >= cut;
        if (in) {
          polygon[count++] = i;
        }
        if (in != (depth(j) >= cut)) {
          polygon[count++] = crossing(i, j);
        }
      }
      for (int c = 1; c + 1 < count; ++c) {
        out.insert(out.end(), {polygon[0], polygon[c], polygon[c + 1]});
      }
    }
    group.indices.swap(out);
  }
}

// Splits every group's triangles that reach in front of `plane` wherever an edge's far end is more
// than 40% deeper than its near end, edge midpoints shared. A midpoint is marked (`marks`, one per
// vertex) when both its ends are. With an easing, a marked (decal) edge is also split while, eased,
// it lies behind the eased curve by more than half a decal's lift: a decal (a path, a stain, a
// shadow) whose triangles aren't the ground's has chords of its own, and lying farther behind the
// curve than its lift, it went under the ground in patches. The ground's chords are never in front
// of the curve, so only the decals' need it, which keeps it to their triangles (all of them split
// so, the frame grew to 470,000 vertices).
void subdivide_near(std::vector<MirrorVertex>& vertices, std::vector<Group>& groups, float plane,
                    std::vector<uint8_t>& marks, const Easing* easing = nullptr) {
  const size_t budget = vertices.size() + 400000;
  // 0106: a flat map, kept from frame to frame (the worker's own): a node map made afresh each
  // frame took most of a close-up frame's finishing (Link's house: about 25 ms in the Simulator).
  // Emptied by erasing (clear() gives a big table's room back) and given room for lately's need.
  static thread_local absl::flat_hash_map<uint64_t, uint32_t> midpoints;
  static thread_local size_t recent = 0; // midpoints lately: room made for them up front
  midpoints.erase(midpoints.begin(), midpoints.end());
  midpoints.reserve(recent);
  const auto depth = [&](uint32_t i) { return -vertices[i].position[2]; };
  const auto splits = [&](uint32_t i, uint32_t j) {
    const float a = depth(i), b = depth(j), near = std::min(a, b), far = std::max(a, b);
    if (near >= plane || vertices.size() >= budget) {
      return false;
    }
    if (easing != nullptr && marks[i] != 0 && marks[j] != 0 && near > 1e-3f) {
      const float lift = std::min(kDecalLift, 4e-3f * easing->scale(near) * near);
      if (easing->behind(a, b) > 0.5f * lift) {
        return true;
      }
    }
    if (far <= near * 1.4f) {
      return false;
    }
    float length2 = 0;
    for (int c = 0; c < 3; ++c) {
      const float d = vertices[i].position[c] - vertices[j].position[c];
      length2 += d * d;
    }
    return length2 > (plane / 128) * (plane / 128);
  };
  const auto midpoint = [&](uint32_t i, uint32_t j) {
    const uint64_t key = static_cast<uint64_t>(std::min(i, j)) << 32 | std::max(i, j);
    if (auto found = midpoints.find(key); found != midpoints.end()) {
      return found->second;
    }
    const MirrorVertex a = vertices[i], b = vertices[j];
    MirrorVertex m{};
    for (int c = 0; c < 3; ++c) {
      m.position[c] = (a.position[c] + b.position[c]) * 0.5f;
    }
    for (int c = 0; c < 2; ++c) {
      m.uv[c] = (a.uv[c] + b.uv[c]) * 0.5f;
    }
    for (int c = 0; c < 4; ++c) {
      m.mul[c] = half((from_half(a.mul[c]) + from_half(b.mul[c])) * 0.5f);
      m.add[c] = half((from_half(a.add[c]) + from_half(b.add[c])) * 0.5f);
    }
    vertices.push_back(m);
    marks.push_back(marks[i] & marks[j]);
    const auto index = static_cast<uint32_t>(vertices.size() - 1);
    midpoints.emplace(key, index);
    return index;
  };
  static thread_local std::vector<uint32_t> out;
  // Children keep their parent's winding.
  const auto emit = [&](auto& self, uint32_t v0, uint32_t v1, uint32_t v2, int level) -> void {
    const bool s01 = level < 24 && splits(v0, v1), s12 = level < 24 && splits(v1, v2), s20 = level < 24 && splits(v2, v0);
    const int count = s01 + s12 + s20;
    if (count == 0) {
      out.insert(out.end(), {v0, v1, v2});
      return;
    }
    if (count == 3) {
      const uint32_t m01 = midpoint(v0, v1), m12 = midpoint(v1, v2), m20 = midpoint(v2, v0);
      self(self, v0, m01, m20, level + 1);
      self(self, m01, v1, m12, level + 1);
      self(self, m20, m12, v2, level + 1);
      self(self, m01, m12, m20, level + 1);
      return;
    }
    // Rotated so the split edges come first: (v0 v1), then (v1 v2).
    if (count == 1) {
      if (s12) {
        return self(self, v1, v2, v0, level);
      }
      if (s20) {
        return self(self, v2, v0, v1, level);
      }
      const uint32_t m01 = midpoint(v0, v1);
      self(self, v0, m01, v2, level + 1);
      self(self, m01, v1, v2, level + 1);
      return;
    }
    if (!s01) {
      return self(self, v1, v2, v0, level);
    }
    if (!s12) {
      return self(self, v2, v0, v1, level);
    }
    const uint32_t m01 = midpoint(v0, v1), m12 = midpoint(v1, v2);
    self(self, m01, v1, m12, level + 1);
    self(self, v0, m01, m12, level + 1);
    self(self, v0, m12, v2, level + 1);
  };
  for (Group& group : groups) {
    bool near = false;
    for (uint32_t i : group.indices) {
      if (depth(i) < plane) {
        near = true;
        break;
      }
    }
    if (!near) {
      continue;
    }
    out.clear();
    for (size_t t = 0; t + 2 < group.indices.size(); t += 3) {
      emit(emit, group.indices[t], group.indices[t + 1], group.indices[t + 2], 0);
    }
    group.indices.swap(out);
  }
  recent = std::max(recent - recent / 8, midpoints.size());
}

// --- Finishing a frame: on a worker of its own, so neither the FIFO thread nor the game thread
// (which drains the FIFO before the HUD and at the frame's end) waits for it.

struct Job {
  Built built;
  std::vector<Group> groups;
  std::vector<std::pair<uint32_t, uint32_t>> backgroundRanges, decalRanges;
  std::vector<AlphaOp> alphaOps;       // 0110
  std::vector<AlphaLayer> alphaLayers;
  float alphaStart = 0.f;
  float farthest = 0, nearestBackground = INFINITY;
  bool scene = false;
  uint32_t tag = 0;
  float focus = 0, tanHalfX = 1, tanHalfY = 1;
  float sill = 0;    // 0111: where the window's foot meets the player's ground (kMarkSill; 0: none)
  float subject = 0; // 0111: the player's nearest view depth, to keep whole (kMarkSubject; 0: none)
  bool framed = false; // 0111: an event's camera, framing its own subject (kMarkFramed)
  bool cut = false;    // 0111: the camera cut since the frame before (kMarkCut)
  std::vector<OwnedTexture> textures;
  std::vector<uint64_t> removed;
};

std::mutex g_jobMutex;
std::condition_variable g_jobReady;
std::condition_variable g_jobTaken; // 0108: for AURORA_MIRROR_NO_DROP
Job g_job; // handed over, waiting for the worker
bool g_jobPending = false;
std::atomic<uint64_t> g_finishNanos{0}, g_finished{0}, g_dropped{0};
// The port's stats (take_stats) and the worker's state: busy from taking a job to finishing it.
std::atomic<bool> g_workerBusy{false};
std::atomic<uint64_t> g_statRecorded{0}, g_statSkipped{0}, g_statDropped{0}, g_statFinished{0};
std::atomic<uint64_t> g_statRecordNs{0}, g_statFinishNs{0};
uint64_t g_recordWallStart = 0;
uint64_t g_lastRecordNs = 0;
// The FIFO thread's CPU time over the last recorded scene (the mirror's own cost and the commands'
// processing: what the game's end of frame waits for), and the stats' total of it.
uint64_t g_lastSceneCpuNs = 0;
bool g_skippedLast = false;
std::atomic<uint64_t> g_statSceneCpuNs{0};
std::atomic<uint32_t> g_lastParts{0};
std::atomic<uint64_t> g_alphaLayerTriangles{0}; // 0110: the pieces cut where lights meet the ground (periodic log)
// The last frame's glass and focus (game units) and its share of vertices squashed into the near
// band, for the periodic log.
std::atomic<float> g_lastPlane{0}, g_lastFocus{0}, g_lastBandShare{0};
// How far the window lets the scene come out in front of its glass (window widths; 0: as far as it
// goes), and how far in front of it the camera is placed (window widths; 0: where its view puts it,
// 1 / (2 tan)) (set_front_allowance).
std::atomic<float> g_frontAllowance{0}, g_cameraWidths{0};
std::atomic<float> g_lastAllowance{0}; // the last frame's, the lesser limit (finish)
std::atomic<float> g_lastCut{0};       // 0111: the last frame's near cut, as a share of the glass's distance
std::atomic<float> g_lastSill{0};      // and the sill, likewise (0: none)
std::atomic<float> g_lastSubject{0};   // and the player's nearest depth, likewise (0: none)
std::atomic<bool> g_lastKeptPlayer{false}; // and whether the near field was eased to keep him whole
std::atomic<float> g_lastPlaneTarget{0}; // and the glass's distance it eases toward (game units)

// 0110: the alpha layers (see AlphaOp), worked into what they lie on. A layer's quad blends a colour
// over the picture by the alpha left at each point: GX: out = C * fs(a) + dst * fd(a), in the
// picture's gamma. Here each solid draw before the layer (and the background) takes it in place, as
// its vertices' colours: mul * fd and add * fd + C * fs, which the shader works out in the picture's
// gamma too (the texture's colour times mul, plus add), so it's GX's own sum at each vertex. Where a
// light's volume meets the ground (one whose operations count only where a surface lies inside it:
// drawn from behind against the surface's depth, from the front, and taken off again, as the game's
// lights are) the ground's triangles are cut along the volume's own planes, each piece taking the
// alpha at its middle: the edge of a light's circle as sharp as the game's. Other volumes (counting
// in front of a surface or behind it too) are taken at the vertices. (Laid over the ground as a
// blended copy instead, the copy fell behind the eased ground near the camera in patches, a cave's
// lit rim, and copying a dark room's every triangle cost the finishing worker 20 ms a frame.)
// And the translucent draws a multiplying layer darkens in place (AlphaLayer::darken).
// Test runs: AURORA_MIRROR_ALPHA_LOG=1 logs the operations and layers now and then.
size_t build_alpha_layers(Job& job) {
  Built& out = job.built;
  const size_t before = out.vertices.size();
  if (job.alphaLayers.empty()) {
    return 0;
  }
  static const bool s_log = [] {
    const char* value = std::getenv("AURORA_MIRROR_ALPHA_LOG");
    return value != nullptr && std::strcmp(value, "1") == 0;
  }();
  static uint32_t s_logged = 0;
  const bool log = s_log && s_logged++ % 150 == 0;
  if (log) {
    for (size_t i = 0; i < job.alphaOps.size(); ++i) {
      const AlphaOp& op = job.alphaOps[i];
      Log.info("mirror alpha op {}: {} faces {}{} depth {}/{} blend {} {}->{} alpha {:.3f}{} centre ({:.0f} {:.0f} {:.0f}) "
               "radius {:.0f}, {} planes, group {}",
               i, op.screen ? "screen" : "volume", op.front ? "F" : "", op.back ? "B" : "", op.depthTest,
               static_cast<int>(op.depthFunc), static_cast<int>(op.mode), static_cast<int>(op.src),
               static_cast<int>(op.dst), op.alpha, op.constant ? fmt::format(" (written {:.3f})", op.constantAlpha) : "",
               op.centre[0], op.centre[1], op.centre[2], op.radius, op.planes.size(), op.group);
    }
  }
  struct Sphere {
    float c[3], r;
  };
  struct Plan {
    AlphaLayer* layer;
    size_t opCount;
    std::vector<uint8_t> live;
    std::vector<Sphere> spheres; // every live volume's
    float away;
    bool everywhere;
  };
  const float origin[3] = {0.f, 0.f, -1.f};
  std::vector<Plan> plans;
  // The volumes cut along (inside-only ones, any layer's), each once.
  std::vector<const AlphaOp*> cut;
  for (AlphaLayer& layer : job.alphaLayers) {
    Plan plan{&layer, std::min(layer.ops, job.alphaOps.size()), {}, {}, 0.f, false};
    const size_t opCount = plan.opCount;
    // Operations that leave nothing behind: a volume drawn again later, untested, setting the alpha
    // to a constant (a real-time shadow's box: drawn into the alpha, then cleared), and such a
    // setting that only gives back the alpha around it.
    plan.live.assign(opCount, 1);
    for (size_t j = 0; j < opCount; ++j) {
      const AlphaOp& reset = job.alphaOps[j];
      const bool sets = !reset.screen && !reset.planes.empty() && !reset.depthTest && reset.front && reset.back &&
                        (reset.constant || reset.mode == GX_BM_NONE);
      if (!sets) {
        continue;
      }
      for (size_t i = 0; i < j; ++i) {
        const AlphaOp& op = job.alphaOps[i];
        if (op.screen || op.planes.empty()) {
          continue;
        }
        float d2 = 0.f;
        for (int c = 0; c < 3; ++c) {
          d2 += (op.centre[c] - reset.centre[c]) * (op.centre[c] - reset.centre[c]);
        }
        // (The same volume: the shadow's box, drawn again.)
        if (std::sqrt(d2) <= 0.01f * reset.radius && std::fabs(op.radius - reset.radius) <= 0.01f * reset.radius) {
          plan.live[i] = 0;
        }
      }
      const float written = reset.constant ? reset.constantAlpha : reset.alpha;
      if (std::fabs(written - alpha_at(job.alphaOps, j, job.alphaStart, origin, false)) < 1.f / 512.f) {
        plan.live[j] = 0;
      }
    }
    plan.away = alpha_at(job.alphaOps, opCount, job.alphaStart, origin, false);
    float fs0 = alpha_factor(layer.src, true, layer.alpha, plan.away);
    float fd0 = alpha_factor(layer.dst, false, layer.alpha, plan.away);
    plan.everywhere = (fs0 * (layer.colour[0] + layer.colour[1] + layer.colour[2]) > 1e-3f) || std::fabs(fd0 - 1.f) > 1e-3f;
    for (size_t i = 0; i < opCount; ++i) {
      const AlphaOp& op = job.alphaOps[i];
      if (!plan.live[i] || op.screen || op.planes.empty()) {
        continue;
      }
      const auto same = [&](const AlphaOp& other) {
        return std::fabs(other.centre[0] - op.centre[0]) + std::fabs(other.centre[1] - op.centre[1]) +
                       std::fabs(other.centre[2] - op.centre[2]) <=
                   0.01f * op.radius &&
               std::fabs(other.radius - op.radius) <= 0.01f * op.radius;
      };
      if (std::any_of(plan.spheres.begin(), plan.spheres.end(), [&](const Sphere& s) {
            return std::fabs(s.c[0] - op.centre[0]) + std::fabs(s.c[1] - op.centre[1]) + std::fabs(s.c[2] - op.centre[2]) <=
                       0.01f * op.radius &&
                   std::fabs(s.r - op.radius) <= 0.01f * op.radius;
          })) {
        continue; // the same volume's other draws
      }
      plan.spheres.push_back({{op.centre[0], op.centre[1], op.centre[2]}, op.radius});
      if (std::any_of(cut.begin(), cut.end(), [&](const AlphaOp* other) { return same(*other); })) {
        continue;
      }
      // Inside-only: just in front of it and just behind it, on the line through its middle, with
      // only its own draws (and those over the whole picture), as away from it.
      // The game's own pattern (dDlst_alphaModel_c and the spot model: from behind, depth GEQUAL,
      // added; from the front, LEQUAL, added; from the front, untested, taken off) counts only inside,
      // wherever the camera is (a searchlight's beam reaching past it).
      int behindAdd = 0, frontAdd = 0, frontSub = 0, others = 0;
      for (size_t k = 0; k < opCount; ++k) {
        const AlphaOp& other = job.alphaOps[k];
        if (!plan.live[k] || other.screen || !same(other)) {
          continue;
        }
        const bool adds = other.mode == GX_BM_BLEND && other.src == GX_BL_ONE && other.dst == GX_BL_ONE;
        if (other.back && !other.front && other.depthTest && other.depthFunc == GX_GEQUAL && adds) {
          ++behindAdd;
        } else if (other.front && !other.back && other.depthTest && other.depthFunc == GX_LEQUAL && adds) {
          ++frontAdd;
        } else if (other.front && !other.back && !other.depthTest && other.mode == GX_BM_SUBTRACT) {
          ++frontSub;
        } else {
          ++others;
        }
      }
      const bool pattern = behindAdd == 1 && frontAdd == 1 && frontSub == 1 && others == 0;
      const float d = std::sqrt(op.centre[0] * op.centre[0] + op.centre[1] * op.centre[1] + op.centre[2] * op.centre[2]);
      bool insideOnly = !pattern && d > 1.1f * op.radius;
      if (insideOnly) {
        std::vector<uint8_t> own(opCount, 0);
        for (size_t k = 0; k < opCount; ++k) {
          own[k] = plan.live[k] && (job.alphaOps[k].screen || same(job.alphaOps[k]));
        }
        const float ownAway = alpha_at(job.alphaOps, opCount, job.alphaStart, origin, false, &own);
        float front[3], behind[3];
        for (int c = 0; c < 3; ++c) {
          front[c] = op.centre[c] * (1.f - 1.05f * op.radius / d);
          behind[c] = op.centre[c] * (1.f + 1.05f * op.radius / d);
        }
        insideOnly = std::fabs(alpha_at(job.alphaOps, opCount, job.alphaStart, front, true, &own) - ownAway) < 1.f / 512.f &&
                     std::fabs(alpha_at(job.alphaOps, opCount, job.alphaStart, behind, true, &own) - ownAway) < 1.f / 512.f;
      }
      if (pattern || insideOnly) {
        cut.push_back(&op);
      }
    }
    plans.push_back(std::move(plan));
  }
  // Nothing to change (the game draws its alpha's quad every frame, lights or none): no volume left,
  // and away from them the layers add nothing and keep everything.
  if (cut.empty() && std::all_of(plans.begin(), plans.end(), [](const Plan& plan) {
        return plan.spheres.empty() && !plan.everywhere;
      })) {
    return 0;
  }
  // The alpha a layer leaves at a point: the one away from them all, outside every volume's sphere.
  const auto alphaAt = [&](const Plan& plan, const float p[3]) {
    const float pp = p[0] * p[0] + p[1] * p[1] + p[2] * p[2];
    for (const Sphere& sphere : plan.spheres) {
      const float cp = sphere.c[0] * p[0] + sphere.c[1] * p[1] + sphere.c[2] * p[2];
      const float t = pp > 0.f ? std::max(cp / pp, 0.f) : 0.f;
      float d2 = 0.f;
      for (int c = 0; c < 3; ++c) {
        const float v = sphere.c[c] - t * p[c];
        d2 += v * v;
      }
      if (d2 <= sphere.r * sphere.r) {
        return alpha_at(job.alphaOps, plan.opCount, job.alphaStart, p, true, &plan.live);
      }
    }
    return plan.away;
  };
  // The layer's blend at a vertex, in place: mul * fd, add * fd + C * fs.
  const auto apply = [](const Plan& plan, MirrorVertex& vertex, float a) {
    const AlphaLayer& layer = *plan.layer;
    const float fs = std::max(alpha_factor(layer.src, true, layer.alpha, a), 0.f);
    const float fd = std::clamp(alpha_factor(layer.dst, false, layer.alpha, a), 0.f, 1.f);
    for (int c = 0; c < 3; ++c) {
      vertex.mul[c] = half(from_half(vertex.mul[c]) * fd);
      vertex.add[c] = half(from_half(vertex.add[c]) * fd + layer.colour[c] * fs);
    }
  };
  const auto lerp = [](const MirrorVertex& a, const MirrorVertex& b, float t) {
    MirrorVertex m{};
    for (int c = 0; c < 3; ++c) {
      m.position[c] = a.position[c] + (b.position[c] - a.position[c]) * t;
    }
    for (int c = 0; c < 2; ++c) {
      m.uv[c] = a.uv[c] + (b.uv[c] - a.uv[c]) * t;
    }
    for (int c = 0; c < 4; ++c) {
      m.mul[c] = half(from_half(a.mul[c]) + (from_half(b.mul[c]) - from_half(a.mul[c])) * t);
      m.add[c] = half(from_half(a.add[c]) + (from_half(b.add[c]) - from_half(a.add[c])) * t);
    }
    return m;
  };
  // A polygon's corner: the vertex, and which of its triangle's edges it lies on (bit k: the edge
  // from corner k to corner k + 1; a corner of the triangle lies on two).
  struct Corner {
    MirrorVertex v;
    uint8_t edges;
  };
  // A convex polygon's parts inside (n.x <= d) and outside a plane.
  const auto split = [&](const std::vector<Corner>& polygon, const std::array<float, 4>& plane, std::vector<Corner>& in,
                         std::vector<Corner>& outside) {
    in.clear();
    outside.clear();
    const size_t n = polygon.size();
    for (size_t i = 0; i < n; ++i) {
      const Corner& a = polygon[i];
      const Corner& b = polygon[(i + 1) % n];
      const float da = plane[0] * a.v.position[0] + plane[1] * a.v.position[1] + plane[2] * a.v.position[2] - plane[3];
      const float db = plane[0] * b.v.position[0] + plane[1] * b.v.position[1] + plane[2] * b.v.position[2] - plane[3];
      (da <= 0.f ? in : outside).push_back(a);
      if ((da <= 0.f) != (db <= 0.f)) {
        const Corner m{lerp(a.v, b.v, da / (da - db)), static_cast<uint8_t>(a.edges & b.edges)};
        in.push_back(m);
        outside.push_back(m);
      }
    }
  };
  // Where a point lies along the edge between two of the frame's vertices, from the lower-numbered.
  const auto edgeKey = [](uint32_t i, uint32_t j) { return static_cast<uint64_t>(std::min(i, j)) << 32 | std::max(i, j); };
  const auto along = [&](uint32_t i, uint32_t j, const float p[3]) {
    const float* a = out.vertices[std::min(i, j)].position;
    const float* b = out.vertices[std::max(i, j)].position;
    float ab[3], ap = 0.f, ab2 = 0.f;
    for (int c = 0; c < 3; ++c) {
      ab[c] = b[c] - a[c];
      ap += (p[c] - a[c]) * ab[c];
      ab2 += ab[c] * ab[c];
    }
    return ab2 > 0.f ? ap / ab2 : 0.f;
  };
  struct Piece {
    std::vector<Corner> corners;
    uint32_t tri[3];
  };
  struct Item {
    uint32_t tri[3];
    uint32_t first, end; // its pieces, when it's cut (first == end: whole)
  };
  static thread_local std::vector<Piece> allPieces;
  static thread_local std::vector<std::vector<Corner>> pieces;
  static thread_local std::vector<Corner> cutIn, cutOut;
  static thread_local std::vector<const AlphaOp*> cutHere;
  static thread_local std::vector<uint8_t> shaded; // original vertices to shade in place
  static thread_local std::vector<uint32_t> kept;
  static thread_local std::vector<float> alphas;
  static thread_local std::vector<Item> items;
  static thread_local std::vector<std::pair<size_t, size_t>> groupItems; // per group, its items (none: not ground)
  // The points cut into each of the frame's edges, to be cut into the triangles on the edge's other
  // side too: with a vertex there on one side only, the easing in front of the glass (which moves
  // every vertex along its own line of sight) opened a crack along the edge.
  static thread_local absl::flat_hash_map<uint64_t, std::vector<std::pair<float, MirrorVertex>>> edgePoints;
  shaded.assign(before, 0);
  allPieces.clear();
  items.clear();
  groupItems.assign(job.groups.size(), {0, 0});
  edgePoints.erase(edgePoints.begin(), edgePoints.end());
  size_t cutTriangles = 0, pieceCount = 0, shadedCount = 0;
  const auto isGround = [](const Group& group) {
    // Solid ground, cut-out included.
    return !group.blended && (group.part.kind == aurora::mirror::PartOpaque || group.part.kind == aurora::mirror::PartCutout) &&
           (group.part.flags & aurora::mirror::PartDepthWrite) != 0 && (group.part.flags & aurora::mirror::PartBackground) == 0;
  };
  // Each triangle cut along the volumes whose edge crosses it, or left whole.
  for (size_t g = 0; g < job.groups.size(); ++g) {
    Group& group = job.groups[g];
    if (!isGround(group)) {
      continue;
    }
    groupItems[g].first = items.size();
    const size_t count = group.indices.size();
    for (size_t t = 0; t + 2 < count; t += 3) {
      Item item{{group.indices[t], group.indices[t + 1], group.indices[t + 2]}, 0, 0};
      const uint32_t* tri = item.tri;
      const uint32_t last = std::max({tri[0], tri[1], tri[2]});
      bool any = false;
      for (const Plan& plan : plans) {
        any = any || last < plan.layer->vertexEnd;
      }
      if (!any || last >= before) {
        items.push_back(item);
        continue;
      }
      float low[3], high[3];
      for (int c = 0; c < 3; ++c) {
        low[c] = std::min({out.vertices[tri[0]].position[c], out.vertices[tri[1]].position[c], out.vertices[tri[2]].position[c]});
        high[c] = std::max({out.vertices[tri[0]].position[c], out.vertices[tri[1]].position[c], out.vertices[tri[2]].position[c]});
      }
      // The volumes whose edge crosses it: not all its corners outside one plane (then it's out), nor
      // all inside every plane (then it's in, and its corners say so).
      cutHere.clear();
      for (const AlphaOp* volume : cut) {
        bool overlaps = true;
        for (int c = 0; c < 3 && overlaps; ++c) {
          overlaps = volume->centre[c] + volume->radius >= low[c] && volume->centre[c] - volume->radius <= high[c];
        }
        if (!overlaps) {
          continue;
        }
        bool outside = false, inside = true;
        for (const auto& plane : volume->planes) {
          int beyond = 0;
          for (int k = 0; k < 3; ++k) {
            const float* q = out.vertices[tri[k]].position;
            beyond += plane[0] * q[0] + plane[1] * q[1] + plane[2] * q[2] - plane[3] > 0.f;
          }
          if (beyond == 3) {
            outside = true;
            break;
          }
          inside = inside && beyond == 0;
        }
        if (!outside && !inside) {
          cutHere.push_back(volume);
        }
      }
      if (cutHere.empty()) {
        items.push_back(item);
        continue;
      }
      ++cutTriangles;
      pieces.clear();
      pieces.push_back({{out.vertices[tri[0]], 0b101}, {out.vertices[tri[1]], 0b011}, {out.vertices[tri[2]], 0b110}});
      for (const AlphaOp* volume : cutHere) {
        const size_t pieceCountNow = pieces.size();
        for (size_t q = 0; q < pieceCountNow; ++q) {
          std::vector<Corner> inside = std::move(pieces[q]);
          pieces[q].clear();
          bool placed = false;
          for (const auto& plane : volume->planes) {
            split(inside, plane, cutIn, cutOut);
            if (cutOut.size() >= 3) {
              if (!placed) {
                pieces[q] = cutOut;
                placed = true;
              } else {
                pieces.push_back(cutOut);
              }
            }
            inside.swap(cutIn);
            if (inside.size() < 3) {
              break;
            }
          }
          if (inside.size() >= 3) {
            if (!placed) {
              pieces[q] = std::move(inside);
            } else {
              pieces.push_back(std::move(inside));
            }
          }
        }
      }
      item.first = static_cast<uint32_t>(allPieces.size());
      for (std::vector<Corner>& piece : pieces) {
        if (piece.size() < 3) {
          continue;
        }
        // Its points on the triangle's edges (a cut, not a corner), for the triangles beyond them.
        for (const Corner& corner : piece) {
          for (int e = 0; e < 3; ++e) {
            if (corner.edges == (1u << e)) {
              const uint32_t i = tri[e], j = tri[(e + 1) % 3];
              edgePoints[edgeKey(i, j)].emplace_back(along(i, j, corner.v.position), corner.v);
            }
          }
        }
        Piece made;
        made.corners = std::move(piece);
        std::copy(tri, tri + 3, made.tri);
        allPieces.push_back(std::move(made));
      }
      item.end = static_cast<uint32_t>(allPieces.size());
      items.push_back(item);
    }
    groupItems[g].second = items.size();
  }
  // A polygon's edges on its triangle's edges with the points cut there from either side put in.
  static thread_local std::vector<Corner> welded;
  const auto weld = [&](const std::vector<Corner>& polygon, const uint32_t tri[3]) {
    welded.clear();
    const size_t n = polygon.size();
    for (size_t k = 0; k < n; ++k) {
      const Corner& a = polygon[k];
      const Corner& b = polygon[(k + 1) % n];
      welded.push_back(a);
      const uint8_t common = a.edges & b.edges;
      for (int e = 0; e < 3; ++e) {
        if ((common & (1u << e)) == 0) {
          continue;
        }
        const uint32_t i = tri[e], j = tri[(e + 1) % 3];
        const auto found = edgePoints.find(edgeKey(i, j));
        if (found == edgePoints.end()) {
          break;
        }
        const float ta = along(i, j, a.v.position), tb = along(i, j, b.v.position);
        const float lo = std::min(ta, tb), hi = std::max(ta, tb), eps = 1e-5f;
        const size_t from = welded.size();
        for (const auto& [t, v] : found->second) {
          if (t > lo + eps && t < hi - eps) {
            welded.push_back({v, static_cast<uint8_t>(1u << e)});
          }
        }
        // In order from a to b, each once.
        std::sort(welded.begin() + static_cast<std::ptrdiff_t>(from), welded.end(), [&](const Corner& x, const Corner& y) {
          const float tx = along(i, j, x.v.position), ty = along(i, j, y.v.position);
          return ta < tb ? tx < ty : tx > ty;
        });
        welded.erase(std::unique(welded.begin() + static_cast<std::ptrdiff_t>(from), welded.end(),
                                 [&](const Corner& x, const Corner& y) {
                                   return std::fabs(along(i, j, x.v.position) - along(i, j, y.v.position)) <= eps;
                                 }),
                     welded.end());
        break;
      }
    }
    return welded.size() > n;
  };
  // Each ground group again: whole triangles kept (welded where a neighbour was cut), pieces in place
  // of the cut ones, each at the alpha at its middle.
  for (size_t g = 0; g < job.groups.size(); ++g) {
    if (!isGround(job.groups[g])) {
      continue;
    }
    Group& group = job.groups[g];
    kept.clear();
    for (size_t it = groupItems[g].first; it < groupItems[g].second; ++it) {
      const Item& item = items[it];
      const uint32_t last = std::max({item.tri[0], item.tri[1], item.tri[2]});
      const bool layered = last < before && std::any_of(plans.begin(), plans.end(), [&](const Plan& plan) {
                             return last < plan.layer->vertexEnd;
                           });
      if (item.first == item.end) {
        if (layered) {
          for (uint32_t v : item.tri) {
            shaded[v] = 1;
          }
        }
        static thread_local std::vector<Corner> whole;
        bool welds = false;
        if (layered && !edgePoints.empty()) {
          whole = {{out.vertices[item.tri[0]], 0b101}, {out.vertices[item.tri[1]], 0b011}, {out.vertices[item.tri[2]], 0b110}};
          welds = weld(whole, item.tri);
        }
        if (!welds) {
          kept.insert(kept.end(), {item.tri[0], item.tri[1], item.tri[2]});
          continue;
        }
        // Its own corners (shaded in place) and the points cut in from beyond (new, shaded here).
        static thread_local std::vector<uint32_t> ring;
        ring.clear();
        for (const Corner& corner : welded) {
          uint32_t index = UINT32_MAX;
          for (int k = 0; k < 3; ++k) {
            const uint8_t cornerEdges = k == 0 ? 0b101 : k == 1 ? 0b011 : 0b110;
            if (corner.edges == cornerEdges) {
              index = item.tri[k];
            }
          }
          if (index == UINT32_MAX) {
            MirrorVertex v = corner.v;
            for (const Plan& plan : plans) {
              if (last < plan.layer->vertexEnd) {
                apply(plan, v, alphaAt(plan, v.position));
              }
            }
            index = static_cast<uint32_t>(out.vertices.size());
            out.vertices.push_back(v);
          }
          ring.push_back(index);
        }
        for (size_t k = 1; k + 1 < ring.size(); ++k) {
          kept.insert(kept.end(), {ring[0], ring[k], ring[k + 1]});
        }
        continue;
      }
      for (uint32_t p = item.first; p < item.end; ++p) {
        const Piece& piece = allPieces[p];
        ++pieceCount;
        float middle[3] = {0.f, 0.f, 0.f};
        for (const Corner& corner : piece.corners) {
          for (int c = 0; c < 3; ++c) {
            middle[c] += corner.v.position[c] / static_cast<float>(piece.corners.size());
          }
        }
        alphas.resize(plans.size());
        for (size_t l = 0; l < plans.size(); ++l) {
          alphas[l] = last < plans[l].layer->vertexEnd ? alphaAt(plans[l], middle) : 0.f;
        }
        const std::vector<Corner>& polygon = weld(piece.corners, piece.tri) ? welded : piece.corners;
        const auto first = static_cast<uint32_t>(out.vertices.size());
        for (const Corner& corner : polygon) {
          MirrorVertex v = corner.v;
          for (size_t l = 0; l < plans.size(); ++l) {
            if (last < plans[l].layer->vertexEnd) {
              apply(plans[l], v, alphas[l]);
            }
          }
          out.vertices.push_back(v);
        }
        for (uint32_t k = 1; k + 1 < polygon.size(); ++k) {
          kept.insert(kept.end(), {first, first + k, first + k + 1});
        }
      }
    }
    group.indices.swap(kept);
  }
  // The whole triangles' corners, each once.
  for (uint32_t v = 0; v < before; ++v) {
    if (!shaded[v]) {
      continue;
    }
    ++shadedCount;
    for (const Plan& plan : plans) {
      if (v < plan.layer->vertexEnd) {
        apply(plan, out.vertices[v], alphaAt(plan, out.vertices[v].position));
      }
    }
  }
  // The translucent draws a multiplying layer darkens in place: their colour (premultiplied: rgb and
  // what they let through alike) times fd at each vertex, as the ground under them is.
  for (const Plan& plan : plans) {
    for (const auto& [first, end] : plan.layer->darken) {
      for (uint32_t v = first; v < end && v < before; ++v) {
        MirrorVertex& vertex = out.vertices[v];
        const float f = std::clamp(alpha_factor(plan.layer->dst, false, plan.layer->alpha, alphaAt(plan, vertex.position)), 0.f, 1.f);
        for (int c = 0; c < 3; ++c) {
          vertex.mul[c] = half(from_half(vertex.mul[c]) * f);
          vertex.add[c] = half(from_half(vertex.add[c]) * f);
        }
      }
    }
    if (log) {
      const AlphaLayer& layer = *plan.layer;
      Log.info("mirror alpha layer: {} operations ({} live), colour ({:.2f} {:.2f} {:.2f}) x {} + dst x {}, alpha {:.2f}; "
               "away {:.3f}{}, {} volumes, {} translucent ranges darkened",
               plan.opCount, std::count(plan.live.begin(), plan.live.end(), 1), layer.colour[0], layer.colour[1], layer.colour[2],
               static_cast<int>(layer.src), static_cast<int>(layer.dst), layer.alpha, plan.away,
               plan.everywhere ? " (everywhere)" : "", plan.spheres.size(), layer.darken.size());
    }
  }
  g_alphaLayerTriangles += pieceCount;
  if (log) {
    Log.info("mirror alpha layers in place: {} volumes cut along, {} triangles cut into {} pieces, {} vertices shaded",
             cut.size(), cutTriangles, pieceCount, shadedCount);
  }
  return out.vertices.size() - before;
}

void finish(Job& job) {
  Built& out = job.built;
  out.tag = job.tag;
  out.scene = job.scene && !job.groups.empty();
  out.focus = job.focus;
  out.plane = std::clamp(job.focus * 0.85f, 50.f, 20000.f);
  // 0111: the glass at the ground's sill (kMarkSill: where the line of sight through the middle of
  // the window's foot, in the window's re-aimed view, meets the ground at the player's feet), when
  // that's nearer: the ground at the window's foot lies on the glass, as through a real window, and
  // never comes out in front of it, where it was eased (it bent) or would be cut (a viewer farther
  // away than the camera's place looked under its cut edge, at sky: TPVR's finding). Not nearer
  // than a fifth of the focus (TPVR's half: Wind Waker's sailing camera, far and low, has the sea
  // meet the window's foot at a quarter of the focus, and with the glass at half of it the sea
  // there was cut, which left a band of the backdrop along the window's foot; nearer than a fifth,
  // a camera looking steeply down would have the world swell). Test runs: AURORA_MIRROR_GLASS=focus
  // keeps the glass at the focus alone; AURORA_MIRROR_GLASS_FLOOR=<share of the focus> (0.5:
  // TPVR's).
  static const bool s_sill = [] {
    const char* value = std::getenv("AURORA_MIRROR_GLASS");
    return value == nullptr || std::strcmp(value, "focus") != 0;
  }();
  static const float s_glassFloor = [] {
    const char* value = std::getenv("AURORA_MIRROR_GLASS_FLOOR");
    const float share = value != nullptr ? std::strtof(value, nullptr) : 0.2f;
    return std::isfinite(share) ? std::clamp(share, 0.05f, 0.85f) : 0.2f;
  }();
  if (s_sill && job.sill > 0.f) {
    out.plane = std::clamp(std::min(out.plane, std::max(job.sill, s_glassFloor * job.focus)), 50.f, 20000.f);
  }
  const float planeTarget = out.plane;
  out.tanHalfX = job.tanHalfX;
  out.tanHalfY = job.tanHalfY;
  out.indices.clear();
  out.parts.clear();
  // 0111: the glass's eased distance and the focus and time it was eased at, the one finishing
  // worker's. A frame without a scene (a load, a film's black) ends the easing: the next scene is a
  // cut.
  static float s_plane = 0.f, s_planeFocus = 0.f;
  static std::chrono::steady_clock::time_point s_planeAt;
  // And the share of it the glass comes nearer, to the player (below), and the focus it was at.
  static float s_share = 1.f, s_shareFocus = 0.f;
  float elapsed = 0.f; // since the finishing worker's last frame with a scene (seconds, at most 0.1)
  static const float s_planeSeconds = [] {
    const char* value = std::getenv("AURORA_MIRROR_PLANE_SECONDS");
    const float seconds = value != nullptr ? std::strtof(value, nullptr) : kPlaneSeconds;
    return std::isfinite(seconds) ? std::max(seconds, 0.f) : kPlaneSeconds;
  }();
  if (!out.scene) {
    out.vertices.clear();
    s_plane = 0.f;
    s_planeFocus = 0.f;
    s_share = 1.f;
    s_shareFocus = 0.f;
  } else {
    // 0111: the glass's distance, and with it the world's scale in the window (one window width
    // spans the picture there), follows its target slowly (time constant kPlaneSeconds): the camera
    // closing on Link and backing off moves the world rather than rescaling it, and the sill's
    // steps don't jolt it. At once on a cut (the game's camera cut, MarkCut, or its focus jumped by
    // 30% in a frame), after a frame without a scene, and after a gap (the window in 2D sends none);
    // and never more than 30% beyond where it's going (a camera closing fast in small steps would
    // leave the glass, and the cut with it, beyond its focus: TPVR's). Nearer than its target it
    // only eases out (TPVR's jumped to within 30%: Link walking out of an event's shot took the
    // sill away and the world shrank in a frame). Test runs: AURORA_MIRROR_PLANE_SECONDS=<s> (0: at
    // once, as before).
    {
      const auto now = std::chrono::steady_clock::now();
      const bool cut = job.cut || !(s_plane > 0.f) || !(s_planeFocus > 0.f) ||
                       std::fabs(job.focus - s_planeFocus) > 0.3f * s_planeFocus ||
                       now - s_planeAt > std::chrono::milliseconds(250);
      elapsed = cut ? 0.f : std::clamp(std::chrono::duration<float>(now - s_planeAt).count(), 0.f, 0.1f);
      if (cut || s_planeSeconds <= 0.f) {
        s_plane = out.plane;
      } else {
        s_plane += (out.plane - s_plane) * (1.f - std::exp(-elapsed / s_planeSeconds));
        s_plane = std::min(s_plane, out.plane * 1.3f);
      }
      s_planeFocus = job.focus;
      s_planeAt = now;
      out.plane = s_plane;
    }
    // The background behind everything else.
    if (!job.backgroundRanges.empty() && job.nearestBackground < INFINITY && job.farthest > 0) {
      const float push = std::clamp(1.25f * job.farthest / std::max(job.nearestBackground, 1e-3f), 1.f, 100.f);
      for (const auto& [first, end] : job.backgroundRanges) {
        for (uint32_t i = first; i < end && i < out.vertices.size(); ++i) {
          for (float& c : out.vertices[i].position) {
            c *= push;
          }
        }
      }
    }
    // Nothing in front of the glass: what's nearer than the plane goes back onto it along its own
    // line of sight (the ground at the picture's foot, grass by the camera). That changes the
    // triangles' shapes, so their textures would no longer be spread as the game spreads them: the
    // ground nearest the camera came out streaked. So first the near triangles are subdivided
    // until the depth along each edge changes little (the decision is the edge's own, so two
    // triangles sharing an edge split it alike, with one midpoint: no cracks).
    // The near geometry keeps its order front to back, in a band 3% deep behind the glass:
    // depths [0, P x 1.03] go to [P, P x 1.03] (laid flat on one plane, it z-fought).
    //
    // Not any more by default: the window's portal no longer cuts the scene at the glass (as in the
    // SHAR port), so what's nearer comes out in front of it, inside the window's opening, as the
    // shape it is. The band folded TP's pitched-down ground into a near-vertical sheet at the glass
    // ("the ground bends near the camera") and squeezed coplanar layers there until they z-fought.
    // Test runs: AURORA_MIRROR_BAND=1 brings the band back (and the portal's clip in the app).
    //
    // How far it comes out has limits, though. Someone farther from the window than the camera's
    // place (most are) looks in front of the glass more widely than the camera did: along the
    // window's foot, below the picture, they see towards the ground under the camera, which isn't
    // there (the draws are kept only out to kClipMargin times the view), and past it, under the
    // ground behind the glass, the sky: a strip of it along the window's foot. Their line of sight
    // still meets the ground if it comes out no farther than where that line leaves the kept
    // draws, which, from however far away, is (1 - 1 / kClipMargin) of the way to the camera.
    // And visionOS cuts a window's content off about half a metre in front of it (the window's
    // allowance, set_front_allowance). So beyond half the lesser of the two, what's nearer eases
    // into the rest along its own line of sight: the picture from the camera stays the same, and
    // the ground keeps its shape to below Link's feet. Easing changes triangles' shapes, so the
    // ones reaching that near are subdivided first, as for the band. (0111: no longer by default:
    // what would come out past the lesser of the two is cut there instead, below, and the glass
    // comes no farther than the ground at the window's foot, above.)
    static const bool s_band = [] {
      const char* band = std::getenv("AURORA_MIRROR_BAND");
      return band != nullptr && std::strcmp(band, "1") == 0;
    }();
    // The camera's distance in front of the glass, and the allowance, in window widths: the same
    // whatever the glass's distance.
    const float cameraWidths = g_cameraWidths.load(std::memory_order_relaxed);
    const float camera = cameraWidths > 0.f ? cameraWidths : 1.f / (2.f * out.tanHalfX);
    const float window = g_frontAllowance.load(std::memory_order_relaxed);
    const float allowance = std::min(window, 0.9f * camera * (1.f - 1.f / kClipMargin));
    // 0111: what's nearer than the glass is cut (below) unless a test run asks otherwise:
    // AURORA_MIRROR_NEAR=ease eases it, as before, =raw leaves it.
    enum class Near { Cut, Ease, Raw };
    static const Near s_near = [] {
      const char* value = std::getenv("AURORA_MIRROR_NEAR");
      if (value != nullptr && std::strcmp(value, "ease") == 0) {
        return Near::Ease;
      }
      if (value != nullptr && std::strcmp(value, "raw") == 0) {
        return Near::Raw;
      }
      return Near::Cut;
    }();
    // 0111: what the window keeps whole nearest the camera: the player (the game's mark), and in an
    // event's camera what lies nearest in the middle of the picture (its middle half each way; the
    // solid draws, not the sky): a close-up's focus can lie far behind the face it frames, and the
    // cut below took Aryll away whole on Outset's lookout. A depth a few hundred of those points
    // deep, not the very nearest, so a stray leaf or spark doesn't count.
    float subject = job.subject;
    if (job.framed) {
      static std::vector<float> s_depths;
      s_depths.clear();
      for (const Group& group : job.groups) {
        if (group.blended || (group.part.flags & aurora::mirror::PartBackground) != 0) {
          continue;
        }
        for (uint32_t i : group.indices) {
          const float* q = out.vertices[i].position;
          const float depth = -q[2];
          if (depth > 0.01f * job.focus && std::fabs(q[0]) <= 0.5f * out.tanHalfX * depth &&
              std::fabs(q[1]) <= 0.5f * out.tanHalfY * depth) {
            s_depths.push_back(depth);
          }
        }
      }
      if (s_depths.size() >= 64) {
        const size_t k = std::max<size_t>(32, s_depths.size() / 200);
        std::nth_element(s_depths.begin(), s_depths.begin() + k, s_depths.end());
        const float framed = s_depths[k];
        subject = subject > 0.f ? std::min(subject, framed) : framed;
      }
    }
    // 0111: the glass comes nearer, to the player (or what an event's camera frames), when he's
    // nearer the camera than the cut below would keep him whole (no nearer than a tenth of the
    // glass's distance beyond it): a camera pushed in close behind him, a lock-on with its focus
    // far beyond him, a close-up (TPVR's, for the player). The picture from the camera is the
    // same; he keeps his shape, and what's beyond goes deeper. Fully from a quarter of the glass's
    // distance, and eased in from 0.15 of it, and nearer by at most a tenth a frame unless the
    // camera cut (its focus jumped, or the game's camera cut: MarkCut), so the window's depths don't
    // jump as he crosses those. Back out it goes slowly (kPlaneSeconds), so the world doesn't swell
    // and shrink with every step or gesture of what it keeps (TPVR's went back as fast). What an
    // event's camera frames is kept down to a twentieth of the glass's distance (the player's floor
    // is for him gliding into first person; nearer than it, he's eased, then cut).
    const float keepFrom = job.framed ? 0.05f : 0.15f, keepSpan = job.framed ? 0.05f : 0.1f;
    {
      float share = 1.f;
      if (s_near == Near::Cut && subject > keepFrom * out.plane && window > 0.f && !s_band) {
        const float floor = 1.f - allowance / camera + 0.1f;
        const float whole = std::min(1.f, 0.95f * subject / (floor * out.plane));
        const float ease = std::clamp((subject - keepFrom * out.plane) / (keepSpan * out.plane), 0.f, 1.f);
        share = 1.f + (whole - 1.f) * ease;
      }
      if (!job.cut && s_shareFocus > 0.f && std::fabs(job.focus - s_shareFocus) < 0.3f * s_shareFocus) {
        share = share < s_share ? std::max(share, s_share * 0.9f)
                : s_planeSeconds > 0.f
                    ? std::min(s_share + (share - s_share) * (1.f - std::exp(-elapsed / s_planeSeconds)), s_share / 0.9f)
                    : std::min(share, s_share / 0.9f);
      }
      s_share = std::min(share, 1.f);
      s_shareFocus = job.focus;
      out.plane = std::clamp(out.plane * s_share, 50.f, 20000.f);
    }
    const float onPlane = out.plane * 1.0005f, band = 0.03f, bandEnd = onPlane * (1.f + band);
    // In window widths in front of the glass, at depth d: k (plane - d) (MirrorScene.place); the
    // camera is k plane in front of it.
    const float k = camera / out.plane;
    // C1 at the knee (it starts at slope 1), then nearing the allowance without reaching it.
    const float knee = allowance * 0.5f;
    const Easing easing{out.plane, k, knee, allowance - knee, out.plane - knee / k};
    // 0111: what's nearer than the glass keeps its shape, and what would come out past the
    // allowance is cut off there, as a camera's near plane cuts (TPVR's, which Trevor approved on
    // the headset). Eased into the allowance instead, everything nearer than the glass was
    // compressed along its line of sight, whether it would have fitted or not, and the ground at
    // the window's foot creased where the easing began, the crease following the camera every
    // frame. The ground there lies on the glass now (the sill, above). The cut never takes the
    // player, or what an event's camera frames: the glass comes to it first (above); while it's
    // still on its way (a lock-on, the camera closing in, a close-up), it within a tenth of the
    // cut's depth beyond it or nearer, what's nearer than the glass is eased into the allowance for
    // those frames, as before, or visionOS's own clip would cut it. Test runs: AURORA_MIRROR_NEAR=
    // ease eases always and keeps the glass off the player, as before; =raw leaves it all
    // (visionOS's own clip cuts it, about 0.47 m out).
    const float cutAt = out.plane - allowance / k;
    const bool keepPlayer = s_near == Near::Cut && subject > 0.f && subject < 1.1f * cutAt &&
                            (job.framed || subject >= 0.12f * out.plane);
    const bool ease = (s_near == Near::Ease || keepPlayer) && !s_band && window > 0.f && easing.end > 0.f;
    const bool cutting =
        s_near == Near::Cut && !keepPlayer && !s_band && window > 0.f && allowance > 0.f && cutAt > 1e-3f;
    // 0110: the alpha layers, worked into the ground they lie on (cut where a light meets it).
    build_alpha_layers(job);
    std::vector<uint8_t> decal(out.vertices.size(), 0);
    for (const auto& [first, end] : job.decalRanges) {
      std::fill(decal.begin() + first, decal.begin() + std::min<size_t>(end, decal.size()), uint8_t{1});
    }
    size_t moved = 0;
    if (s_band) {
      subdivide_near(out.vertices, job.groups, bandEnd, decal);
    } else if (ease) {
      subdivide_near(out.vertices, job.groups, easing.end, decal, &easing);
    } else if (cutting) {
      cut_near(out.vertices, job.groups, cutAt, decal);
    }
    for (size_t v = 0; v < out.vertices.size(); ++v) {
      float* p = out.vertices[v].position;
      const float depth = -p[2];
      float scale = 1.f;
      if (s_band && depth > 1e-3f && depth < bandEnd) {
        scale = (onPlane + band * onPlane * depth / bandEnd) / depth;
        ++moved;
      } else if (ease && depth > 1e-3f && depth < easing.end) {
        scale = easing.scale(depth);
        ++moved;
      }
      // A decal (a path, a stain, a shadow: drawn on what's under it) lifted towards the camera
      // once depths are final (lifted first, the easing shortened the lift): by 0.4% of its
      // distance, at most kDecalLift.
      if (decal[v] != 0) {
        const float distance = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]) * scale;
        scale *= 1.f - std::min(4e-3f, kDecalLift / std::max(distance, 1.f));
      }
      if (scale != 1.f) {
        for (int c = 0; c < 3; ++c) {
          p[c] *= scale;
        }
      }
    }
    g_lastPlane = out.plane;
    g_lastPlaneTarget = planeTarget;
    g_lastFocus = out.focus;
    g_lastAllowance = ease || cutting ? allowance : 0.f;
    g_lastCut = cutting ? cutAt / out.plane : 0.f;
    g_lastSill = job.sill / out.plane;
    g_lastSubject = subject / out.plane;
    g_lastKeptPlayer = keepPlayer && ease;
    // Test runs: AURORA_MIRROR_PLANE_LOG=1 logs each frame's glass (the dolly's evidence).
    static const bool s_planeLog = std::getenv("AURORA_MIRROR_PLANE_LOG") != nullptr;
    if (s_planeLog) {
      Log.info("mirror plane: frame {} focus {:.1f} sill {:.1f} target {:.1f} glass {:.1f} cut {:.1f} kept {:.1f}{}{}",
               job.tag, job.focus, job.sill, planeTarget, out.plane, cutting ? cutAt : 0.f, subject,
               job.framed ? " (an event's camera)" : "", keepPlayer && ease ? " (eased, to keep it)" : "");
    }
    g_lastBandShare = out.vertices.empty() ? 0.f : static_cast<float>(moved) / static_cast<float>(out.vertices.size());
    float low[3] = {INFINITY, INFINITY, INFINITY}, high[3] = {-INFINITY, -INFINITY, -INFINITY};
    for (const MirrorVertex& vertex : out.vertices) {
      // (Runaways' triangles are gone; the sky, pushed at most 100 times, stays inside this.)
      if (!(std::fabs(vertex.position[0]) < kRunaway * 100.f && std::fabs(vertex.position[1]) < kRunaway * 100.f &&
            std::fabs(vertex.position[2]) < kRunaway * 100.f)) {
        continue;
      }
      for (int c = 0; c < 3; ++c) {
        low[c] = std::min(low[c], vertex.position[c]);
        high[c] = std::max(high[c], vertex.position[c]);
      }
    }
    std::copy(low, low + 3, out.boundsMin);
    std::copy(high, high + 3, out.boundsMax);
    // Solid groups, then the blended ones in the game's order.
    for (int pass = 0; pass < 2; ++pass) {
      for (Group& group : job.groups) {
        if (group.blended != (pass == 1) || group.indices.empty()) {
          continue;
        }
        group.part.firstIndex = static_cast<uint32_t>(out.indices.size());
        group.part.indexCount = static_cast<uint32_t>(group.indices.size());
        out.indices.insert(out.indices.end(), group.indices.begin(), group.indices.end());
        out.parts.push_back(group.part);
      }
    }
  }
  g_lastParts = static_cast<uint32_t>(out.parts.size());
  std::lock_guard lock(g_mutex);
  out.serial = ++g_serial;
  g_finishedFrames.push_back(std::move(out));
  while (g_finishedFrames.size() > kKeptFrames) {
    g_spareFrames.push_back(std::move(g_finishedFrames.front()));
    g_finishedFrames.pop_front();
  }
  // The job builds its next frame in a spent one's buffers.
  out = Built{};
  if (!g_spareFrames.empty()) {
    out = std::move(g_spareFrames.back());
    g_spareFrames.pop_back();
  }
  // A texture let go of and back (the same id) before the window took the removal: the removal no
  // longer stands. Taken together, the window would otherwise apply it after the return and keep the
  // texture aside while it's in use.
  for (const auto& texture : job.textures) {
    std::erase(g_pendingRemoved, texture.id);
  }
  for (auto& texture : job.textures) {
    g_pendingTextures.push_back(std::move(texture));
  }
  g_pendingRemoved.insert(g_pendingRemoved.end(), job.removed.begin(), job.removed.end());
  job.textures.clear();
  job.removed.clear();
}

void worker_main() {
#if defined(__APPLE__)
  // 0106: as urgent as the decoding workers (user-interactive): at the default, it lost the cores to
  // them and the game's threads, and took 30 ms of wall time for 3 ms of work at sea, dropping a
  // mirror frame in three.
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
  Job work;
  while (true) {
    {
      std::unique_lock lock(g_jobMutex);
      g_jobReady.wait(lock, [] { return g_jobPending; });
      std::swap(work, g_job);
      g_jobPending = false;
      g_workerBusy = true;
    }
    g_jobTaken.notify_one();
    const auto started = std::chrono::steady_clock::now();
    const uint64_t finishCpu = thread_cpu_ns();
    finish(work);
    g_finishCpuNanos += thread_cpu_ns() - finishCpu;
    const auto finishNs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count());
    g_finishNanos += finishNs;
    g_statFinishNs += finishNs;
    ++g_statFinished;
    g_workerBusy = false;
    ++g_finished;
  }
}

// FIFO thread: the recorded frame to the worker. If it's still busy with the last one, this
// frame's geometry is dropped (the window keeps the last); its new textures still go.
void publish(bool scene, float focus) {
  ++g_frame;
  // Test runs: AURORA_MIRROR_TEST_SWEEP=<frames> lets every texture go that often, as if unused for
  // fifteen seconds (after a menu, or back in an area), to measure what their return costs.
  static const uint64_t s_testSweep = [] {
    const char* value = std::getenv("AURORA_MIRROR_TEST_SWEEP");
    return value != nullptr ? std::strtoull(value, nullptr, 10) : uint64_t{0};
  }();
  if (s_testSweep > 0 && g_frame % s_testSweep == 0) {
    sweep_textures(true);
  } else if (g_frame % 300 == 0) {
    sweep_textures();
  }
  static std::once_flag started;
  std::call_once(started, [] { std::thread(worker_main).detach(); });
  // 0108 test switch: AURORA_MIRROR_NO_DROP=1 waits for the finishing worker instead of dropping the
  // frame's geometry, so every game frame has its mirror frame (comparisons of a held frame).
  static const bool s_noDrop = [] {
    const char* value = std::getenv("AURORA_MIRROR_NO_DROP");
    return value != nullptr && std::strcmp(value, "1") == 0;
  }();
  {
    std::unique_lock lock(g_jobMutex);
    if (s_noDrop) {
      g_jobTaken.wait(lock, [] { return !g_jobPending; });
    }
    if (g_jobPending) {
      ++g_dropped;
      ++g_statDropped;
    } else {
      std::swap(g_job.built, g_building);
      std::swap(g_job.groups, g_groups);
      std::swap(g_job.backgroundRanges, g_backgroundRanges);
      std::swap(g_job.decalRanges, g_decalRanges);
      std::swap(g_job.alphaOps, g_alphaOps);
      std::swap(g_job.alphaLayers, g_alphaLayers);
      g_job.alphaStart = g_alphaStart;
      g_job.farthest = g_farthest;
      g_job.nearestBackground = g_nearestBackground;
      g_job.scene = scene;
      g_job.tag = g_frameTag;
      g_job.focus = focus;
      g_job.sill = g_sill;
      g_job.subject = g_subject;
      g_job.framed = g_framed;
      g_job.cut = g_cameraCut;
      g_job.tanHalfX = g_haveProjection ? 1.f / g_p0 / g_viewScale : 1.f;
      g_job.tanHalfY = g_haveProjection ? 1.f / g_p2 / g_viewScale : 1.f;
      g_jobPending = true;
    }
    // (As in finish: a removal of an earlier frame still waiting here is cancelled by a return.)
    for (const auto& texture : g_newTextures) {
      std::erase(g_job.removed, texture.id);
    }
    for (auto& texture : g_newTextures) {
      g_job.textures.push_back(std::move(texture));
    }
    g_job.removed.insert(g_job.removed.end(), g_removedTextures.begin(), g_removedTextures.end());
  }
  g_jobReady.notify_one();
  g_newTextures.clear();
  g_removedTextures.clear();

  ++g_stats.frames;
  if (g_stats.frames == 600) {
    const double f = static_cast<double>(g_stats.frames);
    const uint64_t finished = std::max<uint64_t>(g_finished.exchange(0), 1);
    Log.info("mirror: per frame {:.0f} draws, {:.0f} recorded ({:.0f} merged, {:.0f} background), skipped {:.0f} lines, {:.0f} screen, "
             "{:.0f} no colour, {:.0f} small viewport, {:.0f} other projection, {:.0f} blend, {:.0f} framebuffer "
             "copies, {:.0f} alpha-never; {:.0f} see-through (refracting); {:.0f} vertices, {:.0f} triangles, {} parts; "
             "recording {:.2f} ms (FIFO thread), decoding {:.2f} ms (workers' CPU), finishing {:.2f} ms (worker), {} "
             "frames dropped; {} textures known",
             g_stats.draws / f, g_stats.recorded / f, g_stats.merged / f, g_stats.background / f, g_stats.lines / f, g_stats.screen / f,
             g_stats.noColour / f, g_stats.viewport / f, g_stats.projection / f, g_stats.blend / f, g_stats.copyOnly / f,
             g_stats.alphaNever / f, g_stats.behind / f, g_stats.vertices / f, g_stats.triangles / f,
             g_lastParts.load(), g_stats.seconds / f * 1000.0, static_cast<double>(g_decodeNanos.exchange(0)) / f / 1e6,
             static_cast<double>(g_finishNanos.exchange(0)) / finished / 1e6, g_dropped.exchange(0), g_textures.size());
    {
      const double scenes = static_cast<double>(std::max<uint64_t>(g_scenes.exchange(0), 1));
      Log.info("mirror: CPU per frame: FIFO thread over the scene {:.2f} ms, finishing {:.2f} ms; {:.2f} bakes "
               "({:.0f} texels), {:.1f} alpha operations, {:.0f} pieces cut where lights meet the ground",
               static_cast<double>(g_sceneCpuNanos.exchange(0)) / scenes / 1e6,
               static_cast<double>(g_finishCpuNanos.exchange(0)) / finished / 1e6, g_stats.bakes / f,
               g_stats.bakeTexels / f, g_stats.alphaOps / f,
               static_cast<double>(g_alphaLayerTriangles.exchange(0)) / static_cast<double>(finished));
    }
    log_tally(f);
    Log.info("mirror: glass at {:.0f} (easing to {:.0f}; focus {:.0f}, sill {:.2f} of the glass's distance, the "
             "player, or what an event's camera frames, from {:.2f} of it), {:.1f}% of vertices moved nearer it "
             "(into its band, or easing into {:.2f} window widths in front{}; clipped ones counted too); near cut "
             "at {:.2f} of the glass's distance",
             g_lastPlane.load(), g_lastPlaneTarget.load(), g_lastFocus.load(), g_lastSill.load(),
             g_lastSubject.load(), g_lastBandShare.load() * 100.f, g_lastAllowance.load(),
             g_lastKeptPlayer.load() ? ", to keep it whole" : "", g_lastCut.load());
    g_stats = {};
  }
}
} // namespace

bool recording() noexcept { return g_recording; }

void mark(u32 mark, f32 value) noexcept {
  if (mark == kMarkViewScale) {
    g_viewScale = std::isfinite(value) ? std::clamp(value, 1.f, 4.f) : 1.f;
    return;
  }
  if (mark == kMarkSill) {
    g_sill = std::isfinite(value) && value > 0.f ? value : 0.f;
    return;
  }
  if (mark == kMarkSubject) {
    g_subject = std::isfinite(value) && value > 0.f ? value : 0.f;
    return;
  }
  if (mark == kMarkFramed) {
    g_framed = value > 0.5f;
    return;
  }
  if (mark == kMarkCut) {
    g_cameraCut = value > 0.5f;
    return;
  }
  if (mark == ::aurora::mirror::MarkGhostPass || mark == ::aurora::mirror::MarkWorld ||
      mark == ::aurora::mirror::MarkSky) {
    bool& flag = mark == ::aurora::mirror::MarkGhostPass ? g_ghostPass
                 : mark == ::aurora::mirror::MarkWorld   ? g_world
                                                         : g_sky;
    const bool on = value > 0.5f;
    if (flag != on) {
      // No merging across it: the draws either side are recorded differently.
      g_mergeTarget = nullptr;
      g_lastLeftOut = false;
    }
    flag = on;
    return;
  }
  if (mark == AURORA_MIRROR_MARK_BEGIN) {
    g_viewScale = 1.f;
    g_ghostPass = false;
    g_world = false;
    g_sky = false;
    g_sill = 0.f;
    g_subject = 0.f;
    g_framed = false;
    g_cameraCut = false;
    g_frameDraws = 0;
    discard_jobs(); // a frame that never ended
    g_frameTag = static_cast<uint32_t>(std::max(value, 0.f)) & 0xFFFFFFu;
    g_recording = g_enabled.load(std::memory_order_relaxed);
    // The port's volume: the game never waits for the mirror. A frame begun while the worker still
    // has an earlier one isn't recorded (the volume keeps showing the last; textures carry on as
    // they were); nor is the one after a scene whose recording cost the FIFO thread more than two
    // thirds of a 30 Hz frame of CPU, never two in a row (at worst every other frame, evenly).
    // (12ac2eb skipped after any scene that took over 20 ms of wall time from its BEGIN to its END,
    // which is mostly the game issuing its draws: on the headset a frame in four or five went
    // unrecorded at random, 66 ms holes in the set's motion, while the FIFO thread's CPU over the
    // scene was 7 to 12 ms.)
    if (g_recording) {
      bool busy;
      {
        std::lock_guard lock(g_jobMutex);
        busy = g_jobPending || g_workerBusy.load();
      }
      static const bool s_keepAll = [] {
        const char* v = std::getenv("AURORA_MIRROR_NO_DROP");
        return v != nullptr && std::strcmp(v, "1") == 0;
      }();
      const bool dear = g_lastSceneCpuNs > 22'000'000ull && !g_skippedLast;
      if (!s_keepAll && (busy || dear)) {
        g_recording = false;
        g_lastSceneCpuNs = 0;
        ++g_statSkipped;
      }
      g_skippedLast = !g_recording;
    }
    g_recordWallStart = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                  std::chrono::steady_clock::now().time_since_epoch())
                                                  .count());
    g_sceneCpuStart = thread_cpu_ns();
    if (g_reset.exchange(false)) {
      forget_textures();
    }
    if (g_recording) {
      begin_frame();
    }
  } else if (mark == AURORA_MIRROR_MARK_END || mark == AURORA_MIRROR_MARK_NO_SCENE) {
    if (!g_recording) {
      return;
    }
    g_recording = false;
    if (mark == AURORA_MIRROR_MARK_END) {
      merge_jobs();
    } else {
      discard_jobs();
    }
    publish(mark == AURORA_MIRROR_MARK_END, value);
    g_lastSceneCpuNs = thread_cpu_ns() - g_sceneCpuStart;
    g_sceneCpuNanos += g_lastSceneCpuNs;
    g_statSceneCpuNs += g_lastSceneCpuNs;
    ++g_scenes;
    g_lastRecordNs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                               std::chrono::steady_clock::now().time_since_epoch())
                                               .count()) -
                     g_recordWallStart;
    g_statRecordNs += g_lastRecordNs;
    ++g_statRecorded;
  }
}

void offscreen(bool begin) noexcept { g_offscreen = std::max(0, g_offscreen + (begin ? 1 : -1)); }

void debug_group(const std::string* label) noexcept {
  if (!kGroupLog) {
    return;
  }
  if (label != nullptr) {
    g_debugGroups.push_back(*label);
  } else if (!g_debugGroups.empty()) {
    g_debugGroups.pop_back();
  }
}

// 0107: the texel alpha (0-255) above which a draw whose alpha is k * T.a + c passes its alpha
// test, for a test of "greater" (or "greater or equal") than a reference: 255 when no texel does
// (a shadow faded out); -1 when the test isn't such a threshold, or every texel passes.
int shadow_threshold(const AlphaCompare& compare, const Sym& alpha) {
  if (alpha.sel < 0 || alpha.k <= 1e-4f || (compare.comp0 != GX_GREATER && compare.comp0 != GX_GEQUAL)) {
    return -1;
  }
  const float texel = (static_cast<float>(compare.ref0) / 255.f - alpha.c) / alpha.k * 255.f;
  // Passed: alpha > texel ("greater"), alpha >= texel ("greater or equal"), as alpha > the result.
  const float above = compare.comp0 == GX_GREATER ? std::floor(texel) : std::ceil(texel) - 1.f;
  if (!(above >= 0.f)) {
    return -1; // every texel passes: no mask
  }
  return static_cast<int>(std::min(above, 255.f));
}

// 0105: how much nearer the camera an eye drawn over the hair is put (game units; Wind Waker's are
// about a centimetre): past the hair in front of the face, short of what stands in front of the
// character.
constexpr float kOverHairPull = 10.f;

// A texture coordinate made from the vertices' own (one of GX_TG_TEX0..7) by a 2x4 matrix, as the
// affine map (s, t) -> (u, v): u = a0 s + a1 t + a2, v = a3 s + a4 t + a5. False for any other
// kind (projected, normalised, post-transformed, a matrix per vertex).
bool coord_affine(const DrawJob& job, int coord, std::array<float, 6>& a, int* source) {
  const ShaderConfig& config = job.config;
  if (coord < 0 || coord >= static_cast<int>(MaxTexCoord)) {
    return false;
  }
  const TcgConfig& tcg = config.tcgs[coord];
  if (tcg.type != GX_TG_MTX2x4 || tcg.src < GX_TG_TEX0 || tcg.src > GX_TG_TEX7 || tcg.normalize ||
      tcg.postMtx != GX_PTIDENTITY || config.attrs[GX_VA_TEX0MTXIDX + coord].attrType != GX_NONE) {
    return false;
  }
  *source = tcg.src;
  if (tcg.mtx == GX_IDENTITY) {
    a = {1.f, 0.f, 0.f, 0.f, 1.f, 0.f};
    return true;
  }
  const auto& m = postex_matrix(job.state, tcg.mtx / 3);
  // (s, t, 1, 1): the matrix's last two columns both add.
  a = {m.m0[0], m.m0[1], m.m0[2] + m.m0[3], m.m1[0], m.m1[1], m.m1[2] + m.m1[3]};
  return true;
}

// 0105: a draw that multiplies two textures (Wind Waker's eye: its white and pupil, scrolled by a
// texture matrix as the eye looks around, times its outline, whose alpha is the eye's shape) keeps
// one; the other stood in as its average, so the eye lost its shape or its pupil. Here both are
// baked into one texture at the size of the one read as it is (the base), the other read through
// the map between their coordinates. The TEV is taken as linear in each sample (five runs with the
// other texture at zero and at each channel, the kept one left unknown), which a product of the two
// is. Under an id per pair and per draw of the frame, baked again in place only when the map or the
// TEV changes. Only for draws with no raster colour (their TEV is the textures alone).
//
// 0108: not only the eyes drawn over the hair: every draw built that way. The other characters'
// eyes are such a draw, an ordinary alpha blend after the face (Aryll's: the iris, scrolled as she
// looks around, times the eye's white, whose alpha is the eye's shape): with the shape averaged, a
// faint square of iris colour over the face, and no eyes. Outside the eyes over the hair, textures
// up to kBakeMaxTexels only (the CPU keeps their pixels), a bake already made this frame from the
// same is shared (a texture tiled over many draws), and at most kBakeTexelsPerFrame texels are
// baked a frame (a texture scrolled every frame is baked again every frame): past it, a bake made
// before stays as it is, for three frames at most (then it's baked whatever the budget), or the
// draw waits a frame for its first.
bool reads_raster(const ShaderConfig& config) {
  for (u32 i = 0; i < config.tevStageCount; ++i) {
    const TevStage& stage = config.tevStages[i];
    const auto& c = stage.colorPass;
    const auto& a = stage.alphaPass;
    for (const GXTevColorArg arg : {c.a, c.b, c.c, c.d}) {
      if (arg == GX_CC_RASC || arg == GX_CC_RASA) {
        return true;
      }
    }
    for (const GXTevAlphaArg arg : {a.a, a.b, a.c, a.d}) {
      if (arg == GX_CA_RASA) {
        return true;
      }
    }
  }
  return false;
}
// Which colour registers (PREV, REG0..2 as bits 0..3) a draw's TEV stages read.
uint32_t regs_read(const ShaderConfig& config) {
  uint32_t used = 0;
  for (u32 i = 0; i < std::min<u32>(config.tevStageCount, MaxTevStages); ++i) {
    const TevStage& stage = config.tevStages[i];
    const auto& c = stage.colorPass;
    const auto& a = stage.alphaPass;
    for (const GXTevColorArg arg : {c.a, c.b, c.c, c.d}) {
      switch (arg) {
      case GX_CC_CPREV: case GX_CC_APREV: used |= 1u; break;
      case GX_CC_C0: case GX_CC_A0: used |= 2u; break;
      case GX_CC_C1: case GX_CC_A1: used |= 4u; break;
      case GX_CC_C2: case GX_CC_A2: used |= 8u; break;
      default: break;
      }
    }
    for (const GXTevAlphaArg arg : {a.a, a.b, a.c, a.d}) {
      switch (arg) {
      case GX_CA_APREV: used |= 1u; break;
      case GX_CA_A0: used |= 2u; break;
      case GX_CA_A1: used |= 4u; break;
      case GX_CA_A2: used |= 8u; break;
      default: break;
      }
    }
  }
  return used;
}
constexpr uint32_t kBakeMaxTexels = 128 * 128;
constexpr uint64_t kBakeTexelsPerFrame = 128 * 128 * 12;

bool bake_pair(DrawJob& job, MirrorPart& part, bool overHair) {
  DrawPlan& plan = job.plan;
  const ShaderConfig& config = job.config;
  if (plan.primary == nullptr || plan.primary->captured || plan.behindMap >= 0 || plan.anyLookup ||
      (overHair ? plan.rasterUsed[0] || plan.rasterUsed[1] : reads_raster(config))) {
    return false;
  }
  int other = -1, otherCoord = -1;
  for (u32 i = 0; i < config.tevStageCount; ++i) {
    const TevStage& stage = config.tevStages[i];
    if (!samples_texture(stage) || stage.texMapId >= MaxTextures || static_cast<int>(stage.texMapId) == plan.primaryMap) {
      continue;
    }
    if ((other >= 0 && static_cast<int>(stage.texMapId) != other) ||
        (otherCoord >= 0 && static_cast<int>(stage.texCoordId) != otherCoord)) {
      return false; // a third texture, or one read two ways
    }
    other = stage.texMapId;
    otherCoord = stage.texCoordId;
  }
  if (other < 0) {
    return false;
  }
  // The primary's coordinate must be the one every one of its stages reads.
  for (u32 i = 0; i < config.tevStageCount; ++i) {
    const TevStage& stage = config.tevStages[i];
    if (samples_texture(stage) && static_cast<int>(stage.texMapId) == plan.primaryMap &&
        static_cast<int>(stage.texCoordId) != plan.primaryCoord) {
      return false;
    }
  }
  const GXTexObj_& primaryObj = g_gxState.loadedTextures[plan.primaryMap];
  const GXTexObj_& otherObj = g_gxState.loadedTextures[other];
  TexEntry* otherEntry = texture_entry(otherObj);
  if (otherEntry == nullptr || !otherEntry->ok || otherEntry->captured) {
    return false;
  }
  std::array<float, 6> primaryMap{}, otherMap{};
  int primarySource = -1, otherSource = -1;
  if (!coord_affine(job, plan.primaryCoord, primaryMap, &primarySource) ||
      !coord_affine(job, otherCoord, otherMap, &otherSource) || primarySource != otherSource) {
    return false;
  }
  if (!overHair && (primaryObj.width() * primaryObj.height() > kBakeMaxTexels ||
                    otherObj.width() * otherObj.height() > kBakeMaxTexels)) {
    return false;
  }
  const auto primaryPixels = lookup_pixels(*plan.primary, primaryObj);
  const auto otherPixels = lookup_pixels(*otherEntry, otherObj);
  if (!primaryPixels || !otherPixels) {
    return false;
  }
  // The base: the one read without a matrix (the shape), else the other.
  const bool primaryBase = primaryMap == std::array<float, 6>{1.f, 0.f, 0.f, 0.f, 1.f, 0.f} &&
                           otherMap != std::array<float, 6>{1.f, 0.f, 0.f, 0.f, 1.f, 0.f};
  const std::array<float, 6>& baseMap = primaryBase ? primaryMap : otherMap;
  const std::array<float, 6>& readMap = primaryBase ? otherMap : primaryMap;
  const float det = baseMap[0] * baseMap[4] - baseMap[1] * baseMap[3];
  if (std::fabs(det) < 1e-8f) {
    return false;
  }
  // The read texture's coordinate from the base's: M = read o base^-1.
  const float i0 = baseMap[4] / det, i1 = -baseMap[1] / det, i3 = -baseMap[3] / det, i4 = baseMap[0] / det;
  const float i2 = -(i0 * baseMap[2] + i1 * baseMap[5]), i5 = -(i3 * baseMap[2] + i4 * baseMap[5]);
  const std::array<float, 6> m{readMap[0] * i0 + readMap[1] * i3,  readMap[0] * i1 + readMap[1] * i4,
                               readMap[0] * i2 + readMap[1] * i5 + readMap[2],
                               readMap[3] * i0 + readMap[4] * i3,  readMap[3] * i1 + readMap[4] * i4,
                               readMap[3] * i2 + readMap[4] * i5 + readMap[5]};
  // The TEV with the other texture known: at zero, and at each channel alone.
  plan.lookups[other].pixels = otherPixels;
  plan.lookups[other].coord = otherCoord;
  const F4 raster[2];
  F4 lookups[MaxTextures];
  std::array<std::array<Sym, 4>, 5> probes;
  for (int j = 0; j < 5; ++j) {
    lookups[other] = {};
    if (j > 0) {
      lookups[other][j - 1] = 1.f;
    }
    probes[j] = run_tev(job, raster, lookups);
  }
  plan.lookups[other] = {};
  // What it's baked from.
  struct Key {
    uint64_t primary, other;
    float m[6];
    float k[5][4], c[5][4];
    int8_t sel[5][4];
    bool primaryBase;
  } key;
  std::memset(&key, 0, sizeof key); // (the hash reads its padding too)
  key.primary = plan.primary->id;
  key.other = otherEntry->id;
  std::copy(m.begin(), m.end(), key.m);
  for (int j = 0; j < 5; ++j) {
    for (int c = 0; c < 4; ++c) {
      key.k[j][c] = probes[j][c].sel >= 0 ? probes[j][c].k : 0.f;
      key.c[j][c] = probes[j][c].c;
      key.sel[j][c] = probes[j][c].sel;
    }
  }
  key.primaryBase = primaryBase;
  const uint64_t keyHash = xxh3_hash_s(&key, sizeof key);
  const GXTexObj_& baseObj = primaryBase ? primaryObj : otherObj;
  const auto use = [&](uint64_t id) {
    part.texture = id;
    part.wrapS = static_cast<uint8_t>(std::min<int>(baseObj.wrap_s(), 2));
    part.wrapT = static_cast<uint8_t>(std::min<int>(baseObj.wrap_t(), 2));
    plan.primaryCoord = primaryBase ? plan.primaryCoord : otherCoord;
    job.baked = true;
    return true;
  };
  // 0108: made already this frame.
  if (const auto made = g_bakeKeys.find(keyHash); made != g_bakeKeys.end()) {
    return use(made->second);
  }
  const uint64_t pair[2] = {key.primary, key.other};
  const uint64_t pairHash = xxh3_hash_s(pair, sizeof pair);
  const uint64_t slot[2] = {pairHash, g_bakeOccurrences[pairHash]++};
  Baked& baked = g_baked[xxh3_hash_s(slot, sizeof slot)];
  const LookupPixels& base = primaryBase ? *primaryPixels : *otherPixels;
  const LookupPixels& read = primaryBase ? *otherPixels : *primaryPixels;
  const uint8_t readWrapS = static_cast<uint8_t>(primaryBase ? otherObj.wrap_s() : primaryObj.wrap_s());
  const uint8_t readWrapT = static_cast<uint8_t>(primaryBase ? otherObj.wrap_t() : primaryObj.wrap_t());
  const uint64_t texels = static_cast<uint64_t>(base.width) * base.height;
  if (baked.key != keyHash && !overHair && g_bakeTexels + texels > kBakeTexelsPerFrame &&
      (baked.id == 0 || g_frame - baked.bakedFrame < 3)) {
    // 0108: over the frame's budget: the last bake a frame longer, or none yet.
    if (baked.id == 0) {
      return false;
    }
    baked.lastFrame = g_frame;
    return use(baked.id);
  }
  if (baked.id == 0) {
    baked.id = g_nextTextureId++;
  }
  baked.lastFrame = g_frame;
  g_bakeKeys.emplace(keyHash, baked.id);
  if (baked.key != keyHash) {
    baked.key = keyHash;
    baked.bakedFrame = g_frame;
    g_bakeTexels += texels;
    ++g_stats.bakes;
    g_stats.bakeTexels += texels;
    std::vector<uint8_t> rgba(static_cast<size_t>(base.width) * base.height * 4);
    for (uint32_t y = 0; y < base.height; ++y) {
      for (uint32_t x = 0; x < base.width; ++x) {
        const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(base.width);
        const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(base.height);
        const auto sampled = sample_lookup(read, m[0] * u + m[1] * v + m[2], m[3] * u + m[4] * v + m[5], readWrapS, readWrapT);
        const uint8_t* texel = &base.rgba[(static_cast<size_t>(y) * base.width + x) * 4];
        std::array<float, 4> own{texel[0] / 255.f, texel[1] / 255.f, texel[2] / 255.f, texel[3] / 255.f};
        const std::array<float, 4>& tp = primaryBase ? own : sampled;
        const std::array<float, 4>& to = primaryBase ? sampled : own;
        uint8_t* out = &rgba[(static_cast<size_t>(y) * base.width + x) * 4];
        for (int c = 0; c < 4; ++c) {
          float value = probes[0][c].c, k = probes[0][c].sel >= 0 ? probes[0][c].k : 0.f;
          int sel = probes[0][c].sel;
          for (int j = 1; j < 5; ++j) {
            value += (probes[j][c].c - probes[0][c].c) * to[j - 1];
            const float kj = probes[j][c].sel >= 0 ? probes[j][c].k : 0.f;
            k += (kj - (probes[0][c].sel >= 0 ? probes[0][c].k : 0.f)) * to[j - 1];
            if (sel < 0) {
              sel = probes[j][c].sel;
            }
          }
          if (sel >= 0) {
            value += k * tp[sel];
          }
          out[c] = static_cast<uint8_t>(std::lround(std::clamp(value, 0.f, 1.f) * 255.f));
        }
      }
    }
    const bool cutout = binary_alpha(rgba);
    g_newTextures.push_back({baked.id, std::move(rgba), base.width, base.height, false, cutout});
  }
  return use(baked.id);
}

// 0109: the TEV's stages in numbers, every texture known (`texels`: per stage, its sample, before the
// swap table), no raster colour read: bake_reads' per-texel run (run_tev's symbols cost too much
// for thousands of texels a frame). Comparison ops are taken as their d, as run_tev takes them.
std::array<float, 4> tev_numeric(const DrawJob& job, const std::array<std::array<float, 4>, MaxTevStages>& texels,
                                 const float raster[8]) {
  const auto& config = job.config;
  const auto& plan = job.plan;
  const u32 stageCount = std::min<u32>(config.tevStageCount, MaxTevStages);
  float regs[MaxTevRegs][4];
  for (u32 r = 0; r < MaxTevRegs; ++r) {
    for (int c = 0; c < 4; ++c) {
      regs[r][c] = job.state.colorRegs[r][c];
    }
  }
  for (u32 s = 0; s < stageCount; ++s) {
    const TevStage& stage = config.tevStages[s];
    const auto& swap = config.tevSwapTable[stage.tevSwapTex];
    float tex[4] = {1.f, 1.f, 1.f, 1.f};
    if (stage.texMapId != GX_TEXMAP_NULL) {
      const int from[4] = {swap_channel(swap.red), swap_channel(swap.green), swap_channel(swap.blue), swap_channel(swap.alpha)};
      for (int c = 0; c < 4; ++c) {
        tex[c] = texels[s][from[c] & 3];
      }
    }
    // The vertex colour (two channels, rgba each), through the stage's swap table.
    const auto& rasSwap = config.tevSwapTable[stage.tevSwapRas];
    const int rasFrom[4] = {swap_channel(rasSwap.red) & 3, swap_channel(rasSwap.green) & 3, swap_channel(rasSwap.blue) & 3,
                            swap_channel(rasSwap.alpha) & 3};
    const bool hasRaster = stage.channelId != GX_COLOR_NULL && stage.channelId != GX_COLOR_ZERO && !is_bump_channel(stage.channelId);
    const int rasBase = hasRaster ? (color_channel(stage.channelId) & 1) * 4 : 0;
    const auto ras = [&](int c) { return hasRaster ? raster[rasBase + rasFrom[c]] : 0.f; };
    const auto colour_arg = [&](GXTevColorArg arg, int c) -> float {
      switch (arg) {
      case GX_CC_CPREV: return regs[0][c];
      case GX_CC_APREV: return regs[0][3];
      case GX_CC_C0: return regs[1][c];
      case GX_CC_A0: return regs[1][3];
      case GX_CC_C1: return regs[2][c];
      case GX_CC_A1: return regs[2][3];
      case GX_CC_C2: return regs[3][c];
      case GX_CC_A2: return regs[3][3];
      case GX_CC_TEXC: return tex[c];
      case GX_CC_TEXA: return tex[3];
      case GX_CC_RASC: return ras(c);
      case GX_CC_RASA: return ras(3);
      case GX_CC_ONE: return 1.f;
      case GX_CC_HALF: return 0.5f;
      case GX_CC_KONST: return plan.konst[s][c];
      default: return 0.f;
      }
    };
    const auto alpha_arg = [&](GXTevAlphaArg arg) -> float {
      switch (arg) {
      case GX_CA_APREV: return regs[0][3];
      case GX_CA_A0: return regs[1][3];
      case GX_CA_A1: return regs[2][3];
      case GX_CA_A2: return regs[3][3];
      case GX_CA_TEXA: return tex[3];
      case GX_CA_RASA: return ras(3);
      case GX_CA_KONST: return plan.konst[s][3];
      default: return 0.f;
      }
    };
    const auto run = [](const TevOp& op, float a, float b, float c, float d) {
      float v = d;
      if (op.op == GX_TEV_ADD || op.op == GX_TEV_SUB) {
        const float mixed = a + (b - a) * c;
        v = (op.op == GX_TEV_SUB ? d - mixed : d + mixed) + tev_bias(op.bias);
        v *= tev_scale(op.scale);
      }
      return op.clamp ? std::clamp(v, 0.f, 1.f) : std::clamp(v, -4.f, 4.f);
    };
    // Comparisons (shader.cpp's tev_op): R8, GR16 and BGR24 compare the colour inputs A and B (in an
    // alpha stage too); RGB8 compares each channel's own (A8 in an alpha stage).
    float ca[3], cb[3];
    for (int c = 0; c < 3; ++c) {
      ca[c] = colour_arg(stage.colorPass.a, c);
      cb[c] = colour_arg(stage.colorPass.b, c);
    }
    const auto packed = [](const float* v, int n) {
      float sum = 0.f, place = 1.f;
      for (int i = 0; i < n; ++i, place *= 256.f) {
        sum += std::round(v[i] * 255.f) * place;
      }
      return sum;
    };
    const auto compared = [&](GXTevOp op, float a, float b, bool& known) {
      known = true;
      switch (op) {
      case GX_TEV_COMP_R8_GT: return packed(ca, 1) > packed(cb, 1);
      case GX_TEV_COMP_R8_EQ: return packed(ca, 1) == packed(cb, 1);
      case GX_TEV_COMP_GR16_GT: return packed(ca, 2) > packed(cb, 2);
      case GX_TEV_COMP_GR16_EQ: return packed(ca, 2) == packed(cb, 2);
      case GX_TEV_COMP_BGR24_GT: return packed(ca, 3) > packed(cb, 3);
      case GX_TEV_COMP_BGR24_EQ: return packed(ca, 3) == packed(cb, 3);
      case GX_TEV_COMP_RGB8_GT: return std::round(a * 255.f) > std::round(b * 255.f);
      case GX_TEV_COMP_RGB8_EQ: return std::round(a * 255.f) == std::round(b * 255.f);
      default: known = false; return false;
      }
    };
    const auto evaluate = [&](const TevOp& op, float a, float b, float c, float d) {
      bool known = false;
      const bool pass = compared(op.op, a, b, known);
      if (!known) {
        return run(op, a, b, c, d);
      }
      const float v = d + (pass ? c : 0.f);
      return op.clamp ? std::clamp(v, 0.f, 1.f) : std::clamp(v, -4.f, 4.f);
    };
    float colour[3];
    for (int c = 0; c < 3; ++c) {
      colour[c] = evaluate(stage.colorOp, ca[c], cb[c], colour_arg(stage.colorPass.c, c), colour_arg(stage.colorPass.d, c));
    }
    const float alpha = evaluate(stage.alphaOp, alpha_arg(stage.alphaPass.a), alpha_arg(stage.alphaPass.b),
                                 alpha_arg(stage.alphaPass.c), alpha_arg(stage.alphaPass.d));
    const u32 colourOut = std::min<u32>(stage.colorOp.outReg, MaxTevRegs - 1);
    for (int c = 0; c < 3; ++c) {
      regs[colourOut][c] = colour[c];
    }
    regs[std::min<u32>(stage.alphaOp.outReg, MaxTevRegs - 1)][3] = alpha;
  }
  std::array<float, 4> result{regs[0][0], regs[0][1], regs[0][2], regs[0][3]};
  if (stageCount > 0) {
    const TevStage& last = config.tevStages[stageCount - 1];
    const u32 colourOut = std::min<u32>(last.colorOp.outReg, MaxTevRegs - 1);
    for (int c = 0; c < 3; ++c) {
      result[c] = regs[colourOut][c];
    }
    result[3] = regs[std::min<u32>(last.alphaOp.outReg, MaxTevRegs - 1)][3];
  }
  return result;
}

// 0109: a draw that reads one texture through two coordinates, both made from the vertices' own by
// matrices: the horizon's cloud band (its picture and its alpha each read twice, scrolled apart,
// the alphas multiplied, the pictures screened), and the foam round the islands' shores (a foam
// texture read two to four times, scrolling apart, the reads multiplied, under a mask).
// bake_pair takes one texture per map; the second read stood in as its average, and the band came
// out faint, the foam a broad white band on the water. Here every read is baked into one texture in
// the coordinates of one of them (the base: read without a matrix, or the kept one's), as fine as
// the finest read across it, the TEV run in numbers at each texel. A TEV that also reads the
// vertex colour (a fade towards the open sea) is kept when each channel is the bake plus the vertex
// colour times a constant, or the bake times one of the vertex colour's channels: that part per
// vertex. Baked again as the reads scroll apart, each read's offset taken to a quarter texel first;
// at most 64x64 (it's baked each frame something scrolls), with bake_pair's sharing and budget.
constexpr uint32_t kBakeReadsMaxTexels = 64 * 64;
bool bake_reads(DrawJob& job, MirrorPart& part) {
  DrawPlan& plan = job.plan;
  const ShaderConfig& config = job.config;
  static const long s_traceTag = [] {
    const char* value = std::getenv("AURORA_MIRROR_TRACE_TAG");
    return value != nullptr ? std::atol(value) : -1L;
  }();
  const bool trace = s_traceTag >= 0 && static_cast<long>(g_frameTag) == s_traceTag;
  const auto refuse = [&](const char* why) {
    if (trace) {
      Log.info("mirror bake_reads: refused ({})", why);
    }
    return false;
  };
  if (plan.primary == nullptr || plan.primary->captured || plan.behindMap >= 0 || plan.anyLookup ||
      config.numIndStages != 0) {
    return false;
  }
  const u32 stageCount = std::min<u32>(config.tevStageCount, MaxTevStages);
  struct Read {
    int map = -1, coord = -1;
  };
  std::array<Read, MaxTevStages> reads{};
  std::array<std::array<float, 6>, MaxTexCoord> maps{};
  std::array<bool, MaxTexCoord> mapped{};
  int source = -1;
  bool twice = false; // some texture read through two coordinates (bound to one map or to two)
  std::array<const void*, MaxTexCoord> dataOf{};
  for (u32 i = 0; i < stageCount; ++i) {
    const TevStage& stage = config.tevStages[i];
    if (stage.texMapId == GX_TEXMAP_NULL || !samples_texture(stage)) {
      continue;
    }
    if (stage.texMapId >= MaxTextures || stage.texCoordId >= MaxTexCoord) {
      return false;
    }
    const int coord = stage.texCoordId;
    if (!mapped[coord]) {
      int from = -1;
      if (!coord_affine(job, coord, maps[coord], &from) || (source >= 0 && from != source)) {
        return refuse("a coordinate not an affine map of one source");
      }
      source = from;
      mapped[coord] = true;
    }
    const int map = stage.texMapId;
    const void* data = g_gxState.loadedTextures[map].data;
    for (int c = 0; c < static_cast<int>(MaxTexCoord); ++c) {
      twice = twice || (c != coord && dataOf[c] == data && data != nullptr);
    }
    if (dataOf[coord] == nullptr) {
      dataOf[coord] = data;
    }
    reads[i] = {map, coord};
  }
  if (!twice) {
    return false;
  }
  // The base: the vertices' own coordinate the reads are all made from (each read's map is from it),
  // over its unit square where a read clamps. (A read's coordinate as the base, scaled, put the clamp
  // in the wrong place: the foam beside the rocks came out a smear.)
  if (source < GX_TG_TEX0 || source > GX_TG_TEX7) {
    return refuse("no source coordinate");
  }
  constexpr float i0 = 1.f, i1 = 0.f, i2 = 0.f, i3 = 0.f, i4 = 1.f, i5 = 0.f;
  // Each read's texture, and its coordinate from the base's (M = read o base^-1), its offsets taken
  // to a quarter texel; the bake as fine as the finest read across it (texels per base unit).
  struct Source {
    const LookupPixels* pixels = nullptr;
    uint8_t wrapS = 0, wrapT = 0;
    std::array<float, 6> m{};
  };
  std::array<Source, MaxTevStages> sources{};
  struct Key {
    uint64_t ids[MaxTevStages];
    float m[MaxTevStages][6];
    float konst[MaxTevStages][4];
    float regs[MaxTevRegs][4];
    uint64_t tev;
    uint32_t width, height;
    uint32_t period[2];
    uint8_t wraps[MaxTevStages][2];
  } key;
  std::memset(&key, 0, sizeof key);
  const GXTexObj_& primaryObj = g_gxState.loadedTextures[plan.primaryMap];
  float densityU = 1.f, densityV = 1.f;
  for (u32 i = 0; i < stageCount; ++i) {
    if (reads[i].map < 0) {
      continue;
    }
    const GXTexObj_& obj = g_gxState.loadedTextures[reads[i].map];
    TexEntry* entry = texture_entry(obj);
    if (entry == nullptr || !entry->ok || entry->captured || obj.width() * obj.height() > kLookupMaxTexels) {
      return refuse("a texture not readable on the CPU");
    }
    Source& src = sources[i];
    src.pixels = lookup_pixels(*entry, obj);
    if (src.pixels == nullptr) {
      return refuse("no pixels");
    }
    src.wrapS = static_cast<uint8_t>(obj.wrap_s());
    src.wrapT = static_cast<uint8_t>(obj.wrap_t());
    key.wraps[i][0] = src.wrapS;
    key.wraps[i][1] = src.wrapT;
    const auto& r = maps[reads[i].coord];
    src.m = {r[0] * i0 + r[1] * i3, r[0] * i1 + r[1] * i4, r[0] * i2 + r[1] * i5 + r[2],
             r[3] * i0 + r[4] * i3, r[3] * i1 + r[4] * i4, r[3] * i2 + r[4] * i5 + r[5]};
    const float w = static_cast<float>(obj.width()), h = static_cast<float>(obj.height());
    const float qs = 4.f * w, qt = 4.f * h;
    src.m[2] = std::round(src.m[2] * qs) / qs;
    src.m[5] = std::round(src.m[5] * qt) / qt;
    key.ids[i] = entry->id;
    std::copy(src.m.begin(), src.m.end(), key.m[i]);
    densityU = std::max({densityU, w * std::fabs(src.m[0]), h * std::fabs(src.m[3])});
    densityV = std::max({densityV, w * std::fabs(src.m[1]), h * std::fabs(src.m[4])});
  }
  // How the result repeats along the base's u and v: a read that wraps round (repeat or mirror) and
  // moves along an axis makes it repeat there, over the least period (up to 4) its scale makes whole;
  // reads that clamp, or don't move, don't. The bake spans one period, wraps as the result does, and
  // the vertices' coordinates are scaled onto it (a foam strip runs many periods along a shore: baked
  // over the first and clamped, the rest was the first's edge, smeared).
  int period[2] = {1, 1};
  bool repeats[2] = {false, false};
  for (u32 i = 0; i < stageCount; ++i) {
    if (reads[i].map < 0) {
      continue;
    }
    const Source& src = sources[i];
    for (int axis = 0; axis < 2; ++axis) {
      // d(s)/d(axis) and d(t)/d(axis), with s's and t's wraps.
      const float ds = src.m[axis], dt = src.m[3 + axis];
      for (const auto& [rate, wrap] : {std::pair<float, uint8_t>{ds, src.wrapS}, std::pair<float, uint8_t>{dt, src.wrapT}}) {
        if (std::fabs(rate) < 1e-4f || wrap == GX_CLAMP) {
          continue;
        }
        repeats[axis] = true;
        const float unit = wrap == GX_MIRROR ? 2.f : 1.f; // a mirrored read repeats every two
        int p = 1;
        while (p < 4 && std::fabs(rate * static_cast<float>(p) / unit - std::round(rate * static_cast<float>(p) / unit)) > 1e-3f) {
          ++p;
        }
        period[axis] = std::max(period[axis], p);
      }
    }
  }
  densityU *= static_cast<float>(period[0]);
  densityV *= static_cast<float>(period[1]);
  const auto pow2 = [](float density) {
    uint32_t n = 1;
    while (static_cast<float>(n) < density - 0.5f && n < 1024) {
      n *= 2;
    }
    return n;
  };
  uint32_t width = pow2(densityU), height = pow2(densityV);
  while (width * height > kBakeReadsMaxTexels) {
    (width >= height ? width : height) /= 2;
  }
  key.width = width;
  key.height = height;
  key.period[0] = static_cast<uint32_t>(period[0]);
  key.period[1] = static_cast<uint32_t>(period[1]);
  for (u32 i = 0; i < stageCount; ++i) {
    for (int c = 0; c < 4; ++c) {
      key.konst[i][c] = plan.konst[i][c];
    }
  }
  const uint32_t regsUsed = regs_read(config);
  for (u32 r = 0; r < MaxTevRegs; ++r) {
    for (int c = 0; c < 4 && (regsUsed >> r & 1u) != 0; ++c) {
      key.regs[r][c] = job.state.colorRegs[r][c];
    }
  }
  key.tev = xxh3_hash_s(config.tevStages.data(), sizeof(TevStage) * stageCount) ^
            xxh3_hash_s(config.tevSwapTable.data(), sizeof(config.tevSwapTable));
  // The TEV at a base texel, with the vertex colour (both channels) as given.
  std::array<std::array<float, 4>, MaxTevStages> samples{};
  const auto at = [&](float u, float v, const float raster[8]) {
    for (u32 i = 0; i < stageCount; ++i) {
      const Source& src = sources[i];
      if (src.pixels != nullptr) {
        samples[i] = sample_lookup(*src.pixels, src.m[0] * u + src.m[1] * v + src.m[2],
                                   src.m[3] * u + src.m[4] * v + src.m[5], src.wrapS, src.wrapT);
      }
    }
    return tev_numeric(job, samples, raster);
  };
  // The vertex colour's part, per channel: added (a constant times it) or multiplying the bake.
  job.bakeRaster = false;
  const bool raster = reads_raster(config);
  std::array<std::array<float, 8>, 4> added{};
  std::array<int8_t, 4> multiplied{-1, -1, -1, -1};
  if (raster) {
    constexpr float kNone[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    std::array<bool, 4> additive{true, true, true, true}, multiplying{true, true, true, true};
    std::array<int, 4> channel{-1, -1, -1, -1};
    for (int t = 0; t < 16; ++t) {
      const float u = (static_cast<float>(t % 4) + 0.37f) / 4.f, v = (static_cast<float>(t / 4) + 0.61f) / 4.f;
      const auto g = at(u, v, kNone);
      for (int j = 0; j < 8; ++j) {
        float unit[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        unit[j] = 1.f;
        const auto h = at(u, v, unit);
        for (int c = 0; c < 4; ++c) {
          const float d = h[c] - g[c];
          // Added: the same constant at every texel (where it changes nothing too). Multiplying:
          // one channel, the bake's own.
          if (t == 0) {
            added[c][j] = d;
          } else if (std::fabs(added[c][j] - d) > 1.f / 255.f) {
            additive[c] = false;
          }
          if (std::fabs(d) < 1e-4f) {
            continue;
          }
          multiplying[c] = multiplying[c] && std::fabs(g[c]) < 1e-4f && (channel[c] < 0 || channel[c] == j);
          channel[c] = j;
        }
      }
    }
    for (int c = 0; c < 4; ++c) {
      if (channel[c] < 0 || additive[c]) {
        multiplied[c] = -1;
      } else if (multiplying[c]) {
        multiplied[c] = static_cast<int8_t>(channel[c]);
        added[c] = {};
      } else {
        return refuse("the vertex colour neither added nor multiplying");
      }
    }
    job.bakeRaster = true;
    job.bakeAdded = added;
    job.bakeMultiplied = multiplied;
    key.tev ^= xxh3_hash_s(multiplied.data(), sizeof multiplied);
  }
  const uint64_t keyHash = xxh3_hash_s(&key, sizeof key);
  const auto use = [&](uint64_t id) {
    part.texture = id;
    part.wrapS = repeats[0] ? static_cast<uint8_t>(aurora::mirror::WrapRepeat) : static_cast<uint8_t>(aurora::mirror::WrapClamp);
    part.wrapT = repeats[1] ? static_cast<uint8_t>(aurora::mirror::WrapRepeat) : static_cast<uint8_t>(aurora::mirror::WrapClamp);
    job.baked = true;
    job.bakeRaw = source - GX_TG_TEX0;
    job.bakeUV[0] = 1.f / static_cast<float>(period[0]);
    job.bakeUV[1] = 1.f / static_cast<float>(period[1]);
    if (trace) {
      Log.info("mirror bake_reads: baked {}x{} over {}x{} periods, {}{}", width, height, period[0], period[1],
               repeats[0] ? (repeats[1] ? "repeating both ways" : "repeating along u") : (repeats[1] ? "repeating along v" : "clamped"),
               raster ? " (with the vertex colour)" : "");
    }
    return true;
  };
  if (const auto made = g_bakeKeys.find(keyHash); made != g_bakeKeys.end()) {
    return use(made->second);
  }
  // A slot per draw of the frame reading these textures, as bake_pair's.
  const uint64_t ids = xxh3_hash_s(key.ids, sizeof key.ids);
  const uint64_t slot[2] = {ids, g_bakeOccurrences[ids]++};
  Baked& baked = g_baked[xxh3_hash_s(slot, sizeof slot)];
  const uint64_t texels = static_cast<uint64_t>(width) * height;
  if (baked.key != keyHash && g_bakeTexels + texels > kBakeTexelsPerFrame &&
      (baked.id == 0 || g_frame - baked.bakedFrame < 3)) {
    if (baked.id == 0) {
      job.bakeRaster = false;
      return refuse("over the frame's budget");
    }
    baked.lastFrame = g_frame;
    return use(baked.id);
  }
  if (baked.id == 0) {
    baked.id = g_nextTextureId++;
  }
  baked.lastFrame = g_frame;
  g_bakeKeys.emplace(keyHash, baked.id);
  if (baked.key != keyHash) {
    baked.key = keyHash;
    baked.bakedFrame = g_frame;
    g_bakeTexels += texels;
    ++g_stats.bakes;
    g_stats.bakeTexels += texels;
    std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4);
    float none[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    for (uint32_t y = 0; y < height; ++y) {
      for (uint32_t x = 0; x < width; ++x) {
        const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(width) * static_cast<float>(period[0]);
        const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(height) * static_cast<float>(period[1]);
        auto value = at(u, v, none);
        for (int c = 0; c < 4; ++c) {
          if (multiplied[c] >= 0) {
            float unit[8] = {0, 0, 0, 0, 0, 0, 0, 0};
            unit[multiplied[c]] = 1.f;
            value[c] = at(u, v, unit)[c];
          }
        }
        uint8_t* out = &rgba[(static_cast<size_t>(y) * width + x) * 4];
        for (int c = 0; c < 4; ++c) {
          out[c] = static_cast<uint8_t>(std::lround(std::clamp(value[c], 0.f, 1.f) * 255.f));
        }
      }
    }
    if (trace) {
      // Test runs: the bake's pixels, as PPM (colour) and PGM (alpha), in TMPDIR.
      if (const char* tmp = std::getenv("TMPDIR"); tmp != nullptr) {
        const std::string name = fmt::format("{}/mirror-bake-{}-{}x{}", tmp, baked.id, width, height);
        if (FILE* f = std::fopen((name + ".ppm").c_str(), "wb")) {
          std::fprintf(f, "P6 %u %u 255\n", width, height);
          for (size_t i = 0; i < rgba.size(); i += 4) {
            std::fwrite(&rgba[i], 1, 3, f);
          }
          std::fclose(f);
        }
        if (FILE* f = std::fopen((name + ".pgm").c_str(), "wb")) {
          std::fprintf(f, "P5 %u %u 255\n", width, height);
          for (size_t i = 3; i < rgba.size(); i += 4) {
            std::fwrite(&rgba[i], 1, 1, f);
          }
          std::fclose(f);
        }
        Log.info("mirror bake_reads: wrote {}", name);
      }
    }
    const bool cutout = binary_alpha(rgba);
    g_newTextures.push_back({baked.id, std::move(rgba), width, height, false, cutout});
  }
  return use(baked.id);
}

// 0109: Wind Waker's blob shadows (dDlst_shadowSimple_c: rupees, rocks, pots, the children and
// other small things) are drawn in the framebuffer's alpha and then by it: a volume (the alpha set to
// the shadow's darkness where it meets the ground), the blob's shape from a texture (the alpha
// multiplied by it: an alpha-only draw), then a quad in the shadow's colour blended by that alpha,
// without a depth test. The mirror left out the alpha-only draws and took the quad as an eye over
// the hair (0105: blended by its own alpha, which is constant): a solid black square under each.
// Now the shape's draw is kept aside (AURORA_MIRROR_BLOB_SHADOWS=0: not), and the quad that follows
// it draws it instead: its texture's alpha as the TEV takes it (baked: a step at the blob's edge),
// times the darkness the alpha was set to (the last alpha-only draw without a texture that set it),
// in the quad's colour, lying on the ground as a decal. A quad blended by an alpha the mirror
// doesn't follow (no shape before it) is left out: nothing, rather than a black square.
const bool kBlobShadows = switch_on("AURORA_MIRROR_BLOB_SHADOWS");
std::unique_ptr<DrawJob> g_mask;      // the last blob shape (FIFO thread)
uint64_t g_maskDraw = UINT64_MAX;     // which draw it was (g_stats.draws)
float g_lastAlphaSet = 0.f;           // the alpha the last alpha-only draw without a texture set
std::unique_ptr<DrawJob> g_scratch;   // for working out a draw's TEV

bool samples_any(const ShaderConfig& config) {
  for (u32 i = 0; i < config.tevStageCount; ++i) {
    if (samples_texture(config.tevStages[i])) {
      return true;
    }
  }
  return false;
}

// A draw's TEV result with no texture and no vertex colour: constants (false if it isn't).
bool constant_tev(DrawJob& job, const ShaderConfig& config, std::array<float, 4>& out) {
  if (samples_any(config) || config.attrs[GX_VA_CLR0].attrType != GX_NONE ||
      config.attrs[GX_VA_CLR1].attrType != GX_NONE) {
    return false;
  }
  job.plan = DrawPlan{};
  plan_draw(config, job.plan, false);
  job.state.capture(config);
  job.config = config;
  const F4 none[2];
  F4 raster[2];
  light(job, none, F3{}, F3{1, 0, 0}, raster);
  const auto tev = run_tev(job, raster);
  for (int c = 0; c < 4; ++c) {
    if (tev[c].sel >= 0 || tev[c].bsel >= 0) {
      return false;
    }
    out[c] = std::clamp(tev[c].c, 0.f, 1.f);
  }
  return true;
}

// An alpha-only draw: a blob's shape kept aside, or the alpha a shadow's volume sets noted.
void note_alpha_draw(GXPrimitive prim, const ShaderConfig& config, u16 vtxCount, const u8* vertices, const u16* indices,
                     u32 indexCount) {
  const auto& st = g_gxState;
  if (!g_scratch) {
    g_scratch = std::make_unique<DrawJob>();
  }
  if (!samples_any(config)) {
    std::array<float, 4> tev{};
    const bool sets = st.blendMode == GX_BM_NONE ||
                      (st.blendMode == GX_BM_BLEND && st.blendFacSrc == GX_BL_ONE && st.blendFacDst == GX_BL_ZERO);
    if (sets && constant_tev(*g_scratch, config, tev)) {
      g_lastAlphaSet = st.dstAlpha != UINT32_MAX ? static_cast<float>(st.dstAlpha & 0xFF) / 255.f : tev[3];
    }
    return;
  }
  // The shape: its alpha multiplies the one there (src dst-alpha, dst zero).
  if (st.blendMode != GX_BM_BLEND || st.blendFacSrc != GX_BL_DSTALPHA || st.blendFacDst != GX_BL_ZERO) {
    return;
  }
  if (!g_mask) {
    g_mask = std::make_unique<DrawJob>();
  }
  DrawJob& job = *g_mask;
  job.plan = DrawPlan{};
  plan_draw(config, job.plan, true);
  if (job.plan.primary == nullptr || job.plan.primary->captured) {
    return;
  }
  job.state.capture(config);
  job.config = config;
  job.prim = prim;
  job.vtxCount = vtxCount;
  job.bytes.assign(vertices, vertices + static_cast<size_t>(vtxCount) * config.vtxStride);
  if (indices != nullptr) {
    job.indices.assign(indices, indices + indexCount);
  } else {
    job.indices.clear();
  }
  job.segments.clear();
  g_maskDraw = g_stats.draws;
}

// The quad blended by the alpha a blob's shape set up: the shape drawn instead (see above). False if
// there's no shape just before it (the caller leaves the quad out).
bool draw_blob_shadow(const ShaderConfig& config, const MirrorPart& quad) {
  if (!g_mask || g_maskDraw + 1 != g_stats.draws) {
    return false;
  }
  g_maskDraw = UINT64_MAX;
  std::array<float, 4> colour{};
  if (!g_scratch) {
    g_scratch = std::make_unique<DrawJob>();
  }
  if (!constant_tev(*g_scratch, config, colour)) {
    return false;
  }
  DrawJob& mask = *g_mask;
  DrawPlan& plan = mask.plan;
  const GXTexObj_& obj = g_gxState.loadedTextures[plan.primaryMap];
  const LookupPixels* pixels = lookup_pixels(*plan.primary, obj);
  if (pixels == nullptr || plan.primaryCoord < 0) {
    return false;
  }
  // The shape's alpha as its TEV takes it, at each texel; in the quad's colour.
  struct Key {
    uint64_t texture, tev;
    float konst[MaxTevStages][4], regs[MaxTevRegs][4], colour[3];
  } key;
  std::memset(&key, 0, sizeof key);
  key.texture = plan.primary->id;
  const u32 stageCount = std::min<u32>(mask.config.tevStageCount, MaxTevStages);
  key.tev = xxh3_hash_s(mask.config.tevStages.data(), sizeof(TevStage) * stageCount) ^
            xxh3_hash_s(mask.config.tevSwapTable.data(), sizeof(mask.config.tevSwapTable));
  for (u32 i = 0; i < stageCount; ++i) {
    for (int c = 0; c < 4; ++c) {
      key.konst[i][c] = plan.konst[i][c];
    }
  }
  // Only the registers its stages read: each shadow's own darkness (REG0's alpha, from its height)
  // isn't, and keyed, made a bake per height.
  const uint32_t regsUsed = regs_read(mask.config);
  for (u32 r = 0; r < MaxTevRegs; ++r) {
    for (int c = 0; c < 4 && (regsUsed >> r & 1u) != 0; ++c) {
      key.regs[r][c] = mask.state.colorRegs[r][c];
    }
  }
  std::copy(colour.begin(), colour.begin() + 3, key.colour);
  const uint64_t keyHash = xxh3_hash_s(&key, sizeof key);
  uint64_t id = 0;
  if (const auto made = g_bakeKeys.find(keyHash); made != g_bakeKeys.end()) {
    id = made->second;
  } else {
    Baked& baked = g_baked[keyHash ^ 0x5348414457ull]; // one per shape and colour
    if (baked.id == 0) {
      baked.id = g_nextTextureId++;
    }
    baked.lastFrame = g_frame;
    if (baked.key != keyHash) {
      baked.key = keyHash;
      baked.bakedFrame = g_frame;
      std::vector<uint8_t> rgba(static_cast<size_t>(pixels->width) * pixels->height * 4);
      std::array<std::array<float, 4>, MaxTevStages> samples{};
      constexpr float kNone[8] = {0, 0, 0, 0, 0, 0, 0, 0};
      for (uint32_t texel = 0; texel < pixels->width * pixels->height; ++texel) {
        const uint8_t* own = &pixels->rgba[static_cast<size_t>(texel) * 4];
        for (u32 i = 0; i < stageCount; ++i) {
          for (int c = 0; c < 4; ++c) {
            samples[i][c] = own[c] / 255.f;
          }
        }
        const auto value = tev_numeric(mask, samples, kNone);
        uint8_t* out = &rgba[static_cast<size_t>(texel) * 4];
        for (int c = 0; c < 3; ++c) {
          out[c] = static_cast<uint8_t>(std::lround(colour[c] * 255.f));
        }
        out[3] = static_cast<uint8_t>(std::lround(std::clamp(value[3], 0.f, 1.f) * 255.f));
      }
      const bool cutout = binary_alpha(rgba);
      g_newTextures.push_back({baked.id, std::move(rgba), pixels->width, pixels->height, false, cutout});
      ++g_stats.bakes;
    }
    id = baked.id;
    g_bakeKeys.emplace(keyHash, id);
  }
  // Into the frame, in the quad's place: an alpha blend, on the ground. Black at opacity a darkens a
  // GX picture to (1 - a) of it, which in linear light is about (1 - a)^2.2 (as 0107's shadows).
  if (g_jobCount == g_jobs.size()) {
    g_jobs.push_back(std::make_unique<DrawJob>());
  }
  std::swap(g_jobs[g_jobCount], g_mask);
  DrawJob& job = *g_jobs[g_jobCount++];
  job.alphaOp = -1;
  job.alphaLayer = -1;
  job.writesAlpha = false;
  MirrorPart part{};
  part.kind = aurora::mirror::PartBlend;
  part.colourBase = 0.f;
  part.colourAlpha = 1.f;
  part.opacityBase = 0.f;
  part.opacityAlpha = 1.f;
  part.opacityLuma = 0.f;
  part.flags = aurora::mirror::PartDepthTest;
  part.texture = id;
  part.wrapS = static_cast<uint8_t>(std::min<int>(obj.wrap_s(), 2));
  part.wrapT = static_cast<uint8_t>(std::min<int>(obj.wrap_t(), 2));
  (void)quad;
  job.behind = false;
  job.background = false;
  const auto& proj = job.state.proj;
  const float far = std::fabs(proj.m2[2]) > 1e-9f ? proj.m2[3] / proj.m2[2] : 1e6f;
  job.nearClip = std::max(1.f, std::fabs(1.f - proj.m2[2]) > 1e-9f ? -proj.m2[2] * far / (1.f - proj.m2[2]) : 1.f);
  job.kx = kClipMargin / proj.m0[0];
  job.ky = kClipMargin / proj.m1[1];
  job.perVertex = false;
  job.pull = 0.f;
  job.baked = true;
  job.bakeRaster = false;
  job.bakeUV[0] = job.bakeUV[1] = 1.f; // a recycled job: bake_reads' fields from its last use
  job.bakeRaw = -1;
  const float darkness = std::clamp(g_lastAlphaSet, 0.f, 1.f);
  job.bakeAlpha = 1.f - std::pow(1.f - darkness, 2.2f);
  job.channelSel = {0, 1, 2, 3};
  Group& group = group_for(part);
  job.group = static_cast<size_t>(&group - g_groups.data());
  job.nearer = 1.f - 1e-6f * static_cast<float>(g_jobCount);
  job.decal = true;
  queue_open_job();
  g_openJob = &job;
  g_mergeTarget = nullptr;
  g_lastLeftOut = true; // (draws merged into the quad aren't the shape's)
  ++g_stats.recorded;
  g_stats.vertices += job.vtxCount;
  return true;
}

// 0108 test switch: AURORA_MIRROR_TRACE_TAG=<game frame> logs every draw of the game's frame n (the
// BEGIN mark's number, as the window and the perf lines count; merged ones aside) with its whole
// state: blend, depth, colour and alpha writes, the destination alpha, the TEV's stages, registers
// and channels, the texture coordinates, and each texture (its pixels written once as PPM and PGM
// into TMPDIR). For finding how the game draws something the mirror gets wrong.
void trace_draw(GXPrimitive prim, const ShaderConfig& config, u16 vtxCount) {
  const auto& st = g_gxState;
  std::string text = fmt::format(
      "mirror draw {}: rec {} off {} prim {} n {} proj {} blend {} {}->{} op {} depth {}/{}{} cull {} colour {} alpha {} "
      "dstalpha {} test {} {} {} {} {} | regs",
      g_frameTag, g_recording, g_offscreen, static_cast<int>(prim), vtxCount, static_cast<int>(st.projType),
      static_cast<int>(st.blendMode), static_cast<int>(st.blendFacSrc), static_cast<int>(st.blendFacDst),
      static_cast<int>(st.blendOp), st.depthCompare ? "test" : "-", static_cast<int>(st.depthFunc),
      st.depthUpdate ? "+write" : "", static_cast<int>(st.cullMode), st.colorUpdate, st.alphaUpdate,
      st.dstAlpha == UINT32_MAX ? -1 : static_cast<int>(st.dstAlpha), static_cast<int>(config.alphaCompare.comp0),
      config.alphaCompare.ref0, static_cast<int>(config.alphaCompare.op), static_cast<int>(config.alphaCompare.comp1),
      config.alphaCompare.ref1);
  for (const auto& r : st.colorRegs) {
    text += fmt::format(" ({:.2f} {:.2f} {:.2f} {:.2f})", r[0], r[1], r[2], r[3]);
  }
  text += " k";
  for (const auto& r : st.kcolors) {
    text += fmt::format(" ({:.2f} {:.2f} {:.2f} {:.2f})", r[0], r[1], r[2], r[3]);
  }
  for (int c = 0; c < 4; ++c) {
    const auto& cc = st.colorChannelConfig[c];
    const auto& cs = st.colorChannelState[c];
    text += fmt::format(" ch{} {}{}{} mat ({:.2f} {:.2f} {:.2f} {:.2f}) amb ({:.2f} {:.2f} {:.2f} {:.2f})", c,
                        cc.lightingEnabled ? "lit" : "-", static_cast<int>(cc.matSrc), static_cast<int>(cc.ambSrc),
                        cs.matColor[0], cs.matColor[1], cs.matColor[2], cs.matColor[3], cs.ambColor[0], cs.ambColor[1],
                        cs.ambColor[2], cs.ambColor[3]);
  }
  text += fmt::format(" clr0 {} clr1 {} |", static_cast<int>(config.attrs[GX_VA_CLR0].attrType),
                      static_cast<int>(config.attrs[GX_VA_CLR1].attrType));
  for (u32 i = 0; i < config.tevStageCount; ++i) {
    const auto& stage = config.tevStages[i];
    text += fmt::format(" [{}: t{} m{} ch{} c {} {} {} {} op{}b{}s{}c{}>{} a {} {} {} {} op{}b{}s{}c{}>{} k{}/{} sw{}/{}", i,
                        static_cast<int>(stage.texCoordId), static_cast<int>(stage.texMapId),
                        static_cast<int>(stage.channelId), static_cast<int>(stage.colorPass.a),
                        static_cast<int>(stage.colorPass.b), static_cast<int>(stage.colorPass.c),
                        static_cast<int>(stage.colorPass.d), static_cast<int>(stage.colorOp.op),
                        static_cast<int>(stage.colorOp.bias), static_cast<int>(stage.colorOp.scale),
                        stage.colorOp.clamp ? 1 : 0, static_cast<int>(stage.colorOp.outReg),
                        static_cast<int>(stage.alphaPass.a), static_cast<int>(stage.alphaPass.b),
                        static_cast<int>(stage.alphaPass.c), static_cast<int>(stage.alphaPass.d),
                        static_cast<int>(stage.alphaOp.op), static_cast<int>(stage.alphaOp.bias),
                        static_cast<int>(stage.alphaOp.scale), stage.alphaOp.clamp ? 1 : 0,
                        static_cast<int>(stage.alphaOp.outReg), static_cast<int>(stage.kcSel),
                        static_cast<int>(stage.kaSel), static_cast<int>(stage.tevSwapRas),
                        static_cast<int>(stage.tevSwapTex));
    if (stage.texMapId < MaxTextures) {
      const auto& obj = st.loadedTextures[stage.texMapId];
      text += fmt::format(" tex f{} {}x{} w{}{} {:p}{}", obj.format(), obj.width(), obj.height(), static_cast<int>(obj.wrap_s()),
                          static_cast<int>(obj.wrap_t()), obj.data, st.copyTextures.contains(obj.data) ? " copy" : "");
      static std::unordered_map<const void*, bool> written;
      const char* tmp = std::getenv("TMPDIR");
      if (tmp != nullptr && obj.has_data() && !st.copyTextures.contains(obj.data) && written.emplace(obj.data, true).second) {
        const auto rgba = decode(obj, palette_of(obj));
        if (!rgba.empty()) {
          const std::string base = fmt::format("{}/mirror-tex-{:p}-{}x{}-f{}", tmp, obj.data, obj.width(), obj.height(), obj.format());
          if (FILE* f = std::fopen((base + ".ppm").c_str(), "wb")) {
            std::fprintf(f, "P6 %u %u 255\n", obj.width(), obj.height());
            for (size_t p = 0; p < rgba.size(); p += 4) {
              std::fwrite(&rgba[p], 1, 3, f);
            }
            std::fclose(f);
          }
          if (FILE* f = std::fopen((base + ".pgm").c_str(), "wb")) {
            std::fprintf(f, "P5 %u %u 255\n", obj.width(), obj.height());
            for (size_t p = 3; p < rgba.size(); p += 4) {
              std::fwrite(&rgba[p], 1, 1, f);
            }
            std::fclose(f);
          }
        }
      }
    }
    text += "]";
  }
  for (u32 i = 0; i < MaxTexCoord; ++i) {
    const auto& tcg = config.tcgs[i];
    if (tcg.src != GX_MAX_TEXGENSRC) {
      text += fmt::format(" tcg{} t{} s{} m{} p{}{}", i, static_cast<int>(tcg.type), static_cast<int>(tcg.src),
                          static_cast<int>(tcg.mtx), static_cast<int>(tcg.postMtx), tcg.normalize ? " n" : "");
      if (tcg.mtx != GX_IDENTITY && tcg.mtx >= GX_TEXMTX0) {
        const auto& m = st.texMtxs[std::min<u32>((tcg.mtx - GX_TEXMTX0) / 3, MaxTexMtx - 1)];
        text += fmt::format(" [{:.3f} {:.3f} {:.3f} {:.3f} / {:.3f} {:.3f} {:.3f} {:.3f}]", m.m0[0], m.m0[1], m.m0[2],
                            m.m0[3], m.m1[0], m.m1[1], m.m1[2], m.m1[3]);
      }
    }
  }
  const auto& m = st.pnMtx[std::min<u32>(st.currentPnMtx, MaxPnMtx - 1)].pos;
  text += fmt::format(" at ({:.0f} {:.0f} {:.0f})", m.m0[3], m.m1[3], m.m2[3]);
  Log.info("{}", text);
}
// 0110: an empty group of its own in an alpha layer's place (or the alpha's set-up's), which keeps
// the blended draws on either side of it from merging into one.
size_t alpha_layer_group() {
  MirrorPart part{};
  part.kind = aurora::mirror::PartBlend;
  part.colourBase = 1.f;
  part.colourAlpha = 0.f;
  part.opacityBase = 0.f;
  part.opacityAlpha = 1.f;
  part.opacityLuma = 0.f;
  part.flags = aurora::mirror::PartDepthTest;
  g_lastBlended = -1; // a group of its own
  Group& group = group_for(part);
  g_lastBlended = -1; // and the next draw's not in it
  return static_cast<size_t>(&group - g_groups.data());
}

// 0110: a draw on the framebuffer's alpha (see AlphaOp): an alpha-only draw without a texture (a
// volume, or over the whole picture when flat), or a flat quad blended by the alpha (a layer, and
// an operation for what it writes in the alpha). True when it was taken as one.
bool record_alpha(GXPrimitive prim, const ShaderConfig& config, u16 vtxCount, const u8* vertices, const u16* indices,
                  u32 indexCount) {
  const auto& st = g_gxState;
  const bool ortho = st.projType == GX_ORTHOGRAPHIC;
  const bool alphaOnly = !st.colorUpdate && st.alphaUpdate;
  const bool readsAlpha = st.blendMode == GX_BM_BLEND &&
                          (st.blendFacSrc == GX_BL_DSTALPHA || st.blendFacSrc == GX_BL_INVDSTALPHA ||
                           st.blendFacDst == GX_BL_DSTALPHA || st.blendFacDst == GX_BL_INVDSTALPHA);
  const bool layer = ortho && st.colorUpdate && readsAlpha;
  if (!(alphaOnly || layer || (ortho && st.alphaUpdate)) || config.lineMode != 0 || st.cullMode == GX_CULL_ALL) {
    return false;
  }
  // Only constants: no texture, no vertex colours.
  for (u32 i = 0; i < config.tevStageCount; ++i) {
    if (samples_texture(config.tevStages[i])) {
      return false;
    }
  }
  if (config.attrs[GX_VA_CLR0].attrType != GX_NONE || config.attrs[GX_VA_CLR1].attrType != GX_NONE) {
    return false;
  }
  if (g_jobCount == g_jobs.size()) {
    g_jobs.push_back(std::make_unique<DrawJob>());
  }
  DrawJob& job = *g_jobs[g_jobCount];
  job.plan = DrawPlan{};
  plan_draw(config, job.plan, false);
  job.state.capture(config);
  job.config = config;
  const F4 none[2];
  F4 raster[2];
  light(job, none, F3{}, F3{1, 0, 0}, raster);
  const auto tev = run_tev(job, raster);
  for (const Sym& c : tev) {
    if (c.sel >= 0 || c.bsel >= 0) {
      return false;
    }
  }
  AlphaOp op;
  op.mode = st.blendMode;
  op.src = st.blendFacSrc;
  op.dst = st.blendFacDst;
  op.alpha = std::clamp(tev[3].c, 0.f, 1.f);
  op.constant = st.dstAlpha != UINT32_MAX;
  op.constantAlpha = op.constant ? static_cast<float>(st.dstAlpha & 0xFF) / 255.f : 0.f;
  op.depthTest = st.depthCompare;
  op.depthFunc = st.depthFunc;
  op.front = st.cullMode != GX_CULL_FRONT;
  op.back = st.cullMode != GX_CULL_BACK;
  g_mergeTarget = nullptr;
  g_lastLeftOut = true;
  if (ortho) {
    if (layer) {
      // The layer reads the alpha as it is before this quad writes its own.
      AlphaLayer made;
      made.ops = g_alphaOps.size();
      made.colour[0] = std::clamp(tev[0].c, 0.f, 1.f);
      made.colour[1] = std::clamp(tev[1].c, 0.f, 1.f);
      made.colour[2] = std::clamp(tev[2].c, 0.f, 1.f);
      made.alpha = op.alpha;
      made.src = st.blendFacSrc;
      made.dst = st.blendFacDst;
      // Multiplying: in the place of the quad that set up the alpha it reads, if there's one.
      int placed = -1;
      if (made.dst != GX_BL_ONE) {
        for (size_t i = g_alphaOps.size(); i-- > 0;) {
          if (g_alphaOps[i].group >= 0) {
            placed = g_alphaOps[i].group;
            made.fromJob = g_alphaOps[i].jobs;
            g_alphaOps[i].group = -1;
            break;
          }
        }
      }
      made.toJob = g_jobCount;
      made.group = placed >= 0 ? static_cast<size_t>(placed) : alpha_layer_group();
      queue_open_job();
      job.alphaOp = -1;
      job.alphaLayer = static_cast<int>(g_alphaLayers.size());
      job.vtxCount = 0;
      job.bytes.clear();
      job.indices.clear();
      job.segments.clear();
      job.group = made.group;
      g_alphaLayers.push_back(made);
      ++g_jobCount; // in the frame's order; nothing to decode
    }
    if (st.alphaUpdate) {
      op.screen = true;
      // Setting the alpha over the whole picture: a place for a layer that multiplies by it.
      if (op.constant || op.mode == GX_BM_NONE) {
        op.jobs = g_jobCount;
        op.group = static_cast<int>(alpha_layer_group());
      }
      g_alphaOps.push_back(std::move(op));
    }
    return true;
  }
  if (!alphaOnly) {
    return false;
  }
  // A volume: decoded on a worker as a draw is, kept out of the picture (merge_jobs).
  job.alphaOp = static_cast<int>(g_alphaOps.size());
  job.alphaLayer = -1;
  g_alphaOps.push_back(std::move(op));
  job.prim = prim;
  job.vtxCount = vtxCount;
  job.bytes.assign(vertices, vertices + static_cast<size_t>(vtxCount) * config.vtxStride);
  if (indices != nullptr) {
    job.indices.assign(indices, indices + indexCount);
  } else {
    job.indices.clear();
  }
  job.segments.clear();
  job.behind = false;
  job.background = false;
  job.perVertex = false;
  job.constantResult = tev;
  job.channelSel = {-1, -1, -1, -1};
  job.pull = 0.f;
  job.baked = false;
  job.decal = false;
  job.nearer = 1.f;
  job.group = 0;
  job.nearClip = 1.f;
  job.kx = job.ky = 1.f;
  ++g_jobCount;
  queue_open_job();
  g_openJob = &job;
  g_mergeTarget = job.indices.empty() ? &job : nullptr;
  g_lastLeftOut = false;
  return true;
}

void record(GXPrimitive prim, const ShaderConfig& config, u16 vtxCount, const u8* vertices, const u16* indices,
            u32 indexCount, bool merged) noexcept {
  static const long s_traceTag = [] {
    const char* value = std::getenv("AURORA_MIRROR_TRACE_TAG");
    return value != nullptr ? std::atol(value) : -1L;
  }();
  if (s_traceTag >= 0 && static_cast<long>(g_frameTag) == s_traceTag && !merged) {
    trace_draw(prim, config, vtxCount);
  }
  // 0109 test switch: AURORA_MIRROR_SKIP=<first>-<last> leaves out the frame's draws with those
  // numbers (as AURORA_MIRROR_TRACE_TAG's lines count them): which draws make something.
  static const std::pair<long, long> s_skip = [] {
    const char* value = std::getenv("AURORA_MIRROR_SKIP");
    long first = -1, last = -1;
    if (value != nullptr && std::sscanf(value, "%ld-%ld", &first, &last) != 2) {
      first = last = -1;
    }
    return std::pair<long, long>{first, last};
  }();
  if (!merged) {
    ++g_frameDraws;
  }
  if (s_skip.first >= 0 && g_frameDraws - 1 >= s_skip.first && g_frameDraws - 1 <= s_skip.second) {
    g_mergeTarget = nullptr;
    g_lastLeftOut = true;
    return;
  }
  // 0107 test switch: AURORA_MIRROR_TRACE_FRAME=<n> logs every draw of mirror frame n (merged ones
  // aside), recorded or not, with its state: what the scene is made of, and what the mirror skips.
  static const long s_traceFrame = [] {
    const char* value = std::getenv("AURORA_MIRROR_TRACE_FRAME");
    return value != nullptr ? std::atol(value) : -1L;
  }();
  if (s_traceFrame >= 0 && static_cast<long>(g_frame) == s_traceFrame && !merged) {
    const auto& st = g_gxState;
    std::string maps;
    for (u32 i = 0; i < config.tevStageCount; ++i) {
      const auto& stage = config.tevStages[i];
      if (stage.texMapId < MaxTextures) {
        const auto& obj = st.loadedTextures[stage.texMapId];
        maps += fmt::format(" map{} fmt{} {}x{}", static_cast<int>(stage.texMapId), obj.format(), obj.width(), obj.height());
      }
    }
    const auto& ac = config.alphaCompare;
    Log.info("mirror trace: recording {} offscreen {} prim {} vertices {} stages {} ind {} proj {} blend {} {}->{} "
             "depth {}{} colour {} alpha {} test {} {} {} {} {}{}",
             g_recording, g_offscreen, static_cast<int>(prim), vtxCount, config.tevStageCount, config.numIndStages,
             static_cast<int>(st.projType), static_cast<int>(st.blendMode), static_cast<int>(st.blendFacSrc),
             static_cast<int>(st.blendFacDst), st.depthCompare ? "test" : "-", st.depthUpdate ? "+write" : "",
             st.colorUpdate, st.alphaUpdate, static_cast<int>(ac.comp0), ac.ref0, static_cast<int>(ac.op),
             static_cast<int>(ac.comp1), ac.ref1, maps);
  }
  if (!g_recording || g_offscreen > 0 || vtxCount == 0 || g_sky) {
    tally(g_offscreen > 0 ? TallyOffscreen : TallyOutside);
    g_mergeTarget = nullptr;
    g_lastLeftOut = false;
    return;
  }
  // 0106: a draw the command processor merged into the last (no GX state changed between them)
  // joins the last's job, or is left out as it was: its state, material and plan are the same.
  static const bool s_mergeJobs = [] {
    const char* value = std::getenv("AURORA_MIRROR_MERGE");
    return value == nullptr || std::strcmp(value, "0") != 0;
  }();
  if (merged && indices == nullptr && s_mergeJobs) {
    if (g_mergeTarget != nullptr) {
      DrawJob& job = *g_mergeTarget;
      if (job.segments.empty()) {
        job.segments.push_back({job.prim, 0, job.vtxCount});
      }
      job.segments.push_back({prim, job.vtxCount, vtxCount});
      job.bytes.insert(job.bytes.end(), vertices, vertices + static_cast<size_t>(vtxCount) * config.vtxStride);
      job.vtxCount += vtxCount;
      ++g_stats.draws;
      ++g_stats.recorded;
      ++g_stats.merged;
      g_stats.vertices += vtxCount;
      return;
    }
    if (g_lastLeftOut) {
      ++g_stats.draws;
      ++g_stats.merged;
      return;
    }
  }
  g_mergeTarget = nullptr;
  g_lastLeftOut = true; // until its job is made, below
  struct Timer {
    uint64_t started = clock_ns();
    ~Timer() { g_stats.seconds += static_cast<double>(clock_ns() - started) * 1e-9; }
  } timer;
  const auto& st = g_gxState;
  ++g_stats.draws;
  // A blob shadow's alpha-only draws (0109), noted before the alpha's own record takes the volume.
  if (kBlobShadows && !st.colorUpdate && st.alphaUpdate && config.lineMode == 0 && st.projType != GX_ORTHOGRAPHIC &&
      st.proj.m0[0] > 0.f && st.proj.m1[1] > 0.f) {
    note_alpha_draw(prim, config, vtxCount, vertices, indices, indexCount);
  }
  if (kAlphaBuffer && record_alpha(prim, config, vtxCount, vertices, indices, indexCount)) {
    ++g_stats.alphaOps;
    return;
  }
  if (config.lineMode != 0) {
    ++g_stats.lines;
    tally(TallyLines);
    return;
  }
  if (st.projType == GX_ORTHOGRAPHIC || st.proj.m0[0] <= 0.f || st.proj.m1[1] <= 0.f) {
    ++g_stats.screen;
    tally(TallyScreen);
    return;
  }
  if (!st.colorUpdate) {
    ++g_stats.noColour; // (a blob's alpha-only draws noted above)
    tally(TallyNoColour);
    return;
  }
  const auto fb = logical_fb_size();
  if (st.logicalViewport.width * st.logicalViewport.height < 0.8f * static_cast<float>(fb.x) * static_cast<float>(fb.y)) {
    ++g_stats.viewport;
    tally(TallyViewport);
    return;
  }
  if (!g_haveProjection) {
    g_haveProjection = true;
    g_p0 = st.proj.m0[0];
    g_p2 = st.proj.m1[1];
  } else if (std::fabs(st.proj.m0[0] - g_p0) > 0.03f * g_p0 || std::fabs(st.proj.m1[1] - g_p2) > 0.03f * g_p2) {
    ++g_stats.projection;
    tally(TallyProjection);
    return;
  }

  // The material.
  MirrorPart part{};
  if (!blend_weights(part)) {
    ++g_stats.blend;
    tally(TallyBlend);
    return;
  }
  // The Luigi's Mansion port's ghost pass (MarkGhostPass): what the draw would leave in the ghost
  // layer is added over the scene, weighted by the layer's alpha (the ghost's fade level, the
  // destination alpha it's drawn with; without one, the draw's own alpha). A blended draw in the
  // layer (a Boo's see-through parts) adds as much as it would show there.
  if (g_ghostPass) {
    const bool constantAlpha = st.dstAlpha != UINT32_MAX;
    const float fade = constantAlpha ? static_cast<float>(st.dstAlpha & 0xFF) / 255.f : 1.f;
    if (fade <= 0.f) {
      ++g_stats.blend;
      tally(TallyBlend);
      return;
    }
    if (part.kind != aurora::mirror::PartBlend) {
      // An alpha-tested draw (a cut-out ghost texture) adds only where its texels pass: weighted by
      // its own alpha, which the test would otherwise have cut (the review: a halo of the texels
      // under alpha 0).
      const bool tested = alpha_threshold(config.alphaCompare) > 0.f;
      part.colourBase = constantAlpha && !tested ? fade : 0.f;
      part.colourAlpha = constantAlpha && !tested ? 0.f : fade;
    } else {
      part.colourBase *= fade;
      part.colourAlpha *= fade;
    }
    part.kind = aurora::mirror::PartBlend;
    part.opacityBase = 0.f;
    part.opacityAlpha = 0.f;
    part.opacityLuma = 0.f;
  }
  // 0105: Wind Waker draws eyes and eyebrows over the hair. A mask of where they're in view goes
  // into the framebuffer's alpha first (colour off: not mirrored), then the eye, with no depth test,
  // blended by that alpha (dst alpha, 1 - dst alpha). Read as GX's factors that was an additive
  // blend, and the dark eyes vanished. The mask is the eye's own alpha where nothing nearer hides it:
  // a plain alpha blend, tested against depth from kOverHairPull nearer (decode_vertex), so the hair
  // in front of the face doesn't hide it and what stands in front of the character still does.
  //
  // 0107: and Wind Waker's real-time shadows (a character's, cast on the ground: dDlst_shadowReal_c)
  // blend the same way, with a depth test: the silhouette goes into the framebuffer's alpha first,
  // then the ground beneath it is drawn again in black (its alpha the same silhouette, a copy
  // projected from the light), blended by that alpha. As GX's factors, additive black: no shadow.
  // As an alpha blend by its own alpha, the ground darkens where the silhouette falls.
  const bool dstAlphaBlend = st.blendMode == GX_BM_BLEND && st.blendFacSrc == GX_BL_DSTALPHA &&
                             st.blendFacDst == GX_BL_INVDSTALPHA;
  const bool dstAlphaDepthTest = st.depthCompare && st.depthFunc != GX_ALWAYS;
  // 0109: a quad blended by the alpha a blob shadow's shape set up (no texture of its own): the
  // shadow, drawn from its shape; or, with no shape before it, left out.
  if (kBlobShadows && dstAlphaBlend && !samples_any(config)) {
    if (!draw_blob_shadow(config, part)) {
      tally(TallyBlend);
    }
    return;
  }
  const bool overHair = kOverHair && dstAlphaBlend && !dstAlphaDepthTest;
  const bool realShadow = kRealShadows && dstAlphaBlend && dstAlphaDepthTest;
  if (overHair || realShadow) {
    part.kind = aurora::mirror::PartBlend;
    part.colourBase = 0.f;
    part.colourAlpha = 1.f;
    part.opacityBase = 0.f;
    part.opacityAlpha = 1.f;
    part.opacityLuma = 0.f;
  }
  // (0106: the last answer kept: draws in a row mostly test alpha alike.)
  static AlphaCompare s_lastCompare{};
  static float s_lastThreshold = alpha_threshold(s_lastCompare);
  if (std::memcmp(&s_lastCompare, &config.alphaCompare, sizeof s_lastCompare) != 0) {
    s_lastCompare = config.alphaCompare;
    s_lastThreshold = alpha_threshold(s_lastCompare);
  }
  const float threshold = s_lastThreshold;
  if (threshold > 1.f) {
    ++g_stats.alphaNever;
    tally(TallyAlphaNever);
    return;
  }
  if (threshold > 0.f) {
    // Alpha-tested blending (foliage, fences) is a cut-out that keeps its depth.
    // (Only those writing depth: a "greater than 0" test on smoke or a glow keeps its soft edge.)
    if (part.kind == aurora::mirror::PartOpaque ||
        (st.depthUpdate && part.colourBase == 0 && part.colourAlpha == 1 && part.opacityBase == 0 &&
         part.opacityAlpha == 1 && part.opacityLuma == 0)) {
      part.kind = aurora::mirror::PartCutout;
    }
    part.cutoff = threshold;
  }
  const bool depthTest = (st.depthCompare && st.depthFunc != GX_ALWAYS) || overHair;
  if (depthTest) {
    part.flags |= aurora::mirror::PartDepthTest;
  }
  // (A ghost writes depth in its own layer, not over the scene.)
  if (st.depthUpdate && !g_ghostPass) {
    part.flags |= aurora::mirror::PartDepthWrite;
  }
  if (g_world) {
    part.flags |= aurora::mirror::PartWorld;
  }
  if (st.cullMode == GX_CULL_BACK) {
    part.flags |= aurora::mirror::PartCullBack;
  } else if (st.cullMode == GX_CULL_FRONT) {
    part.flags |= aurora::mirror::PartCullFront;
  } else if (st.cullMode == GX_CULL_ALL) {
    return;
  }
  // The sky: drawn before the level writes any depth, writing none itself (its dome and its blended
  // cloud and glow layers), or solid without a depth test.
  const bool solid = part.kind != aurora::mirror::PartBlend;
  const bool background = (solid && !depthTest) || (!st.depthUpdate && !g_sawDepthWrite);
  if (background) {
    part.flags |= aurora::mirror::PartBackground;
  } else if (solid && depthTest && st.depthUpdate) {
    g_sawDepthWrite = true;
  }

  // 0106: planned in the job it will go into (claimed below once the draw is kept), not copied there.
  if (g_jobCount == g_jobs.size()) {
    g_jobs.push_back(std::make_unique<DrawJob>());
  }
  DrawPlan& plan = g_jobs[g_jobCount]->plan;
  plan = DrawPlan{};
  plan_draw(config, plan, part.kind == aurora::mirror::PartBlend);
  bool behind = false;
  if (plan.behindMap >= 0) {
    if (g_solidRefraction.load(std::memory_order_relaxed) && g_world && !background &&
        (part.kind == aurora::mirror::PartOpaque || plan.primary != nullptr)) {
      // The port's volume (set_solid_refraction), the room's own shell (MarkWorld): what's behind is
      // read as solid black, and the surface keeps its own kind. Read as see-through (or, blended,
      // as the copy's average: all zero, alpha too, for a copy the mirror can't read), the Foyer's
      // window panes came out as holes onto the room the volume stands in. (Only the shell: the
      // flashlight's beam reads a copy too, and came out as a solid white cone.)
      static int s_logged = 0;
      if (s_logged < 6) {
        ++s_logged;
        const auto& copy = st.loadedTextures[plan.behindMap];
        Log.info("mirror: a surface of the room drawn from a framebuffer copy (map {} {}x{}) kept solid: kind {}, "
                 "blend {} {}->{}, {} stages, {} vertices, primary {}",
                 plan.behindMap, copy.width(), copy.height(), part.kind, static_cast<int>(st.blendMode),
                 static_cast<int>(st.blendFacSrc), static_cast<int>(st.blendFacDst), config.tevStageCount, vtxCount,
                 plan.primary != nullptr ? fmt::format("{}x{}", st.loadedTextures[plan.primaryMap].width(),
                                                       st.loadedTextures[plan.primaryMap].height())
                                         : std::string("none"));
      }
      plan.average[plan.behindMap] = {0.f, 0.f, 0.f, 1.f};
      plan.behindMap = -1;
      ++g_stats.behind;
    } else if (part.kind == aurora::mirror::PartOpaque && !background) {
      // A refracting surface (water): it draws what's behind it from a copy of the framebuffer. In
      // the mirror, what's behind is really there: the surface becomes see-through, as much as the
      // copy weighs in its colour (premultiplied, its opacity in alpha).
      behind = true;
      part.kind = aurora::mirror::PartBlend;
      part.colourBase = 1;
      part.colourAlpha = 0;
      part.opacityBase = 0;
      part.opacityAlpha = 1;
      part.opacityLuma = 0;
      ++g_stats.behind;
    } else if (plan.primary == nullptr) {
      // Only a framebuffer copy (bloom, heat haze, a shadow image): a screen effect.
      ++g_stats.copyOnly;
      tally(TallyCopyOnly);
      log_skipped(config, part, vtxCount);
      return;
    } else {
      plan.behindMap = -1; // as its average
    }
  }
  if (plan.primary != nullptr) {
    const auto& obj = st.loadedTextures[plan.primaryMap];
    part.wrapS = static_cast<uint8_t>(std::min<int>(obj.wrap_s(), 2));
    part.wrapT = static_cast<uint8_t>(std::min<int>(obj.wrap_t(), 2));
  }

  DrawJob& job = *g_jobs[g_jobCount++];
  job.alphaOp = -1;
  job.alphaLayer = -1;
  job.writesAlpha = st.alphaUpdate;
  tally(TallyKept);
  log_textures(config, plan, part, vtxCount);
  job.state.capture(config);
  job.config = config;
  job.prim = prim;
  job.vtxCount = vtxCount;
  job.bytes.assign(vertices, vertices + static_cast<size_t>(vtxCount) * config.vtxStride);
  if (indices != nullptr) {
    job.indices.assign(indices, indices + indexCount);
  } else {
    job.indices.clear();
  }
  job.segments.clear();
  job.behind = behind;
  job.background = background;
  // The GPU's clip: the near plane, and the view's sides with a margin for a viewer leaning to look
  // past the window's edges.
  const auto& proj = st.proj;
  const float far = std::fabs(proj.m2[2]) > 1e-9f ? proj.m2[3] / proj.m2[2] : 1e6f;
  job.nearClip = std::max(1.f, std::fabs(1.f - proj.m2[2]) > 1e-9f ? -proj.m2[2] * far / (1.f - proj.m2[2]) : 1.f);
  job.kx = kClipMargin / proj.m0[0];
  job.ky = kClipMargin / proj.m1[1];
  // Without vertex colours, lighting or lookups, every vertex's TEV result is the same: run it once.
  job.perVertex = config.attrs[GX_VA_CLR0].attrType != GX_NONE || config.attrs[GX_VA_CLR1].attrType != GX_NONE ||
                  plan.anyLookup;
  job.pull = overHair ? kOverHairPull : 0.f;
  job.baked = false;
  job.bakeRaster = false;
  job.bakeAlpha = 1.f;
  job.bakeUV[0] = job.bakeUV[1] = 1.f;
  job.bakeRaw = -1;
  for (int c = 0; c < 2; ++c) {
    job.perVertex = job.perVertex || (plan.rasterUsed[c] && (st.colorChannelConfig[c].lightingEnabled ||
                                                             st.colorChannelConfig[c + GX_ALPHA0].lightingEnabled));
  }
  // Which channel of T each output channel reads is the TEV's structure, the same at every vertex.
  const F4 none[2];
  F4 raster[2];
  light(job, none, F3{}, F3{1, 0, 0}, raster);
  const auto structure = run_tev(job, raster);
  job.constantResult = structure;
  if (realShadow && !job.perVertex) {
    // 0107: the shadow's darkness is the register's alpha (A0, which its mask's silhouette selects).
    // GX blends in the picture's gamma, RealityKit in linear light: black at opacity a darkens a
    // GX picture to (1 - a) of it, which in linear light is about (1 - a)^2.2.
    auto& alpha = job.constantResult[3];
    alpha.k = 1.f - std::pow(1.f - std::clamp(alpha.k, 0.f, 1.f), 2.2f);
  }
  for (int c = 0; c < 4; ++c) {
    job.channelSel[c] = plan.primary != nullptr ? structure[c].sel : -1;
  }
  if (behind) {
    job.channelSel[3] = -1; // alpha carries the opacity
  }
  // 0105: an eye's two textures, baked into one (0108: any draw's two, AURORA_MIRROR_BAKE=0 aside).
  if ((overHair || kBakePairs) && (bake_pair(job, part, overHair) || (kBakePairs && bake_reads(job, part)))) {
    for (int c = 0; c < 4; ++c) {
      job.channelSel[c] = static_cast<int8_t>(c);
    }
  } else if (plan.primary != nullptr) {
    // The texture as the TEV reads it.
    uint8_t swizzle = 0;
    for (int c = 0; c < 4; ++c) {
      const int from = job.channelSel[c] >= 0 ? job.channelSel[c] : c;
      swizzle |= static_cast<uint8_t>(from << (2 * c));
    }
    if (plan.primary->captured) {
      // A read-back copy, swizzled as it arrives: read back right here, after this draw, before the
      // game draws the next shadow into the same copy.
      // 0107: a real-time shadow's mask as its alpha test reads it: in the silhouette or not.
      const int threshold = realShadow ? shadow_threshold(config.alphaCompare, structure[3]) : -1;
      const bool mask = threshold >= 0;
      CaptureVariant& variant =
          plan.captureSlot->variants[plan.captureOccurrence][static_cast<uint16_t>(swizzle | (mask ? 1 : 0) << 8)];
      if (variant.id == 0) {
        variant.id = g_nextTextureId++;
      }
      part.texture = variant.id;
      // Test runs: AURORA_MIRROR_CAPTURE_LOG=1 also says how each masked draw is drawn.
      static const bool s_capturePartLog = [] {
        const char* value = std::getenv("AURORA_MIRROR_CAPTURE_LOG");
        return value != nullptr && std::strcmp(value, "1") == 0;
      }();
      if (s_capturePartLog) {
        static std::unordered_map<uint64_t, uint32_t> seen;
        if (seen[variant.id]++ % 600 == 0) {
          Log.info("mirror: masked draw, capture {} (occurrence {}, revision {}, swizzle {:#04x}): kind {} colour {:.2f}+{:.2f}a "
                   "opacity {:.2f}+{:.2f}a+{:.2f}luma, depth {}{}, {} vertices, blend {} {}->{}",
                   variant.id, plan.captureOccurrence, plan.captureRevision, swizzle, part.kind, part.colourBase,
                   part.colourAlpha, part.opacityBase, part.opacityAlpha, part.opacityLuma,
                   st.depthCompare ? "test" : "-", st.depthUpdate ? "+write" : "", vtxCount,
                   static_cast<int>(st.blendMode), static_cast<int>(st.blendFacSrc), static_cast<int>(st.blendFacDst));
        }
      }
      if (variant.frame != g_frame || variant.revision != plan.captureRevision) {
        variant.frame = g_frame;
        variant.revision = plan.captureRevision;
        if (g_captureTask == gfx::InvalidEncoderTask) {
          g_captureTask = gfx::register_encoder_task_type(
              {.label = "Mirror capture", .callback = capture_task, .afterSubmit = capture_submitted});
        }
        const uint32_t slot = g_captureNext++ % kCaptureRing;
        {
          std::lock_guard lock(g_captureMutex);
          g_captureRing[slot] = {part.texture, plan.captureHandle, swizzle, mask,
                                 static_cast<uint8_t>(mask ? threshold : 0)};
        }
        gfx::push_encoder_task_in_stream(g_captureTask, &slot, sizeof slot);
      }
    } else {
      part.texture = texture_variant(*plan.primary, st.loadedTextures[plan.primaryMap], swizzle);
    }
  }
  Group& group = group_for(part);
  job.group = static_cast<size_t>(&group - g_groups.data());
  // Coplanar draws: a decal (blended, depth-tested, writing no depth: paths, stains, shadows) lies on
  // what's under it; a mesh drawn again where it was just drawn (a second pass) is over the first;
  // and every draw a hair nearer than the ones before, in the game's order. (SHAR's rules.)
  job.nearer = 1.f - 1e-6f * static_cast<float>(g_jobCount);
  job.decal = part.kind == aurora::mirror::PartBlend && !behind && depthTest && !st.depthUpdate && !background;
  {
    struct PassKey {
      uint64_t bytes;
      uint32_t count, matrix;
      float translation[3];
    } key{};
    key.bytes = xxh3_hash_s(vertices, std::min<size_t>(job.bytes.size(), 256));
    key.count = vtxCount;
    key.matrix = st.currentPnMtx;
    const auto& m = st.pnMtx[std::min<u32>(st.currentPnMtx, MaxPnMtx - 1)].pos;
    key.translation[0] = m.m0[3];
    key.translation[1] = m.m1[3];
    key.translation[2] = m.m2[3];
    const unsigned repeat = g_passes[xxh3_hash_s(&key, sizeof key)]++;
    if (repeat > 0) {
      job.nearer *= 1.f - 2e-4f * static_cast<float>(std::min(repeat, 8u));
    }
  }
  // Held back until the next draw is recorded: merged draws join it (queue_open_job).
  queue_open_job();
  g_openJob = &job;
  g_mergeTarget = job.indices.empty() ? &job : nullptr;
  g_lastLeftOut = false;
  ++g_stats.recorded;
  g_stats.vertices += vtxCount;
}
} // namespace aurora::gx::mirror

namespace aurora::mirror {
void set_enabled(bool enabled) noexcept {
  if (enabled && !gx::mirror::g_enabled.load()) {
    gx::mirror::g_reset = true;
  }
  gx::mirror::g_enabled = enabled;
}

bool enabled() noexcept { return gx::mirror::g_enabled.load(); }

void set_front_allowance(float widths, float cameraWidths) noexcept {
  gx::mirror::g_frontAllowance = std::isfinite(widths) ? std::max(widths, 0.f) : 0.f;
  gx::mirror::g_cameraWidths = std::isfinite(cameraWidths) ? std::max(cameraWidths, 0.f) : 0.f;
}

void set_solid_refraction(bool on) noexcept { gx::mirror::g_solidRefraction = on; }

void take_stats(Stats& out) noexcept {
  out.recorded = gx::mirror::g_statRecorded.exchange(0);
  out.skipped = gx::mirror::g_statSkipped.exchange(0);
  out.dropped = gx::mirror::g_statDropped.exchange(0);
  out.finished = gx::mirror::g_statFinished.exchange(0);
  out.recordMs = static_cast<double>(gx::mirror::g_statRecordNs.exchange(0)) / 1e6;
  out.sceneCpuMs = static_cast<double>(gx::mirror::g_statSceneCpuNs.exchange(0)) / 1e6;
  out.finishMs = static_cast<double>(gx::mirror::g_statFinishNs.exchange(0)) / 1e6;
}

bool acquire(Frame& frame, uint32_t upTo) noexcept {
  using namespace gx::mirror;
  {
    std::lock_guard lock(g_mutex);
    // The newest finished frame no later than upTo (or, if they're all later, the oldest).
    size_t take = g_finishedFrames.size();
    for (size_t i = 0; i < g_finishedFrames.size(); ++i) {
      if (not_after(g_finishedFrames[i].tag, upTo & 0xFFFFFFu)) {
        take = i;
      }
    }
    if (take == g_finishedFrames.size() && !g_finishedFrames.empty() && g_finishedFrames.size() == kKeptFrames) {
      take = 0;
    }
    if (take < g_finishedFrames.size()) {
      std::swap(g_acquired, g_finishedFrames[take]);
      for (size_t i = 0; i <= take; ++i) {
        g_spareFrames.push_back(std::move(g_finishedFrames.front()));
        g_finishedFrames.pop_front();
      }
    }
    // Textures go with the first frame taken and then each call (before it, the caller has no
    // frame to take them with).
    g_acquiredTextures.clear();
    g_acquiredRemoved.clear();
    if (g_acquired.serial != 0) {
      g_acquiredTextures.swap(g_pendingTextures);
      g_acquiredRemoved.swap(g_pendingRemoved);
    }
  }
  g_textureViews.clear();
  for (const auto& texture : g_acquiredTextures) {
    g_textureViews.push_back({texture.id, texture.rgba.data(), texture.width, texture.height, texture.mipmapped,
                              texture.cutout});
  }
  const Built& built = g_acquired;
  frame = {};
  frame.serial = built.serial;
  frame.tag = built.tag;
  frame.scene = built.scene;
  frame.vertices = built.vertices.data();
  frame.vertexCount = static_cast<uint32_t>(built.vertices.size());
  frame.indices = built.indices.data();
  frame.indexCount = static_cast<uint32_t>(built.indices.size());
  frame.parts = built.parts.data();
  frame.partCount = static_cast<uint32_t>(built.parts.size());
  std::copy(built.boundsMin, built.boundsMin + 3, frame.boundsMin);
  std::copy(built.boundsMax, built.boundsMax + 3, frame.boundsMax);
  frame.tanHalfX = built.tanHalfX;
  frame.tanHalfY = built.tanHalfY;
  frame.focus = built.focus;
  frame.plane = built.plane;
  frame.textures = g_textureViews.data();
  frame.textureCount = static_cast<uint32_t>(g_textureViews.size());
  frame.removed = g_acquiredRemoved.data();
  frame.removedCount = static_cast<uint32_t>(g_acquiredRemoved.size());
  return built.serial != 0;
}
} // namespace aurora::mirror
