// The scene mirror's frame builder: see scene_mirror.h. Generalised from the TPVR port's
// (tpvr-visionos: extern/aurora/lib/gx/mirror.cpp), where every rule below fixed something seen
// in the window first.

#include "scene_mirror.h"

#include <os/log.h>
#include <pthread.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace scene_mirror {
namespace {

using OutVertex = lmvr_visionos_mirror_vertex;
using Part = lmvr_visionos_mirror_part;
static_assert(sizeof(OutVertex) == 36, "MirrorScene.swift reads 36-byte vertices");

// How much wider than the view the world is kept (each side), for a viewer leaning to look past the
// window's edges, or farther from it than the camera, who sees in front of the glass more widely
// than the camera did (the easing counts on it). 1.4 left the ground under the camera out, and the
// window's foot showed sky (TPVR).
constexpr float kMargin = 2.f;
// Coplanar draws: each a hair nearer the camera than the ones before (RealityKit can't settle a tie
// in favour of the later draw, as a LEQUAL depth test does), a repeated pass of one mesh at one
// place a little more. Scaling about the camera keeps the picture from the camera unchanged.
constexpr float kDrawNudge = 1e-6f;
constexpr float kRepeatNudge = 2e-4f;
// A decal is lifted along the normal of what's under it when the engine gives one (the same from any
// angle: SHAR's blob shadows), else towards the camera by 0.4% of its distance, at most 2 cm:
// uncapped, a far shadow floated tens of centimetres off the ground, and even capped, a slide moves
// against the ground as the viewer leans (TPVR, SHAR).
constexpr float kDecalNormalLiftMetres = 0.03f;
constexpr float kDecalSlide = 4e-3f;
constexpr float kDecalLiftMetres = 0.02f;
// Farther than this, a vertex is a runaway (a particle flung to 1e20): its triangles go, or the
// frame's bounds take everything with them (SHAR).
constexpr float kRunawayMetres = 1e4f;
// The window's glass: a little short of what the camera looks at (so the hero stands behind it),
// within a sane range. What's nearer comes out in front of it, eased into the window's allowance.
constexpr float kGlassFraction = 0.85f;
constexpr float kGlassMinMetres = 0.5f, kGlassMaxMetres = 200.f;
constexpr float kDefaultFocusMetres = 6.f;
// The window pairs a mirror frame with its own frame of the same game frame, which may be a frame
// or two behind: keep a few.
constexpr size_t kKeptFrames = 3;
// Textures unused this many frames (15 s at 60 Hz) are forgotten.
constexpr uint32_t kTextureIdleFrames = 900;

void Log(const char* format, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* format, ...) {
    char text[768];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof text, format, args);
    va_end(args);
    os_log(OS_LOG_DEFAULT, "[LMVR] mirror: %{public}s", text);
    if (isatty(STDERR_FILENO)) {
        std::fprintf(stderr, "[LMVR] mirror: %s\n", text);  // simctl launch --console-pty
    }
}

// IEEE half floats: the CPU's own conversion on ARM64 (hundreds of thousands a frame), otherwise
// round to nearest, clamped to the finite range, small values flushed to zero.
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
    const uint32_t mantissa = bits & 0x7FFFFFu;
    if (exponent <= 0) {
        return static_cast<uint16_t>(sign);
    }
    uint32_t result = sign | static_cast<uint32_t>(exponent) << 10 | mantissa >> 13;
    if ((mantissa & 0x1FFFu) > 0x1000u || ((mantissa & 0x1FFFu) == 0x1000u && (result & 1u))) {
        ++result;  // may carry into the exponent, as rounding should
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

uint64_t Fnv(const void* data, size_t size) {
    uint64_t hash = 1469598103934665603ull;
    for (size_t i = 0; i < size; ++i) {
        hash = (hash ^ static_cast<const uint8_t*>(data)[i]) * 1099511628211ull;
    }
    return hash;
}

// Materials that differ by less than a step of these are one (fades made SHAR 3,600 materials a
// minute before it quantised them).
uint64_t MaterialKey(const Material& m) {
    struct Key {
        uint64_t texture;
        uint32_t kind, flags;
        int32_t cutoff, weights[5];
        uint8_t wrapS, wrapT, pad[6];
    } key{};
    key.texture = m.texture;
    key.kind = m.kind;
    key.flags = m.flags;
    key.cutoff = m.kind == LMVR_VISIONOS_MIRROR_PART_CUTOUT ? static_cast<int32_t>(std::lround(m.cutoff * 255.f)) : 0;
    if (m.kind == LMVR_VISIONOS_MIRROR_PART_BLEND) {
        const float weights[5] = {m.colour_base, m.colour_alpha, m.opacity_base, m.opacity_alpha, m.opacity_luma};
        for (int i = 0; i < 5; ++i) {
            key.weights[i] = static_cast<int32_t>(std::lround(weights[i] * 16.f));
        }
    }
    key.wrapS = m.wrap_s;
    key.wrapT = m.wrap_t;
    return Fnv(&key, sizeof key);
}

struct Group {
    uint64_t key = 0;
    Part part{};
    std::vector<uint32_t> indices;
    bool blended = false;
};

struct Built {
    uint64_t serial = 0;
    uint32_t tag = 0;
    bool scene = false;
    std::vector<OutVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<Part> parts;
    float boundsMin[3]{}, boundsMax[3]{};
    float tanHalfX = 1, tanHalfY = 1, focus = 0, plane = 1;
};

struct OwnedTexture {
    uint64_t id = 0;
    std::vector<uint8_t> rgba;
    uint32_t width = 0, height = 0;
    bool mipmapped = false, cutout = false;
};

std::atomic_bool g_enabled{false};
std::atomic_bool g_reset{false};
std::atomic<float> g_unitsPerMetre{1.f};
// How far the window lets the scene come out in front of its glass (window widths; 0: unlimited),
// and the narrowest view it lays out as is (set_front_allowance).
std::atomic<float> g_frontAllowance{0.f}, g_minViewTangent{0.7f};

// A decal draw's vertices, and the normal they're lifted along (zero: towards the camera).
struct DecalRange {
    uint32_t first = 0, end = 0;
    float normal[3]{};
};

// --- The engine's thread.
bool g_recording = false;
uint32_t g_tag = 0;
float g_tanX = 1, g_tanY = 1, g_near = 0.01f;
Built g_building;
std::vector<Group> g_groups;
std::unordered_map<uint64_t, size_t> g_solidGroups;
int g_lastBlended = -1;
std::vector<std::pair<uint32_t, uint32_t>> g_backgroundRanges;
std::vector<DecalRange> g_decalRanges;
float g_farthest = 0, g_nearestBackground = INFINITY;
uint32_t g_drawIndex = 0;
std::unordered_map<uint64_t, unsigned> g_passes;
std::unordered_map<uint64_t, uint32_t> g_textureLastUse;  // the textures the app has or will get
uint32_t g_frameCounter = 0;
std::vector<OwnedTexture> g_newTextures;
std::vector<uint64_t> g_removedTextures;
std::vector<OutVertex> g_scratch;
std::vector<uint8_t> g_sane;
std::vector<uint32_t> g_triangles;
struct Stats {
    uint64_t frames = 0, draws = 0, vertices = 0, triangles = 0, background = 0;
    double seconds = 0;
} g_stats;

// --- Shared with the finishing worker and the consumer (g_mutex).
std::mutex g_mutex;
std::deque<Built> g_finished;  // oldest first
std::vector<Built> g_spare;
std::vector<OwnedTexture> g_pendingTextures;
std::vector<uint64_t> g_pendingRemoved;
uint64_t g_serial = 0;
// The consumer's.
Built g_acquired;
std::vector<OwnedTexture> g_acquiredTextures;
std::vector<uint64_t> g_acquiredRemoved;
std::vector<lmvr_visionos_mirror_texture> g_textureViews;

// Frame numbers are 24 bits: whether `a` is `b` or before it.
bool NotAfter(uint32_t a, uint32_t b) {
    return ((b - a) & 0xFFFFFFu) < 0x800000u;
}

// The distance from the camera (the origin) to the nearest point of triangle abc (Ericson,
// Real-Time Collision Detection 5.1.5). A sky plane's nearest point is inside its triangles, not at
// a corner: measured at the corners, a flat sky stayed nearer than the level's far end.
float NearestDistance(const float a[3], const float b[3], const float c[3]) {
    const auto sub = [](const float* x, const float* y) {
        return std::array<float, 3>{x[0] - y[0], x[1] - y[1], x[2] - y[2]};
    };
    const auto dot = [](const std::array<float, 3>& x, const std::array<float, 3>& y) {
        return x[0] * y[0] + x[1] * y[1] + x[2] * y[2];
    };
    const float origin[3] = {0, 0, 0};
    const auto ab = sub(b, a), ac = sub(c, a), ap = sub(origin, a);
    const auto length = [&](const std::array<float, 3>& p) { return std::sqrt(dot(p, p)); };
    const auto point = [&](float v, float w) {
        return std::array<float, 3>{a[0] + ab[0] * v + ac[0] * w, a[1] + ab[1] * v + ac[1] * w,
                                    a[2] + ab[2] * v + ac[2] * w};
    };
    const float d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0 && d2 <= 0) return length(point(0, 0));
    const auto bp = sub(origin, b);
    const float d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) return length(point(1, 0));
    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) return length(point(d1 / (d1 - d3), 0));
    const auto cp = sub(origin, c);
    const float d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0 && d5 <= d6) return length(point(0, 1));
    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) return length(point(0, d2 / (d2 - d6)));
    const float va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
        const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return length(point(1 - w, w));
    }
    const float denominator = va + vb + vc;
    if (std::fabs(denominator) < 1e-20f) return length(point(0, 0));
    return length(point(vb / denominator, vc / denominator));
}

bool Runaway(const float p[3], float limit) {
    return !(std::fabs(p[0]) < limit && std::fabs(p[1]) < limit && std::fabs(p[2]) < limit);
}

Group& GroupFor(const Material& material) {
    const uint64_t key = MaterialKey(material);
    Part part{};
    part.texture = material.texture;
    part.kind = material.kind;
    part.flags = material.flags;
    part.cutoff = material.cutoff;
    part.colour_base = material.colour_base;
    part.colour_alpha = material.colour_alpha;
    part.opacity_base = material.opacity_base;
    part.opacity_alpha = material.opacity_alpha;
    part.opacity_luma = material.opacity_luma;
    part.wrap_s = material.wrap_s;
    part.wrap_t = material.wrap_t;
    if (material.kind != LMVR_VISIONOS_MIRROR_PART_BLEND) {
        if (auto found = g_solidGroups.find(key); found != g_solidGroups.end()) {
            return g_groups[found->second];
        }
        g_solidGroups.emplace(key, g_groups.size());
        g_groups.push_back({key, part, {}, false});
        return g_groups.back();
    }
    // Blended draws keep the game's order: a run of one material is one group.
    if (g_lastBlended >= 0 && g_groups[static_cast<size_t>(g_lastBlended)].key == key) {
        return g_groups[static_cast<size_t>(g_lastBlended)];
    }
    g_lastBlended = static_cast<int>(g_groups.size());
    g_groups.push_back({key, part, {}, true});
    return g_groups.back();
}

// Appends a newer batch of texture changes to an older one still on its way: the newer wins. A
// texture removed and sent again (a level reloaded at the same address) must end up present, and
// one sent and then removed, absent; applied in the wrong order, the app deleted a texture the
// engine thought it still had, and its parts never drew again.
void MergeTextures(std::vector<OwnedTexture>& textures, std::vector<uint64_t>& removed,
                   std::vector<OwnedTexture>& newer, const std::vector<uint64_t>& newerRemoved) {
    for (uint64_t id : newerRemoved) {
        std::erase_if(textures, [id](const OwnedTexture& t) { return t.id == id; });
    }
    for (auto& texture : newer) {
        std::erase(removed, texture.id);
        std::erase_if(textures, [&](const OwnedTexture& t) { return t.id == texture.id; });
        textures.push_back(std::move(texture));
    }
    for (uint64_t id : newerRemoved) {
        if (std::find(removed.begin(), removed.end(), id) == removed.end()) {
            removed.push_back(id);
        }
    }
    newer.clear();
}

void ForgetTextures() {
    g_textureLastUse.clear();
    g_newTextures.clear();
    g_removedTextures.clear();
    std::lock_guard lock(g_mutex);
    g_pendingTextures.clear();
    g_pendingRemoved.clear();
}

void SweepTextures() {
    for (auto it = g_textureLastUse.begin(); it != g_textureLastUse.end();) {
        if (g_frameCounter - it->second > kTextureIdleFrames) {
            g_removedTextures.push_back(it->first);
            it = g_textureLastUse.erase(it);
        } else {
            ++it;
        }
    }
}

// --- Finishing a frame, on a worker of its own: the engine's thread doesn't wait for it.

struct Job {
    Built built;
    std::vector<Group> groups;
    std::vector<std::pair<uint32_t, uint32_t>> backgroundRanges;
    std::vector<DecalRange> decalRanges;
    float farthest = 0, nearestBackground = INFINITY;
    bool scene = false;
    uint32_t tag = 0;
    float focus = 0, tanHalfX = 1, tanHalfY = 1, unitsPerMetre = 1;
    std::vector<OwnedTexture> textures;
    std::vector<uint64_t> removed;
};

std::mutex g_jobMutex;
std::condition_variable g_jobReady;
Job g_job;
bool g_jobPending = false;
std::atomic<uint64_t> g_finishNanos{0}, g_finishedCount{0}, g_dropped{0};

// The easing of what's nearer than the glass into the window's allowance (Finish): a point at view
// depth d < end moves along its line of sight to depth plane - eased(k (plane - d)) / k, where
// k (plane - d) is how far in front of the glass it is, in window widths.
struct Easing {
    float plane, k, knee, rest, end;
    // How much a point at `depth` is scaled about the camera.
    float scale(float depth) const {
        if (!(depth > 1e-3f && depth < end)) {
            return 1.f;
        }
        const float front = k * (plane - depth);
        const float eased = knee + rest * (1.f - std::exp(-(front - knee) / rest));
        return (plane - eased / k) / depth;
    }
    // How far behind the eased curve the eased edge between depths a and b lies, through the edge's
    // middle (never in front: the easing bows towards the camera).
    float behind(float a, float b) const {
        const float sa = scale(a), sb = scale(b), middle = (a + b) * 0.5f;
        return middle * (2.f / (1.f / sa + 1.f / sb) - scale(middle));
    }
};

// Splits every group's triangles that reach in front of `plane` wherever an edge's far end is more
// than 40% deeper than its near end, edge midpoints shared (no cracks). Easing near vertices changes
// the triangles' shapes, and with them how the texture spreads: without this the ground at the
// picture's foot came out streaked. A decal's edge (`marks`, one per vertex) is also split while,
// eased, it lies behind the eased curve by more than half its lift: its chords aren't the ground's,
// and it went under the ground in patches (TPVR). Only decals: the ground's chords never lie in
// front of the curve, and splitting everything so grew a frame to 470,000 vertices.
void SubdivideNear(std::vector<OutVertex>& vertices, std::vector<Group>& groups, float plane,
                   std::vector<uint32_t>& marks, const Easing& easing, float liftCap) {
    const size_t budget = vertices.size() + 400000;
    std::unordered_map<uint64_t, uint32_t> midpoints;
    const auto depth = [&](uint32_t i) { return -vertices[i].position[2]; };
    const auto splits = [&](uint32_t i, uint32_t j) {
        const float a = depth(i), b = depth(j), nearer = std::min(a, b), farther = std::max(a, b);
        if (nearer >= plane || vertices.size() >= budget) {
            return false;
        }
        if (marks[i] != 0 && marks[j] != 0 && nearer > 1e-3f) {
            const float lift = std::min(liftCap, kDecalSlide * easing.scale(nearer) * nearer);
            if (easing.behind(a, b) > 0.5f * lift) {
                return true;
            }
        }
        if (farther <= nearer * 1.4f) {
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
        const OutVertex a = vertices[i], b = vertices[j];
        OutVertex m{};
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
        marks.push_back(marks[i] == marks[j] ? marks[i] : 0);
        const auto index = static_cast<uint32_t>(vertices.size() - 1);
        midpoints.emplace(key, index);
        return index;
    };
    std::vector<uint32_t> out;
    // Children keep their parent's winding.
    const auto emit = [&](auto& self, uint32_t v0, uint32_t v1, uint32_t v2, int level) -> void {
        const bool s01 = level < 24 && splits(v0, v1), s12 = level < 24 && splits(v1, v2),
                   s20 = level < 24 && splits(v2, v0);
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
        if (count == 1) {  // rotated so the split edge is (v0 v1)
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
        if (!s01) {  // two: rotated so they are (v0 v1) and (v1 v2)
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
        bool reaches = false;
        for (uint32_t i : group.indices) {
            if (depth(i) < plane) {
                reaches = true;
                break;
            }
        }
        if (!reaches) {
            continue;
        }
        out.clear();
        for (size_t t = 0; t + 2 < group.indices.size(); t += 3) {
            emit(emit, group.indices[t], group.indices[t + 1], group.indices[t + 2], 0);
        }
        group.indices.swap(out);
    }
}

void Finish(Job& job) {
    Built& out = job.built;
    const float upm = job.unitsPerMetre;
    out.tag = job.tag;
    out.scene = job.scene && !job.groups.empty();
    out.focus = std::isfinite(job.focus) && job.focus > 0 ? job.focus : kDefaultFocusMetres * upm;
    out.plane = std::clamp(out.focus * kGlassFraction, kGlassMinMetres * upm, kGlassMaxMetres * upm);
    out.tanHalfX = job.tanHalfX;
    out.tanHalfY = job.tanHalfY;
    out.indices.clear();
    out.parts.clear();
    if (!out.scene) {
        out.vertices.clear();
    } else {
        // The sky behind everything else: pushed along the camera's rays past the level's farthest
        // point. From the camera it's unchanged; from anywhere else it's still a sky.
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
        // What's nearer than the glass comes out in front of it, as the shape it is (squashed onto
        // the glass, TPVR's ground bent near the camera and z-fought), but only so far: visionOS cuts
        // a plain window's content off ~0.47 m in front of its face (the window's allowance), and a
        // viewer farther off than the camera sees, below the picture, towards ground under the
        // camera that was never kept, past (1 - 1 / kMargin) of the way to it: a strip of sky along
        // the window's foot. So past half the lesser of the two, nearer geometry eases along its
        // line of sight towards the allowance, never reaching it: the same picture from the camera,
        // the ground's shape kept to below the hero's feet. In window widths in front of the glass,
        // depth d is k (plane - d) (MirrorScene.place).
        const float k = 1.f / (2.f * out.plane * std::max(out.tanHalfX, g_minViewTangent.load()));
        const float window = g_frontAllowance.load();
        const float allowance = std::min(window, 0.9f * k * out.plane * (1.f - 1.f / kMargin));
        const float knee = allowance * 0.5f;  // C1 there: the easing starts at slope 1
        const Easing easing{out.plane, k, knee, allowance - knee, out.plane - knee / k};
        const bool ease = window > 0.f && easing.end > 0.f;
        std::vector<uint32_t> decal(out.vertices.size(), 0);  // a decal range's index + 1
        for (size_t r = 0; r < job.decalRanges.size(); ++r) {
            const DecalRange& range = job.decalRanges[r];
            std::fill(decal.begin() + std::min<size_t>(range.first, decal.size()),
                      decal.begin() + std::min<size_t>(range.end, decal.size()), static_cast<uint32_t>(r + 1));
        }
        const float liftCap = kDecalLiftMetres * upm, normalLift = kDecalNormalLiftMetres * upm;
        if (ease) {
            SubdivideNear(out.vertices, job.groups, easing.end, decal, easing, liftCap);
        }
        // Decals lifted once depths are final (lifted first, the easing shortened the lift).
        for (size_t v = 0; v < out.vertices.size(); ++v) {
            float* p = out.vertices[v].position;
            float scale = ease ? easing.scale(-p[2]) : 1.f;
            if (decal[v] != 0) {
                const float* normal = job.decalRanges[decal[v] - 1].normal;
                if (normal[0] != 0 || normal[1] != 0 || normal[2] != 0) {
                    for (int c = 0; c < 3; ++c) {
                        p[c] = p[c] * scale + normal[c] * normalLift;
                    }
                    continue;
                }
                const float distance = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]) * scale;
                scale *= 1.f - std::min(kDecalSlide, liftCap / std::max(distance, 1e-3f));
            }
            if (scale != 1.f) {
                for (int c = 0; c < 3; ++c) {
                    p[c] *= scale;
                }
            }
        }
        float low[3] = {INFINITY, INFINITY, INFINITY}, high[3] = {-INFINITY, -INFINITY, -INFINITY};
        const float limit = kRunawayMetres * upm * 100.f;  // the sky, pushed at most 100 times, is inside
        for (const OutVertex& vertex : out.vertices) {
            if (Runaway(vertex.position, limit)) {
                continue;
            }
            for (int c = 0; c < 3; ++c) {
                low[c] = std::min(low[c], vertex.position[c]);
                high[c] = std::max(high[c], vertex.position[c]);
            }
        }
        if (low[0] > high[0]) {
            std::fill(low, low + 3, 0.f);
            std::fill(high, high + 3, 0.f);
        }
        std::copy(low, low + 3, out.boundsMin);
        std::copy(high, high + 3, out.boundsMax);
        // Solid groups, then the blended ones in the game's order.
        for (int pass = 0; pass < 2; ++pass) {
            for (Group& group : job.groups) {
                if (group.blended != (pass == 1) || group.indices.empty()) {
                    continue;
                }
                group.part.first_index = static_cast<uint32_t>(out.indices.size());
                group.part.index_count = static_cast<uint32_t>(group.indices.size());
                out.indices.insert(out.indices.end(), group.indices.begin(), group.indices.end());
                out.parts.push_back(group.part);
            }
        }
    }
    std::lock_guard lock(g_mutex);
    out.serial = ++g_serial;
    g_finished.push_back(std::move(out));
    while (g_finished.size() > kKeptFrames) {
        g_spare.push_back(std::move(g_finished.front()));
        g_finished.pop_front();
    }
    // The job builds its next frame in a spent one's buffers.
    out = Built{};
    if (!g_spare.empty()) {
        out = std::move(g_spare.back());
        g_spare.pop_back();
    }
    MergeTextures(g_pendingTextures, g_pendingRemoved, job.textures, job.removed);
    job.removed.clear();
}

void WorkerMain() {
    pthread_setname_np("scene mirror");
    Job work;
    while (true) {
        {
            std::unique_lock lock(g_jobMutex);
            g_jobReady.wait(lock, [] { return g_jobPending; });
            std::swap(work.built, g_job.built);
            std::swap(work.groups, g_job.groups);
            std::swap(work.backgroundRanges, g_job.backgroundRanges);
            std::swap(work.decalRanges, g_job.decalRanges);
            std::swap(work.textures, g_job.textures);
            std::swap(work.removed, g_job.removed);
            work.farthest = g_job.farthest;
            work.nearestBackground = g_job.nearestBackground;
            work.scene = g_job.scene;
            work.tag = g_job.tag;
            work.focus = g_job.focus;
            work.tanHalfX = g_job.tanHalfX;
            work.tanHalfY = g_job.tanHalfY;
            work.unitsPerMetre = g_job.unitsPerMetre;
            g_jobPending = false;
        }
        const auto started = std::chrono::steady_clock::now();
        Finish(work);
        g_finishNanos += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count());
        ++g_finishedCount;
    }
}

// The recorded frame to the worker. If it's still busy with the last one, this frame's geometry is
// dropped (the window keeps the last); its textures still go.
void Publish(bool scene, float focus) {
    static std::once_flag started;
    std::call_once(started, [] { std::thread(WorkerMain).detach(); });
    {
        std::lock_guard lock(g_jobMutex);
        if (g_jobPending) {
            ++g_dropped;
        } else {
            std::swap(g_job.built, g_building);
            std::swap(g_job.groups, g_groups);
            std::swap(g_job.backgroundRanges, g_backgroundRanges);
            std::swap(g_job.decalRanges, g_decalRanges);
            g_job.farthest = g_farthest;
            g_job.nearestBackground = g_nearestBackground;
            g_job.scene = scene;
            g_job.tag = g_tag;
            g_job.focus = focus;
            g_job.tanHalfX = g_tanX;
            g_job.tanHalfY = g_tanY;
            g_job.unitsPerMetre = g_unitsPerMetre.load();
            g_jobPending = true;
        }
        MergeTextures(g_job.textures, g_job.removed, g_newTextures, g_removedTextures);
    }
    g_jobReady.notify_one();
    g_newTextures.clear();
    g_removedTextures.clear();
}

void ClearBuilding() {
    g_building.vertices.clear();
    g_groups.clear();
    g_solidGroups.clear();
    g_lastBlended = -1;
    g_backgroundRanges.clear();
    g_decalRanges.clear();
    g_farthest = 0;
    g_nearestBackground = INFINITY;
    g_drawIndex = 0;
    g_passes.clear();
}

}  // namespace

void set_enabled(bool enabled) {
    if (enabled && !g_enabled.load()) {
        g_reset = true;
    }
    g_enabled = enabled;
}

bool enabled() {
    return g_enabled.load(std::memory_order_relaxed);
}

void set_units_per_metre(float units) {
    if (std::isfinite(units) && units > 0) {
        g_unitsPerMetre = units;
    }
}

void set_front_allowance(float widths, float min_view_tangent) {
    g_frontAllowance = std::isfinite(widths) ? std::max(widths, 0.f) : 0.f;
    if (std::isfinite(min_view_tangent) && min_view_tangent > 0) {
        g_minViewTangent = min_view_tangent;
    }
}

uint64_t texture_id(const uint8_t* rgba, uint32_t width, uint32_t height) {
    // FNV-1a over 64-bit words (every pixel: a sample of them let two textures of one shape share
    // an id, TPVR), then the size.
    uint64_t hash = 1469598103934665603ull;
    const size_t bytes = rgba != nullptr ? static_cast<size_t>(width) * height * 4 : 0;
    size_t i = 0;
    for (; i + 8 <= bytes; i += 8) {
        uint64_t word;
        std::memcpy(&word, rgba + i, sizeof word);
        hash = (hash ^ word) * 1099511628211ull;
        hash ^= hash >> 29;
    }
    for (; i < bytes; ++i) {
        hash = (hash ^ rgba[i]) * 1099511628211ull;
    }
    hash = (hash ^ (static_cast<uint64_t>(width) << 32 | height)) * 1099511628211ull;
    return hash != 0 ? hash : 1;
}

void begin_frame(uint32_t game_frame) {
    g_tag = game_frame & 0xFFFFFFu;
    g_recording = g_enabled.load(std::memory_order_relaxed);
    if (g_reset.exchange(false)) {
        ForgetTextures();
    }
    ClearBuilding();
    g_tanX = g_tanY = 1;
    g_near = 0.01f * g_unitsPerMetre.load();
}

void set_projection(float tan_half_x, float tan_half_y, float near_plane) {
    if (tan_half_x > 0 && tan_half_y > 0) {
        g_tanX = tan_half_x;
        g_tanY = tan_half_y;
    }
    if (near_plane > 0) {
        g_near = near_plane;
    }
}

void add_draw(const Draw& draw) {
    if (!g_recording || draw.vertices == nullptr || draw.vertex_count == 0) {
        return;
    }
    const auto started = std::chrono::steady_clock::now();
    const float upm = g_unitsPerMetre.load(std::memory_order_relaxed);
    const float runawayLimit = kRunawayMetres * upm;
    const bool background = (draw.material.flags & LMVR_VISIONOS_MIRROR_BACKGROUND) != 0;
    const uint32_t count = draw.vertex_count;
    auto& built = g_scratch;
    built.resize(count);
    g_sane.resize(count);
    float nearest = INFINITY, farthest = 0;
    for (uint32_t v = 0; v < count; ++v) {
        const Vertex& in = draw.vertices[v];
        OutVertex& out = built[v];
        std::copy(in.position, in.position + 3, out.position);
        std::copy(in.uv, in.uv + 2, out.uv);
        for (int c = 0; c < 4; ++c) {
            out.mul[c] = half(in.mul[c]);
            out.add[c] = half(in.add[c]);
        }
        g_sane[v] = !Runaway(out.position, runawayLimit);
        if (g_sane[v]) {
            const float distance = std::sqrt(in.position[0] * in.position[0] + in.position[1] * in.position[1] +
                                             in.position[2] * in.position[2]);
            nearest = std::min(nearest, distance);
            farthest = std::max(farthest, distance);
        }
    }

    // Clipped as the GPU clips: at the near plane and the view's sides (with kMargin). The sky only
    // at the near plane: it goes on past the sides, for a viewer leaning in. Unclipped, the sky dome
    // and the ground reach behind the camera, between the window and the viewer, and cover it.
    const float kx = kMargin * g_tanX, ky = kMargin * g_tanY, nearClip = g_near;
    const auto inside = [&](int plane, uint32_t i) {
        const float* p = built[i].position;
        const float d = -p[2];
        switch (plane) {
        case 0:
            return d - nearClip;
        case 1:
            return kx * d + p[0];
        case 2:
            return kx * d - p[0];
        case 3:
            return ky * d + p[1];
        default:
            return ky * d - p[1];
        }
    };
    const auto between = [&](uint32_t p, uint32_t q, float fp, float fq) {
        const OutVertex a = built[p], b = built[q];
        const float t = fp / (fp - fq);
        OutVertex out{};
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
    g_triangles.clear();
    const auto triangle = [&](uint32_t a, uint32_t b, uint32_t c) {
        if (a == b || b == c || a == c || a >= count || b >= count || c >= count || !g_sane[a] || !g_sane[b] ||
            !g_sane[c]) {
            return;
        }
        uint32_t polygon[9] = {a, b, c};
        int corners = 3;
        const int planes = background ? 1 : 5;
        for (int plane = 0; plane < planes && corners >= 3; ++plane) {
            float values[9];
            int outside = 0;
            for (int i = 0; i < corners; ++i) {
                values[i] = inside(plane, polygon[i]);
                outside += values[i] < 0.f;
            }
            if (outside == 0) {
                continue;
            }
            if (outside == corners) {
                return;
            }
            uint32_t next[9];
            int nextCount = 0;
            for (int i = 0; i < corners && nextCount < 8; ++i) {
                const int j = (i + 1) % corners;
                if (values[i] >= 0.f) {
                    next[nextCount++] = polygon[i];
                }
                if ((values[i] >= 0.f) != (values[j] >= 0.f)) {
                    next[nextCount++] = between(polygon[i], polygon[j], values[i], values[j]);
                }
            }
            std::copy(next, next + nextCount, polygon);
            corners = nextCount;
        }
        for (int i = 1; i + 1 < corners; ++i) {
            if (draw.clockwise_front) {
                g_triangles.insert(g_triangles.end(), {polygon[0], polygon[i + 1], polygon[i]});
            } else {
                g_triangles.insert(g_triangles.end(), {polygon[0], polygon[i], polygon[i + 1]});
            }
        }
    };
    if (draw.indices != nullptr) {
        for (uint32_t i = 0; i + 2 < draw.index_count; i += 3) {
            triangle(draw.indices[i], draw.indices[i + 1], draw.indices[i + 2]);
        }
    } else {
        for (uint32_t i = 0; i + 2 < count; i += 3) {
            triangle(i, i + 1, i + 2);
        }
    }
    if (g_triangles.empty()) {
        return;
    }

    // Coplanar draws (SHAR's rules): every draw a hair nearer than the ones before, a mesh drawn
    // again where it was just drawn more. Decals are lifted later, once depths are final (Finish).
    float nearer = 1.f - kDrawNudge * static_cast<float>(g_drawIndex++);
    if (draw.pass_key != 0) {
        const unsigned repeat = g_passes[draw.pass_key]++;
        if (repeat > 0) {
            nearer *= 1.f - kRepeatNudge * static_cast<float>(std::min(repeat, 8u));
        }
    }
    if (!background) {
        for (OutVertex& vertex : built) {
            for (float& c : vertex.position) {
                c *= nearer;
            }
        }
    }

    const auto base = static_cast<uint32_t>(g_building.vertices.size());
    g_building.vertices.insert(g_building.vertices.end(), built.begin(), built.end());
    if (draw.decal && !background) {
        DecalRange range{base, static_cast<uint32_t>(g_building.vertices.size()), {}};
        const float* n = draw.decal_normal;
        const float length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (std::isfinite(length) && length > 1e-6f) {
            for (int c = 0; c < 3; ++c) {
                range.normal[c] = n[c] / length;
            }
        }
        g_decalRanges.push_back(range);
    }
    auto& indices = GroupFor(draw.material).indices;
    indices.reserve(indices.size() + g_triangles.size());
    for (uint32_t index : g_triangles) {
        indices.push_back(base + index);
    }
    if (background) {
        g_backgroundRanges.emplace_back(base, static_cast<uint32_t>(g_building.vertices.size()));
        for (size_t t = 0; t + 2 < g_triangles.size(); t += 3) {
            nearest = std::min(nearest, NearestDistance(built[g_triangles[t]].position, built[g_triangles[t + 1]].position,
                                                        built[g_triangles[t + 2]].position));
        }
        g_nearestBackground = std::min(g_nearestBackground, nearest);
        ++g_stats.background;
    } else {
        g_farthest = std::max(g_farthest, farthest);
    }
    if (draw.material.texture != 0) {
        if (auto found = g_textureLastUse.find(draw.material.texture); found != g_textureLastUse.end()) {
            found->second = g_frameCounter;
        }
    }
    ++g_stats.draws;
    g_stats.vertices += count;
    g_stats.triangles += g_triangles.size() / 3;
    g_stats.seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
}

void end_frame(bool scene, float focus) {
    if (!g_recording) {
        return;
    }
    g_recording = false;
    ++g_frameCounter;
    if (g_frameCounter % 300 == 0) {
        SweepTextures();
    }
    Publish(scene && !g_groups.empty(), focus);
    if (++g_stats.frames == 600) {
        const double f = static_cast<double>(g_stats.frames);
        const uint64_t finished = std::max<uint64_t>(g_finishedCount.exchange(0), 1);
        Log("per frame %.0f draws (%.0f background), %.0f vertices, %.0f triangles; adding %.2f ms (engine thread), "
            "finishing %.2f ms (worker); %llu frames dropped; %zu textures known",
            g_stats.draws / f, g_stats.background / f, g_stats.vertices / f, g_stats.triangles / f,
            g_stats.seconds / f * 1000.0, static_cast<double>(g_finishNanos.exchange(0)) / finished / 1e6,
            static_cast<unsigned long long>(g_dropped.exchange(0)), g_textureLastUse.size());
        g_stats = {};
    }
}

void discard_frame() {
    g_recording = false;
    ClearBuilding();
}

bool knows_texture(uint64_t id) {
    return g_textureLastUse.count(id) != 0;
}

void add_texture(uint64_t id, const uint8_t* rgba, uint32_t width, uint32_t height, bool mipmapped, bool cutout) {
    if (!g_enabled.load(std::memory_order_relaxed) || id == 0 || rgba == nullptr || width == 0 || height == 0) {
        return;
    }
    g_textureLastUse[id] = g_frameCounter;
    std::erase(g_removedTextures, id);  // removed and sent again in one frame: present
    OwnedTexture texture{id, std::vector<uint8_t>(rgba, rgba + static_cast<size_t>(width) * height * 4), width, height,
                         mipmapped, cutout};
    const auto waiting =
        std::find_if(g_newTextures.begin(), g_newTextures.end(), [id](const OwnedTexture& t) { return t.id == id; });
    if (waiting != g_newTextures.end()) {
        *waiting = std::move(texture);  // newer pixels before the old were sent
    } else {
        g_newTextures.push_back(std::move(texture));
    }
}

void remove_texture(uint64_t id) {
    if (g_textureLastUse.erase(id) != 0) {
        g_removedTextures.push_back(id);
    }
    std::erase_if(g_newTextures, [id](const OwnedTexture& t) { return t.id == id; });
}

bool acquire(lmvr_visionos_mirror_frame* frame, uint32_t up_to) {
    if (frame == nullptr) {
        return false;
    }
    {
        std::lock_guard lock(g_mutex);
        // The newest finished frame no later than up_to (or, if they're all later, the oldest).
        size_t take = g_finished.size();
        for (size_t i = 0; i < g_finished.size(); ++i) {
            if (NotAfter(g_finished[i].tag, up_to & 0xFFFFFFu)) {
                take = i;
            }
        }
        if (take == g_finished.size() && g_finished.size() == kKeptFrames) {
            take = 0;
        }
        if (take < g_finished.size()) {
            std::swap(g_acquired, g_finished[take]);
            for (size_t i = 0; i <= take; ++i) {
                g_spare.push_back(std::move(g_finished.front()));
                g_finished.pop_front();
            }
        }
        // Textures go with the first frame taken and then each call: before it, the app has no frame
        // to take them with and drops them.
        g_acquiredTextures.clear();
        g_acquiredRemoved.clear();
        if (g_acquired.serial != 0) {
            g_acquiredTextures.swap(g_pendingTextures);
            g_acquiredRemoved.swap(g_pendingRemoved);
        }
    }
    g_textureViews.clear();
    for (const auto& texture : g_acquiredTextures) {
        g_textureViews.push_back(
            {texture.id, texture.rgba.data(), texture.width, texture.height, texture.mipmapped, texture.cutout});
    }
    const Built& built = g_acquired;
    *frame = {};
    frame->serial = built.serial;
    frame->game_frame = built.tag;
    frame->scene = built.scene;
    frame->vertices = built.vertices.data();
    frame->vertex_count = static_cast<uint32_t>(built.vertices.size());
    frame->indices = built.indices.data();
    frame->index_count = static_cast<uint32_t>(built.indices.size());
    frame->parts = built.parts.data();
    frame->part_count = static_cast<uint32_t>(built.parts.size());
    std::copy(built.boundsMin, built.boundsMin + 3, frame->bounds_min);
    std::copy(built.boundsMax, built.boundsMax + 3, frame->bounds_max);
    frame->tan_half_x = built.tanHalfX;
    frame->tan_half_y = built.tanHalfY;
    frame->focus = built.focus;
    frame->plane = built.plane;
    frame->textures = g_textureViews.data();
    frame->texture_count = static_cast<uint32_t>(g_textureViews.size());
    frame->removed = g_acquiredRemoved.data();
    frame->removed_count = static_cast<uint32_t>(g_acquiredRemoved.size());
    return built.serial != 0;
}

}  // namespace scene_mirror

// The bridge's, here so that both hosts have it.
extern "C" void lmvr_visionos_set_mirror_front_allowance(float widths, float min_view_tangent) {
    scene_mirror::set_front_allowance(widths, min_view_tangent);
}
