// Enhanced lighting, port side (enhanced_lighting.h, docs/RENDERING.md "Enhanced lighting").
#include "recomp/enhanced_lighting.h"
#include "recomp/ghost_lights.h"
#include "recomp/world_lights.h"

#include "port/settings.h"
#include "recomp/hle.h"
#include "recomp/interp.h"
#include "recomp/runtime.h"
#include "recomp/version.h"

#include <aurora/enhanced.h>
#include <dolphin/gx.h>
#include <dolphin/gx/GXAurora.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string_view>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

namespace recomp {
u32 copyDispCount();  // hle_gx.cpp
namespace gx {
void recheckDrawState();  // hle_gx.cpp: re-evaluate the draw class before the next raw draw
}

namespace {

std::atomic<bool> sActive{false};
bool sInScene = false, sInCaster = false;  // guest thread
u8 sClass = 0xFF;                          // last draw class sent
int sPerfPhase = -1;       // LM_ENH_PERF_AB phase (host thread)
bool sPerfNoMap = false;
bool sGhostPass = false;
bool sCharPass = false;
u8 sMaterial = 0xFF;  // last material hint sent

// Extra lights submitted for the coming main view (guest thread only). Providers (ghost lights,
// world lights, test lights) append; beginScene culls what cannot reach the view and keeps the
// MaxExtraLights strongest.
constexpr uint32_t kMaxCandidates = 512;
std::array<aurora::gfx::ExtraLight, kMaxCandidates> sPending{};
uint32_t sPendingCount = 0;

// Camera (lbl_803A3820): the player's view eye/target in world space.
constexpr u32 kCamViewEye = 0x00, kCamViewTarget = 0x0C;

// LM_TEST_LIGHTS=<count>[,<radius>[,<intensity>]]: colored point lights orbiting Luigi (US/PAL),
// to check extra lights before a real provider exists.
struct TestLights {
    int count = 0;
    float radius = 450.0f, intensity = 1.5f;
    float speed = 1.0f, dist = 220.0f, height = 110.0f;  // orbit speed (0 = fixed), distance, height above Luigi
};
const TestLights sTest = [] {
    TestLights t;
    if (const char* v = std::getenv("LM_TEST_LIGHTS")) {
        std::sscanf(v, "%d,%f,%f,%f,%f,%f", &t.count, &t.radius, &t.intensity, &t.speed, &t.dist, &t.height);
        t.count = std::clamp(t.count, 0, int(aurora::gfx::MaxExtraLights));
    }
    return t;
}();

void hueToRgb(float h, float out[3]) {
    const float h6 = h * 6.0f;
    out[0] = std::clamp(std::fabs(h6 - 3.0f) - 1.0f, 0.0f, 1.0f);
    out[1] = std::clamp(2.0f - std::fabs(h6 - 2.0f), 0.0f, 1.0f);
    out[2] = std::clamp(2.0f - std::fabs(h6 - 4.0f), 0.0f, 1.0f);
}

void addTestLights() {
    if (sTest.count <= 0) return;
    const u32 actor = rd32(VA(0x803D48A0));
    if (actor < 0x80000000u || actor >= 0x81800000u) return;
    const float luigi[3] = {rdf32(actor + 0x44), rdf32(actor + 0x48), rdf32(actor + 0x4C)};
    static u32 sFrame = 0;
    const float t = float(sFrame++) / 30.0f;
    for (int i = 0; i < sTest.count && sPendingCount < kMaxCandidates; i++) {
        const float a = t * 0.9f * sTest.speed + 6.2831853f * float(i) / float(sTest.count);
        const float world[3] = {luigi[0] + sTest.dist * std::cos(a),
                                luigi[1] + sTest.height + 40.0f * sTest.speed * std::sin(t * 1.7f + float(i)),
                                luigi[2] + sTest.dist * std::sin(a)};
        aurora::gfx::ExtraLight l{};
        if (!enh::worldToView(world, l.pos)) return;
        hueToRgb(float(i) / float(sTest.count), l.color);
        l.radius = sTest.radius;
        l.intensity = sTest.intensity;
        l.flags = aurora::gfx::ExtraLightTranslucent | aurora::gfx::ExtraLightSpecular |
                  (i < 16 ? aurora::gfx::ExtraLightCastShadows : 0u);
        l.id = 0x7E570000u + u32(i);
        sPending[sPendingCount++] = l;
    }
}

}  // namespace

namespace enh {
bool worldToView(const float world[3], float view[3]);
// Realistic flashlight level in effect (setting, or the LM_REALISTIC_FLASHLIGHT=<0..3> override).
int realisticLevel() {
    static const int sReal = [] {
        const char* v = std::getenv("LM_REALISTIC_FLASHLIGHT");
        return v ? std::atoi(v) : -1;
    }();
    return std::clamp(sReal >= 0 ? sReal : port::settings().enhancedLighting.realisticFlashlight, 0, 3);
}
bool realisticOn() { return realisticLevel() != 0; }
}

namespace {
// Realistic flashlight (Aurora 0196): three pseudo-lights carrying the view positions of a world
// point and of points 256 units along world +X / +Y, so the beam's dust and haze stay put in the
// world while the camera moves (frame interpolation blends them like lights, by id). The point is
// the camera target snapped to 1024 units (Aurora's noise repeats every 1024), so it stays close
// to the camera: under rotation a far point would move too far for interp's light pairing.
// Takes the last slots when the set is full.
uint32_t addBeamAnchor(aurora::gfx::ExtraLight* l, uint32_t count) {
    if (!enh::realisticOn()) return count;
    const u32 cam = VA(0x803A3820);
    float o[3];
    for (int k = 0; k < 3; k++) o[k] = std::round(rdf32(cam + kCamViewTarget + 4 * k) / 1024.0f) * 1024.0f;
    if (!std::isfinite(o[0] + o[1] + o[2])) return count;
    count = std::min(count, aurora::gfx::MaxExtraLights - 3);
    for (int k = 0; k < 3; k++) {
        float w[3] = {o[0], o[1], o[2]};
        if (k == 1) w[0] += 256.0f;
        if (k == 2) w[1] += 256.0f;
        aurora::gfx::ExtraLight a{};
        if (!enh::worldToView(w, a.pos)) return count;
        a.flags = AURORA_EXTRA_LIGHT_ANCHOR;
        a.id = AURORA_BEAM_ANCHOR_ID + u32(k);
        l[count + k] = a;
    }
    return count + 3;
}
}  // namespace

void submitExtraLights(const aurora::gfx::ExtraLight* lights, uint32_t count) {
    count = std::min<uint32_t>(count, kMaxCandidates - sPendingCount);
    std::memcpy(sPending.data() + sPendingCount, lights, count * sizeof(aurora::gfx::ExtraLight));
    sPendingCount += count;
}

namespace {
// Lights whose sphere cannot touch the view frustum are dropped (generous side margins: the
// widescreen/FOV options widen the view); the rest are ranked by intensity x reach over distance
// and the strongest MaxExtraLights kept, in that order (Aurora gives shadow slots to the first
// flagged lights).
// Stable selection (docs/RENDERING.md "Enhanced lighting fixes"): the raw score follows the
// providers' flame flicker and ghost pulses every frame, so two candelabras of about the same
// strength swapped places from frame to frame. Ranking and shadow slots therefore use a score
// smoothed per light id, lights already chosen keep their place unless another is clearly
// stronger (hysteresis), and a shadow slot only goes to a new light once the light losing it has
// faded out, so Aurora never sees more shadow-casting lights than it has slots for (it gave the
// slots to the first flagged lights in order: a fading light could take a wanted light's slot,
// which then blinked as the order changed).
struct LightTrack {
    float score = 0.0f;   // smoothed rank score
    // 0187: peak-held score (decays to half in ~1.5 s) for the shadow-slot decisions: a pulsing or
    // fading ghost and a flickering flame keep their place through their dim phases (a ghost light
    // dropping to a fifth of its score every few seconds traded its slot with a candelabra over and
    // over: both shadows faded out and in again, in the dining room every ~1 s).
    float peak = 0.0f;
    u32 gained = 0;       // tick the light got its shadow slot
    float w = 0.0f;       // shadow weight 0..1 (fades in/out)
    bool selected = false;  // in the sent set last frame
    bool holder = false;    // owns a shadow slot
    u32 seen = 0;
};
std::unordered_map<u32, LightTrack> sTrack;
u32 sTrackTick = 0;
// Test aid LM_LIGHT_SELECT_LOG=1: shadow-slot changes (gained / lost) with scores.
const int sSelectLog = [] {
    const char* v = std::getenv("LM_LIGHT_SELECT_LOG");
    return v ? std::max(std::atoi(v), 1) : 0;
}();

uint32_t selectLights(aurora::gfx::ExtraLight* l, uint32_t n) {
    struct Ranked {
        float score;
        uint32_t i;
    };
    static std::vector<Ranked> rank;
    rank.clear();
    sTrackTick++;
    constexpr float kSmooth = 0.2f;       // score smoothing per game frame (~5 frames)
    constexpr float kKeepBonus = 1.25f;   // a light already in the set ranks 25% higher
    constexpr float kSwapRatio = 1.35f;   // a shadow slot changes hands only for a 35% stronger light
    for (uint32_t i = 0; i < n; i++) {
        const auto& x = l[i];
        if (!(x.intensity > 0.0f) || !(x.radius > 0.0f)) continue;
        const float z = -x.pos[2];  // distance in front of the camera
        if (z < -x.radius) continue;  // entirely behind the camera
        const float side = std::max(z, 0.0f) * 2.5f + x.radius;  // ~136 degrees wide, 2.5:1
        if (std::fabs(x.pos[0]) > side || std::fabs(x.pos[1]) > side) continue;
        const float d = std::sqrt(x.pos[0] * x.pos[0] + x.pos[1] * x.pos[1] + x.pos[2] * x.pos[2]);
        float s = x.intensity * x.radius / (1.0f + std::max(d - x.radius, 0.0f) / 800.0f);
        if (x.id != 0) {
            auto& t = sTrack[x.id];
            t.score = t.seen + 1 == sTrackTick ? t.score + (s - t.score) * kSmooth : s;
            t.peak = t.seen + 1 == sTrackTick ? std::max(t.score, t.peak * 0.985f) : t.score;
            t.seen = sTrackTick;
            s = t.score * (t.selected ? kKeepBonus : 1.0f);
        }
        rank.push_back({s, i});
    }
    std::stable_sort(rank.begin(), rank.end(), [](const Ranked& a, const Ranked& b) { return a.score > b.score; });
    static std::array<aurora::gfx::ExtraLight, aurora::gfx::MaxExtraLights> out;
    uint32_t m = 0;
    for (auto& [id, t] : sTrack) t.selected = false;
    for (const auto& r : rank) {
        if (m == aurora::gfx::MaxExtraLights) break;
        out[m] = l[r.i];
        if (out[m].id != 0) sTrack[out[m].id].selected = true;
        m++;
    }
    // Shadow slots. Candidates: flagged lights in the sent set.
    constexpr u32 kShadow = aurora::gfx::ExtraLightCastShadows;
    const u32 cap = enh::shadowLightCap();
    static std::vector<uint32_t> cand;
    cand.clear();
    for (uint32_t i = 0; i < m; i++)
        if ((out[i].flags & kShadow) && out[i].id != 0) cand.push_back(i);
    // Lights that left the set (or lost their flag) stop casting at once: they are not drawn. 0187:
    // a holder that vanished (a ghost hiding for a moment, a light culled for a few frames) keeps
    // its slot reserved for ~0.7 s, so another light does not take it and give it back right away
    // (each hand-over fades two shadows out and in).
    constexpr u32 kReserve = 20;
    u32 reserved = 0;
    for (auto& [id, t] : sTrack) {
        if (t.seen != sTrackTick) {
            t.w = 0.0f;
            if (t.holder && sTrackTick - t.seen <= kReserve) reserved++;
            else t.holder = false;
        } else if (!t.selected) {
            t.holder = false, t.w = 0.0f;
        }
    }
    for (uint32_t i = 0; i < m; i++) {
        if (out[i].id == 0 || (out[i].flags & kShadow)) continue;
        auto& t = sTrack[out[i].id];
        t.holder = false, t.w = 0.0f;
    }
    auto trackOf = [&](uint32_t i) -> LightTrack& { return sTrack[out[i].id]; };
    // Too many holders (the cap was lowered): drop the weakest at once.
    {
        u32 holders = 0;
        for (uint32_t i : cand) holders += trackOf(i).holder;
        while (holders > cap) {
            uint32_t weakest = UINT32_MAX;
            for (uint32_t i : cand)
                if (trackOf(i).holder && (weakest == UINT32_MAX || trackOf(i).peak < trackOf(weakest).peak)) weakest = i;
            trackOf(weakest).holder = false;
            trackOf(weakest).w = 0.0f;
            holders--;
        }
    }
    // Hysteresis: the strongest non-holder replaces the weakest holder only when clearly stronger
    // (peak-held scores) and the holder has had its slot for at least ~1.5 s.
    {
        uint32_t best = UINT32_MAX, weakest = UINT32_MAX;
        u32 holders = 0;
        for (uint32_t i : cand) {
            const auto& t = trackOf(i);
            if (t.holder) {
                holders++;
                if (weakest == UINT32_MAX || t.peak < trackOf(weakest).peak) weakest = i;
            } else if (best == UINT32_MAX || t.peak > trackOf(best).peak) {
                best = i;
            }
        }
        constexpr u32 kMinHold = 45;  // game frames
        if (best != UINT32_MAX && weakest != UINT32_MAX && holders >= cap && trackOf(best).peak > trackOf(weakest).peak * kSwapRatio &&
            sTrackTick - trackOf(weakest).gained >= kMinHold) {
            trackOf(weakest).holder = false;  // fades out; the newcomer gets the slot once it is free
            if (sSelectLog)
                log("[lights] f%u shadow slot: %08X (%.0f) yields to %08X (%.0f)", copyDispCount(), out[weakest].id, trackOf(weakest).peak,
                    out[best].id, trackOf(best).peak);
        }
    }
    // Free slots (holders + lights still fading out) go to the strongest waiting candidates.
    {
        u32 used = reserved;
        for (uint32_t i : cand) used += trackOf(i).holder || trackOf(i).w > 0.0f;
        static std::vector<uint32_t> waiting;
        waiting.clear();
        for (uint32_t i : cand)
            if (!trackOf(i).holder && trackOf(i).w <= 0.0f) waiting.push_back(i);
        std::stable_sort(waiting.begin(), waiting.end(), [&](uint32_t a, uint32_t b) { return trackOf(a).peak > trackOf(b).peak; });
        for (uint32_t i : waiting) {
            if (used >= cap) break;
            trackOf(i).holder = true;
            trackOf(i).gained = sTrackTick;
            used++;
            if (sSelectLog) log("[lights] f%u shadow slot: %08X (%.0f) gains a slot", copyDispCount(), out[i].id, trackOf(i).peak);
        }
    }
    // Weights fade toward the holder state (~5 game frames); the result is multiplied with the
    // provider's own weight (flags bits 8..15, 0 = full).
    for (uint32_t i = 0; i < m; i++) {
        auto& x = out[i];
        if (!(x.flags & kShadow) || x.id == 0) {
            x.flags &= ~(kShadow | 0xFF00u);
            continue;
        }
        auto& t = trackOf(i);
        t.w += ((t.holder ? 1.0f : 0.0f) - t.w) * 0.25f;
        if (!t.holder && t.w < 0.03f) t.w = 0.0f;
        if (t.holder && t.w > 0.97f) t.w = 1.0f;
        const u32 own = (x.flags >> 8) & 0xFFu;
        const float ow = own ? float(own) / 255.0f : 1.0f;
        const float fw = t.w * ow;
        x.flags &= ~(kShadow | 0xFF00u);
        if (fw > 0.0f) x.flags |= kShadow | (std::clamp(u32(fw * 255.0f + 0.5f), 1u, 255u) << 8);
    }
    if (sSelectLog >= 2) {  // every frame: the shadow-casting lights with their weights
        char buf[512];
        int len = std::snprintf(buf, sizeof(buf), "[lights] f%u n %u shadows:", copyDispCount(), m);
        for (uint32_t i = 0; i < m && len < 440; i++)
            if (out[i].flags & kShadow)
                len += std::snprintf(buf + len, sizeof(buf) - len, " %08X:%u", out[i].id, (out[i].flags >> 8) & 0xFFu);
        log("%s", buf);
    }
    if ((sTrackTick & 63) == 0) {
        for (auto it = sTrack.begin(); it != sTrack.end();) {
            if (sTrackTick - it->second.seen > 60) it = sTrack.erase(it);
            else ++it;
        }
    }
    std::memcpy(l, out.data(), m * sizeof(aurora::gfx::ExtraLight));
    return m;
}
}  // namespace

namespace enh {

// LM_ENHANCED_AB=<n>: A/B test aid. Game frames alternate in blocks of n between the enhanced
// rendering and the plain one (draws classed "other", no extra lights, no scene end: exactly
// the plain renderer), so LM_FRAME_DUMP frames of one run compare both. With n = 1 odd dump
// frames are enhanced, even ones plain.
const u32 sAb = [] {
    const char* v = std::getenv("LM_ENHANCED_AB");
    return v ? u32(std::max(std::atoi(v), 0)) : 0u;
}();
bool frameOn() {
    if (sAb == 0) return true;
    return ((copyDispCount() + 1) / sAb) % 2 == 1;  // copyDispCount() + 1: the dump number of this frame
}

bool active() { return sActive.load(std::memory_order_relaxed) && frameOn(); }

uint32_t shadowLightCap() {
    const auto& l = port::settings().enhancedLighting;
    if (!l.extraLightShadows) return 0;
    static constexpr u32 kXlLights[6] = {0, 2, 4, 8, 12, 16};
    u32 n = kXlLights[std::clamp(l.extraShadowLights, 0, 5)];
    if (const char* v = std::getenv("LM_XL_SHADOWS")) {
        int mode = -1, lights = -1;
        std::sscanf(v, "%d,%d", &mode, &lights);
        if (lights >= 0) n = u32(lights);
        if (mode == 0) n = std::min(n, 4u);
    } else if (l.extraShadowMode == 0) {
        n = std::min(n, 4u);
    }
    return std::min(n, 16u);
}
bool masterOn() { return sActive.load(std::memory_order_relaxed); }
bool configuredOn() {
    static const int sForce = [] {
        const char* v = std::getenv("LM_ENHANCED");
        return v ? std::atoi(v) : -1;
    }();
    return sForce >= 0 ? sForce != 0 : port::settings().enhancedLighting.enabled;
}
int perfPhase() { return sPerfPhase; }

void applySettings() {
    auto& l = port::settings().enhancedLighting;
    // Presets fill in the quality fields (Custom keeps whatever was set).
    switch (l.preset) {
    case port::EnhancedPreset::Low:
        l.softShadowQuality = 0, l.volumetricQuality = 0, l.extraShadowQuality = 0;
        l.extraShadowLights = 1, l.extraShadowMapSize = 0;
        break;
    case port::EnhancedPreset::Medium:
        l.softShadowQuality = 1, l.volumetricQuality = 1, l.extraShadowQuality = 1;
        l.extraShadowLights = 1, l.extraShadowMapSize = 1;
        break;
    case port::EnhancedPreset::High:
        l.softShadowQuality = 2, l.volumetricQuality = 1, l.extraShadowQuality = 1;
        l.extraShadowLights = 2, l.extraShadowMapSize = 1;
        break;
    case port::EnhancedPreset::Ultra:
        l.softShadowQuality = 3, l.volumetricQuality = 2, l.extraShadowQuality = 2;
        l.extraShadowLights = 3, l.extraShadowMapSize = 2;
        break;
    default:
        break;
    }
    static const int sForce = [] {
        const char* v = std::getenv("LM_ENHANCED");  // test override: 0 off, 1 on
        return v ? std::atoi(v) : -1;
    }();
    AuroraEnhancedConfig c{};
    c.enabled = sForce >= 0 ? sForce != 0 : l.enabled;
    c.perPixelLighting = l.perPixelLighting;
    c.flashlightQuality = l.flashlightQuality;
    c.flashlightSoftness = l.flashlightSoftness;
    c.flashlightSpecular = l.flashlightSpecular;
    c.softShadows = l.softShadows;
    c.softShadowQuality = u32(std::clamp(l.softShadowQuality, 0, 3));
    c.softShadowRadius = l.softShadowRadius;
    c.softShadowPenumbra = l.softShadowPenumbra;
    c.shadowDarkness = l.shadowDarkness;
    c.shadowAmbientKeep = l.shadowAmbientKeep;
    c.extraLights = l.extraLights;
    c.extraLightIntensity = l.extraLightIntensity;
    c.extraLightSaturation = l.extraLightSaturation;
    c.extraShadows = l.extraLightShadows;
    c.extraShadowQuality = u32(std::clamp(l.extraShadowQuality, 0, 2));
    static const int sXlMode = [] {  // test override LM_XL_SHADOWS=<mode 0/1>[,<lights>[,<face size>]]
        const char* v = std::getenv("LM_XL_SHADOWS");
        return v ? std::atoi(v) : -1;
    }();
    static const std::array<int, 2> sXlOver = [] {
        std::array<int, 2> o{-1, -1};
        if (const char* v = std::getenv("LM_XL_SHADOWS")) std::sscanf(v, "%*d,%d,%d", &o[0], &o[1]);
        return o;
    }();
    static constexpr u32 kXlLights[6] = {0, 2, 4, 8, 12, 16};
    static constexpr u32 kXlSize[4] = {256, 512, 1024, 2048};
    c.extraShadowMode = u32(sXlMode >= 0 ? sXlMode : std::clamp(l.extraShadowMode, 0, 1));
    c.extraShadowLights = sXlOver[0] >= 0 ? u32(sXlOver[0]) : kXlLights[std::clamp(l.extraShadowLights, 0, 5)];
    c.extraShadowMapSize = sXlOver[1] > 0 ? u32(sXlOver[1]) : kXlSize[std::clamp(l.extraShadowMapSize, 0, 3)];
    c.extraShadowSoftness = l.extraShadowSoftness;
    c.volumetric = l.volumetricBeam;
    c.volumetricIntensity = l.volumetricIntensity;
    c.volumetricQuality = u32(std::clamp(l.volumetricQuality, 0, 2));
    c.realisticFlashlight = u32(realisticLevel());  // test override LM_REALISTIC_FLASHLIGHT=<0..3>
    // Test aid LM_BEAM_AB=1: realistic flashlight only on odd dump frames (consecutive
    // LM_FRAME_DUMP frames compare the previous beam with the realistic one).
    static const bool sBeamAb = std::getenv("LM_BEAM_AB") != nullptr;
    if (sBeamAb && ((copyDispCount() + 1) & 1) == 0) c.realisticFlashlight = 0;
    c.beamDensity = l.beamDensity;
    c.beamDust = l.beamDust;
    c.bloom = l.bloom;
    c.bloomIntensity = l.bloomIntensity;
    c.bloomThreshold = l.bloomThreshold;
    c.bloomRadius = l.bloomRadius;
    c.bloomFlashlightSuppress = l.bloomFlashlightSuppress;
    c.bloomScene = l.bloomScene;
    c.bloomEmissive = l.bloomEmissive;
    // Ambient occlusion (horizon-based since Aurora 0224). Test override LM_AO=0/1.
    static const int sAoTest = std::getenv("LM_AO") ? std::atoi(std::getenv("LM_AO")) : -1;
    c.ssao = sAoTest >= 0 ? sAoTest != 0 : l.ssao;
    c.ssaoIntensity = l.ssaoIntensity;
    c.ssaoRadius = l.ssaoRadius;
    // Test aid LM_AO_AB=1: ambient occlusion only on odd dump frames (intensity 0 on even ones, so
    // no pipeline changes), consecutive LM_FRAME_DUMP frames compare with / without.
    static const bool sAoAb = std::getenv("LM_AO_AB") != nullptr;
    if (sAoAb && ((copyDispCount() + 1) & 1) == 0) c.ssaoIntensity = 0.0f;
    // LM_ENH_PERF_AB=<seconds>,<part>: performance A/B within one run. Every <seconds> the part
    // (all, ppl, flq, soft, xl, xls, vol, bloom, ssao, map) is switched off/on; LM_FPS_LOG lines
    // carry the phase (perfPhase()).
    static const struct PerfAb {
        float seconds = 0.0f;
        char part[16] = {};
    } sPerf = [] {
        PerfAb p;
        if (const char* v = std::getenv("LM_ENH_PERF_AB")) std::sscanf(v, "%f,%15s", &p.seconds, p.part);
        return p;
    }();
    if (sPerf.seconds > 0.0f) {
        const auto t = std::chrono::duration<float>(std::chrono::steady_clock::now().time_since_epoch()).count();
        sPerfPhase = int(t / sPerf.seconds) % 2;
        if (sPerfPhase == 1) {
            const std::string_view part = sPerf.part;
            if (part == "all") c.enabled = false;
            if (part == "ppl") c.perPixelLighting = false;
            if (part == "flq") c.flashlightQuality = false;
            if (part == "soft") c.softShadows = false;
            if (part == "xl") c.extraLights = false;
            if (part == "xls") c.extraShadows = false;
            if (part == "vol") c.volumetric = false;
            if (part == "beam") c.realisticFlashlight = 0;
            if (part == "bloom") c.bloom = false;
            if (part == "ssao") c.ssao = false;
            if (part == "map") sPerfNoMap = true;
        } else {
            sPerfNoMap = false;
        }
    }
    aurora_enhanced_set_config(&c);
    // LM_XL_SHADOW_LOG=1: shadow-map statistics every 120 host frames (lights, caster draws,
    // casters skipped while their pipeline compiles).
    static const int sXlLog = [] {
        const char* v = std::getenv("LM_XL_SHADOW_LOG");
        return v ? std::max(std::atoi(v), 1) : 0;
    }();
    static u32 sXlTick = 0;
    if (sXlLog && ++sXlTick % (sXlLog >= 2 ? 1 : 120) == 0) {  // 2: every host frame
        u32 lights = 0, casters = 0, skipped = 0;
        aurora_enhanced_shadow_stats(&lights, &casters, &skipped);
        log("[xlshadow] f%u lights %u casters %u skipped %u", copyDispCount(), lights, casters, skipped);
    }
    sActive.store(c.enabled, std::memory_order_relaxed);
}

// Draw class (docs/RENDERING.md "Enhanced lighting"): set at hook boundaries, never between the
// game's own GX commands: WORLD inside the main 3D view (fn_8000BB98), GHOST inside its ghost
// pass, OTHER in the shadow-map caster pass and everywhere else (HUD/2D, GBH, copies). Aurora
// additionally ignores non-perspective draws for its re-rendered passes.
void emitMaterial();
void emitClass() {
    if (!masterOn()) return;
    u8 cls = AURORA_DRAW_CLASS_OTHER;
    if (sInScene && !sInCaster && active()) cls = sGhostPass ? AURORA_DRAW_CLASS_GHOST : AURORA_DRAW_CLASS_WORLD;
    if (cls == sClass) return;
    GXSetDrawClassAurora(cls);
    sClass = cls;
}

void setGhostPass(bool on) {
    if (sGhostPass == on) return;
    sGhostPass = on;
    emitClass();
    emitMaterial();
}
bool ghostPass() { return sGhostPass; }

u32 sSceneCount = 0;
void emitMaterial() {
    if (!masterOn()) return;
    // Test aid LM_CHAR_MATERIAL=0 (off: characters lit like the world, as before) / 2 (only on
    // even frames: consecutive frame dumps compare both).
    static const int sMode = [] {
        const char* v = std::getenv("LM_CHAR_MATERIAL");
        return v ? std::atoi(v) : 1;
    }();
    const bool allowed = sMode == 1 || (sMode == 2 && (sSceneCount & 1) == 0);
    const u8 m = allowed && sInScene && sCharPass && !sGhostPass && !sInCaster ? AURORA_MATERIAL_CHARACTER : AURORA_MATERIAL_WORLD;
    if (m == sMaterial) return;
    GXSetMaterialHintAurora(m);
    sMaterial = m;
}
void setCharacterPass(bool on) {
    if (sCharPass == on) return;
    sCharPass = on;
    emitMaterial();
}
bool characterPass() { return sCharPass; }

void setCasterPass(bool on) {
    if (sInCaster == on) return;
    sInCaster = on;
    emitClass();
    emitMaterial();
}

void beginScene() {
    addTestLights();
    const uint32_t count = selectLights(sPending.data(), sPendingCount);
    // Test aid LM_XL_SHADOWS_AB=1: colored-light shadows only on even frames (consecutive frame
    // dumps compare with / without in the same scene).
    static const bool sXlAb = std::getenv("LM_XL_SHADOWS_AB") != nullptr;
    if (sXlAb && (sSceneCount & 1))
        for (uint32_t i = 0; i < count; i++) sPending[i].flags &= ~(aurora::gfx::ExtraLightCastShadows | 0xFF00u);
    sPendingCount = 0;  // a frame in which nobody submits has no extra lights
    sClass = 0xFF;      // every frame (and interpolation tape) sets its class again
    sMaterial = 0xFF;
    sSceneCount++;
    sInScene = true;
    emitClass();
    emitMaterial();
    if (!active()) return;
    // Test aid LM_BEAM_FLARE_AT=<dump frame>[,<period>]: the realistic flashlight flares as for a
    // stunned ghost at that frame (and every <period> frames after it).
    static const std::array<u32, 2> sFlareAt = [] {
        std::array<u32, 2> v{0, 0};
        if (const char* e = std::getenv("LM_BEAM_FLARE_AT")) std::sscanf(e, "%u,%u", &v[0], &v[1]);
        return v;
    }();
    if (sFlareAt[0] != 0) {
        const u32 f = copyDispCount() + 1;
        if (f == sFlareAt[0] || (sFlareAt[1] != 0 && f > sFlareAt[0] && (f - sFlareAt[0]) % sFlareAt[1] == 0))
            aurora_enhanced_beam_flare(0.85f);
    }
    static const bool sBeamLog = std::getenv("LM_BEAM_LOG") != nullptr;
    if (sBeamLog) log("[beam] main view of dump frame %u", copyDispCount() + 1);
    const uint32_t sent = addBeamAnchor(sPending.data(), count);
    const u32 at = interp::mark();
    aurora::gfx::set_extra_lights(sPending.data(), sent);
    // (all of them: the realistic flashlight's anchors too, so they follow the blended camera)
    interp::noteExtraLights(at, sent);
}

void endScene() {
    sInScene = false;
    emitClass();
    emitMaterial();
    if (active()) GXSceneEndAurora();
}

uint32_t shadowMapWidth() {
    if (!active()) return 0;
    static const int sForce = [] {
        const char* v = std::getenv("LM_SHADOW_MAP");  // test override: 0 render res, 1 2048, 2 4096
        return v ? std::atoi(v) : -1;
    }();
    if (sPerfNoMap) return 0;
    const int size = sForce >= 0 ? sForce : port::settings().enhancedLighting.shadowMapSize;
    return size == 1 ? 2048 : size == 2 ? 4096 : 0;
}

void onGhostInBeam(uint32_t actor) {
    if (!active() || !realisticOn()) return;
    static std::unordered_map<u32, u32> sLast;  // actor -> last frame it was in the beam
    const u32 now = copyDispCount();
    auto [it, fresh] = sLast.try_emplace(actor, now);
    const bool rising = fresh || now - it->second > 45;
    it->second = now;
    if (sLast.size() > 256) sLast.clear();
    if (!rising) return;
    aurora_enhanced_beam_flare(0.85f);
    static const bool sLog = std::getenv("LM_BEAM_LOG") != nullptr;  // test aid: dump frame of each flare
    if (sLog) log("[beam] flare: ghost %08X in the beam, dump frame %u", actor, now + 1);
}

bool worldToView(const float world[3], float view[3]) {
    const u32 cam = VA(0x803A3820);
    const float eye[3] = {rdf32(cam + kCamViewEye), rdf32(cam + kCamViewEye + 4), rdf32(cam + kCamViewEye + 8)};
    const float at[3] = {rdf32(cam + kCamViewTarget), rdf32(cam + kCamViewTarget + 4), rdf32(cam + kCamViewTarget + 8)};
    // MTXLookAt with up (0, 1, 0): z = normalize(eye - target), x = normalize(up x z), y = z x x.
    float z[3] = {eye[0] - at[0], eye[1] - at[1], eye[2] - at[2]};
    const float zl = std::sqrt(z[0] * z[0] + z[1] * z[1] + z[2] * z[2]);
    if (!(zl > 1e-3f)) return false;
    for (float& v : z) v /= zl;
    float x[3] = {z[2], 0.0f, -z[0]};  // (0,1,0) x z
    const float xl = std::sqrt(x[0] * x[0] + x[2] * x[2]);
    if (!(xl > 1e-5f)) return false;
    x[0] /= xl, x[2] /= xl;
    const float y[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]};
    const float d[3] = {world[0] - eye[0], world[1] - eye[1], world[2] - eye[2]};
    view[0] = x[0] * d[0] + x[1] * d[1] + x[2] * d[2];
    view[1] = y[0] * d[0] + y[1] * d[1] + y[2] * d[2];
    view[2] = z[0] * d[0] + z[1] * d[1] + z[2] * d[2];
    return true;
}

namespace {
// Room table lbl_804D8170: 0x30 per room, int min xyz at +0x8, max xyz at +0x14.
bool roomBounds(int room, float lo[3], float hi[3]) {
    const u32 rooms = rd32(VA(0x804D8170));
    if (room < 0 || room >= 80 || rooms < 0x80000000u || rooms >= 0x81800000u) return false;
    const u32 b = rooms + u32(room) * 0x30;
    for (int k = 0; k < 3; k++) {
        lo[k] = float(s32(rd32(b + 8 + 4 * k)));
        hi[k] = float(s32(rd32(b + 0x14 + 4 * k)));
        if (!(hi[k] > lo[k]) || hi[k] - lo[k] > 20000.0f) return false;
    }
    return true;
}
}  // namespace

int roomAt(const float p[3]) {
    for (int r = 0; r < 80; r++) {
        float lo[3], hi[3];
        if (!roomBounds(r, lo, hi)) continue;
        bool in = true;
        for (int k = 0; k < 3 && in; k++) in = p[k] >= lo[k] - 1.0f && p[k] <= hi[k] + 1.0f;
        if (in) return r;
    }
    return -1;
}

void limitToRoom(aurora::gfx::ExtraLight& l, int room, const float world[3]) {
    // Test aid LM_ROOM_BOX=0: lights not limited to their room; =2: boxes shrunk to a quarter of
    // the room around its centre (makes the cut visible, to check the box placement).
    static const int sMode = [] {
        const char* v = std::getenv("LM_ROOM_BOX");
        return v && *v ? std::atoi(v) : 1;
    }();
    float lo[3], hi[3];
    if (sMode == 0 || !roomBounds(room, lo, hi)) return;
    if (sMode == 2)
        for (int k = 0; k < 3; k += 2) {
            const float c = (lo[k] + hi[k]) * 0.5f, h = (hi[k] - lo[k]) * 0.25f;
            lo[k] = c - h, hi[k] = c + h;
        }
    // The GX view matrix (lbl_803A3820 +0x18C, +0x198 on PAL): the light positions' view space.
    float m[12];
    for (int i = 0; i < 12; i++) m[i] = rdf32(VA(0x803A3820) + VOFF(0x18C, 0x198) + 4 * i);
    // The room's floor and ceiling are its box's y bounds; a little slack above and below so the
    // floor and ceiling surfaces themselves are inside.
    lo[1] -= 20.0f, hi[1] += 20.0f;
    float c[3];
    for (int k = 0; k < 3; k++) c[k] = (lo[k] + hi[k]) * 0.5f - world[k];
    for (int r = 0; r < 3; r++) l.box[r] = m[r * 4] * c[0] + m[r * 4 + 1] * c[1] + m[r * 4 + 2] * c[2];
    for (int k = 0; k < 3; k++) {
        const float h = (hi[k] - lo[k]) * 0.5f;
        for (int r = 0; r < 3; r++) l.box[3 + 3 * k + r] = m[r * 4 + k] * h;
    }
    l.flags |= aurora::gfx::ExtraLightRoomBox;
}

}  // namespace enh

// Main 3D view of the game scene (rooms, actors, Luigi, effects, the ghost layer), called by the
// scene draw fn_8000BCE8 before the screen copy and the HUD. r3 != 0 selects a reduced pass
// (rooms and some actors only), which is left alone. The European function is identical apart
// from relocations.
namespace tags {
void init();  // tags.cpp
}
HLE(fn_8000BB98) {
    worldlights::HiddenPlayerParticles hiddenPlayerParticles;
    const bool main = c.r[3] == 0;
    // Frame interpolation: everything drawn in this view is under the view's tag scope (its camera's
    // identity; tags.cpp, docs/INTERPOLATION.md "Tagged interpolation").
    tags::init();
    interp::TagScope viewTag(interp::TagCat::View, VA(0x8000BB98), 0, c.r[3]);
    if (main) ghostlights::onMainView(c);  // ghost_lights.cpp: this frame's ghost lights
    if (main) worldlights::onMainView(c);  // world_lights.cpp: candles, fires, lamps, sprays
    if (main) enh::beginScene();
    callOriginal(c, "fn_8000BB98");
    if (main) enh::endScene();
}

}  // namespace recomp
