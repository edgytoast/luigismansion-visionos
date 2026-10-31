// Unused air and water nozzles (F1 > Enhancements > Unused content, off by default).
//
// The Poltergust's nozzle is one actor (Luigi +0x7A4) that the game re-types on the fly:
// Luigi's mode selector fn_800B36B8 picks the mode (+0x1180: 1 vacuum, 2-4 fire/water/ice,
// 5 flashlight, 6 air) and, on a change, calls fn_800B9064(luigi, type), which turns the
// nozzle into character type 6 (`vhead`) for every mode except the flashlight, which uses
// type 7 (`wplight`). Types 8 (`wpwater`) and 9 (`wpair`) are registered right after them,
// with their own models and their own attach tables in fn_80078008, but the switch never
// selects them. With the option on, the mode selector runs unchanged and the nozzle is then
// re-typed through the same function: water spray -> wpwater, air -> wpair. No map ever
// requests those two models, so they are loaded first with the game's own model loader.
// Fire, ice and the vacuum keep `vhead`, the flashlight `wplight`. Docs: docs/UNUSED_CONTENT.md.
#include "port/install.h"
#include "port/settings.h"
#include "recomp/hle.h"
#include "recomp/runtime.h"
#include "recomp/version.h"
#include "recomp/vr_host.h"
#include "recomp/vr_avatar_guest.h"

#include <cstdlib>
#include <bit>
#include "recomp/skip.h"

namespace recomp {
namespace {

constexpr u32 kNozzle = 0x7A4;   // nozzle actor (MoveObj); character type at +0x3C
constexpr u32 kMode = 0x1180;    // Poltergust mode chosen by fn_800B36B8
constexpr u32 kCharType = 0x3C;
constexpr u32 kModeWater = 3, kModeAir = 6, kModeLight = 5;
constexpr u32 kTypeHose = 6, kTypeLight = 7, kTypeWater = 8, kTypeAir = 9;

bool enabled() {
    static const bool sEnv = [] {
        const char* v = std::getenv("LM_NOZZLES");
        return v && *v && *v != '0';
    }();
    return sEnv || port::settings().enhancements.airWaterNozzles;
}

bool inMem1(u32 a) { return a >= 0x80000000u && a < 0x81800000u; }

// Character models: type table lbl_8033B590 (0x1C per type, +0 = model index) and model
// registry lbl_8034E46C (0x34 per model: +0 file name, +8 loaded .mdl, +0x30 load state).
// The game loads a model when a map needs it (fn_8006227C at map setup; per-event models via
// fn_800620F4 from the event code, JmpMessage). wpwater/wpair are never needed, so their
// entries stay empty and re-typing the nozzle to them would leave it without a model.
// Both releases (European fn_800B36B8/fn_800B9064 are identical; VA() maps the tables).
#define kTypeTable VA(0x8033B590)
#define kModelRegistry VA(0x8034E46C)

u32 modelEntry(u32 type) { return kModelRegistry + 0x34 * rd32(kTypeTable + 0x1C * type); }

// True once the type's model is resident; otherwise starts the game's own async load
// (fn_800620F4: disc read into the map's model heap, then the same setup as the map loader).
// The registry is cleared at every map setup, so this runs again after each map change.
bool modelReady(Ctx& c, u32 type) {
    const u32 e = modelEntry(type);
    if (rd32(e + 8)) return true;
    if (rd32(e + 0x30) == 0) {
        static const u32 sLoad = guestSymbol("fn_800620F4__Fi");
        const Ctx saved = c;
        guestCall(c, sLoad, rd32(kTypeTable + 0x1C * type));
        c = saved;
        log("[nozzle] loading %s", gstr(rd32(e)));
    }
    return false;
}

}  // namespace

// Luigi's Poltergust mode selector (once per Luigi update).
HLE(fn_800B36B8) {
    const u32 luigi = c.r[3];
    const u32 nozzle = inMem1(luigi) ? rd32(luigi + kNozzle) : 0;
    // Luigi's own type 1/0xB3 uses its 0xB5.. nozzle set, which has no air/water models.
    const u32 self = inMem1(luigi) ? rd32(luigi + kCharType) : 0;
    if (!inMem1(nozzle) || self == 1 || self == 0xB3) {
        callOriginal(c, "fn_800B36B8");
        return;
    }
    const bool on = enabled();
    const u32 oldMode=rd32(luigi+kMode),motion=rd32(luigi+0x7EC),controller=rd32(luigi+0x7E4);
    std::array<u32,7> before{};
    const bool motionValid=inMem1(motion+0x2DB)&&inMem1(controller+0x317);
    if(motionValid)for(unsigned i=0;i<7;++i)before[i]=rd32(motion+0x2C0+4*i);
    callOriginal(c, "fn_800B36B8");
    // Native mode changes request the body/arm transition through fn_8007D748
    // and fn_8007D76C. Its short state-1 blend inhibits walking. Tracked VR
    // hands already present the held nozzle, so retain the prior locomotion
    // transition state while keeping the native mode/attack update intact.
    static const bool blendTest=std::getenv("LM_VR_VACUUM_START_TEST")!=nullptr;
    const bool vrMode=vrhost::firstPerson()||(blendTest&&vravatar::diagnosticEnabled());
    if(motionValid&&!skip::cinematicCamera()&&vr::bypassVacuumStartBlend(vrMode,oldMode,rd32(luigi+kMode),rd32(controller+0x310),before[0],rd32(motion+0x2C0))) {
        for(unsigned i=0;i<7;++i)wr32(motion+0x2C0+4*i,before[i]);
        static unsigned reports=0;if(reports++<8)log("[vr vacuum locomotion] native suction starts without the body-only transition pause");
    }

    static const bool trace=std::getenv("LM_VR_VACUUM_START_TRACE")!=nullptr;
    if(trace&&inMem1(luigi+0x1183)) {
        const u32 ctrl=rd32(luigi+0x7E4),motion=rd32(luigi+0x7EC);
        if(inMem1(ctrl+0x317)&&inMem1(motion+0x2C3))log("[vr vacuum start] f%u pos %.3f %.3f %.3f ctrl %u/%u motion %u mode %u timer %.2f/%.2f",skip::guestFrames(),std::bit_cast<float>(rd32(luigi+0x44)),std::bit_cast<float>(rd32(luigi+0x48)),std::bit_cast<float>(rd32(luigi+0x4C)),rd32(ctrl+0x310),rd32(ctrl+0x314),rd32(motion+0x2C0),rd32(luigi+kMode),std::bit_cast<float>(rd32(luigi+0x1114)),std::bit_cast<float>(rd32(luigi+0x110C)));
    }
    const u32 mode = rd32(luigi + kMode);
    const u32 type = rd32(nozzle + kCharType);
    u32 want = type;
    // Load both as soon as the option is on, so the first spray already has its model.
    const bool waterReady = on && modelReady(c, kTypeWater);
    const bool airReady = on && modelReady(c, kTypeAir);
    if (waterReady && mode == kModeWater) want = kTypeWater;
    else if (airReady && mode == kModeAir) want = kTypeAir;
    else if (type == kTypeWater || type == kTypeAir) want = mode == kModeLight ? kTypeLight : kTypeHose;  // back to retail
    // Render the held nozzle even before its trigger starts suction. Native mode
    // logic still decides attacks/elements; this changes only the model type.
    const char* toolTest=std::getenv("LM_VR_TOOL_TEST");
    const bool diagnosticTools=vravatar::diagnosticEnabled()&&toolTest&&toolTest[0]=='1';
    if((vrhost::beltMode()||diagnosticTools)&&vravatar::captureAnchor().actorId==luigi) {
        unsigned hand=0;float trigger=0;
        const bool vacuum=diagnosticTools||vrhost::heldEquipmentControl(hand,trigger,vr::GrabKind::Poltergust);
        want=vacuum?kTypeHose:kTypeLight;
        if((rd16(luigi+0x792)&4)&&!vravatar::toolModelReady(vr::GrabKind::Poltergust))want=kTypeHose;
        else if(!vravatar::toolModelReady(vr::GrabKind::Flashlight))want=kTypeLight;
    }
    if (want != type) {
        static const u32 sRetype = guestSymbol("fn_800B9064");
        const Ctx saved = c;
        guestCall(c, sRetype, luigi, want);
        c = saved;
        static int sLogged = 0;
        if (sLogged++ < 40) log("[nozzle] mode %u: nozzle type %u -> %u", mode, type, want);
    }
}

}  // namespace recomp
