// Luigi states left unfinished in the retail code (F1 > Enhancements > Gameplay, all off by
// default; both releases). Docs: docs/UNUSED_CONTENT.md ("Second batch").
//
// Luigi's pieces used here (USA offsets):
//   +0x0FC  health (s16), set only through fn_800B9488(luigi, hp)
//   +0x7E4  action controller: +0x310 state (1 free, 2 busy), +0x314 sub-state, +0x68 a table of
//           motion ids per motion slot, +0x360 a motion-slot override that only the reset code
//           ever writes (fn_80073AF8 returns slot +0x360's motion when it is not 0)
//   +0x7E8  fear state: +0 mode (1 calm, 2/3 scared, 4 panic), +4 time left, mode table at +0x24
//           (lbl_80354B9C: per mode duration, damage factor, next mode per event). fn_8007CE04(obj,
//           0, event) advances it, but no retail code calls it with 0: only the reset (1) is used.
//   +0x1004/+0x1008 Poltergust heat / overheat lock. fn_800B3BAC still lets the lock (+0x1008 > 0)
//           block the vacuum and counts it down, but resets the heat (+0x1004) to its maximum
//           every frame, so the cut overheat never happens. The status block (+0x80C) still
//           exports the heat as the element meter value when no element is equipped.
//   message handler fn_800AEEA8(luigi, id, msg): 2 startle, 3 attack, 0x38 size mushroom.
//
// Poison: message 0x38 with a scale below 1 is the shrinking (poison) mushroom. With the option
// on it starts a poison instead: the controller's motion slot 0x41 (motion 42, key doku_lp; the
// slot exists in Luigi's slot table but nothing selects it) is forced through the override, the
// key's texture pattern doku_lp.txp gives the dark "poisoned" eyes, Luigi stands still, and his
// health drops by the set amount over the set time. The coin loss of the mushroom is kept, and
// so is its sound, but not the high voice that starting that sound switches on (voicePitch).
//
// Crawling fear: when a startle (message 2) comes from behind Luigi, the fear state gets the
// game's own "panic" event (mode table +0x24: every mode -> mode 4). Mode 4 (300 frames) blocks
// the Poltergust (fn_800B36B8, fn_800B4CA8), halves damage (factor 0.5) and starts the panic
// effect (fn_800B7090 -> fn_80164F5C(0xE)); when it runs out fn_8007D244 sends the controller
// request 10, the get-up sub-state. While it lasts the controller itself picks the cowering
// motions (cow1wait sitting, cow1move scooting along the floor; keys never seen in retail).
//
// Overheat: heat rises while vacuuming and falls otherwise (faster while spraying with L); full
// heat sets the game's own lock +0x1008 (the vacuum stops, fn_800B3BAC counts it down), burns
// Luigi through the game's damage path (fn_800B9124, fire type) and shakes the meter. The meter
// is the element meter, shown with the fire colours while the heat is above zero.
#include "recomp/vr_host.h"
#include "recomp/skip.h"
#include "port/install.h"
#include "port/settings.h"
#include "recomp/coop.h"
#include "recomp/dev_tools.h"
#include "recomp/gameplay.h"
#include "recomp/guest_util.h"
#include "recomp/hle.h"
#include "recomp/runtime.h"
#include "recomp/skip.h"
#include "recomp/unused2.h"
#include "recomp/version.h"
#include "recomp/widescreen.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace recomp::unused2 {
namespace {

bool envFlag(const char* n) {
    const char* v = std::getenv(n);
    return v && *v && *v != '0';
}
// Both releases: the Luigi fields used here sit below +0x134C (unchanged on the European game),
// the functions are matched by name, data through VA(), patch 51 has a European line.
bool usCode() { return true; }
bool poisonOn() {
    static const bool e = envFlag("LM_POISON");
    return usCode() && (e || port::settings().unused2.poisonState);
}
bool crawlOn() {
    static const bool e = envFlag("LM_CRAWL_FEAR");
    return usCode() && (e || port::settings().unused2.crawlFear);
}
bool overheatOn() {
    static const bool e = envFlag("LM_OVERHEAT");
    return usCode() && (e || port::settings().unused2.overheat);
}

constexpr u32 kHp = 0xFC, kCtrl = 0x7E4, kFear = 0x7E8, kMode = 0x1180, kElement = 0x1184;
constexpr u32 kHeatLock = 0x1008, kPos = 0x44, kFacing = 0xF48, kMotion = 0xC4;
constexpr u32 kCtrlState = 0x310, kCtrlSub = 0x314, kCtrlOverride = 0x360;
constexpr u32 kSlotPoison = 0x41;  // -> motion 42 doku_lp
constexpr u32 kModePanic = 4;

// Guest pointer in MEM1 or the host-backed area above it (extraAlloc blocks).
bool gptr(u32 a) { return a >= 0x80000000u && a < 0x81F00000u; }

u32 sMsgBuf = 0;  // a message block for the game's handler (0x200 bytes, extra memory)
u32 msgBuf(Ctx& c) {
    if (!sMsgBuf) sMsgBuf = extraAlloc(0x200);
    if (sMsgBuf) gu::call(c, GU_SYM("fn_800AD3C0"), {sMsgBuf});
    return sMsgBuf;
}

u32 luigi(Ctx& c) {
    const u32 p = gu::call(c, GU_SYM("fn_800E9750__Q24Koga9EnManagerFl"), {0});
    return gu::inMem1(p) && rd32(p + 0x3C) == 0 ? p : 0;  // type 0 = Luigi
}

// ---- poison ----
// Luigi's voice pitch (float; 1.0 normal, 2.0 while shrunk by the poison mushroom). The sound
// starter fn_80189F78 sets it to 2.0 when it starts sound 0x847 (shrink) and to 1.0 for 0x846
// (grow back; also fn_80189468(0/1) and the audio scene setup fn_8018C9F8), and applies it to
// every Luigi voice line (sounds 0x2xxx).
u32 voicePitch() { return VA(0x804D783C); }
constexpr u32 kShrinkTime = 0x1170, kShrinkAnim = 0x1174;

// A voice pitch left raised by an older build's beta poison (it started sound 0x847 without the
// 0x846 that undoes it) is put back while Luigi is not shrunk by a real mushroom.
void poisonPitchFix(u32 l) {
    const u32 p = voicePitch();
    if (!p || gu::rdf(p) == 1.0f) return;
    if (gu::rdf(l + kShrinkTime) > 0.0f || gu::rdf(l + kShrinkAnim) > 0.0f) return;
    log("[states] voice pitch %.2f with Luigi not shrunk: reset to 1.0", gu::rdf(p));
    gu::wrf(p, 1.0f);
}

struct Poison {
    u32 actor = 0;
    float left = 0;      // health still to lose
    float perFrame = 0;  // health per frame
    float acc = 0;
    u32 frames = 0;
} sPoison;

void startPoison(Ctx& c, u32 l) {
    const auto& u = port::settings().unused2;
    sPoison.actor = l;
    sPoison.left = u.poisonDamage;
    sPoison.frames = u32(u.poisonSeconds * 30.0f + 0.5f);
    sPoison.perFrame = sPoison.left / float(sPoison.frames ? sPoison.frames : 1);
    sPoison.acc = 0;
    log("[states] poisoned: %.0f health over %u frames", sPoison.left, sPoison.frames);
    (void)c;
}

void poisonFrame(Ctx& c, u32 l) {
    if (!sPoison.actor) return;
    const u32 ctrl = rd32(l + kCtrl);
    if (sPoison.actor != l || !gu::inMem1(ctrl)) {
        sPoison = {};
        return;
    }
    if (sPoison.frames == 0 || s16(rd16(l + kHp)) <= 0) {
        if (rd32(ctrl + kCtrlOverride) == kSlotPoison) wr32(ctrl + kCtrlOverride, 0);
        log("[states] poison over (health %d)", s16(rd16(l + kHp)));
        sPoison = {};
        return;
    }
    wr32(ctrl + kCtrlOverride, kSlotPoison);  // player 1's controls are held meanwhile (hle_pad.cpp)
    sPoison.frames--;
    sPoison.acc += sPoison.perFrame;
    if (sPoison.acc >= 1.0f && sPoison.left > 0) {
        const int d = int(sPoison.acc);
        sPoison.acc -= float(d);
        sPoison.left -= float(d);
        const int hp = s16(rd16(l + kHp));
        // Written directly: the health setter (fn_800B9488) would burst hearts and drop coins
        // for every point. Poison alone never finishes Luigi.
        if (hp > 1) wr16(l + kHp, u16(hp - d < 1 ? 1 : hp - d));
    }
}

// ---- crawling fear ----
u32 sCrawlActor = 0;

void startPanic(Ctx& c, u32 l) {
    const u32 fear = rd32(l + kFear);
    if (!gu::inMem1(fear) || rd32(fear) == kModePanic) return;
    const u32 m = msgBuf(c);
    if (!m) return;
    wr32(m + 4, 0);
    wr32(m + 8, 0);   // >= 0: no position for the scare source
    wr32(m + 0xC, 2); // event 2: next mode = table +0x24 (4 from every mode)
    gu::call(c, GU_SYM("fn_8007CE04"), {fear, 0, m});
    sCrawlActor = l;
    log("[states] crawling fear: fear mode -> %u", rd32(fear));
}

void crawlFrame(Ctx& c, u32 l) {
    (void)c;
    if (!sCrawlActor) return;
    const u32 fear = rd32(l + kFear);
    if (sCrawlActor != l || !gu::inMem1(fear) || rd32(fear) != kModePanic) {
        log("[states] crawling fear over");
        sCrawlActor = 0;
    }
}

// ---- overheat ----
struct Heat {
    float heat = 0;   // 0..1
    bool hot = false; // overheated (vacuum locked)
    u32 actor = 0;
} sHeat;

void overheatFrame(Ctx& c, u32 l) {
    const auto& u = port::settings().unused2;
    if (sHeat.actor != l) sHeat = {0, false, l};
    const u32 mode = rd32(l + kMode);
    const bool vacuum = mode == 1;
    const bool spray = mode == 2 || mode == 3 || mode == 4 || mode == 6;
    if (vacuum && !sHeat.hot) {
        sHeat.heat += 1.0f / (u.overheatFillSeconds * 30.0f);
    } else {
        float cool = 1.0f / (u.overheatCoolSeconds * 30.0f);
        if (spray) cool *= u.overheatBlowCool;
        sHeat.heat -= cool;
    }
    if (sHeat.heat < 0) sHeat.heat = 0;
    if (sHeat.heat >= 1.0f && !sHeat.hot) {
        sHeat.heat = 1.0f;
        sHeat.hot = true;
        // The game's own lock: no vacuum until it has counted down (1 per frame while not vacuuming).
        gu::wrf(l + kHeatLock, 90.0f);
        // Burn through the game's own element-hit message (0x2A kind 0: on fire, burn_lop/burn_end).
        const u32 m = msgBuf(c);
        if (m) {
            wr32(m + 0xAC, 0);
            wr32(m + 0xB0, u32(u.overheatDamage));
            const u32 r = gu::call(c, GU_SYM("fn_800AEEA8"), {l, 0x2A, m});
            log("[states] Poltergust overheated: Luigi catches fire (%d damage) %s", u.overheatDamage, r ? "" : "(refused)");
        }
    }
    if (sHeat.hot && gu::rdf(l + kHeatLock) < 1.0f) gu::wrf(l + kHeatLock, 1.0f);  // keep locked until cool
    if (sHeat.hot && sHeat.heat < 0.5f) {
        sHeat.hot = false;
        gu::wrf(l + kHeatLock, 0.0f);
    }
}

// ---- test aids ----
struct Timed {
    int a = 0, b = 0;
    u32 from = 0, to = 0;
};
std::vector<Timed> parse(const char* env) {
    std::vector<Timed> v;
    const char* s = std::getenv(env);
    for (const char* p = s; p && *p;) {
        Timed t;
        int from = 0, to = 0;
        if (std::sscanf(p, "%d:%d@%d-%d", &t.a, &t.b, &from, &to) == 4 || std::sscanf(p, "%d@%d-%d", &t.a, &from, &to) == 3) {
            t.from = u32(from), t.to = u32(to);
            v.push_back(t);
        } else if (std::sscanf(p, "%d:%d@%d", &t.a, &t.b, &from) == 3 || std::sscanf(p, "%d@%d", &t.a, &from) == 2) {
            t.from = t.to = u32(from);
            v.push_back(t);
        }
        const char* comma = std::strchr(p, ',');
        if (!comma) break;
        p = comma + 1;
    }
    return v;
}

void testAids(Ctx& c, u32 l) {
    const u32 now = skip::guestFrames();
    static const auto sSlots = parse("LM_TEST_SLOT");       // slot@from-to
    static const auto sDamage = parse("LM_TEST_DAMAGE");    // type:damage@frame
    static const auto sFear = parse("LM_TEST_FEAR");        // event@frame
    static const auto sPoisonT = parse("LM_TEST_POISON");   // 0@frame
    static const auto sStartle = parse("LM_TEST_STARTLE");  // behind(0/1)@frame
    const u32 ctrl = rd32(l + kCtrl);
    for (const auto& t : sSlots)
        if (now >= t.from && now <= t.to) wr32(ctrl + kCtrlOverride, now == t.to ? 0 : u32(t.a));
    for (const auto& t : sDamage)
        if (now == t.from) {
            const u32 m = msgBuf(c);
            wr32(m + 0x18, u32(t.b));
            wr32(m + 0x20, u32(t.a));
            const u32 r = gu::call(c, GU_SYM("fn_800B9124"), {l, m, 0});
            log("[states] test damage type %d (%d): %u", t.a, t.b, r);
        }
    for (const auto& t : sFear)
        if (now == t.from) {
            const u32 m = msgBuf(c);
            wr32(m + 8, 0);
            wr32(m + 0xC, u32(t.a));
            gu::call(c, GU_SYM("fn_8007CE04"), {rd32(l + kFear), 0, m});
            sCrawlActor = l;
            log("[states] test fear event %d: mode %u", t.a, rd32(rd32(l + kFear)));
        }
    static const auto sElem = parse("LM_TEST_ELEM");  // kind:damage@frame (message 0x2A: 0 burn?, 1, 2)
    for (const auto& t : sElem)
        if (now == t.from) {
            const u32 m = msgBuf(c);
            wr32(m + 0xAC, u32(t.a));
            wr32(m + 0xB0, u32(t.b));
            const u32 r = gu::call(c, GU_SYM("fn_800AEEA8"), {l, 0x2A, m});
            log("[states] test element hit %d (%d): %u", t.a, t.b, r);
        }
    for (const auto& t : sPoisonT)
        if (now == t.from && t.a == 1) {
            // What older builds' beta poison did: start the mushroom sound 0x847 alone, which
            // leaves the voice pitch raised (poisonPitchFix puts it back).
            gu::call(c, GU_SYM("fn_80189F78"), {0x847, l + kPos, 0});
            log("[states] test: sound 0x847 alone, voice pitch %.2f", gu::rdf(voicePitch()));
        } else if (now == t.from) {
            // As the poison mushroom: message 0x38, scale 0.5 (float) for 300 frames (an int: the
            // handler converts it to the float shrink timer +0x1170).
            const u32 m = msgBuf(c);
            gu::wrf(m + 0xFC, 0.5f);
            wr32(m + 0x100, 300);
            const u32 r = gu::call(c, GU_SYM("fn_800AEEA8"), {l, 0x38, m});
            log("[states] test mushroom: %u", r);
        }
    for (const auto& t : sStartle)
        if (now == t.from) {
            // A startle message from a point 150 units in front of / behind Luigi.
            const u32 m = msgBuf(c);
            const u32 pt = m + 0x180;
            const float sgn = t.a ? -1.0f : 1.0f;
            for (int i = 0; i < 3; i++)
                gu::wrf(pt + 4 * i, gu::rdf(l + kPos + 4 * i) + (i == 1 ? 0.0f : sgn * 150.0f * gu::rdf(l + kFacing + 4 * i)));
            wr32(m + 4, pt);
            wr32(m + 8, u32(-1));
            const u32 r = gu::call(c, GU_SYM("fn_800AEEA8"), {l, 2, m});
            log("[states] test startle (%s): %u", t.a ? "behind" : "front", r);
        }
    static const bool sLog = envFlag("LM_STATE_LOG");
    if (sLog) {
        static u32 sLast[7] = {};
        const u32 fear = rd32(l + kFear);
        const u32 cur[7] = {rd32(ctrl + kCtrlState), rd32(ctrl + kCtrlSub), rd32(l + kMotion), gu::inMem1(fear) ? rd32(fear) : 0,
                            u32(s16(rd16(l + kHp))), rd32(l + kMode), rd32(voicePitch())};
        if (std::memcmp(cur, sLast, sizeof cur)) {
            std::memcpy(sLast, cur, sizeof cur);
            log("[states] f%u ctrl %u/%u motion %u fear %u hp %d mode %u heat %.2f voice pitch %.2f shrink %.0f", now, cur[0],
                cur[1], cur[2], cur[3], s32(cur[4]), cur[5], sHeat.heat, gu::rdf(voicePitch()), gu::rdf(l + kShrinkTime));
        }
    }
}

}  // namespace

// Player 1's controls are held (stick centred, buttons but START released) while poisoned.
bool playerHeld() { return sPoison.actor != 0; }

void statesMapSetup(Ctx&) {
    sPoison = {};
    sCrawlActor = 0;
    sHeat = {};
}

void statesFrame(Ctx& c) {
    if (!usCode()) return;
    static int sPanicPatch = -1;
    // A panic that is still running keeps both fixes until it ends, even when the option was
    // turned off meanwhile: with the retail factor back, mode 4 would halve Luigi's health
    // every frame and kill him within a few frames.
    const bool panicFix = crawlOn() || sCrawlActor != 0;
    if (int(panicFix) != sPanicPatch) {
        sPanicPatch = panicFix;
        setPatch(51, sPanicPatch != 0);  // patch 51: no panic effect 0xE
        // fn_800B61C8 multiplies the maximum health by the fear mode's factor every frame (mode 4:
        // 0.5, so Luigi's health halves each frame and he dies in a few frames). With the option on,
        // mode 4's factor is 1.0 (data written only over the retail value).
        // lbl_80354B9C (mode table; reached through the pointer lbl_804D2428 so the European copy
        // is found too), mode 4 entry (+0x9C) +8.
        const u32 f = rd32(VA(0x804D2428)) + 0xA4;
        const u32 want = sPanicPatch ? 0x3F800000u : 0x3F000000u;
        if (rd32(f) == 0x3F000000u || rd32(f) == 0x3F800000u) wr32(f, want);
        else log("[states] fear table value %08X unexpected, left alone", rd32(f));
    }
    if (!dev::inGameScene()) return;  // Luigi exists only in the game scene
    const u32 l = luigi(c);
    if (!l || !gu::inMem1(rd32(l + kCtrl))) return;
    testAids(c, l);
    poisonFrame(c, l);
    if (poisonOn()) poisonPitchFix(l);
    crawlFrame(c, l);
    if (overheatOn()) overheatFrame(c, l);
    else if (sHeat.heat > 0 || sHeat.hot) {
        if (sHeat.hot) gu::wrf(l + kHeatLock, 0.0f);
        sHeat = {};
    }
}

// The HUD reads the heat for the element meter while it is above zero.
bool overheatMeter(int* value, bool* hot) {
    if (!overheatOn() || sHeat.heat <= 0.0f) return false;
    *value = int(std::ceil(sHeat.heat * 10.0f));
    *hot = sHeat.hot;
    return true;
}

}  // namespace recomp::unused2

namespace recomp {

// Element meter update (meter object): takes the element (+0x58) and the meter value (+0x52) from
// player 0's status block (fn_800AD39C(0)). While the Poltergust's heat is above zero the meter
// is presented the heat (0..10) with the fire element's colours.
HLE(fn_80045F58) {
    int value = 0;
    bool hot = false;
    // Test aids: LM_METER_LOG=1 logs the meter object and player 0's status block on change;
    // LM_METER_FORCE=<n> sets the status block's level (+0x52), which the update passes to the
    // draw. With no element the fill is drawn with vertex alpha 0 (invisible on hardware).
    static const bool sLog = std::getenv("LM_METER_LOG") != nullptr || std::getenv("LM_METER_FORCE") != nullptr;
    if (sLog) {
        const u32 obj = c.r[3], r13 = c.r[13];
        const u32 s = gu::call(c, GU_SYM("fn_800AD39C"), {0});
        c.r[3] = obj;
        static std::string sLast;
        char b[256];
        std::snprintf(b, sizeof b, "obj %08X +4 %u +5 %u +10 %d +14 %d +1E %d | st+52 %d st+58 %d | hudA %u y %d",
                      obj, rd8(obj + 4), rd8(obj + 5), s32(rd32(obj + 0x10)), s32(rd32(obj + 0x14)),
                      s16(rd16(obj + 0x1E)), gu::inMem1(s) ? s16(rd16(s + 0x52)) : -1,
                      gu::inMem1(s) ? s32(rd32(s + 0x58)) : -1, rd8(r13 - 0x7A32), s16(rd16(VA(0x804D8434))));
        if (sLast != b) log("[meter] %s", b), sLast = b;
        static const char* sForce = std::getenv("LM_METER_FORCE");
        if (sForce && gu::inMem1(s)) wr16(s + 0x52, u16(std::atoi(sForce)));
    }
    const u32 st = unused2::overheatMeter(&value, &hot) ? gu::call(c, GU_SYM("fn_800AD39C"), {0}) : 0;
    if (!gu::inMem1(st)) {
        callOriginal(c, "fn_80045F58");
        return;
    }
    const u32 element = rd32(st + 0x58);
    const u16 meter = rd16(st + 0x52);
    wr32(st + 0x58, 2);
    wr16(st + 0x52, u16(value));
    callOriginal(c, "fn_80045F58");
    wr32(st + 0x58, element);
    wr16(st + 0x52, meter);
}

// Element meter draw: shakes while the Poltergust is overheated (meter x at lbl_804D858C).
HLE(fn_80046120) {
    // Suppress the complete meter draw (frame, icon and fill), not its update.
    if(ws::hideGameplayHud())return;
    int value = 0;
    bool hot = false;
    if (!unused2::overheatMeter(&value, &hot) || !hot) {
        callOriginal(c, "fn_80046120");
        return;
    }
    const u32 x = VA(0x804D858C), y = VA(0x804D858E);
    const u16 ox = rd16(x), oy = rd16(y);
    static u32 sN = 0;
    sN++;
    const s16 dx = s16((sN * 7) % 5) - 2, dy = s16((sN * 3) % 3) - 1;
    wr16(x, u16(s16(ox) + dx));
    wr16(y, u16(s16(oy) + dy));
    callOriginal(c, "fn_80046120");
    wr16(x, ox);
    wr16(y, oy);
}

// Luigi's message handler (self, message id, message block).
HLE(fn_800AEEA8) {
    using namespace unused2;
    const u32 l = c.r[3], id = c.r[4], msg = c.r[5];
    if (id == 0x38 && poisonOn() && gptr(msg) && gu::rdf(msg + 0xFC) < 1.0f && rd32(l + 0x3C) == 0) {
        // Same conditions as the mushroom case: not busy, not already changed size.
        const bool busy = rd8(l + 0x1142) != 0 || gu::call(c, GU_SYM("fn_80074D60"), {rd32(l + kCtrl)}) != 0 ||
                          gu::rdf(l + 0x1170) != 0.0f;
        if (!busy && !sPoison.actor) {
            startPoison(c, l);
            // The mushroom's sound. Starting sound 0x847 also sets the voice pitch (lbl_804D783C,
            // 2.0 = the shrunk Luigi's high voice) and only sound 0x846 (played when the shrink
            // timer +0x1170 runs out) sets it back, so the pitch is kept as it was.
            const float pitch = gu::rdf(voicePitch());
            gu::call(c, GU_SYM("fn_80189F78"), {0x847, l + kPos, 0});
            gu::wrf(voicePitch(), pitch);
            const u32 m = msgBuf(c);
            if (m) {
                wr32(m + 0x90, 10);  // the 10 coins the mushroom takes (message 0x25, as the original)
                gu::call(c, GU_SYM("fn_800AEEA8"), {l, 0x25, m});
            }
        }
        c.r[3] = 1;
        return;
    }
    if(id==2&&vrhost::firstPerson()&&!skip::cinematicCamera()&&gptr(msg)) {
        const u32 slot=rd32(msg+8),sourcePosition=rd32(msg+4);
        u32 source=slot<128?rd32(VA(0x803D48A0)+4*slot):0;
        if(s32(slot)<0&&gu::inMem1(sourcePosition))for(unsigned i=0;i<128;++i) {
            const u32 actor=rd32(VA(0x803D48A0)+4*i);
            if(gptr(actor)&&actor+0x44==sourcePosition){source=actor;break;}
        }
        {
            if(gptr(source)&&gu::inMem1(source+0x3F)) {
                const u32 type=rd32(source+0x3C);
                if(type<0x29C8/0x1C&&(rd16(VA(0x8033B590)+0x1C*type+8)&8)) {
                    static unsigned reports=0;if(reports++<8)log("[vr encounter] ghost startle kept in live first person (type %u)",type);
                    c.r[3]=1;return;
                }
            }
        }
    }
    gameplay::callLuigiMessage(c);  // gameplay_hooks.cpp (Luigi's health, recovery hearts)
    if (id == 2 && c.r[3] && crawlOn() && gptr(msg)) {
        const u32 src = gu::call(c, GU_SYM("fn_800AD504"), {rd32(msg + 4), rd32(msg + 8)});
        if (gptr(src)) {
            float d = 0;
            for (int i = 0; i < 3; i += 2) d += (gu::rdf(src + 4 * i) - gu::rdf(l + kPos + 4 * i)) * gu::rdf(l + kFacing + 4 * i);
            if (d < 0) startPanic(c, l);
        }
    }
}

}  // namespace recomp
