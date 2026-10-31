// Unused assets restored as F1 > Enhancements > Unused content options (all off by default).
// Data-table patches in guest memory plus resource redirects through the game's own archive
// lookup; no game code is rewritten. Findings and addresses: docs/UNUSED_CONTENT.md.
#include "port/settings.h"
#include "recomp/hle.h"
#include "recomp/unused2.h"
#include "recomp/world_lights.h"
#include "recomp/vr_avatar_guest.h"
#include "recomp/runtime.h"
#include "recomp/version.h"

#include <dolphin/dvd.h>

#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace recomp {
namespace {

bool envFlag(const char* name) {
    const char* v = std::getenv(name);
    return v && *v && *v != '0';
}

const port::EnhancementSettings& opt() { return port::settings().enhancements; }

// ---- Guest-memory patches with verified originals ----------------------------------------

struct Patch32 {
    u32 addr, original, value;
};
struct Patch8 {
    u32 addr;
    u8 original, value;
};

// Writes `value` (on) or `original` (off), but only over one of the two known values, so a
// table that is not what we expect is left alone and reported once.
template <typename P, typename Rd, typename Wr>
void applyPatches(const P* list, size_t n, bool on, const char* what, Rd rd, Wr wr) {
    static bool sWarned = false;
    for (size_t i = 0; i < n; i++) {
        const auto cur = rd(list[i].addr);
        const auto want = on ? list[i].value : list[i].original;
        if (cur == want) continue;
        if (cur != list[i].original && cur != list[i].value) {
            if (!sWarned) log("[unused] %s: unexpected value %X at %08X, not patching", what, u32(cur), list[i].addr);
            sWarned = true;
            continue;
        }
        wr(list[i].addr, want);
    }
}

// Portrificationizer: table lbl_8036E52C (0x18 per ghost; read by fn_80121E14/fn_80124890).
// Field +4 of entries 1..21 is 0x60, the generic blue blob; each ghost's own portrait-scene
// models are the unused ids below (the same values as the known Gecko code). The European
// table has the same layout (VA() maps it).
#define kPortraitTable VA(0x8036E52C)
constexpr u32 kPortraitModels[] = {0x064, 0x121, 0x125, 0x129, 0x12D, 0x131, 0x135, 0x139, 0x13D, 0x141, 0x145,
                                   0x149, 0x14D, 0x151, 0x155, 0x159, 0x15D, 0x161, 0x165, 0x169, 0x16D};

void applyPortraits() {
    static const bool sForce = envFlag("LM_UNUSED_PORTRAITS");
    static std::vector<Patch32> sList;
    if (sList.empty() && kPortraitTable)
        for (size_t i = 0; i < std::size(kPortraitModels); i++)
            sList.push_back({kPortraitTable + 0x18 * u32(i + 1) + 4, 0x60, kPortraitModels[i]});
    applyPatches(sList.data(), sList.size(), sForce || opt().unusedPortraitModels, "portrait table",
                 [](u32 a) { return rd32(a); }, [](u32 a, u32 v) { wr32(a, v); });
}

// Map 2 door table (lbl_802FF218[2]->+0x18, 0x1C per door). Byte +6 is the door type: it picks
// the model (lbl_802FF868: /iwamoto/Door/door_NN.bin) and the lock/unlock cutscenes
// (fn_8011496C: scene 3 + (type-1)*4 + ..., i.e. oo/co/ho/nodemoNN.szp). Types 07 (heart) and
// 08 (star) have models and all four cutscenes but no door uses them. Door 46 is the Nursery
// (Chauncey) door, door 45 the Twins' room door; both ship as type 01.
// Reached through the map table (lbl_802FF218[2] +0x18; USA 0x80302F80), which also finds the
// European copy (identical doors at another address).
constexpr u32 kDoorSize = 0x1C, kHeartDoorNo = 46, kStarDoorNo = 45;
u32 mansionDoors() {
    const u32 map2 = rd32(VA(0x802FF218) + 4 * 2);
    const u32 doors = (map2 >> 28) == 8 ? rd32(map2 + 0x18) : 0;
    return (doors >> 28) == 8 ? doors : 0;
}

void applyDoors() {
    static const bool sHeart = envFlag("LM_HEART_DOOR"), sStar = envFlag("LM_STAR_DOOR");
    auto rd = [](u32 a) { return u8(rd8(a)); };
    auto wr = [](u32 a, u8 v) { wr8(a, v); };
    const u32 doors = mansionDoors();
    if (!doors) return;
    const Patch8 heart[] = {{doors + kDoorSize * kHeartDoorNo + 6, 0x01, 0x07}};
    const Patch8 star[] = {{doors + kDoorSize * kStarDoorNo + 6, 0x01, 0x08}};
    applyPatches(heart, 1, sHeart || opt().heartDoor, "heart door", rd, wr);
    applyPatches(star, 1, sStar || opt().starDoor, "star door", rd, wr);
}

// ---- Host-side archive reading (for the beta GBH-map Luigi) ------------------------------

std::vector<u8> readDisc(const char* path) {
    DVDFileInfo fi{};
    if (!DVDOpen(path, &fi)) return {};
    std::vector<u8> buf((fi.length + 31) & ~31u);
    const s32 got = DVDReadPrio(&fi, buf.data(), s32(buf.size()), 0, 2);
    DVDClose(&fi);
    if (got <= 0) return {};
    buf.resize(fi.length);
    return buf;
}

u32 be32(const u8* p) { return u32(p[0]) << 24 | u32(p[1]) << 16 | u32(p[2]) << 8 | p[3]; }
u16 be16(const u8* p) { return u16(p[0] << 8 | p[1]); }

std::vector<u8> yaz0(const std::vector<u8>& in) {
    const u32 size = be32(&in[4]);
    std::vector<u8> out;
    out.reserve(size);
    size_t p = 16;
    while (out.size() < size && p < in.size()) {
        const u8 code = in[p++];
        for (int b = 7; b >= 0 && out.size() < size; b--) {
            if (code & (1 << b)) {
                out.push_back(in[p++]);
            } else {
                const u8 b1 = in[p++], b2 = in[p++];
                const size_t dist = ((b1 & 0x0F) << 8 | b2) + 1;
                size_t n = b1 >> 4;
                n = n ? n + 2 : size_t(in[p++]) + 0x12;
                for (size_t i = 0; i < n; i++) out.push_back(out[out.size() - dist]);
            }
        }
    }
    return out;
}

std::vector<u8> yay0(const std::vector<u8>& in) {
    const u32 size = be32(&in[4]);
    u32 linkOff = be32(&in[8]), chunkOff = be32(&in[12]), maskOff = 16, mask = 0;
    int bits = 0;
    std::vector<u8> out;
    out.reserve(size);
    while (out.size() < size) {
        if (bits == 0) {
            mask = be32(&in[maskOff]);
            maskOff += 4;
            bits = 32;
        }
        if (mask & 0x80000000u) {
            out.push_back(in[chunkOff++]);
        } else {
            const u16 link = be16(&in[linkOff]);
            linkOff += 2;
            const size_t dist = (link & 0xFFF) + 1;
            size_t n = link >> 12;
            n = n ? n + 2 : size_t(in[chunkOff++]) + 0x12;
            for (size_t i = 0; i < n; i++) out.push_back(out[out.size() - dist]);
        }
        mask <<= 1;
        bits--;
    }
    return out;
}

// Returns the bytes of the first file called `name` in a (possibly compressed) RARC.
std::vector<u8> rarcFile(std::vector<u8> data, const char* name) {
    if (data.size() >= 16 && !std::memcmp(data.data(), "Yaz0", 4)) data = yaz0(data);
    else if (data.size() >= 16 && !std::memcmp(data.data(), "Yay0", 4)) data = yay0(data);
    if (data.size() < 0x40 || std::memcmp(data.data(), "RARC", 4)) return {};
    const u32 dataOff = be32(&data[0x0C]) + 0x20;
    const u32 info = 0x20;
    const u32 numEntries = be32(&data[info + 0x08]);
    const u32 entryOff = be32(&data[info + 0x0C]) + info;
    const u32 strOff = be32(&data[info + 0x14]) + info;
    for (u32 i = 0; i < numEntries; i++) {
        const u8* e = &data[entryOff + i * 20];
        const u32 flagsName = be32(e + 4), off = be32(e + 8), size = be32(e + 12);
        if ((flagsName >> 24) & 0x02) continue;  // directory
        const char* n = reinterpret_cast<const char*>(&data[strOff + (flagsName & 0xFFFFFF)]);
        if (!std::strcmp(n, name)) return {data.begin() + dataOff + off, data.begin() + dataOff + off + size};
    }
    return {};
}

// Guest copy of a string (for redirected resource paths).
u32 guestString(const char* s) {
    const u32 n = u32(std::strlen(s) + 1);
    const u32 g = hleAlloc(n, 4);
    std::memcpy(hostp(g), s, n);
    return g;
}

// ---- Beta skybox -------------------------------------------------------------------------
// Map 2's sky descriptor (lbl_80300B60, via lbl_80300C7C[map]) loads /vrball01.bmd and
// /vrball01.btk from /Iwamoto/vrball_M.szp. The always-mounted game archive
// (fn_80066FB4) also carries an older sky under iwamoto/vrbox/ that nothing loads.
#define kSkyDesc VA(0x804D8380)     // current sky descriptor (lbl_804D8380)
#define kMansionSky VA(0x80300B60)  // map 2 descriptor
bool sSkyLoading = false;

u32 gameArchive(Ctx& c) {
    static const u32 fn = guestSymbol("fn_80066FB4");
    const Ctx saved = c;
    const u32 a = guestCall(c, fn);
    c = saved;
    return a;
}

// ---- Beta Game Boy Horror map Luigi ------------------------------------------------------
// The GBH map loads /gide_lui.bin (the Luigi marker drawn at Luigi's position, model
// lbl_804D830C + 0x318) from the guide-map archive (guidemap.szp in the DOL's static data).
// The disc still has an older guide-map archive, /Iwamoto/map2/gidemap.szp, whose
// gide_lui.bin is a full Luigi model instead of the retail head.
u32 sBetaLuigi = 0, sBetaLuigiSize = 0;
bool sBetaLuigiTried = false;

void loadBetaLuigi() {
    if (sBetaLuigiTried) return;
    sBetaLuigiTried = true;
    const std::vector<u8> f = rarcFile(readDisc("/Iwamoto/map2/gidemap.szp"), "gide_lui.bin");
    if (f.empty()) {
        log("[unused] /Iwamoto/map2/gidemap.szp: gide_lui.bin not found");
        return;
    }
    sBetaLuigi = hleAlloc(u32(f.size()), 32);
    sBetaLuigiSize = u32(f.size());
    std::memcpy(hostp(sBetaLuigi), f.data(), f.size());
    log("[unused] beta GBH-map Luigi loaded (%u bytes)", sBetaLuigiSize);
}

}  // namespace

// Portrificationizer scenes: both readers of the portrait table patch it first.
HLE(fn_80121E14) {
    applyPortraits();
    callOriginal(c, "fn_80121E14");
}
HLE(fn_80124890) {
    applyPortraits();
    callOriginal(c, "fn_80124890");
}

// Map setup (doors are instantiated from the door table right after this).
HLE(fn_800122BC) {
    applyDoors();
    callOriginal(c, "fn_800122BC");
}

// Sky load callback (fn_800067D0 finished mounting the sky archive).
HLE(fn_8002F4DC) {
    static const bool sForce = envFlag("LM_BETA_SKY");
    sSkyLoading = (sForce || opt().betaSkybox) && rd32(kSkyDesc) == kMansionSky;
    log("[unused] sky load: descriptor %08X%s", rd32(kSkyDesc), sSkyLoading ? " (beta)" : "");
    callOriginal(c, "fn_8002F4DC");
    sSkyLoading = false;
}

// JKRArchive::getResource(const char*): redirects for the options above.
HLE(getResource__10JKRArchiveFPCc) {
    const char* path = gstr(c.r[4]);
    vravatar::notePlayerSceneResource(path);
    if (sSkyLoading) {
        static u32 sBmd = 0, sBtk = 0;
        if (!sBmd) sBmd = guestString("/iwamoto/vrbox/vrball01.bmd"), sBtk = guestString("/iwamoto/vrbox/vrball01.btk");
        const u32 alt = !std::strcmp(path, "/vrball01.bmd") ? sBmd : !std::strcmp(path, "/vrball01.btk") ? sBtk : 0;
        if (alt) {
            const u32 arc = gameArchive(c);
            if (arc) {
                c.r[3] = arc;
                c.r[4] = alt;
                log("[unused] beta skybox: %s -> %s", path, gstr(alt));
            }
        }
    } else if (const u32 alt = unused2::resourceRedirect(path)) {
        c.r[4] = alt;  // colour variants: the unused .prm (placements.cpp)
    } else if (!std::strcmp(path, "/gide_lui.bin")) {
        static const bool sForce = envFlag("LM_BETA_MAP_LUIGI");
        if (sForce || opt().betaMapLuigi) {
            loadBetaLuigi();
            if (sBetaLuigi) {
                c.r[3] = sBetaLuigi;
                return;
            }
        }
    }
    callOriginal(c, "getResource__10JKRArchiveFPCc");
    worldlights::noteResource(path, c.r[3]);  // world_lights.cpp: the map's object/room tables
}

// JKRArchive::getResSize(const void*): size of the redirected GBH-map Luigi.
HLE(getResSize__10JKRArchiveCFPCv) {
    if (sBetaLuigi && c.r[4] == sBetaLuigi) {
        c.r[3] = sBetaLuigiSize;
        return;
    }
    callOriginal(c, "getResSize__10JKRArchiveCFPCv");
}


u16 padButtons();  // hle_pad.cpp
u32 copyDispCount();  // hle_gx.cpp

// Called once per gameplay frame (after the GBH HUD update).
void unusedAssetsFrame(Ctx& c) {
    static const bool sDump = envFlag("LM_NOZZLE_DUMP");
    if (!sDump) return;
    const u32 actor = rd32(VA(0x803D48A0));  // actor slot 0 = Luigi
    if (actor < 0x80000000u || actor >= 0x81800000u) return;
    constexpr u32 kSize = 0x1400;
    static std::vector<u8> idle(kSize), prev(kSize), noisy(kSize);
    static bool havePrev = false, prevL = false;
    static int held = 0;
    const u8* cur = hostp(actor);
    const bool l = padButtons() & 0x0040;
    const u32 st = rd32(actor + 0x7E4);
    {
        // Luigi's Poltergust mode (+0x1180: 0 none, 1 vacuum, 2-4 element, 5 flashlight, 6 air),
        // element (+0x1184), meter (+0x1188) and the nozzle actor's character type ([+0x7A4] +0x3C).
        static std::string lastF;
        char b[160];
        const u32 nz = rd32(actor + 0x7A4);
        std::snprintf(b, sizeof b, "btn %04X mode %d element %d meter %.0f nozzle type %d", padButtons(),
                      s32(rd32(actor + 0x1180)), s32(rd32(actor + 0x1184)), std::bit_cast<float>(rd32(actor + 0x1188)),
                      nz ? s32(rd32(nz + 0x3C)) : -1);
        if (lastF != b) log("[nozzle] frame %u: %s", copyDispCount(), b), lastF = b;
    }
    if (st >= 0x80000000u && st < 0x81800000u) {
        static u32 last = ~0u;
        const u32 v = rd32(st + 0x310) << 16 | rd32(st + 0x314) << 8 | rd32(st + 0x334);
        static int sN = 0;
        if (v != last || sN++ % 30 == 0) {
            std::string in;
            for (u32 i = 0; i < 0x50; i += 4) {
                char b[12];
                std::snprintf(b, sizeof b, " %08X", rd32(st + i));
                in += b;
            }
            log("[nozzle] action %u/%u/%u L=%d timers %.2f %.2f in:%s", rd32(st + 0x310), rd32(st + 0x314), rd32(st + 0x334), l,
                std::bit_cast<float>(rd32(actor + 0x1114)), std::bit_cast<float>(rd32(actor + 0x110C)), in.c_str());
        }
        last = v;
    }
    if (havePrev && !l && !prevL)
        for (u32 i = 0; i < kSize; i++) noisy[i] |= cur[i] != prev[i];
    if (!l) std::memcpy(idle.data(), cur, kSize), held = 0;
    else if (++held == 15) {
        std::string s;
        for (u32 i = 0; i < kSize; i++)
            if (!noisy[i] && cur[i] != idle[i]) s += " " + std::to_string(i) + ":" + std::to_string(idle[i]) + ">" + std::to_string(cur[i]);
        log("[nozzle] L-held diff:%s", s.c_str());
    }
    std::memcpy(prev.data(), cur, kSize);
    havePrev = true;
    prevL = l;
}

}  // namespace recomp
