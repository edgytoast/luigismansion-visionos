#include "recomp/vr_avatar_guest.h"
// World lights (F1 > Visuals > Enhanced lighting > World lights; docs/RENDERING.md,
// "Enhanced lighting > World lights").
//
// Flames and glowing effects come from the game's own particle system (the LPS manager
// lbl_803D990C, drawn by fn_801604FC): every live emitter is in the manager's active list, its
// resource (.pcr) tells what it is (candle_*, danro_fire, elemh_fire_*, kira_*, ...) and +0x214
// is its world position. Resource names are recorded when the game loads them
// (JKRArchive::getResource(type, name)) and emitters are matched to their resource when they are
// created (fn_8015FE9C(manager, pcr, texture, ...)). An emitter is lit while it is drawn
// (+0xBE bit 8), emitting (bit 2) and in a visible room (bit 0x40 clear: fn_8015F990, the
// draw's own room test).
// Lightning: the strike fn_80029250(thunder entry, ...) starts the room's lightning and points the
// game's shadow light camera (lbl_803A3820 +0x24 eye) at the window; the flash is a cool light
// there, flickering and fading over ~1.5 s.
#include "recomp/world_lights.h"

#include "port/settings.h"
#include "recomp/enhanced_lighting.h"
#include "recomp/hle.h"
#include "recomp/rarc.h"
#include "recomp/silly_world.h"
#include "recomp/runtime.h"
#include "recomp/version.h"

#include <aurora/lights.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace recomp::worldlights {
namespace {

using aurora::gfx::ExtraLight;

int envInt(const char* name, int def) {
    const char* v = std::getenv(name);
    return v && *v ? std::atoi(v) : def;
}
bool envFlag(const char* name) {
    const char* v = std::getenv(name);
    return v && *v && *v != '0';
}
// LM_WORLD_LIGHT_LOG=N: every N frames, every emitter and the lights sent.
const int sLog = envInt("LM_WORLD_LIGHT_LOG", 0);
const bool sForce = envFlag("LM_WORLD_LIGHTS");  // test aid: on regardless of the setting
// LM_WORLD_LIGHT_AB=1: lights only on even game frames (consecutive frame dumps compare with /
// without in the same scene).
const bool sAB = envFlag("LM_WORLD_LIGHT_AB");

bool guestPtr(u32 p) { return p >= 0x80000000u && p < 0x81800000u; }

#define kPtclMgr VA(0x803D990C)  // lbl_803D990C: LPS particle manager
#define kCamera VA(0x803A3820)   // game camera
#define kCamViewMtx VOFF(0x18C, 0x198)
// Manager +0xAB4: the active group; group +0x2A0: sentinel emitter of its active list. Emitter
// +0x20C next, +0xBE flags (8 drawn, 2 emitting, 0x40 room not visible), +0x214 world position.
constexpr u32 kMgrGroup = 0xAB4, kGroupList = 0x2A0;
constexpr u32 kEmNext = 0x20C, kEmFlags = 0xBE, kEmPos = 0x214;

// What a particle resource is as a light source.
struct Kind {
    const char* prefix;
    int cat;          // port::WorldLightCategory
    u8 rgb[3];        // sRGB
    float radius;     // world units
    float intensity;  // at category intensity 1
    float flicker;    // 0 steady .. 1 strong flame flicker
    float height;     // light above the emitter (flame centre)
    float merge;      // merge distance for nearby emitters of the same kind (0 = never)
};
// Matched by prefix, first match wins.
constexpr Kind kKinds[] = {
    {"candle_vio", port::kWlCandles, {200, 130, 255}, 340.0f, 1.1f, 0.8f, 6.0f, 150.0f},
    {"candle", port::kWlCandles, {255, 172, 85}, 360.0f, 1.2f, 0.8f, 6.0f, 150.0f},
    {"danro_fire", port::kWlFires, {255, 155, 60}, 760.0f, 2.2f, 1.0f, 30.0f, 0.0f},
    {"torch", port::kWlFires, {255, 160, 65}, 600.0f, 1.8f, 1.0f, 10.0f, 0.0f},
    {"tc_s", port::kWlFires, {255, 160, 65}, 600.0f, 1.8f, 1.0f, 10.0f, 0.0f},
    {"chandelier", port::kWlLamps, {255, 205, 140}, 900.0f, 1.6f, 0.15f, 0.0f, 0.0f},
    {"shanyb", port::kWlLamps, {255, 205, 140}, 900.0f, 1.6f, 0.15f, 0.0f, 0.0f},
    {"m_light1_y", port::kWlLamps, {255, 220, 120}, 500.0f, 1.2f, 0.1f, 0.0f, 0.0f},
    {"m_light1_r", port::kWlLamps, {255, 80, 70}, 500.0f, 1.2f, 0.1f, 0.0f, 0.0f},
    {"m_light1_b", port::kWlLamps, {90, 140, 255}, 500.0f, 1.2f, 0.1f, 0.0f, 0.0f},
    {"elemh_fire", port::kWlSprays, {255, 150, 55}, 620.0f, 1.0f, 0.7f, 0.0f, 400.0f},
    {"elemh_wat", port::kWlSprays, {70, 140, 255}, 420.0f, 0.35f, 0.2f, 0.0f, 400.0f},
    {"elemh_ice", port::kWlSprays, {150, 230, 255}, 420.0f, 0.35f, 0.2f, 0.0f, 400.0f},
    {"fireball", port::kWlFireFx, {255, 110, 30}, 450.0f, 1.4f, 0.8f, 0.0f, 0.0f},
    {"firehit", port::kWlFireFx, {255, 120, 40}, 420.0f, 1.6f, 0.9f, 0.0f, 0.0f},
    {"kpa_fire", port::kWlFireFx, {255, 100, 25}, 600.0f, 1.8f, 0.9f, 0.0f, 300.0f},
    {"lg_eledmg_f", port::kWlFireFx, {255, 110, 30}, 380.0f, 1.2f, 0.9f, 40.0f, 0.0f},
    {"kobk_el_fire", port::kWlFireFx, {255, 110, 30}, 450.0f, 1.3f, 0.9f, 0.0f, 0.0f},
    {"kaen_hinoko", port::kWlFireFx, {255, 120, 40}, 300.0f, 0.6f, 1.0f, 0.0f, 200.0f},
    {"iceball", port::kWlFireFx, {150, 230, 255}, 350.0f, 0.5f, 0.2f, 0.0f, 0.0f},
    {"kira_gold", port::kWlSparkles, {255, 205, 70}, 200.0f, 0.45f, 0.5f, 0.0f, 120.0f},
    {"kira_ruby", port::kWlSparkles, {255, 50, 70}, 200.0f, 0.45f, 0.5f, 0.0f, 120.0f},
    {"kira_rdia", port::kWlSparkles, {255, 60, 90}, 200.0f, 0.45f, 0.5f, 0.0f, 120.0f},
    {"kira_sapphire", port::kWlSparkles, {60, 110, 255}, 200.0f, 0.45f, 0.5f, 0.0f, 120.0f},
    {"kira_emerald", port::kWlSparkles, {50, 230, 110}, 200.0f, 0.45f, 0.5f, 0.0f, 120.0f},
    {"kira_", port::kWlSparkles, {235, 240, 255}, 200.0f, 0.4f, 0.5f, 0.0f, 120.0f},
    {"coin_ef", port::kWlSparkles, {255, 205, 70}, 180.0f, 0.4f, 0.5f, 0.0f, 120.0f},
    {"key_kira", port::kWlSparkles, {255, 225, 120}, 220.0f, 0.5f, 0.4f, 0.0f, 0.0f},
    {"bkey_y", port::kWlSparkles, {255, 225, 90}, 260.0f, 0.6f, 0.3f, 0.0f, 120.0f},
    {"bkey_b", port::kWlSparkles, {80, 140, 255}, 260.0f, 0.6f, 0.3f, 0.0f, 120.0f},
    {"bkey_g", port::kWlSparkles, {80, 230, 120}, 260.0f, 0.6f, 0.3f, 0.0f, 120.0f},
    {"bkey_p", port::kWlSparkles, {230, 110, 255}, 260.0f, 0.6f, 0.3f, 0.0f, 120.0f},
    {"btel_", port::kWlSparkles, {210, 235, 255}, 300.0f, 0.6f, 0.3f, 0.0f, 150.0f},
    {"dancer_kira", port::kWlSparkles, {255, 180, 240}, 250.0f, 0.4f, 0.4f, 0.0f, 150.0f},
    {"takarabako", port::kWlSparkles, {255, 215, 110}, 300.0f, 0.6f, 0.4f, 0.0f, 0.0f},
};

const Kind* kindFor(const std::string& name) {
    for (const Kind& k : kKinds)
        if (name.compare(0, std::strlen(k.prefix), k.prefix) == 0) return &k;
    return nullptr;
}

struct Res {
    std::string name;
    const Kind* kind;
};
std::unordered_map<u32, Res> sRes;           // .pcr resource data -> name/kind
std::unordered_map<u32, u32> sEmitterRes;   // emitter -> .pcr resource data
u32 sFrame = 0;
u32 sStrikeFrame = 0;                       // lightning
float sStrikePos[3] = {};

float srgbToLinear(u32 v) {
    const float c = float(v) / 255.0f;
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

// Smooth value noise in [-1, 1] (hash of integer steps, cosine blend), for flame flicker.
float hash1(u32 n) {
    n = (n << 13) ^ n;
    n = n * (n * n * 15731u + 789221u) + 1376312589u;
    return 1.0f - float(n & 0x7FFFFFFF) / 1073741824.0f;
}
float vnoise(float x, u32 seed) {
    const float fl = std::floor(x);
    const u32 i = u32(s32(fl)) + seed * 7919u;
    const float f = x - fl, u = (1.0f - std::cos(f * 3.14159265f)) * 0.5f;
    return hash1(i) * (1.0f - u) + hash1(i + 1) * u;
}
// Flame: slow breathing, faster licks, rare dips. ~1 on average.
float flameFlicker(float t, u32 seed, float amount) {
    const float v = 0.55f * vnoise(t * 2.1f, seed) + 0.30f * vnoise(t * 6.3f, seed + 101) + 0.15f * vnoise(t * 13.7f, seed + 211);
    return std::max(0.25f, 1.0f + amount * 0.32f * v);
}

struct Source {
    float pos[3];  // world
    const Kind* kind;
    u32 id;
    int count;
    float level;  // 0 out .. 1 burning (per flame; a merged group: sum of its flames)
};

// Flame state per emitter (docs/RENDERING.md "World lights follow the game's flames"). The game's
// candles are objects (pool from fn_80164608, 0x50 bytes; emitter +0xC4 points back, object +0x30
// is the emitter) with a state byte at +0x10: 1 lit, 2 flickering (Luigi close), 3 blown by the
// vacuum, 4 out (relit by fire: fn_80164140), 5 out until the room's light comes on, 0 freed. Going
// out the game clears the emitter's emitting bit (+0xBE 0x0E -> 0x0C; removed objects 0x1C) and
// relighting sets it again; the flame's particles then die out or grow back. The light follows that:
// on while the flame burns, fading with the live particle count (emitter +0x74 + +0xB0, the count the
// game's own candle light fn_801799AC is kept for) when it goes out or is relit; fireplaces, torches
// and fire effects (no candle object) follow their emitter and particles the same way.
struct Flame {
    float peak = 0.0f, level = 0.0f;
    u32 seen = 0;
    bool init = false;
};
std::unordered_map<u32, Flame> sFlame;
u32 sFlameTick = 0;
std::unordered_map<u32, float> sLampLevel;  // lamp id -> room light fade

float flameLevel(u32 e, u32 flags) {
    Flame& f = sFlame[e];
    f.seen = sFlameTick;
    const float particles = float(std::max(s32(rd32(e + 0x74)) + s32(rd32(e + 0xB0)), 0));
    bool lit = (flags & 2) != 0;
    const u32 owner = rd32(e + 0xC4);
    if (guestPtr(owner) && rd32(owner + 0x30) == e) {
        const u8 state = rd8(owner + 0x10);
        lit = lit && state >= 1 && state <= 3;
    }
    if (lit) f.peak = std::max(f.peak * 0.999f, particles);
    const float ratio = f.peak > 0.5f ? std::clamp(particles / f.peak, 0.0f, 1.0f) : (lit ? 1.0f : 0.0f);
    const float shown = std::clamp(ratio * 2.0f, 0.0f, 1.0f);  // full from half the usual particle count
    float target = lit ? shown : std::min(shown, f.level);  // going out: only down, with the dying particles
    if (!lit && particles < 1.0f) target = 0.0f;
    if (!f.init) {
        f.init = true;
        f.level = target;  // first sight (map load, room comes into view): no fade-in
    }
    f.level += (target - f.level) * (target > f.level ? 0.2f : 0.3f);
    if (f.level < 0.005f) f.level = 0.0f;
    return f.level;
}

void toView(const float m[12], const float p[3], float out[3]) {
    for (int r = 0; r < 3; r++) out[r] = m[r * 4] * p[0] + m[r * 4 + 1] * p[1] + m[r * 4 + 2] * p[2] + m[r * 4 + 3];
}

bool enabled() {
    const auto& st = port::settings();
    return sForce || (st.worldLights.enabled && enh::masterOn() && st.enhancedLighting.extraLights);
}

const char* emitterName(u32 em) {
    const auto it = sEmitterRes.find(em);
    if (it == sEmitterRes.end()) return "?";
    const auto n = sRes.find(it->second);
    return n == sRes.end() ? "?" : n->second.name.c_str();
}

// ---- Lamps and chandeliers: the map's jmp tables -------------------------------------------
// The map archive's jmp/objinfo (122 "chandelier" objects in map 2), jmp/furnitureinfo (light
// fittings by model name) and jmp/roominfo (each room's LightColorR/G/B) are read from the disc
// (the game loads them through its tool-data loader, not by name). JMP layout
// (tools/jmp_dump.py): s32 count, s32 fields, s32 data offset, u32 entry size; per field u32 name
// hash, u32 mask, u16 offset, u8 shift, u8 type (0 int, 1 string of 32 bytes, 2 float).
#define kCurMapNo VA(0x804D80C8)  // sCurMapNo
int sLampsMap = -1;                // map the lamp list was built for

u32 jmpHash(const char* s) {
    u32 h = 0;
    for (; *s; s++) h = (u32(u8(*s)) + (h << 8)) % 0x1FFFFD9u;
    return h;
}
u32 be32h(const rarc::Bytes& d, size_t o) {
    return o + 4 <= d.size() ? u32(d[o]) << 24 | u32(d[o + 1]) << 16 | u32(d[o + 2]) << 8 | u32(d[o + 3]) : 0;
}
struct JmpField {
    u32 off = ~0u, mask = 0;
    u8 shift = 0, type = 0;
};
struct Jmp {
    const rarc::Bytes* d = nullptr;
    u32 count() const { return d && d->size() >= 16 ? be32h(*d, 0) : 0; }
    JmpField field(const char* name) const {
        const u32 nf = be32h(*d, 4), h = jmpHash(name);
        for (u32 i = 0; i < nf && i < 64; i++) {
            const size_t f = 0x10 + i * 12;
            if (be32h(*d, f) == h) return {be32h(*d, f + 8) >> 16, be32h(*d, f + 4), (*d)[f + 10], (*d)[f + 11]};
        }
        return {};
    }
    size_t row(u32 e) const { return be32h(*d, 8) + size_t(e) * be32h(*d, 12); }
    std::string str(size_t r, const JmpField& f) const {
        std::string s;
        for (u32 i = 0; f.off != ~0u && i < 32 && r + f.off + i < d->size(); i++) {
            const char ch = char((*d)[r + f.off + i]);
            if (!ch) break;
            s += ch;
        }
        return s;
    }
    float f32v(size_t r, const JmpField& f) const {
        const u32 v = f.off != ~0u ? be32h(*d, r + f.off) : 0;
        float x;
        std::memcpy(&x, &v, 4);
        return x;
    }
    s32 i32v(size_t r, const JmpField& f) const { return f.off != ~0u ? s32((be32h(*d, r + f.off) & f.mask) >> f.shift) : -1; }
};

// Room containing a world position (room table lbl_804D8170; enhanced_lighting.cpp), -1 if none.
int roomAt(const float p[3]) { return enh::roomAt(p); }

struct Lamp {
    float pos[3];
    int room;
    bool chandelier;
    u32 id;
};
std::vector<Lamp> sLamps;
std::unordered_map<int, u32> sRoomColor;  // room -> 0xRRGGBB (roominfo LightColor)

// Furniture models that are light fittings (model names in the map's furnitureinfo).
bool lampModel(const std::string& m, bool& chandelier) {
    chandelier = m.rfind("syan", 0) == 0 || m.rfind("o_syan", 0) == 0 || m.rfind("osyan", 0) == 0 ||
                 m.rfind("shan", 0) == 0;
    return chandelier || m.rfind("a_lamp", 0) == 0 || m.rfind("lamp", 0) == 0 || m.rfind("a_ligh", 0) == 0 ||
           m.rfind("a_lump", 0) == 0;
}

void buildLamps() {
    const int map = s32(rd32(kCurMapNo));
    if (map == sLampsMap || rd32(VA(0x804D8170)) == 0) return;
    sLampsMap = map;
    sLamps.clear();
    sRoomColor.clear();
    rarc::Archive arc;
    if (map < 0 || map > 20 || !arc.parse(rarc::decompress(rarc::readDisc(("/Map/map" + std::to_string(map) + ".szp").c_str()))))
        return;
    if (const rarc::Item* it = arc.find("jmp/objinfo")) {
        const Jmp j{&it->data};
        const JmpField name = j.field("name"), px = j.field("pos_x"), py = j.field("pos_y"), pz = j.field("pos_z");
        for (u32 e = 0; e < std::min<u32>(j.count(), 4000); e++) {
            const size_t r = j.row(e);
            if (j.str(r, name) != "chandelier") continue;
            Lamp l{{j.f32v(r, px), j.f32v(r, py) - 40.0f, j.f32v(r, pz)}, -1, true, 0x57100000u + e};
            l.room = roomAt(l.pos);
            sLamps.push_back(l);
        }
    }
    if (const rarc::Item* it = arc.find("jmp/furnitureinfo")) {
        const Jmp j{&it->data};
        const JmpField name = j.field("dmd_name"), px = j.field("pos_x"), py = j.field("pos_y"), pz = j.field("pos_z");
        for (u32 e = 0; e < std::min<u32>(j.count(), 4000); e++) {
            const size_t r = j.row(e);
            bool chand = false;
            if (!lampModel(j.str(r, name), chand)) continue;
            // Furniture sits on its origin: a chandelier model hangs below it, a lamp's bulb is above.
            Lamp l{{j.f32v(r, px), j.f32v(r, py) + (chand ? -60.0f : 90.0f), j.f32v(r, pz)}, -1, chand, 0x57200000u + e};
            l.room = roomAt(l.pos);
            sLamps.push_back(l);
        }
    }
    if (const rarc::Item* it = arc.find("jmp/roominfo")) {
        const Jmp j{&it->data};
        const JmpField no = j.field("RoomNo"), cr = j.field("LightColorR"), cg = j.field("LightColorG"), cb = j.field("LightColorB");
        for (u32 e = 0; e < std::min<u32>(j.count(), 200); e++) {
            const size_t r = j.row(e);
            sRoomColor[j.i32v(r, no)] = u32(j.i32v(r, cr) & 0xFF) << 16 | u32(j.i32v(r, cg) & 0xFF) << 8 | u32(j.i32v(r, cb) & 0xFF);
        }
    }
    if (sLog) log("[worldlight] map %d lamps: %zu, %zu room colors", map, sLamps.size(), sRoomColor.size());
}

// Room flags the game draws by (fn_8001CE1C(room, -1): table lbl_803A4BB0[lbl_804D8158], u16 at
// room * 0xC, bit 1 = visible) and whether the room's lights are on: the game's own test
// fn_80018D78(room object) (room objects lbl_804D8178, 0x14C each): +6 bit 0x80 (set when the room
// is cleared, CondLightOn), and 0 in the mansion while the breaker is off (fn_80037894: map 2 and
// flag 61, the blackout). Evaluated once per frame per room (sLitCache).
bool roomVisible(int room) {
    const u32 tab = rd32(VA(0x803A4BB0) + 4 * u32(s32(s8(rd8(VA(0x804D8158))))));
    return guestPtr(tab) && (rd16(tab + u32(room) * 0xC) & 1);
}
Ctx* sCtx = nullptr;  // guest context of the current onMainView (for the game's room-lit test)
std::unordered_map<int, bool> sLitCache;
bool roomLit(int room) {
    if (room < 0 || room >= 128) return false;
    const auto it = sLitCache.find(room);
    if (it != sLitCache.end()) return it->second;
    const u32 rooms = rd32(VA(0x804D8178));
    bool lit = false;
    if (guestPtr(rooms)) {
        static const u32 fn = guestSymbol("fn_80018D78");
        if (sCtx && fn) {
            const Ctx saved = *sCtx;
            lit = guestCall(*sCtx, fn, rooms + u32(room) * 0x14C) != 0;
            *sCtx = saved;
        } else {
            lit = (rd8(rooms + u32(room) * 0x14C + 6) & 0x80) != 0;
        }
    }
    sLitCache[room] = lit;
    return lit;
}

}  // namespace

void noteResource(const char* path, u32 data) {
    if (sLog >= 2 && path && std::strstr(path, "info")) log("[worldlight] resource %s -> %08X", path, data);
    // Both archive lookup overloads call this: retail effects need not use the
    // typed lookup used by the debug particle menu.
    if(!path || !guestPtr(data)) return;
    const size_t len=strnlen(path,128);
    if(len>=128 || len<5 || std::strcmp(path+len-4,".pcr")) return;
    std::string base(path,len-4);
    if(const auto slash=base.rfind('/');slash!=std::string::npos) base.erase(0,slash+1);
    sRes[data]={base,kindFor(base)};
}

HiddenPlayerParticles::HiddenPlayerParticles() {
    static const bool test=std::getenv("LM_VR_COLD_BREATH_TEST")!=nullptr;
    if(!vravatar::hiddenKeyGhostPlayer()&&!test)return;
    // Cold breath uses the older particle manager, not LPS .pcr emitters.
    // Its init handler copies a member-function pointer to group +0x178.
    // Native draw fn_8012CB74 draws only groups with byte +0x3D == 1.
    const u32 manager=VOFF(0x803D7F00,0x803C54C0);
    const u32 sentinel=rd32(manager+0x3D8);
    const u32 handler=VOFF(0x8012A904,0x8012DD80);
    unsigned count=0;
    for(u32 group=guestPtr(sentinel+0x143)?rd32(sentinel+0x140):0;
        guestPtr(group)&&guestPtr(group+0x1AF)&&group!=sentinel&&count++<256;
        group=rd32(group+0x140)) {
        if(rd32(group+0x178)!=handler)continue;
        const u8 shown=rd8(group+0x3D);saved_.push_back({group,shown});wr8(group+0x3D,0);
        static unsigned reports=0;if(reports++<3)
            log("[vr cold breath] legacy group %08X handler %08X draw %u -> 0",group,handler,unsigned(shown));
    }
}
HiddenPlayerParticles::~HiddenPlayerParticles() {
    for(auto it=saved_.rbegin();it!=saved_.rend();++it)wr8(it->first+0x3D,it->second);
}
void onMainView(Ctx& c) {
    sFrame++;
    sCtx = &c;
    sLitCache.clear();
    struct CtxScope {
        ~CtxScope() { sCtx = nullptr; }
    } ctxScope;
    // Test aid LM_ROOM_LIGHTS_ON=1: sets the current room's "light on" flag (room objects
    // lbl_804D8178, 0x14C each, +6 bit 0x80: what CondLightOn / fn_80018D78 test, the state a
    // cleared room is in) so lit rooms can be tested without clearing them.
    static const bool sRoomOn = envFlag("LM_ROOM_LIGHTS_ON");
    if (sRoomOn) {
        const u32 rooms = rd32(VA(0x804D8178));
        const u32 room = rd32(VA(0x803A3CA0) + 0xC) >> 24;
        if (guestPtr(rooms) && room < 0x80) {
            const u32 f = rooms + room * 0x14C + 6;
            if (!(rd8(f) & 0x80)) {
                wr8(f, rd8(f) | 0x80);
                log("[worldlight] f%u room %u light on (test aid)", sFrame, room);
            }
        }
    }
    const bool logNow = sLog > 0 && sFrame % u32(std::max(sLog, 1)) == 0;
    if (!enabled() && !logNow) return;
    const auto& w = port::settings().worldLights;
    const float t = float(sFrame) / 30.0f;

    // Live, visible emitters of a known kind.
    std::vector<Source> src;
    const u32 group = rd32(kPtclMgr + kMgrGroup);
    const u32 sentinel = guestPtr(group) ? rd32(group + kGroupList) : 0;
    // Test aid LM_TEST_BLOWOUT=<frame>[,<relight frame>] (main-view frames of the map): every visible
    // lit candle goes out at <frame> and is relit at <relight frame>, with exactly the writes the
    // game makes (fn_80163D58 vacuum blow-out: emitter +0xBE = 0x0C, state 4; fn_80164140 fire
    // relight: 0x0E, state 1).
    static const std::pair<int, int> sBlow = [] {
        std::pair<int, int> b{-1, -1};
        if (const char* v = std::getenv("LM_TEST_BLOWOUT")) std::sscanf(v, "%d,%d", &b.first, &b.second);
        return b;
    }();
    if (int(sFrame) == sBlow.first || int(sFrame) == sBlow.second) {
        const bool out = int(sFrame) == sBlow.first;
        for (u32 e = guestPtr(sentinel) ? rd32(sentinel + kEmNext) : 0; guestPtr(e) && e != sentinel; e = rd32(e + kEmNext)) {
            const u32 owner = rd32(e + 0xC4);
            if (!guestPtr(owner) || rd32(owner + 0x30) != e || (rd16(e + kEmFlags) & 0x40)) continue;
            if (out && rd8(owner + 0x10) == 1) {
                wr16(e + kEmFlags, 0x0C), wr8(owner + 0x10, 4);
                log("[worldlight] f%u test: candle %08X blown out", sFrame, e);
            } else if (!out && rd8(owner + 0x10) == 4) {
                wr16(e + kEmFlags, 0x0E), wr8(owner + 0x10, 1);
                log("[worldlight] f%u test: candle %08X relit", sFrame, e);
            }
        }
    }
    int n = 0;
    for (u32 e = guestPtr(sentinel) ? rd32(sentinel + kEmNext) : 0; guestPtr(e) && e != sentinel && n < 512;
         e = rd32(e + kEmNext), n++) {
        const u32 flags = rd16(e + kEmFlags);
        const float p[3] = {rdf32(e + kEmPos), rdf32(e + kEmPos + 4), rdf32(e + kEmPos + 8)};
        const auto it = sEmitterRes.find(e);
        const Res* res = nullptr;
        if (it != sEmitterRes.end()) {
            const auto r = sRes.find(it->second);
            if (r != sRes.end()) res = &r->second;
        }
        if (logNow) {
            const u32 owner = rd32(e + 0xC4);
            log("[worldlight] f%u emitter %08X %-16s flags %04X pos %.0f,%.0f,%.0f particles %d owner %08X state %d", sFrame, e,
                res ? res->name.c_str() : "?", flags, p[0], p[1], p[2], s32(rd32(e + 0x74)) + s32(rd32(e + 0xB0)), owner,
                guestPtr(owner) ? int(rd8(owner + 0x10)) : -1);
        }
        if (!res || !res->kind) continue;
        if ((flags & 0x48) != 0x08) continue;            // drawn, room visible
        if (p[0] == 0.0f && p[1] == 0.0f && p[2] == 0.0f) continue;  // pooled, never placed
        const Kind& k = *res->kind;
        if (!w.on[k.cat] && !sForce) continue;
        float level = flameLevel(e, flags);
        // Electric fittings (chandelier / lamp glow emitters): light only while the game has the
        // room's lights on (fades like the jmp-table lamps below). Candles, fires and effects are
        // independent flames and keep their own state.
        if (k.cat == port::kWlLamps) {
            const float wp[3] = {p[0], p[1] + k.height, p[2]};
            float& on = sLampLevel[0x57300000u ^ e];
            on += ((roomLit(roomAt(wp)) ? 1.0f : 0.0f) - on) * 0.3f;
            if (on < 0.01f) on = 0.0f;
            level *= on;
        }
        src.push_back({{p[0], p[1] + k.height, p[2]}, &k, e, 1, level});
    }
    sFlameTick++;
    if ((sFlameTick & 255) == 0)
        for (auto it = sFlame.begin(); it != sFlame.end();) it = sFlameTick - it->second.seen > 300 ? sFlame.erase(it) : std::next(it);
    // Research aid (LM_WORLD_LIGHT_LOG >= 3): once, 200 frames into a map, where the candle
    // emitters are referenced from (holder objects).
    if (sLog >= 3 && sFrame == 200) {
        for (u32 e = guestPtr(sentinel) ? rd32(sentinel + kEmNext) : 0; guestPtr(e) && e != sentinel; e = rd32(e + kEmNext)) {
            if (std::strncmp(emitterName(e), "candle", 6) != 0) continue;
            for (u32 a = 0x80000000u; a < 0x81800000u; a += 4)
                if (rd32(a) == e) log("[worldlight] emitter %08X referenced at %08X", e, a);
        }
    }
    if (!enabled()) return;

    // Nearby emitters of one kind become one light (a candelabra, the spray's several emitters).
    // Unlit flames stay members (level 0): the group's id and shadow slot do not change when one
    // of its candles goes out; the light dims by the flames still burning and leans toward them.
    if (w.mergeNearby) {
        std::vector<Source> merged;
        std::vector<bool> used(src.size(), false);
        for (size_t i = 0; i < src.size(); i++) {
            if (used[i]) continue;
            Source s = src[i];
            const float m = s.kind->merge;
            std::vector<size_t> members{i};
            for (size_t j = i + 1; j < src.size() && m > 0.0f; j++) {
                if (used[j] || src[j].kind != s.kind) continue;
                const float dx = src[j].pos[0] - s.pos[0], dy = src[j].pos[1] - s.pos[1], dz = src[j].pos[2] - s.pos[2];
                if (dx * dx + dy * dy + dz * dz > m * m) continue;
                used[j] = true;
                members.push_back(j);
                s.id = std::min(s.id, src[j].id);
            }
            float sum[3] = {}, wsum = 0.0f, lsum = 0.0f;
            for (size_t j : members) {
                const float wt = 0.05f + src[j].level;  // unlit members keep the centre from jumping
                for (int k = 0; k < 3; k++) sum[k] += src[j].pos[k] * wt;
                wsum += wt;
                lsum += src[j].level;
            }
            for (int k = 0; k < 3; k++) s.pos[k] = sum[k] / wsum;
            s.count = int(members.size());
            s.level = lsum;  // burning flames (sum of levels)
            merged.push_back(s);
        }
        src.swap(merged);
    }

    float view[12];
    for (int i = 0; i < 12; i++) view[i] = rdf32(kCamera + kCamViewMtx + 4 * i);
    std::vector<ExtraLight> lights;
    lights.reserve(src.size() + 1);
    const float master = std::clamp(w.masterIntensity, 0.0f, 3.0f);
    for (const Source& s : src) {
        const Kind& k = *s.kind;
        float rgb[3], mx = 1e-4f;
        for (int i = 0; i < 3; i++) mx = std::max(mx, rgb[i] = srgbToLinear(k.rgb[i]));
        // Several flames together: brighter and a bit wider, not n times. Only flames that burn
        // count (level: the game's flame state, fading with the flame's particles).
        if (s.level < 0.01f) continue;
        const float group = std::pow(float(s.count), 0.6f);
        float I = k.intensity * std::pow(s.level, 0.6f) * master * std::clamp(w.intensity[k.cat], 0.0f, 3.0f);
        if (w.flicker && k.flicker > 0.0f) I *= flameFlicker(t, s.id >> 4, k.flicker);
        ExtraLight l{};
        toView(view, s.pos, l.pos);
        for (int i = 0; i < 3; i++) l.color[i] = rgb[i] / mx;
        l.radius = k.radius * (1.0f + 0.15f * (group - 1.0f));
        l.intensity = I;
        l.flags = aurora::gfx::ExtraLightSpecular | aurora::gfx::ExtraLightTranslucent;
        const bool flame = (k.cat == port::kWlCandles || k.cat == port::kWlFires || k.cat == port::kWlFireFx) && k.rgb[2] < 120;
        if (flame) l.flags |= aurora::gfx::ExtraLightFire;  // yellow core, red-orange falloff (0025)
        // Shadows (0028): fireplaces, braziers and torches, candelabras (three or more candles
        // together) and big fire effects; the strongest few get a shadow slot (selectLights).
        // With more than 8 shadow slots (12 / 16 lights), smaller candle groups and single candles
        // become candidates too; the strongest still win the slots (selectLights).
        const bool many = enh::shadowLightCap() > 8;
        if (k.cat == port::kWlFires || (k.cat == port::kWlCandles && s.count >= (many ? 1 : 3)) ||
            (k.cat == port::kWlFireFx && k.radius >= 450.0f) || (many && k.cat == port::kWlLamps))
            l.flags |= aurora::gfx::ExtraLightCastShadows;
        l.id = 0x57000000u ^ s.id;
        enh::limitToRoom(l, roomAt(s.pos), s.pos);
        lights.push_back(l);
        if (logNow)
            log("[worldlight] f%u light %-12s x%d lit %.2f world %.0f,%.0f,%.0f view %.0f,%.0f,%.0f I %.2f r %.0f", sFrame, k.prefix,
                s.count, s.level, s.pos[0], s.pos[1], s.pos[2], l.pos[0], l.pos[1], l.pos[2], l.intensity, l.radius);
    }

    // Lamps and chandeliers of lit rooms (the light-on bit a cleared room has), visible rooms
    // only, in the room's own light color (roominfo LightColorR/G/B; warm white fallback).
    if (w.on[port::kWlLamps] || sForce) {
        buildLamps();
        int nl = 0;
        for (const Lamp& lp : sLamps) {
            if (lp.room < 0 || !roomVisible(lp.room)) continue;
            // The room's light switching on or off fades the fitting's light (~6 frames).
            float& on = sLampLevel[lp.id];
            on += ((roomLit(lp.room) ? 1.0f : 0.0f) - on) * 0.3f;
            if (on < 0.01f) {
                on = 0.0f;
                continue;
            }
            const auto c = sRoomColor.find(lp.room);
            const u32 rgb8 = c != sRoomColor.end() && c->second ? c->second : 0xFFD8A0u;
            const u32 cc[3] = {rgb8 >> 16, (rgb8 >> 8) & 0xFF, rgb8 & 0xFF};
            float rgb[3], mx = 1e-4f;
            for (int i = 0; i < 3; i++) mx = std::max(mx, rgb[i] = srgbToLinear(cc[i]));
            ExtraLight l{};
            toView(view, lp.pos, l.pos);
            for (int i = 0; i < 3; i++) l.color[i] = rgb[i] / mx;
            if (sillyworld::discoOn()) sillyworld::discoLight(u32(lp.room), l.color);  // F1 > Enhancements > Silly
            l.radius = lp.chandelier ? 950.0f : 520.0f;
            float I = (lp.chandelier ? 1.5f : 1.0f) * master * std::clamp(w.intensity[port::kWlLamps], 0.0f, 3.0f);
            if (w.flicker && lp.chandelier) I *= flameFlicker(t, lp.id, 0.12f);  // candle-lit chandeliers breathe a little
            l.intensity = I * on;
            l.flags = aurora::gfx::ExtraLightSpecular | aurora::gfx::ExtraLightTranslucent |
                      (enh::shadowLightCap() > 8 ? aurora::gfx::ExtraLightCastShadows : 0u);  // 12 / 16 slots: fittings cast too
            l.id = lp.id;
            enh::limitToRoom(l, lp.room, lp.pos);
            lights.push_back(l);
            nl++;
            if (logNow)
                log("[worldlight] f%u lamp %s room %d world %.0f,%.0f,%.0f rgb %06X", sFrame, lp.chandelier ? "chandelier" : "lamp",
                    lp.room, lp.pos[0], lp.pos[1], lp.pos[2], rgb8);
        }
    }

    // Game Boy Horror screen: the GBH actor (Luigi +0x7AC, character type 0x0A) hangs on Luigi's
    // belt (~77 units up) and is raised to his face during calls; only a raised GBH gives its
    // screen's soft green glow (the belt one stays dark).
    if (w.on[port::kWlSparkles] || sForce) {
        const u32 lu = rd32(VA(0x803D48A0));
        const u32 gbh = guestPtr(lu) ? rd32(lu + 0x7AC) : 0;
        if (guestPtr(gbh) && !(rd32(gbh + 0x40) & 0x6000) && rd32(gbh + 0x3C) == 0x0A) {
            const float p[3] = {rdf32(gbh + 0x44), rdf32(gbh + 0x48) + 10.0f, rdf32(gbh + 0x4C)};
            const float up = rdf32(gbh + 0x48) - rdf32(lu + 0x48);
            if (sLog && logNow) log("[worldlight] f%u gbh %.0f above Luigi", sFrame, up);
            if ((p[0] != 0.0f || p[2] != 0.0f) && up > 105.0f) {
                ExtraLight l{};
                toView(view, p, l.pos);
                l.color[0] = 0.35f, l.color[1] = 1.0f, l.color[2] = 0.45f;
                l.radius = 230.0f;
                l.intensity = 0.7f * master * std::clamp(w.intensity[port::kWlSparkles], 0.0f, 3.0f);
                l.flags = aurora::gfx::ExtraLightTranslucent;
                l.id = 0x57006000u;
                enh::limitToRoom(l, roomAt(p), p);
                lights.push_back(l);
                if (logNow) log("[worldlight] f%u gbh glow at %.0f,%.0f,%.0f", sFrame, p[0], p[1], p[2]);
            }
        }
    }

    // Poltergust element spray: Luigi's Poltergust mode (+0x1180: 2 fire, 3 water, 4 ice, while L
    // is held; nozzles.cpp) and the nozzle actor (+0x7A4, position +0x44 at the nozzle). The
    // spray itself is a model (elefire.bmd), not particles: the light sits ~170 units ahead of
    // the nozzle (horizontal direction Luigi -> nozzle), ramps in over a few frames and out.
    {
        static float sSpray = 0.0f;
        static int sElem = 0;
        static float sPos[3] = {};
        const u32 lu = rd32(VA(0x803D48A0));
        const int mode = guestPtr(lu) ? s32(rd32(lu + 0x1180)) : 0;
        const u32 noz = guestPtr(lu) ? rd32(lu + 0x7A4) : 0;
        const bool spraying = (mode >= 2 && mode <= 4) && guestPtr(noz) && (w.on[port::kWlSprays] || sForce);
        if (spraying) {
            sElem = mode;
            const float lp[3] = {rdf32(lu + 0x44), rdf32(lu + 0x48), rdf32(lu + 0x4C)};
            const float np[3] = {rdf32(noz + 0x44), rdf32(noz + 0x48), rdf32(noz + 0x4C)};
            float dx = np[0] - lp[0], dz = np[2] - lp[2];
            const float len = std::sqrt(dx * dx + dz * dz);
            if (len > 1.0f) dx /= len, dz /= len;
            sPos[0] = np[0] + dx * 170.0f, sPos[1] = np[1] + 10.0f, sPos[2] = np[2] + dz * 170.0f;
        }
        sSpray += ((spraying ? 1.0f : 0.0f) - sSpray) * (spraying ? 0.35f : 0.25f);
        if (sSpray > 0.01f && sElem) {
            static const u8 kRgb[3][3] = {{255, 150, 55}, {70, 140, 255}, {150, 230, 255}};
            static const float kI[3] = {2.2f, 0.7f, 0.7f}, kR[3] = {750.0f, 480.0f, 480.0f};
            const int e = sElem - 2;
            float rgb[3], mx = 1e-4f;
            for (int i = 0; i < 3; i++) mx = std::max(mx, rgb[i] = srgbToLinear(kRgb[e][i]));
            ExtraLight l{};
            toView(view, sPos, l.pos);
            for (int i = 0; i < 3; i++) l.color[i] = rgb[i] / mx;
            float I = kI[e] * sSpray * master * std::clamp(w.intensity[port::kWlSprays], 0.0f, 3.0f);
            if (e == 0 && w.flicker) I *= flameFlicker(t * 1.6f, 77, 1.0f);
            l.radius = kR[e];
            l.intensity = I;
            l.flags = aurora::gfx::ExtraLightSpecular | aurora::gfx::ExtraLightTranslucent |
                      (e == 0 ? aurora::gfx::ExtraLightFire | aurora::gfx::ExtraLightCastShadows : 0u);
            l.id = 0x57005000u;
            enh::limitToRoom(l, roomAt(sPos), sPos);
            lights.push_back(l);
            if (logNow) log("[worldlight] f%u spray element %d world %.0f,%.0f,%.0f I %.2f", sFrame, sElem, sPos[0], sPos[1], sPos[2], I);
        }
    }

    // Lightning: a cool flash from the strike's light, flickering twice and fading over ~1.5 s.
    if ((w.on[port::kWlLightning] || sForce) && sStrikeFrame && sFrame - sStrikeFrame < 45) {
        const float a = float(sFrame - sStrikeFrame);
        const float env = (a < 3.0f ? 1.0f : a < 6.0f ? 0.35f : a < 9.0f ? 0.9f : std::exp(-(a - 9.0f) / 9.0f));
        ExtraLight l{};
        // The strike points the game's shadow light camera at the window (camera +0x24 eye):
        // the flash comes from there.
        const u32 cam = kCamera;
        sStrikePos[0] = rdf32(cam + 0x24), sStrikePos[1] = rdf32(cam + 0x28), sStrikePos[2] = rdf32(cam + 0x2C);
        // That eye is far outside (thousands of units): the point light goes 700 units from Luigi
        // toward it, so the room gets the flash from the window side without a huge radius.
        const u32 lu = rd32(VA(0x803D48A0));
        if (guestPtr(lu)) {
            const float lp[3] = {rdf32(lu + 0x44), rdf32(lu + 0x48) + 100.0f, rdf32(lu + 0x4C)};
            float d[3] = {sStrikePos[0] - lp[0], sStrikePos[1] - lp[1], sStrikePos[2] - lp[2]};
            const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (len > 700.0f)
                for (int i = 0; i < 3; i++) sStrikePos[i] = lp[i] + d[i] / len * 700.0f;
        }
        if (logNow) log("[worldlight] f%u lightning flash at %.0f,%.0f,%.0f env %.2f", sFrame, sStrikePos[0], sStrikePos[1], sStrikePos[2], env);
        toView(view, sStrikePos, l.pos);
        const float c[3] = {0.62f, 0.78f, 1.0f};
        for (int i = 0; i < 3; i++) l.color[i] = c[i];
        l.radius = 1900.0f;
        l.intensity = 2.5f * env * master * std::clamp(w.intensity[port::kWlLightning], 0.0f, 3.0f);
        l.flags = aurora::gfx::ExtraLightSpecular | aurora::gfx::ExtraLightTranslucent;
        l.id = 0x5700FFFFu;
        // The flash lights Luigi's room (it comes in through that room's windows).
        if (guestPtr(lu)) {
            const float lp[3] = {rdf32(lu + 0x44), rdf32(lu + 0x48) + 100.0f, rdf32(lu + 0x4C)};
            enh::limitToRoom(l, roomAt(lp), sStrikePos);
        }
        if (l.intensity > 0.01f) lights.push_back(l);
    }

    if (sAB && (sFrame & 1)) return;
    if (!lights.empty()) submitExtraLights(lights.data(), u32(lights.size()));
    if (logNow) {
        log("[worldlight] f%u %d emitters, %zu lights", sFrame, n, lights.size());
        const u32 lu = rd32(VA(0x803D48A0));
        if (guestPtr(lu)) {
            const u32 noz = rd32(lu + 0x7A4);
            const float lp[3] = {rdf32(lu + 0x44), rdf32(lu + 0x48) + 50.0f, rdf32(lu + 0x4C)};
            const int lr = roomAt(lp);
            log("[worldlight] f%u luigi room id %u table room %d lit %d visible %d", sFrame, rd32(VA(0x803A3CA0) + 0xC) >> 24, lr,
                int(roomLit(lr)), lr >= 0 ? int(roomVisible(lr)) : -1);
            log("[worldlight] f%u luigi %.0f,%.0f,%.0f v50 %.2f,%.2f,%.2f v5c %.2f,%.2f,%.2f mode %d nozzle %.0f,%.0f,%.0f", sFrame,
                rdf32(lu + 0x44), rdf32(lu + 0x48), rdf32(lu + 0x4C), rdf32(lu + 0x50), rdf32(lu + 0x54), rdf32(lu + 0x58),
                rdf32(lu + 0x5C), rdf32(lu + 0x60), rdf32(lu + 0x64), s32(rd32(lu + 0x1180)), guestPtr(noz) ? rdf32(noz + 0x44) : 0.f,
                guestPtr(noz) ? rdf32(noz + 0x48) : 0.f, guestPtr(noz) ? rdf32(noz + 0x4C) : 0.f);
        }
    }
}

// JKRArchive::getResource(type, name): remember particle resources by their data pointer.
HLE(getResource__10JKRArchiveFUlPCc) {
    const u32 namePtr = c.r[5];
    callOriginal(c, "getResource__10JKRArchiveFUlPCc");
    if (!guestPtr(namePtr) || !guestPtr(c.r[3])) return;
    const char* name = gstr(namePtr);
    const size_t len = strnlen(name, 64);
    vravatar::notePlayerSceneResource(name);
    noteResource(name, c.r[3]);
    if (len > 4 && !std::strcmp(name + len - 4, ".pcr")) {
        std::string base(name, len - 4);
        if (const size_t slash = base.rfind('/'); slash != std::string::npos) base.erase(0, slash + 1);
        sRes[c.r[3]] = {base, kindFor(base)};
    }
}

// LPS emitter creation (manager, .pcr data, texture, ...): returns the emitter.
HLE(fn_8015FE9C) {
    const u32 res = c.r[4];
    const u32 lr = c.lr, sp = c.r[1];
    callOriginal(c, "fn_8015FE9C");
    if (guestPtr(c.r[3])) {
        sEmitterRes[c.r[3]] = res;sFlame.erase(c.r[3]); // pooled emitters are reused
        if(std::strcmp(emitterName(c.r[3]),"key_get")==0) vravatar::notePickup();
    }
    if (sLog >= 3 && guestPtr(c.r[3])) {
        u32 s = sp, chain[4] = {};
        for (int i = 0; i < 4 && guestPtr(s); i++) chain[i] = rd32((s = rd32(s)) + 4);
        log("[worldlight] create emitter %08X %s lr %08X<%08X<%08X<%08X", c.r[3], emitterName(c.r[3]), lr, chain[0], chain[1], chain[2]);
    }
}

// Lightning strike (thunder light entry: light fov/near/far, ...).
HLE(fn_80029250) {
    const u32 entry = c.r[3];
    callOriginal(c, "fn_80029250");
    if (!guestPtr(entry)) return;
    sStrikeFrame = sFrame;
    if (sLog) log("[worldlight] f%u lightning strike (thunder entry %08X)", sFrame, entry);
}

}  // namespace recomp::worldlights
