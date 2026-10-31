// Apple Vision Pro "On your table": the volume's world-locked set (volume.h).
//
// The set never follows the game camera: it is placed at a cut (a new map or room, a camera cut
// in gameplay, the volume resized) and then holds still, gravity-aligned, turned only in quarter
// turns (to the game camera's heading at the cut, so the stick still feels like the game's). The
// mirror's draws arrive in the game camera's view space each frame, so the placement handed on is
// volumeFromView = set (volume metres from world units) x worldFromView.
//
// Sizing (volume.cpp's fit): the volume always holds everything the flat game camera shows.
//   - Whole room: the room's box (room table lbl_804D8170) fits in the volume, as long as Luigi
//     stays at least kLuigiMinMetres tall at that scale.
//   - Big rooms: at the smallest scale (Luigi kLuigiMinMetres tall) the set shows the part of the
//     room the game camera sees (its frustum's rays to the room's box), Luigi and the active
//     ghosts, and slides there slowly (a dead zone, eased) as they move: never turned or rescaled
//     mid-room. Where even that part is bigger than the volume, the scale goes lower for that room.
//   - A room taller than the volume at that scale: the set's height follows the same region
//     (vertical slide, same dead zone) instead of standing on the volume's floor.
// LM_VOLUME_AUDIT=1 logs every fit ("[volume] fit") and every frame the frustum's region leaves
// the volume ("[volume] outside"), and at each map load the fit of every room in its table
// ("[volume] room").

#include "recomp/volume.h"

#include "port/settings.h"

#include "recomp/hle.h"
#include "recomp/runtime.h"
#include "recomp/skip.h"
#include "recomp/version.h"

#include <dolphin/gx.h>
#include <dolphin/gx/GXAurora.h>
#include <aurora/mirror.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>

#ifdef LM_VR_VISIONOS
#include "lmvr_game_hooks.h"  // the port's visionOS host
#endif

namespace recomp {
u32 copyDispCount();  // hle_gx.cpp
}

namespace recomp::volume {
namespace {

using Clock = std::chrono::steady_clock;

// USA addresses (VA() maps them on the European game).
#define kVolActorSlots VA(0x803D48A0)  // lbl_803D48A0: live characters (slot 0 = Luigi)
#define kVolActorCount VA(0x804D8750)
#define kVolTypeTable VA(0x8033B590)   // character types, 0x1C each; flags +0x08 (8: ghost)
constexpr u32 kActType = 0x3C, kActPos = 0x44, kActRoom = 0xB4;
constexpr u32 kTypeCount = 0x29C8 / 0x1C;

// Luigi's height in game units (his model's crown above his feet, measured in the Foyer from the
// audit's log of him in the game camera's view), and the least he may come out on the table to stay
// readable.
constexpr float kLuigiUnits = 125.0f;
constexpr float kLuigiMinMetres = 0.045f;
constexpr float kMinScale = kLuigiMinMetres / kLuigiUnits;
// A small room isn't blown up past Luigi this tall.
constexpr float kMaxScale = 0.11f / kLuigiUnits;
// The share of the volume the set may use (the rest is a margin, so nothing grazes its walls).
constexpr float kUseX = 0.94f, kUseY = 0.90f, kUseZ = 0.94f;
// The volume's floor margin under the room's floor (metres).
constexpr float kFloorMargin = 0.02f;
// The volume the fits are designed for (the app's default size, metres). A room that wouldn't fit
// whole in it with Luigi at least kLuigiMinMetres tall is excessively large (the audit's 8 of 74:
// long halls, the big outdoor rooms) and follows the camera; every other room is fitted whole at
// any size the viewer gives the volume, however small (Trevor: rooms always fit, unless they're
// excessively large). Before, a room fitted whole only while Luigi stayed 4.5 cm tall: in a smaller
// volume the training room followed the camera instead, a wall just cut off.
constexpr float kDesignVolume[3] = {1.2f, 0.6f, 1.0f};
// The check after a whole fit: the room's box, turned and placed, kept this far inside the
// volume's faces (metres).
constexpr float kBoundsMargin = 0.01f;
// The slide: dead zone (a share of the volume's half-size the region may move off-centre before
// the set follows), and the easing's time constant.
constexpr float kDeadZone = 0.25f;
constexpr float kSlideSeconds = 1.2f;

bool guestPtr(u32 p) { return p >= 0x80000000u && p < 0x81800000u; }

struct V3 {
    float x = 0, y = 0, z = 0;
};

std::atomic<float> sP00{0}, sP11{0};
std::atomic<bool> sWantPerspective{false};  // the main view's first perspective is its scene's

struct Region {
    V3 lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
    bool valid() const { return hi.x >= lo.x; }
    void add(V3 p) {
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    }
    V3 centre() const { return {(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f}; }
};

bool roomBox(int room, V3& lo, V3& hi) {
    const u32 rooms = rd32(VA(0x804D8170));
    if (room < 0 || room >= 80 || !guestPtr(rooms)) return false;
    const u32 b = rooms + u32(room) * 0x30;
    float l[3], h[3];
    for (int k = 0; k < 3; k++) {
        l[k] = float(s32(rd32(b + 8 + 4 * k)));
        h[k] = float(s32(rd32(b + 0x14 + 4 * k)));
        if (!(h[k] > l[k]) || h[k] - l[k] > 20000.0f) return false;
    }
    lo = {l[0], l[1], l[2]};
    hi = {h[0], h[1], h[2]};
    return true;
}

// The room's box for the fit: its own, joined with the rooms that are one open space with it, which
// the game draws with it through an open passage (portal_fix.cpp) and the set must hold too. The
// Foyer (map 2): the hall (room 2) and its upper landing (room 31, behind the hall's box: its back
// wall came out 1 cm past the volume's back face, cut off, holes onto the room the volume stands in).
constexpr int kOpenSpaces[][2] = {{2, 31}};  // map 2

// The room that stands for an open space (its first): going from one of its rooms to another isn't
// a new room for the set (no refit, no fade on the Foyer's stairs).
int spaceOf(unsigned map, int room) {
    if (map != 2) return room;
    for (const auto& pair : kOpenSpaces)
        if (room == pair[1]) return pair[0];
    return room;
}

bool fitBox(unsigned map, int room, V3& lo, V3& hi) {
    if (!roomBox(room, lo, hi)) return false;
    if (map != 2) return true;
    for (const auto& pair : kOpenSpaces) {
        const int other = room == pair[0] ? pair[1] : room == pair[1] ? pair[0] : -1;
        V3 olo, ohi;
        if (other < 0 || !roomBox(other, olo, ohi)) continue;
        lo = {std::min(lo.x, olo.x), std::min(lo.y, olo.y), std::min(lo.z, olo.z)};
        hi = {std::max(hi.x, ohi.x), std::max(hi.y, ohi.y), std::max(hi.z, ohi.z)};
    }
    return true;
}

// Rooms the game draws as a stage, seen through a wall in front of them: E. Gadd's lab (map 1,
// room 0) is a cave behind a near-black wall with a cave-shaped opening, standing on the room box's
// front face (world z 72), which the game's camera looks through. On the table that wall stood in
// front of the set: a black card that hid the lab from every side but the front, where it looked
// like a flat picture. Such a room is always fitted whole, turned as the wall faces (whatever the
// camera's heading: a side close-up or a smaller volume must not bring the wall back), and stands
// with that face `cut` world units past the volume's front face, where the volume cuts it off (as
// it cut the Foyer's back wall, 1 cm past its back face).
struct FrontWall {
    unsigned map;
    int room;
    float cut;    // world units of the room behind its front face that go with the wall
    int quarter;  // the set's turn that puts the wall at the front (its face towards the viewer)
};
constexpr FrontWall kFrontWalls[] = {{1, 0, 8.0f, 0}};

const FrontWall* frontWall(unsigned map, int room) {
    for (const auto& w : kFrontWalls)
        if (w.map == map && w.room == room) return &w;
    return nullptr;
}

// The set's turn in quarters (0..3): the game camera's heading, snapped.
int quarterOf(float heading) {
    const float q = std::round(heading / 1.5707963f);
    return ((int(q) % 4) + 4) % 4;
}

// World extents seen from the volume: a quarter turn swaps x and z.
void turned(int quarter, const V3& size, float& across, float& deep) {
    across = (quarter & 1) ? size.z : size.x;
    deep = (quarter & 1) ? size.x : size.z;
}

// The scale that fits a room's box whole in a volume, at a turn.
float wholeScale(const V3& size, int quarter, const float vol[3]) {
    float across, deep;
    turned(quarter, size, across, deep);
    return std::min({vol[0] * kUseX / across, vol[2] * kUseZ / deep, (vol[1] * kUseY - kFloorMargin) / size.y});
}

// Whether a room is fitted whole in a volume at a turn: it fits there with Luigi at least
// kLuigiMinMetres tall, or it isn't excessively large (it fits so in the design volume at either
// turn: a property of the room, not of the camera's heading). Otherwise it follows the camera.
bool fitsWhole(const V3& size, int quarter, const float vol[3], float& scale) {
    const float whole = wholeScale(size, quarter, vol);
    scale = std::min(whole, kMaxScale);
    const bool byDesign = std::max(wholeScale(size, 0, kDesignVolume), wholeScale(size, 1, kDesignVolume)) >= kMinScale;
    return whole > 0 && (whole >= kMinScale || byDesign);
}

struct State {
    bool valid = false;
    unsigned map = ~0u;
    int room = -2, pendingRoom = -2, pendingFrames = 0;
    int quarter = 0;
    float scale = 0;
    V3 anchor, target;  // the world point at the volume's reference (x, z: centre; y: see floorMode)
    bool floorMode = true;  // the room's floor stands on the volume's floor; else centred on y
    int klass = 0;          // 0 whole room, 1 following a region, 2 following, scale lowered
    bool roomValid = false;
    V3 roomLo, roomHi;
    V3 lastEye;
    float lastHeading = 0;
    bool haveCamera = false;
    std::array<float, 3> volume{};
    u32 serial = 0;      // fits
    u32 fadeSerial = 0;  // fits the app fades in (not a resize's)
    Clock::time_point last{};
    unsigned auditedMap = ~0u;
    u32 outsideFrames = 0;
    // The latest view (for the stick, read before the next frame's draw).
    float forward[2] = {0, -1};
} sState;
std::mutex sStickMutex;
float sStickCamForward[2] = {0, -1};
int sStickQuarter = 0, sStickViewpoint = 0;
bool sStickValid = false;
std::atomic<u32> sStickSeen{0};  // the audit: the frame a turned stick was last applied

const bool sAudit = [] {
    const char* v = std::getenv("LM_VOLUME_AUDIT");
    return v && *v && *v != '0';
}();

// The volume's size (metres) and the viewer's side, from the app.
std::array<float, 3> volumeSize(int& viewpoint) {
    float size[3] = {1.2f, 0.6f, 1.0f};
    viewpoint = 0;
#ifdef LM_VR_VISIONOS
    lmvr_window_volume(size, &viewpoint);
#endif
    for (float& s : size)
        if (!(s > 0.05f) || !std::isfinite(s)) s = 0.5f;
    return {size[0], size[1], size[2]};
}

// Rotation by the quarter: volume = R * world (about y).
void rotate(int quarter, float wx, float wz, float& vx, float& vz) {
    const float t = 1.5707963f * float(quarter);
    const float c = std::cos(t), s = std::sin(t);
    vx = c * wx + s * wz;
    vz = -s * wx + c * wz;
}

// What the game camera sees of the room: its rays (a grid over the picture) from the eye to the
// room's box, both where they enter and leave it, with Luigi and the active ghosts.
Region frustumRegion(const float view[12], V3 eye, const State& st, V3 luigi, bool luigiValid, unsigned map, int room) {
    Region r;
    const float p00 = sP00.load(std::memory_order_relaxed), p11 = sP11.load(std::memory_order_relaxed);
    const float tx = p00 != 0 ? 1.0f / std::fabs(p00) : 0.75f, ty = p11 != 0 ? 1.0f / std::fabs(p11) : 0.42f;
    if (st.roomValid) {
        for (int j = 0; j <= 4; j++)
            for (int i = 0; i <= 6; i++) {
                const float u = -1.0f + 2.0f * float(i) / 6.0f, v = -1.0f + 2.0f * float(j) / 4.0f;
                const float dv[3] = {u * tx, v * ty, -1.0f};
                // World direction: the view's rotation transposed.
                float d[3];
                for (int k = 0; k < 3; k++) d[k] = view[0 * 4 + k] * dv[0] + view[1 * 4 + k] * dv[1] + view[2 * 4 + k] * dv[2];
                const float o[3] = {eye.x, eye.y, eye.z};
                const float lo[3] = {st.roomLo.x, st.roomLo.y, st.roomLo.z}, hi[3] = {st.roomHi.x, st.roomHi.y, st.roomHi.z};
                float t0 = 0.0f, t1 = 1e9f;
                bool hit = true;
                for (int k = 0; k < 3 && hit; k++) {
                    if (std::fabs(d[k]) < 1e-6f) {
                        hit = o[k] >= lo[k] && o[k] <= hi[k];
                        continue;
                    }
                    float a = (lo[k] - o[k]) / d[k], b = (hi[k] - o[k]) / d[k];
                    if (a > b) std::swap(a, b);
                    t0 = std::max(t0, a);
                    t1 = std::min(t1, b);
                    hit = t0 <= t1;
                }
                if (!hit) continue;
                r.add({o[0] + d[0] * t0, o[1] + d[1] * t0, o[2] + d[2] * t0});
                r.add({o[0] + d[0] * t1, o[1] + d[1] * t1, o[2] + d[2] * t1});
            }
    }
    const auto addFigure = [&](V3 p, float radius, float height) {
        r.add({p.x - radius, p.y, p.z - radius});
        r.add({p.x + radius, p.y + height, p.z + radius});
    };
    if (luigiValid) addFigure(luigi, 60.0f, kLuigiUnits + 20.0f);
    const u32 count = std::min<u32>(rd32(kVolActorCount), 128);
    for (u32 i = 1; i < count; i++) {
        const u32 a = rd32(kVolActorSlots + 4 * i);
        if (!guestPtr(a)) continue;
        const u32 type = rd32(a + kActType);
        if (type >= kTypeCount || !(rd16(kVolTypeTable + type * 0x1C + 0x08) & 8)) continue;
        if (room >= 0 && spaceOf(map, int(rd32(a + kActRoom) >> 24)) != room) continue;
        const V3 p{rdf32(a + kActPos), rdf32(a + kActPos + 4), rdf32(a + kActPos + 8)};
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) continue;
        if (st.roomValid && (p.x < st.roomLo.x - 200 || p.x > st.roomHi.x + 200 || p.z < st.roomLo.z - 200 || p.z > st.roomHi.z + 200))
            continue;
        addFigure(p, 80.0f, 160.0f);
    }
    if (!r.valid() && luigiValid) addFigure(luigi, 600.0f, 300.0f);
    return r;
}

// Where the region lands in the volume, as offsets of its corners from the volume's centre.
void regionInVolume(const State& st, const Region& r, const std::array<float, 3>& vol, float lo[3], float hi[3]) {
    lo[0] = lo[1] = lo[2] = 1e9f;
    hi[0] = hi[1] = hi[2] = -1e9f;
    for (int c = 0; c < 8; c++) {
        const V3 p{(c & 1) ? r.hi.x : r.lo.x, (c & 2) ? r.hi.y : r.lo.y, (c & 4) ? r.hi.z : r.lo.z};
        float vx, vz;
        rotate(st.quarter, p.x - st.anchor.x, p.z - st.anchor.z, vx, vz);
        const float base = st.floorMode ? -vol[1] * 0.5f + kFloorMargin : 0.0f;
        const float v[3] = {vx * st.scale, base + (p.y - st.anchor.y) * st.scale, vz * st.scale};
        for (int k = 0; k < 3; k++) lo[k] = std::min(lo[k], v[k]), hi[k] = std::max(hi[k], v[k]);
    }
}

// The fit for this room and turn.
void fit(State& st, const Region& region, const std::array<float, 3>& vol, bool alwaysWhole = false) {
    const float useX = vol[0] * kUseX, useY = vol[1] * kUseY, useZ = vol[2] * kUseZ;
    st.klass = 0;
    st.floorMode = true;
    if (st.roomValid) {
        const V3 size{st.roomHi.x - st.roomLo.x, st.roomHi.y - st.roomLo.y, st.roomHi.z - st.roomLo.z};
        float scale = 0;
        const bool whole = fitsWhole(size, st.quarter, vol.data(), scale);
        if (scale > 0 && (whole || alwaysWhole)) {
            st.scale = scale;
            st.anchor = {(st.roomLo.x + st.roomHi.x) * 0.5f, st.roomLo.y, (st.roomLo.z + st.roomHi.z) * 0.5f};
            st.target = st.anchor;
            return;
        }
    }
    // Following: the smallest scale, lower where the region itself is bigger than the volume.
    st.klass = 1;
    st.scale = kMinScale;
    if (region.valid()) {
        const V3 size{region.hi.x - region.lo.x, region.hi.y - region.lo.y, region.hi.z - region.lo.z};
        float across, deep;
        turned(st.quarter, size, across, deep);
        const float needed = std::min(useX / std::max(across, 1.0f), useZ / std::max(deep, 1.0f));
        if (needed < st.scale) {
            st.scale = needed;
            st.klass = 2;
        }
    }
    const float roomHeight = st.roomValid ? st.roomHi.y - st.roomLo.y : 0.0f;
    st.floorMode = st.roomValid && roomHeight * st.scale <= useY - kFloorMargin;
    const V3 c = region.valid() ? region.centre() : st.anchor;
    st.anchor = {c.x, st.floorMode ? st.roomLo.y : c.y, c.z};
    st.target = st.anchor;
}

// The check after a whole fit, on every fit (a new room, a resize): the room's box as the set places
// it (turned by the quarter, its floor on the volume's floor margin) against the volume's faces less
// kBoundsMargin; whatever pokes out scales the set down about its anchor until it fits. (Before the
// front wall's move: that room's wall is cut off past the front face on purpose, afterwards.)
void keepInside(State& st, const std::array<float, 3>& vol) {
    if (st.klass != 0 || !st.roomValid || st.scale <= 0) return;
    const float t = 1.5707963f * float(st.quarter);
    const float c = std::cos(t), s = std::sin(t), k = st.scale;
    const float base = st.floorMode ? -vol[1] * 0.5f + kFloorMargin : 0.0f;
    float most[3] = {0, 0, 0};  // the furthest a corner goes from the anchor's place, per axis (metres)
    for (int i = 0; i < 8; i++) {
        const float wx = (i & 1) ? st.roomHi.x : st.roomLo.x, wy = (i & 2) ? st.roomHi.y : st.roomLo.y;
        const float wz = (i & 4) ? st.roomHi.z : st.roomLo.z;
        const float vx = k * (c * (wx - st.anchor.x) + s * (wz - st.anchor.z));
        const float vy = k * (wy - st.anchor.y);
        const float vz = k * (-s * (wx - st.anchor.x) + c * (wz - st.anchor.z));
        most[0] = std::max(most[0], std::fabs(vx));
        most[1] = std::max(most[1], st.floorMode ? vy : std::fabs(vy));
        most[2] = std::max(most[2], std::fabs(vz));
    }
    // Room on each axis from the anchor's place: x and z to the faces; y up to the top face (floor
    // mode: the anchor sits on the floor margin) or to both faces (centred).
    const float room[3] = {vol[0] * 0.5f - kBoundsMargin,
                           st.floorMode ? vol[1] * 0.5f - kBoundsMargin - base : vol[1] * 0.5f - kBoundsMargin,
                           vol[2] * 0.5f - kBoundsMargin};
    float f = 1.0f;
    for (int a = 0; a < 3; a++)
        if (most[a] > room[a] && room[a] > 0) f = std::min(f, room[a] / most[a]);
    if (f < 1.0f) {
        static int sLogged = 0;  // (a resize refits every frame of the drag)
        if (sLogged++ < 20) log("[volume] the fitted room poked out of the volume: scale %.6f -> %.6f", st.scale, st.scale * f);
        st.scale *= f;
    }
}

// A whole room drawn behind a front wall (kFrontWalls): the set moves towards the viewer until the
// room box's face nearest the viewer lies the wall's cut past the volume's front face.
void behindFrontWall(State& st, const std::array<float, 3>& vol, unsigned map, int room) {
    const FrontWall* wall = frontWall(map, room);
    if (wall == nullptr || st.klass != 0 || !st.roomValid || st.quarter != wall->quarter) return;
    const float t = 1.5707963f * float(st.quarter);
    const float c = std::cos(t), s = std::sin(t), k = st.scale;
    // Volume z (towards the viewer) of the box's corners: -k s (x - ax) + k c (z - az).
    float front = -1e30f;
    for (int i = 0; i < 4; i++) {
        const float wx = (i & 1) ? st.roomHi.x : st.roomLo.x, wz = (i & 2) ? st.roomHi.z : st.roomLo.z;
        front = std::max(front, k * (-s * (wx - st.anchor.x) + c * (wz - st.anchor.z)));
    }
    const float shift = vol[2] * 0.5f + wall->cut * k - front;  // volume metres, towards the viewer
    st.anchor.x += s * shift / k;
    st.anchor.z -= c * shift / k;
    st.target = st.anchor;
}

// The slide (following rooms): the region kept inside the volume's inner part, eased.
void slide(State& st, const Region& region, const std::array<float, 3>& vol, float dt) {
    if (st.klass == 0 || !region.valid()) return;
    // Where the region's centre sits off the set's centre, in volume metres; past the dead zone the
    // target moves just far enough to bring it back to the zone's edge.
    const V3 c = region.centre();
    float vx, vz;
    rotate(st.quarter, c.x - st.target.x, c.z - st.target.z, vx, vz);
    const float zone[3] = {vol[0] * 0.5f * kDeadZone, vol[1] * 0.5f * kDeadZone, vol[2] * 0.5f * kDeadZone};
    // Each axis: the move that brings the region's centre back to the dead zone's edge, or (if
    // more) the one that brings its whole extent inside the volume's usable part; a region bigger
    // than that part is centred.
    const float use[3] = {vol[0] * 0.5f * kUseX, vol[1] * 0.5f * kUseY, vol[2] * 0.5f * kUseZ};
    float ex, ez;
    {
        const V3 half{(region.hi.x - region.lo.x) * 0.5f, (region.hi.y - region.lo.y) * 0.5f, (region.hi.z - region.lo.z) * 0.5f};
        turned(st.quarter, half, ex, ez);
    }
    const auto move = [](float o, float e, float zoneHalf, float useHalf) {
        if (e > useHalf) return o;
        float m = std::fabs(o) > zoneHalf ? o - std::copysign(zoneHalf, o) : 0.0f;
        const float over = o + e > useHalf ? o + e - useHalf : o - e < -useHalf ? o - e + useHalf : 0.0f;
        return std::fabs(over) > std::fabs(m) ? over : m;
    };
    float mx = 0, mz = 0, my = 0;
    const float ox = vx * st.scale, oz = vz * st.scale;
    mx = move(ox, ex * st.scale, zone[0], use[0]) / st.scale;
    mz = move(oz, ez * st.scale, zone[2], use[2]) / st.scale;
    if (!st.floorMode) {
        const float oy = (c.y - st.target.y) * st.scale;
        my = move(oy, (region.hi.y - region.lo.y) * 0.5f * st.scale, zone[1], use[1]) / st.scale;
    }
    if (mx != 0 || mz != 0) {
        // Back into world: the inverse turn.
        float wx, wz;
        rotate((4 - st.quarter) % 4, mx, mz, wx, wz);
        st.target.x += wx;
        st.target.z += wz;
    }
    st.target.y += my;
    // Faster as the region nears the volume's walls (the volume always holds it): eased slowly for
    // the dead zone, briskly once it's in the outer part, at once past the walls.
    float lo[3], hi[3];
    regionInVolume(st, region, vol, lo, hi);
    bool outside = false, near = false;
    for (int k = 0; k < 3; k++) {
        outside |= lo[k] < -vol[k] * 0.5f || hi[k] > vol[k] * 0.5f;
        if (k != 1 || !st.floorMode) near |= lo[k] < -vol[k] * 0.42f || hi[k] > vol[k] * 0.42f;
    }
    const float tau = outside ? 0.1f : near ? 0.4f : kSlideSeconds;
    const float a = 1.0f - std::exp(-dt / tau);
    st.anchor.x += (st.target.x - st.anchor.x) * a;
    st.anchor.z += (st.target.z - st.anchor.z) * a;
    if (!st.floorMode) st.anchor.y += (st.target.y - st.anchor.y) * a;
}

// Every room of a newly loaded map, for the audit: its class at each turn.
void auditRooms(unsigned map, const std::array<float, 3>& vol) {
    for (int room = 0; room < 80; room++) {
        V3 lo, hi;
        if (!fitBox(map, room, lo, hi)) continue;
        const V3 size{hi.x - lo.x, hi.y - lo.y, hi.z - lo.z};
        for (int quarter = 0; quarter < 2; quarter++) {
            float scale = 0;
            const bool fits = fitsWhole(size, quarter, vol.data(), scale);  // (as fit() decides)
            if (!fits) scale = kMinScale;
            const bool tall = size.y * scale > vol[1] * kUseY - kFloorMargin;
            log("[volume] room map %u room %d turn %d size %.0f x %.0f x %.0f: %s, scale %.5f (Luigi %.1f cm), height "
                "%.2f m of %.2f%s",
                map, room, quarter * 90, size.x, size.y, size.z, fits ? "whole" : "follow", scale, scale * kLuigiUnits * 100.0f,
                size.y * scale, vol[1] * kUseY - kFloorMargin, tall ? " (taller than the volume: vertical slide)" : "");
        }
    }
}

void multiply4(const float a[16], const float b[16], float out[16]) {  // column-major
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a[k * 4 + r] * b[c * 4 + k];
            out[c * 4 + r] = s;
        }
}

}  // namespace

bool enabled() {
    static const bool on = [] {
        const char* v = std::getenv("LM_VOLUME");
        return v && *v == '1';
    }();
    return on;
}

namespace {
int sMirrorDepth[16] = {};
}

MirrorScope::MirrorScope(u32 mark, bool on) : mark_(mark), on_(on && enabled() && mark < 16) {
    if (on_ && sMirrorDepth[mark_]++ == 0) GXAuroraMirrorMark(mark_, 1.0f);
}

MirrorScope::~MirrorScope() {
    if (on_ && --sMirrorDepth[mark_] == 0) GXAuroraMirrorMark(mark_, 0.0f);
}

void notePerspective(float m00, float m11) {
    if (!enabled() || !sWantPerspective.exchange(false)) return;
    sP00.store(m00, std::memory_order_relaxed);
    sP11.store(m11, std::memory_order_relaxed);
}

void hostBeginFrame() {
#ifdef LM_VR_VISIONOS
    if (enabled()) lmvr_window_begin_frame();
#endif
}

namespace {
std::atomic<u32> sCaptureFrame{0};  // copyDispCount() of the last capture update (+1; 0 none)
std::atomic<u32> sCaptureGhost{0};  // the ghost being captured (its actor)
// A capture is on while its update ran in the last few frames.
bool capturing() {
    const u32 seen = sCaptureFrame.load(std::memory_order_relaxed);
    return seen != 0 && copyDispCount() + 1 - seen < 4;
}
}

void noteCapture(u32 ghost) {
    if (!enabled()) return;
    sCaptureGhost.store(ghost, std::memory_order_relaxed);
    sCaptureFrame.store(copyDispCount() + 1, std::memory_order_relaxed);
}

namespace {
struct Overridden {
    bool taken = false;
    int resolutionScale = 0;
    bool vsync = false, pauseOnFocusLoss = false, enhancedLighting = false, textureCopyFilter = false,
         displayCopyFilter = false;
} sPlayers;  // the player's own values, as loaded
void putBack(port::Settings& s) {
    s.video.resolutionScale = sPlayers.resolutionScale;
    s.video.vsync = sPlayers.vsync;
    s.enhancements.pauseOnFocusLoss = sPlayers.pauseOnFocusLoss;
    s.enhancedLighting.enabled = sPlayers.enhancedLighting;
    s.rendering.textureCopyFilter = sPlayers.textureCopyFilter;
    s.rendering.displayCopyFilter = sPlayers.displayCopyFilter;
}
}

// The table's internal resolution: the window's own (0, the default: 1280 x 960 on the headset, as
// c460600 played it), or at most 2 x 480p. Forcing 3 x (1920 x 1440) in its place (base patch 0032)
// drew 2.25 times the pixels, and every frame copied that much again into the window's surfaces
// (the scene, its retake as the 2D begins, the final): on the headset the game fell under 30 fps
// for 43% of a session (f5531f8), most of all in the Foyer.
int resolutionScale(int setting) { return setting > 2 ? 2 : setting; }

void forgetPlayersSettings() {
    if (!enabled()) return;
    sPlayers = Overridden{};
    port::setSaveFilter(nullptr);
}

void applySettingsOverrides() {
    if (!enabled()) return;
    auto& s = port::settings();
    if (!sPlayers.taken) {
        sPlayers.taken = true;
        sPlayers.resolutionScale = s.video.resolutionScale;
        sPlayers.vsync = s.video.vsync;
        sPlayers.pauseOnFocusLoss = s.enhancements.pauseOnFocusLoss;
        sPlayers.enhancedLighting = s.enhancedLighting.enabled;
        sPlayers.textureCopyFilter = s.rendering.textureCopyFilter;
        sPlayers.displayCopyFilter = s.rendering.displayCopyFilter;
        port::setSaveFilter(&putBack);
    }
    s.video.resolutionScale = resolutionScale(s.video.resolutionScale);
    s.video.vsync = false;
    s.enhancements.pauseOnFocusLoss = false;
    s.enhancedLighting.enabled = false;
    s.rendering.textureCopyFilter = false;
    s.rendering.displayCopyFilter = false;
}

void beforeEndFrame(bool settingsOpen) {
#ifdef LM_VR_VISIONOS
    if (enabled()) lmvr_window_settings(settingsOpen);
#else
    (void)settingsOpen;
#endif
}

void hostFrameDone() {
#ifdef LM_VR_VISIONOS
    // A capture: the game asks for the stick against where the ghost is in its own shot, so the shot
    // shows in its square (and the set shows which way to pull: sceneEnd).
    if (enabled()) lmvr_window_frame_done(skip::gameBoyView(), capturing());
#endif
}

namespace {
bool sSceneOpen = false;
// Main views drawn in a row on the same map: the mirror records only once a new map has settled.
// Recording through a save file's first frames (its models still being built) crashed the game now
// and then in the Simulator (garbage J3D materials or animations, right after the sky loads);
// starting a little later never did.
unsigned sSteadyMap = ~0u;
u32 sSteadyFrames = 0;
constexpr u32 kSettleFrames = 45;
}

bool settled() {
    // Only in play (the game scene, no scene change pending) on the map that settled: the culling
    // opened through a map's load (or on the title's scene before it) crashed the game's model
    // loader in two Simulator runs of three (call to null, lr 801C0B34 / 801C0CE4).
    return enabled() && sSteadyFrames > kSettleFrames && rd32(VA(0x804D80C8)) == sSteadyMap &&
           rd32(VA(0x804D80A0)) == 2 && rd32(VA(0x804D8088)) == 0;
}

void beforeOverlay() {
#ifdef LM_VR_VISIONOS
    if (enabled() && lmvr_window_enabled()) lmvr_window_overlay_begins();
#endif
}

void sceneBegin() {
#ifdef LM_VR_VISIONOS
    if (!enabled() || !lmvr_window_enabled()) return;
    static const bool solidRefraction = (aurora::mirror::set_solid_refraction(true), true);  // nothing behind the set
    (void)solidRefraction;
    sWantPerspective = true;
    // (A cutscene fast-forwarding runs several guest frames in one window frame: the first is recorded.)
    const unsigned map = rd32(VA(0x804D80C8));
    if (map != sSteadyMap) {
        sSteadyMap = map;
        sSteadyFrames = 0;
    }
    ++sSteadyFrames;
    sSceneOpen = aurora::mirror::enabled() && !lmvr_window_scene_seen() && sSteadyFrames > kSettleFrames;
    if (sSceneOpen) GXAuroraMirrorMark(AURORA_MIRROR_MARK_BEGIN, float(lmvr_window_game_frame() & 0xFFFFFFu));
#endif
}

void sceneEnd() {
#ifdef LM_VR_VISIONOS
    if (!enabled() || !lmvr_window_enabled()) return;
    State& st = sState;
    // The GX view matrix (lbl_803A3820 +0x18C, +0x198 on PAL): rows of a 3x4, world into view.
    float view[12];
    for (int i = 0; i < 12; i++) view[i] = rdf32(VA(0x803A3820) + VOFF(0x18C, 0x198) + 4 * i);
    bool viewOk = true;
    for (float v : view) viewOk = viewOk && std::isfinite(v);
    // The eye: -R^T t. Forward: -(row 2).
    const V3 eye{-(view[0] * view[3] + view[4] * view[7] + view[8] * view[11]),
                 -(view[1] * view[3] + view[5] * view[7] + view[9] * view[11]),
                 -(view[2] * view[3] + view[6] * view[7] + view[10] * view[11])};
    const float fx = -view[8], fz = -view[10];
    const float heading = std::atan2(fx, -fz);
    const u32 player = rd32(kVolActorSlots);
    V3 luigi;
    bool luigiValid = guestPtr(player);
    if (luigiValid) {
        luigi = {rdf32(player + kActPos), rdf32(player + kActPos + 4), rdf32(player + kActPos + 8)};
        luigiValid = std::isfinite(luigi.x) && std::isfinite(luigi.y) && std::isfinite(luigi.z);
    }
    const unsigned map = rd32(VA(0x804D80C8));
    // The set's room: the rooms of an open space count as one.
    const int room = spaceOf(map, int(rd32(VA(0x803A3CA0) + 0xC) >> 24));
    int viewpoint = 0;
    const auto vol = volumeSize(viewpoint);
    const bool cinematic = skip::cinematicCamera();
    const auto now = Clock::now();
    const float dt = st.last == Clock::time_point{} ? 0.0f
                                                     : std::clamp(std::chrono::duration<float>(now - st.last).count(), 0.0f, 0.1f);
    st.last = now;

    // What changed since the last frame.
    float moved = 0, turnedBy = 0;
    if (st.haveCamera) {
        moved = std::hypot(eye.x - st.lastEye.x, eye.y - st.lastEye.y, eye.z - st.lastEye.z);
        turnedBy = std::fabs(std::remainder(heading - st.lastHeading, 6.2831853f));
    }
    const bool cut = st.haveCamera && (moved > 250.0f || turnedBy > 0.5236f);
    st.lastEye = eye;
    st.lastHeading = heading;
    st.haveCamera = viewOk;
    if (room != st.pendingRoom) {
        st.pendingRoom = room;
        st.pendingFrames = 0;
    }
    const bool roomChanged = room != st.room && ++st.pendingFrames >= 3;
    const bool mapChanged = map != st.map;
    const bool resized = vol != st.volume;
    // A camera cut in gameplay refits when it changes what the set should be: the camera's
    // quarter, or (following) a region the set no longer holds. Never in a cutscene: there the set
    // stays as it was and the game's own shot goes on the panel (picture in picture).
    const FrontWall* wall = frontWall(map, room);
    const int wantQuarter = wall != nullptr ? wall->quarter : quarterOf(heading);
    const bool turnChanged = wantQuarter != st.quarter;
    bool refit = !st.valid || mapChanged || roomChanged || resized || (cut && !cinematic && turnChanged);
    Region region = frustumRegion(view, eye, st, luigi, luigiValid, map, room);
    const auto outside = [&](const Region& r) {
        float lo[3], hi[3];
        regionInVolume(st, r, vol, lo, hi);
        bool out = false;
        for (int k = 0; k < 3; k++) out |= lo[k] < -vol[k] * 0.5f || hi[k] > vol[k] * 0.5f;
        return out;
    };
    // The volume always holds what the game camera shows: a new room (before its three frames are
    // up: a warp, a fast door) or a gameplay camera cut whose view the set no longer holds refits now.
    if (!refit && st.valid && region.valid() && (room != st.room || (cut && !cinematic && st.klass != 0)))
        refit = outside(region);
    // A resize alone places the set again without the fade (it can change every frame of a drag).
    const bool fade = refit && !(resized && st.valid && !mapChanged && !roomChanged && !cut);
    if (refit) {
        st.map = map;
        st.room = room;
        st.pendingRoom = room;
        st.volume = vol;
        st.quarter = wantQuarter;
        st.roomValid = fitBox(map, room, st.roomLo, st.roomHi);
        region = frustumRegion(view, eye, st, luigi, luigiValid, map, room);
    }
    if (refit && viewOk) {
        fit(st, region, vol, wall != nullptr);
        keepInside(st, vol);
        behindFrontWall(st, vol, map, room);
        // (Once per room, kind or a 5% change of scale: a resize refits every frame of the drag.)
        static unsigned sLoggedMap = ~0u;
        static int sLoggedRoom = -2, sLoggedKlass = -1;
        static float sLoggedScale = 0;
        if (map != sLoggedMap || room != sLoggedRoom || st.klass != sLoggedKlass ||
            std::fabs(st.scale - sLoggedScale) > 0.05f * sLoggedScale) {
            sLoggedMap = map;
            sLoggedRoom = room;
            sLoggedKlass = st.klass;
            sLoggedScale = st.scale;
            log("[volume] fit: map %u room %d, %s, scale %.6f (Luigi %.1f cm), volume %.2f x %.2f x %.2f m", map, room,
                st.klass == 0 ? "whole" : "following", st.scale, st.scale * kLuigiUnits * 100.0f, vol[0], vol[1], vol[2]);
        }
        st.valid = true;
        st.serial++;
        if (fade) st.fadeSerial++;
        st.outsideFrames = 0;
        if (sAudit) {
            V3 size{};
            if (st.roomValid) size = {st.roomHi.x - st.roomLo.x, st.roomHi.y - st.roomLo.y, st.roomHi.z - st.roomLo.z};
            log("[volume] fit #%u: map %u room %d (%s%s%s) %.0f x %.0f x %.0f, turn %d, %s, scale %.5f (Luigi %.1f cm), "
                "region %.0f x %.0f x %.0f, volume %.2f x %.2f x %.2f m",
                st.serial, map, room,
                mapChanged ? "map" : roomChanged ? "room" : resized ? "resize" : cut ? "cut" : "new room, outside",
                cinematic ? ", cutscene" : "", st.floorMode ? "" : ", vertical slide", size.x, size.y, size.z,
                st.quarter * 90, st.klass == 0 ? "whole room" : st.klass == 1 ? "following" : "following, scale lowered",
                st.scale, st.scale * kLuigiUnits * 100.0f, region.hi.x - region.lo.x, region.hi.y - region.lo.y,
                region.hi.z - region.lo.z, vol[0], vol[1], vol[2]);
        }
    } else if (st.valid && !cinematic) {
        slide(st, region, vol, dt);
    }
    if (sAudit && st.valid && mapChanged && map != st.auditedMap) {
        st.auditedMap = map;
        auditRooms(map, vol);
    }
    if (sAudit && st.valid && region.valid()) {
        float lo[3], hi[3];
        regionInVolume(st, region, vol, lo, hi);
        bool out = false;
        for (int k = 0; k < 3; k++) out |= lo[k] < -vol[k] * 0.5f - 0.005f || hi[k] > vol[k] * 0.5f + 0.005f;
        if (out && (st.outsideFrames++ % 30) == 0)
            log("[volume] outside: map %u room %d, region x %.2f..%.2f y %.2f..%.2f z %.2f..%.2f m of %.2f x %.2f x %.2f%s%s",
                map, room, lo[0], hi[0], lo[1], hi[1], lo[2], hi[2], vol[0], vol[1], vol[2], cinematic ? " (cutscene)" : "",
                frontWall(map, room) != nullptr ? " (its front wall, cut off by design)" : "");
    }

    // volumeFromView = T(base) * S(scale) * R(quarter) * T(-anchor) * worldFromView.
    lmvr_window_scene scene{};
    if (st.valid && viewOk) {
        const float t = 1.5707963f * float(st.quarter);
        const float c = std::cos(t), s = std::sin(t), k = st.scale;
        const float base = st.floorMode ? -vol[1] * 0.5f + kFloorMargin : 0.0f;
        // Column-major set (volume from world): rows [kc 0 ks | 0 k 0 | -ks 0 kc].
        float set[16] = {k * c, 0, -k * s, 0, 0, k, 0, 0, k * s, 0, k * c, 0, 0, 0, 0, 1};
        set[12] = -(k * c * st.anchor.x + k * s * st.anchor.z);
        set[13] = base - k * st.anchor.y;
        set[14] = -(-k * s * st.anchor.x + k * c * st.anchor.z);
        // worldFromView: [R^T | eye], column-major.
        const float worldFromView[16] = {view[0], view[1], view[2], 0, view[4], view[5], view[6], 0,
                                         view[8], view[9], view[10], 0, eye.x, eye.y, eye.z, 1};
        multiply4(set, worldFromView, scene.volume_from_view);
        scene.placed = true;
        // A capture: Luigi and the ghost in the volume, for the arrow that shows which way to pull
        // (away from the ghost, as the viewer sees it: the stick is the viewer's, applyMovement).
        // Test runs: LM_VOLUME_TEST_ARROW=1 shows it all the time, for a ghost 300 units along the world's +x.
        static const bool testArrow = std::getenv("LM_VOLUME_TEST_ARROW") != nullptr;
        const u32 ghost = sCaptureGhost.load(std::memory_order_relaxed);
        if (luigiValid && ((capturing() && guestPtr(ghost)) || testArrow)) {
            const V3 g = capturing() && guestPtr(ghost)
                             ? V3{rdf32(ghost + kActPos), rdf32(ghost + kActPos + 4), rdf32(ghost + kActPos + 8)}
                             : V3{luigi.x + 300.0f, luigi.y, luigi.z};
            if (std::isfinite(g.x) && std::isfinite(g.y) && std::isfinite(g.z)) {
                const auto toVolume = [&](V3 w, float out[3]) {
                    for (int r = 0; r < 3; r++) out[r] = set[r] * w.x + set[4 + r] * w.y + set[8 + r] * w.z + set[12 + r];
                };
                toVolume(luigi, scene.capture_luigi);
                toVolume(g, scene.capture_ghost);
                scene.capture = true;
            }
        }
        const bool walking = copyDispCount() - sStickSeen.load(std::memory_order_relaxed) < 2;
        if (sAudit && luigiValid && (refit || copyDispCount() % 300 == 0 || (walking && copyDispCount() % 15 == 0))) {
            const float w[4] = {luigi.x, luigi.y, luigi.z, 1};
            float v[3];
            for (int r = 0; r < 3; r++) v[r] = set[r] * w[0] + set[4 + r] * w[1] + set[8 + r] * w[2] + set[12 + r];
            float c[3];  // in the game camera's view (his feet), for measuring him in its picture
            for (int r = 0; r < 3; r++) c[r] = view[r * 4] * luigi.x + view[r * 4 + 1] * luigi.y + view[r * 4 + 2] * luigi.z + view[r * 4 + 3];
            log("[volume] Luigi at %.0f,%.0f,%.0f: in the volume %.2f,%.2f,%.2f m; eye %.0f,%.0f,%.0f heading %.0f deg; "
                "in view %.0f,%.0f,%.0f, tan %.3f x %.3f",
                luigi.x, luigi.y, luigi.z, v[0], v[1], v[2], eye.x, eye.y, eye.z, heading * 57.29578f, c[0], c[1], c[2],
                1.0f / std::max(std::fabs(sP00.load()), 1e-3f), 1.0f / std::max(std::fabs(sP11.load()), 1e-3f));
        }
    }
    const float p00 = sP00.load(std::memory_order_relaxed), p11 = sP11.load(std::memory_order_relaxed);
    scene.tan_half_x = p00 != 0 ? 1.0f / std::fabs(p00) : 0;
    scene.tan_half_y = p11 != 0 ? 1.0f / std::fabs(p11) : 0;
    scene.focus = luigiValid ? std::hypot(luigi.x - eye.x, luigi.y + 60.0f - eye.y, luigi.z - eye.z) : 600.0f;
    scene.set_serial = st.fadeSerial;
    scene.fit_serial = st.serial;  // every fit, a resize's too (the volume's held shell lets go)
    scene.cinematic = cinematic;
    scene.scale = st.scale;
    lmvr_window_scene_drawn(&scene);
    {
        std::lock_guard lock(sStickMutex);
        const float n = std::hypot(fx, fz);
        sStickValid = st.valid && n > 1e-4f;
        if (sStickValid) sStickCamForward[0] = fx / n, sStickCamForward[1] = fz / n;
        sStickQuarter = st.quarter;
        sStickViewpoint = viewpoint;
    }
    if (sSceneOpen) {
        sSceneOpen = false;
        GXAuroraMirrorMark(AURORA_MIRROR_MARK_END, scene.focus);
    }
#endif
}

void applyMovement(s8& x, s8& y) {
#ifdef LM_VR_VISIONOS
    if (!enabled() || (x == 0 && y == 0)) return;
    if (skip::cinematicCamera() || skip::gameBoyView() || skip::choiceMenuActive()) return;
    float camF[2];
    int quarter, viewpoint;
    {
        std::lock_guard lock(sStickMutex);
        if (!sStickValid) return;
        camF[0] = sStickCamForward[0], camF[1] = sStickCamForward[1];
        quarter = sStickQuarter;
        viewpoint = sStickViewpoint;
    }
    // The viewer's forward and right in the volume (x, z), by the side they look from: 0 front
    // (looking along -z), 1 right (from +x, looking along -x), 2 back, 3 left.
    static const float kForward[4][2] = {{0, -1}, {-1, 0}, {0, 1}, {1, 0}};
    static const float kRight[4][2] = {{1, 0}, {0, -1}, {-1, 0}, {0, 1}};
    const int v = ((viewpoint % 4) + 4) % 4;
    const float mx = float(x) * kRight[v][0] + float(y) * kForward[v][0];
    const float mz = float(x) * kRight[v][1] + float(y) * kForward[v][1];
    // Into the world: the set's turn undone.
    float wx, wz;
    rotate((4 - quarter) % 4, mx, mz, wx, wz);
    // Into the game camera's stick: up is its forward, right its right (-fz, fx).
    const float camR[2] = {-camF[1], camF[0]};
    const float sx = wx * camR[0] + wz * camR[1], sy = wx * camF[0] + wz * camF[1];
    const s8 inX = x, inY = y;
    x = s8(std::clamp(std::lround(sx), -127L, 127L));
    y = s8(std::clamp(std::lround(sy), -127L, 127L));
    if (sAudit) {
        static u32 logged = 0;
        sStickSeen.store(copyDispCount(), std::memory_order_relaxed);
        if (logged++ % 30 == 0)
            log("[volume] stick %d,%d from the %s -> the game camera's %d,%d (set turn %d)", inX, inY,
                v == 0 ? "front" : v == 1 ? "right" : v == 2 ? "back" : "left", x, y, quarter * 90);
    }
#else
    (void)x;
    (void)y;
#endif
}

}  // namespace recomp::volume
