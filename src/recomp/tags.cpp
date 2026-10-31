// Identity tags for frame interpolation (interp.h, docs/INTERPOLATION.md "Tagged interpolation").
//
// Hooks on the game's draw entry points open a tag scope around each thing they draw, so every
// matrix, light and CPU-built vertex block the GX HLE notes inside carries a stable identity: the
// scope path (view > object > part > mesh), the GX call site and its occurrence there. The
// interpolator pairs values between game frames only by identical identities.
//
// Each hook wraps the original (callOriginal) and changes nothing else. Addresses are USA; hooks are
// tagged `us pal` in tools/recomp/hooks.txt where the European function was checked to take the same
// arguments, and call-site ranges go through VA().
#include "recomp/hle.h"
#include "recomp/interp.h"
#include "recomp/mirror_world.h"
#include "recomp/silly_world.h"
#include "recomp/version.h"
#include "recomp/vr_host.h"
#include "recomp/vr_avatar_guest.h"
#include "recomp/widescreen.h"
#include "recomp/guest_util.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <bit>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace recomp {

u32 copyDispCount();  // hle_gx.cpp

// LM_TRACK_LOG (interp.cpp, docs/INTERPOLATION.md "The hose nozzle"): Luigi (actor slot 0), his
// Poltergust (+0x7A4) and nozzle (+0x7A8) actors' world places (+0x44).
bool interp::detail::trackPoints(float out[9]) {
    const auto inMem1 = [](u32 p) { return p >= 0x80000000u && p < 0x81800000u; };
    const u32 l = rd32(VA(0x803D48A0));
    if (!inMem1(l)) return false;
    const u32 actors[3] = {l, rd32(l + 0x7A4), rd32(l + 0x7A8)};
    for (int k = 0; k < 3; k++)
        for (int c = 0; c < 3; c++)
            out[3 * k + c] = inMem1(actors[k]) ? std::bit_cast<float>(rd32(actors[k] + 0x44 + 4 * c)) : std::nanf("");
    return std::isfinite(out[0] + out[1] + out[2]);
}

namespace {

using interp::TagCat;
using interp::TagScope;

// Furniture/object scopes open: bin models drawn inside are objects, outside them map geometry.
int sObjectDepth = 0;
u32 sRoomModel=0;
struct FoyerLowerModel {u32 model=0,table=0,upper=0,upperTable=0;bool drawing=false;unsigned frame=~0u,upperFrame=~0u;} sFoyerLower;

struct RoomModelScope {
    u32 previous;
    explicit RoomModelScope(u32 model):previous(sRoomModel) {sRoomModel=model;}
    ~RoomModelScope() {sRoomModel=previous;}
};
struct RoomFrameScope {
    u32 parts=0;
    u16 count=0;
    explicit RoomFrameScope(int node) {
        if(!vrhost::hideRoomFrames() || !sRoomModel || node<0 || node>4095 || !gu::inMem1(sRoomModel+0x3C)) return;
        const u32 table=rd32(sRoomModel+0x3C), address=table+u32(node)*0x8C;
        if(!gu::inMem1(table) || !gu::inMem1(address) || !gu::inMem1(address+0x8B) || !(rd16(address+8)&0x80)) return;
        // Keep tree traversal and other room parts; suppress only this fadeable node's own batches.
        parts=address+0x4C;count=rd16(parts);wr16(parts,0);
    }
    ~RoomFrameScope() {if(parts) wr16(parts,count);}
};
void traceRoomNode(int node) {
    static const bool enabled=std::getenv("LM_VR_ROOM_TRACE")!=nullptr;
    if(!enabled || !vrhost::enabled() || !sRoomModel || node<0 || node>4095 || !gu::inMem1(sRoomModel+0x3C)) return;
    const u32 table=rd32(sRoomModel+0x3C), address=table+u32(node)*0x8C;
    if(!gu::inMem1(table) || !gu::inMem1(address) || !gu::inMem1(address+0x8B)) return;
    static std::unordered_set<u64> seen;
    static u32 map=~0u;
    const u32 current=rd32(VA(0x804D80C8));
    if(current!=map) {map=current;seen.clear();}
    if(seen.size()>2048 || !seen.insert((u64(sRoomModel)<<16)|u16(node)).second) return;
    log("[vr room] map %u model %08X node %d flags %04X parts %u bounds %.1f %.1f %.1f -> %.1f %.1f %.1f",
        current,sRoomModel,node,rd16(address+8),rd16(address+0x4C),
        gu::rdf(address+0x30),gu::rdf(address+0x34),gu::rdf(address+0x38),
        gu::rdf(address+0x3C),gu::rdf(address+0x40),gu::rdf(address+0x44));
}
// A stream item drawn with a shared S3D model (fn_800F3120 from fn_8011FB40): the model's group is
// per item, not per model (u32 item address; 0: none).
u32 sStreamItem = 0;
struct ObjectDepth {
    ObjectDepth() { sObjectDepth++; }
    ~ObjectDepth() { sObjectDepth--; }
};

// Call-site tags: draw loops that handle one item per iteration inside one function.
void registerSiteTags() {
    static bool done = false;
    if (done) return;
    done = true;
    // (VA() needs the version map, which is ready once the game runs.)
    // Dust and other particle groups (fn_801304B0 -> fn_8012FD14(manager, view, group)): one
    // particle per iteration in r30, its group in r29.
    interp::addSiteTag({VA(0x8012FD14), VA(0x8012FE84), TagCat::Particle, 30, 29});
    // Second particle draw (fn_8012CB74 -> fn_8012B6C4(manager, view, group)): particle r29, group r28.
    // Its remaining life counts down in the u16 at +0x24 (98% of frames in the vacuum scenario).
    interp::addSiteTag({VA(0x8012B6C4), VA(0x8012B918), TagCat::Particle, 29, 28, 0x24, -1});
    // fn_80162AE8 -> fn_801796EC: 45 fixed slots of 0x18 bytes (array at [r25]), slot offset in r28.
    interp::addSiteTag({VA(0x801796EC), VA(0x8017996C), TagCat::Particle, 28, 25});
    // fn_80091A70: a cloth sheet (16x16 grid in world space) drawn as one GXBegin quad per cell from
    // four grid points (column r27, row offset r29); cells whose points are not valid are skipped, so
    // the n-th quad is not always the same cell. (Not the Poltergust hose, which is part of an S3D model.)
    interp::addSiteTag({VA(0x80091EF4), VA(0x80092110), TagCat::Effect, 27, 29});
    // JPA-style emitters (fn_801604FC -> renderer -> fn_8016169C(renderer, view)): one particle per
    // iteration in r19 (list via +0x5C), its renderer in r30; CPU-built quads.
    // Its age (+1 per frame, every particle) is the u16 at +0x32.
    interp::addSiteTag({VA(0x8016169C), VA(0x80161ECC), TagCat::Particle, 19, 30, 0x32, 1});
    // The other JPA-style renderers: fn_801620AC (particle r19, renderer r26, age +0x32) and
    // fn_801611CC (child items r30 via +0x24, owner r29): Bowser's flames, the boss arenas' skies.
    interp::addSiteTag({VA(0x801620AC), VA(0x801628E4), TagCat::Particle, 19, 26, 0x32, 1});
    interp::addSiteTag({VA(0x801611CC), VA(0x80161618), TagCat::Particle, 30, 29});
}

}  // namespace

namespace tags {
void init() { registerSiteTags(); }
}  // namespace tags

// --- views -------------------------------------------------------------------------------------
// (fn_8000BB98, the per-view scene draw, is hooked in enhanced_lighting.cpp: it opens the View scope.)

// Events 5/6 (Mario in the painting, the 2F bathroom): a model drawn with its own camera into a
// 128x128 copy (fn_8006033C, docs/RENDERING.md "Indirect effects").
HLE(fn_8006033C) {
    registerSiteTags();
    TagScope v(TagCat::View, VA(0x8006033C), 0, c.r[3]);
    callOriginal(c, "fn_8006033C");
}

// --- S3DModel (characters, Luigi, ghosts, Poltergust, props drawn as actors) ---------------------
// Model draws take the model instance in r3 (its data at +4); node draws (model, node index);
// fn_80058A60 loads a mesh's matrices (model, mesh entry), fn_800595B8 draws it (model, mesh).
HLE(fn_80059C54) {
    ws::beforeS3dOpaqueDraw(c);
    vravatar::DrawPose avatar(c,c.r[3]);
    registerSiteTags();
    TagScope t(TagCat::Object, c.r[3], rd32(c.r[3] + 4));
    interp::tagGroup(c.r[3]);  // one instance: its matrices blend alike (interp.cpp, replayPair)
    const u32 object=c.r[3];
    if(!avatar.hidden())callOriginal(c, "fn_80059C54");
    vravatar::drawOtherTool(c,object,false);
}
HLE(fn_80059F6C) {
    vravatar::DrawPose avatar(c,c.r[3]);
    registerSiteTags();
    TagScope t(TagCat::Object, c.r[3], rd32(c.r[3] + 4));
    // One instance: its matrices blend alike (interp.cpp, replayPair). A shared model drawn once per
    // stream item is one instance per item.
    interp::tagGroup(sStreamItem ? sStreamItem : c.r[3]);
    const u32 item = sStreamItem;
    sStreamItem = 0;  // (not for models drawn further inside)
    const u32 object=c.r[3];
    if(!avatar.hidden())callOriginal(c, "fn_80059F6C");
    vravatar::drawOtherTool(c,object,true);
    sStreamItem = item;
}

// The element streams' items (fn_8011FB40; water drops, fire and ice bits): two loops set ONE shared
// S3D model's matrix from each item (fn_80059710) and draw it through fn_800F3120(holder) ->
// fn_80059F6C(holder + 8). Without a scope per item the draws of all items were one model instance
// paired by occurrence: drops swapped partners (random turns, shown at the game frame) and one
// respawning drop switched all of them (~320 per frame in the Pipe Room) to 30 Hz (sweep A; the
// water spray's shimmer). Loop 1 (call at 0x801201F4): items of 0x34 bytes at [r31 + 0x4C] + r27;
// loop 2 (0x801202F8): items of 0x68 bytes at [r31 + 0x48] + r26.
//
// Loop 1's items are not slots that keep their drop: the list is rebuilt every game frame (its n-th
// item is another drop from one game frame to the next: of the drops followed in the nursery, 12% were
// in the same item, the rest anywhere in the list; LM_STREAM_LOG). Paired by item, drops blended
// toward other drops 100-700 units away, or were switched: "jumps" and "turns" at the game frame in
// the judder detector. Each loop-1 drop is followed by its place instead (DropTracker: the nearest
// drop of the game frame before, carried by its own last step) and that track is its identity. Loop
// 2's items keep their drop; a slot whose life counter (+3) went up holds a new one (new generation).
// Every drop takes a random orientation every game frame: tumblers (interp::tagTumbler). A drop
// without a predecessor moves into its place along the step of the nearest followed drop (or, with
// none within 80 units, of the nearest stream point within 120: the first drops of a burst)
// (interp::tagBackStep) instead of standing still for its first interval.
namespace {
struct DropTracker {
    struct Drop {
        u32 id;
        float p[3], step[3];
        bool stepKnown;
    };
    u32 frame = ~0u;
    std::vector<Drop> prev, cur;
    std::vector<u8> taken;
    std::unordered_map<u32, size_t> byItem;  // this game frame: item -> index into cur (drawn in several views)
    u32 nextId = 0;
    void beginFrame(u32 f) {
        if (f == frame) return;
        if (f == frame + 1) prev.swap(cur);
        else prev.clear();
        cur.clear();
        byItem.clear();
        taken.assign(prev.size(), 0);
        frame = f;
    }
    // The track of a drop at world place w (item `item`), and its step since the game frame before.
    Drop follow(u32 item, const float w[3]) {
        if (const auto it = byItem.find(item); it != byItem.end()) return cur[it->second];
        s32 best = -1;
        float bestD = 1e30f;
        for (size_t k = 0; k < prev.size(); k++) {
            if (taken[k]) continue;
            const Drop& q = prev[k];
            float d2 = 0, s2 = 0;
            for (int c = 0; c < 3; c++) {
                const float pr = q.p[c] + (q.stepKnown ? q.step[c] : 0.0f);
                d2 += (w[c] - pr) * (w[c] - pr), s2 += q.stepKnown ? q.step[c] * q.step[c] : 0.0f;
            }
            const float tol = q.stepKnown ? 8.0f + 0.3f * std::sqrt(s2) : 40.0f;
            if (d2 <= tol * tol && d2 < bestD) bestD = d2, best = s32(k);
        }
        Drop d{};
        std::copy(w, w + 3, d.p);
        if (best >= 0) {
            taken[best] = 1;
            const Drop& q = prev[best];
            d.id = q.id;
            for (int c = 0; c < 3; c++) d.step[c] = w[c] - q.p[c];
            d.stepKnown = true;
        } else {
            // A new track: an id no track of this or the last game frame has (12 bits: the drops'
            // scope addresses stay few).
            for (;;) {
                d.id = ++nextId & 0xFFF;
                bool clash = d.id == 0;
                for (const Drop& q : prev) clash |= q.id == d.id;
                for (const Drop& q : cur) clash |= q.id == d.id;
                if (!clash) break;
            }
        }
        byItem[item] = cur.size();
        cur.push_back(d);
        return d;
    }
    // The step of the nearest followed drop of this game frame within 80 units (a newborn drop moves
    // like its neighbours of the same burst), or false.
    bool neighbourStep(const float w[3], float out[3]) const {
        float bestD = 80.0f * 80.0f;
        bool found = false;
        for (const Drop& q : cur) {
            if (!q.stepKnown) continue;
            float d2 = 0;
            for (int c = 0; c < 3; c++) d2 += (w[c] - q.p[c]) * (w[c] - q.p[c]);
            if (d2 < bestD) bestD = d2, found = true, std::copy(q.step, q.step + 3, out);
        }
        return found;
    }
};
DropTracker sDrops;
// The stream points that moved by their own velocity in the last two game frames (fn_80120C30): where
// they are and their step. A drop born with no followed drop near it moves like the nearest point.
struct PointStep {
    u32 frame;
    float p[3], step[3];
};
std::vector<PointStep> sPointSteps;
void notePointStep(u32 frame, const float p[3], const float step[3]) {
    if (!sPointSteps.empty() && sPointSteps.front().frame + 1 < frame) {
        std::erase_if(sPointSteps, [&](const PointStep& q) { return q.frame + 1 < frame; });
    }
    if (sPointSteps.size() < 4096) sPointSteps.push_back({frame, {p[0], p[1], p[2]}, {step[0], step[1], step[2]}});
}
bool pointStepNear(u32 frame, const float w[3], float out[3]) {
    float bestD = 120.0f * 120.0f;
    bool found = false;
    for (const PointStep& q : sPointSteps) {
        if (q.frame + 1 < frame || q.frame > frame) continue;
        const float k = q.frame == frame ? 0.0f : 1.0f;  // last game frame's: carried by its step
        float d2 = 0;
        for (int c = 0; c < 3; c++) d2 += (w[c] - q.p[c] - k * q.step[c]) * (w[c] - q.p[c] - k * q.step[c]);
        if (d2 < bestD) bestD = d2, found = true, std::copy(q.step, q.step + 3, out);
    }
    return found;
}
// Loop 2: per item, the game frame it was last drawn in and its life counter then.
std::unordered_map<u32, std::pair<u32, u8>> sSlotLife;
// LM_STREAM_OLD=1 (test aid): the items themselves as identities, no tumbler or back step (before).
bool streamOld() {
    static const bool v = std::getenv("LM_STREAM_OLD") != nullptr;
    return v;
}
}  // namespace
HLE(fn_800F3120) {
    u32 item = 0;
    const bool loop1 = c.lr == VA(0x801201F4);
    if (loop1) item = rd32(c.r[31] + 0x4C) + c.r[27];
    else if (c.lr == VA(0x801202F8)) item = rd32(c.r[31] + 0x48) + c.r[26];
    if (!item) {
        callOriginal(c, "fn_800F3120");
        return;
    }
    if (!interp::capturing() || streamOld()) {
        TagScope t(TagCat::Particle, item, 0x11FB40);
        interp::tagStreamItem();
        sStreamItem = item;
        callOriginal(c, "fn_800F3120");
        sStreamItem = 0;
        return;
    }
    // The world place of its matrix (by now item + 4 / + 0x34 holds view x model: the camera at
    // 0x803A39AC, a rigid view).
    float w[3];
    {
        const u32 m = item + (loop1 ? 4 : 0x34), v = VA(0x803A3820) + 0x18C;
        float d[3];
        for (int r = 0; r < 3; r++) d[r] = rdf32(m + 16 * r + 12) - rdf32(v + 16 * r + 12);
        for (int r = 0; r < 3; r++) w[r] = rdf32(v + r * 4) * d[0] + rdf32(v + 16 + r * 4) * d[1] + rdf32(v + 32 + r * 4) * d[2];
    }
    const u32 frame = copyDispCount();
    sDrops.beginFrame(frame);
    u32 obj = item, id = 0;
    bool fresh = false;
    float step[3] = {};
    if (loop1) {
        const DropTracker::Drop d = sDrops.follow(item, w);
        id = d.id;
        obj = 0x7F000000u | (d.id << 4);  // the track is the identity (no guest object lives there)
        fresh = !d.stepKnown;
    } else {
        auto [it, isNew] = sSlotLife.try_emplace(item, std::pair<u32, u8>{~0u, 0});
        const u8 life = rd8(item + 3);
        if (it->second.first != frame) {
            const bool reborn = !isNew && it->second.first + 1 == frame && life > it->second.second;
            fresh = isNew || it->second.first + 1 != frame || reborn;
            if (reborn) interp::tagGeneration(item);
            it->second = {frame, life};
        }
        if (sSlotLife.size() > 4096) sSlotLife.clear();
    }
    const bool haveStep = fresh && (sDrops.neighbourStep(w, step) || pointStepNear(frame, w, step));
    // LM_STREAM_LOG=<file> (test aid): per drawn item, the game frame, loop, item address, its first
    // word (active, life), the world place of its matrix, its track (loop 1), whether it is new and
    // the step it is born moving along.
    static FILE* sStreamLog = [] {
        const char* v = std::getenv("LM_STREAM_LOG");
        return v && *v ? std::fopen(v, "w") : nullptr;
    }();
    if (sStreamLog)
        std::fprintf(sStreamLog, "f%u L%d %08X %08X %.1f %.1f %.1f id %u fresh %d step %d %.1f %.1f %.1f\n", frame, loop1 ? 1 : 2, item,
                     rd32(item), w[0], w[1], w[2], id, int(fresh), int(haveStep), step[0], step[1], step[2]);
    TagScope t(TagCat::Particle, obj, 0x11FB40);
    interp::tagTumbler();
    if (haveStep) interp::tagBackStep(step);
    sStreamItem = obj;
    callOriginal(c, "fn_800F3120");
    sStreamItem = 0;
}
HLE(fn_8005A280) {  // shadow casters
    vravatar::DrawPose avatar(c,c.r[3],true);
    registerSiteTags();
    TagScope t(TagCat::Object, c.r[3], rd32(c.r[3] + 4));
    interp::tagGroup(c.r[3]);  // one instance: its matrices blend alike (interp.cpp, replayPair)
    callOriginal(c, "fn_8005A280");
}
// Node and mesh draws of an S3D model are one instance (the model in r3) also when the game draws
// them directly instead of through the model draw above: those had no instance, so a skinned body
// was blended matrix by matrix. At the start of one of Chauncey's spins some of his envelope
// matrices blended rigidly, others entrywise, and his skin tore into shards for half an interval
// (sweep C, map 10 frame 6990; 1092 such mesh matrices per replay in his arena).
HLE(fn_800597C0) {
    TagScope t(TagCat::Object, c.r[3], 0x597C0, c.r[4]);
    interp::tagGroupIfNone(c.r[3]);
    callOriginal(c, "fn_800597C0");
}
HLE(fn_800598E0) {
    TagScope t(TagCat::Object, c.r[3], 0x598E0, c.r[4]);
    interp::tagGroupIfNone(c.r[3]);
    callOriginal(c, "fn_800598E0");
}
HLE(fn_800599E8) {
    TagScope t(TagCat::Object, c.r[3], 0x599E8, c.r[4]);
    interp::tagGroupIfNone(c.r[3]);
    callOriginal(c, "fn_800599E8");
}
HLE(fn_80058A60) {
    TagScope t(TagCat::Object, c.r[3], 0x58A60, c.r[4]);
    interp::tagGroupIfNone(c.r[3]);
    callOriginal(c, "fn_80058A60");
}
HLE(fn_800595B8) {
    TagScope t(TagCat::Object, c.r[3], 0x595B8, c.r[4]);
    interp::tagGroupIfNone(c.r[3]);
    callOriginal(c, "fn_800595B8");
}

// Actor lighting (fn_80060E70's actor loop: the actor in the loop's r21): the GX lights loaded for
// one actor belong to it.
HLE(fn_80057F90) {
    const bool inLoop = c.lr >= VA(0x80060E70) && c.lr < VA(0x80061278);
    TagScope t(inLoop ? TagCat::Object : TagCat::View, inLoop ? c.r[21] : 0, inLoop ? 0x57F90 : 0);
    callOriginal(c, "fn_80057F90");
}
HLE(fn_800581E8) {
    const bool inLoop = c.lr >= VA(0x80060E70) && c.lr < VA(0x80061278);
    TagScope t(inLoop ? TagCat::Object : TagCat::View, inLoop ? c.r[21] : 0, inLoop ? 0x581E8 : 0);
    const u32 actor = c.r[21];
    if (inLoop) sillyworld::ghostMaterialColor(actor, c.r[3]);  // ghost colour shuffle (F1 > Enhancements > Silly)
    callOriginal(c, "fn_800581E8");
    if (inLoop) sillyworld::ghostLightingDone(c, actor);
}

// --- map rooms and furniture (bin models) ---------------------------------------------------------
// fn_80015CF4(model, node matrices, pass, ...): rooms (fn_8001798C: no matrices) are map
// geometry; inside a furniture scope (fn_8001F098 / fn_8001F138(object)) the model is an object.
HLE(fn_8001F098) {
    registerSiteTags();
    ObjectDepth d;
    TagScope t(TagCat::Object, c.r[3], 0x1F098);
    callOriginal(c, "fn_8001F098");
}
HLE(fn_8001F138) {
    registerSiteTags();
    ObjectDepth d;
    TagScope t(TagCat::Object, c.r[3], 0x1F138);
    callOriginal(c, "fn_8001F138");
}
HLE(fn_80015CF4) {
    registerSiteTags();
    RoomModelScope model(sObjectDepth==0 && c.r[4]==0?c.r[3]:0);
    TagScope t(sObjectDepth > 0 || c.r[4] != 0 ? TagCat::Object : TagCat::World, c.r[3], c.r[4], c.r[5] & 0xFF);
    const u32 object=c.r[3],pass=c.r[5],alpha=c.r[6];
    const bool staticRoom=sObjectDepth==0 && c.r[4]==0;
    const u32 map=rd32(VA(0x804D80C8)),state=VA(0x803A3CA0);
    const unsigned room=rd8(state+0xC);
    // The foyer's lower room 2 and landing 31 are one open volume. Native
    // camera transitions fade room 2 away after arriving on the landing.
    // Retain only that already-loaded static model, never neighboring rooms.
    if(staticRoom && !sFoyerLower.drawing) {
        if(map!=2 || (room!=2 && room!=31))sFoyerLower={};
        else if(room==2 && pass==0 && alpha==255 && copyDispCount()!=sFoyerLower.frame) {
            sFoyerLower.frame=copyDispCount();sFoyerLower.model=object;
            sFoyerLower.table=gu::inMem1(object+0x3C)?rd32(object+0x3C):0;
        } else if(room==31 && pass==0 && alpha==255 && copyDispCount()!=sFoyerLower.upperFrame) {
            sFoyerLower.upperFrame=copyDispCount();sFoyerLower.upper=object;
            sFoyerLower.upperTable=gu::inMem1(object+0x3C)?rd32(object+0x3C):0;
        }
    }
    const bool immersive=vrhost::firstPerson() || (vravatar::diagnosticEnabled() && std::getenv("LM_VR_FOYER_TEST"));
    // Keep a foyer floor solid while the original camera cross-fades it. The
    // native fade draw is replaced, not added over the opaque model.
    const bool knownFloor=(object==sFoyerLower.model && rd32(object+0x3C)==sFoyerLower.table) ||
        (object==sFoyerLower.upper && rd32(object+0x3C)==sFoyerLower.upperTable);
    if(staticRoom && immersive && map==2 && (room==2||room==31) && knownFloor &&
       (rd8(state+0x1C)==1||rd8(state+0x1C)==2) && pass==2 && !sFoyerLower.drawing) {
        sFoyerLower.drawing=true;
        gu::call(c,GU_SYM("fn_80015CF4"),{object,0,0,255});
        gu::call(c,GU_SYM("fn_80015CF4"),{object,0,1,255});
        sFoyerLower.drawing=false;
        static bool reported=false;if(!reported){reported=true;log("[vr foyer] preserve opaque floors throughout native camera fade");}
        return;
    }
    callOriginal(c, "fn_80015CF4");
    if(staticRoom && (vrhost::firstPerson() || (vravatar::diagnosticEnabled() && std::getenv("LM_VR_FOYER_TEST"))) && map==2 && room==31 && pass==0 && alpha==255 &&
       rd8(state+0x1C)==0 && !sFoyerLower.drawing && sFoyerLower.model && object!=sFoyerLower.model &&
       gu::inMem1(sFoyerLower.model+0x3C) && rd8(sFoyerLower.model)==2 &&
       gu::inMem1(sFoyerLower.table) && rd32(sFoyerLower.model+0x3C)==sFoyerLower.table) {
        sFoyerLower.drawing=true;
        gu::call(c,GU_SYM("fn_80015CF4"),{sFoyerLower.model,0,0,255});
        gu::call(c,GU_SYM("fn_80015CF4"),{sFoyerLower.model,0,1,255});
        sFoyerLower.drawing=false;
        static bool reported=false;if(!reported){reported=true;log("[vr foyer] retaining loaded downstairs model after landing camera fade");}
    }
}
// fn_80015BDC(room lighting r3, ambient colour r4), from the room loop fn_80011824: loads the room's
// lights and its ambient colour (GXSetChanAmbColor at 0x80015C88). Untagged, the ambient fades
// (arriving in the foyer, a room's lights coming on, lit/dark changes: ~10/255 per game frame)
// stepped at 30 Hz (sweep C: 94% of the foyer's ramp at the tape change).
// Also called by the Game Boy Horror map draw with the map's view matrix, which the mirror
// replaces there (mirror_world.cpp).
HLE(fn_80015BDC) {
    TagScope t(TagCat::World, c.r[3], 0x15BDC);
    mirror::mapViewMatrix(c);
    callOriginal(c, "fn_80015BDC");
    mirror::afterMapViewMatrix();
}

HLE(fn_80015844) {  // one node (r6) of the model being drawn, recursive
    traceRoomNode(s16(c.r[6]));
    RoomFrameScope frame(s16(c.r[6]));
    const TagCat cat = sObjectDepth > 0 ? TagCat::Object : TagCat::World;
    TagScope t(cat, 0, 0x15844, c.r[6] & 0xFFFF);
    callOriginal(c, "fn_80015844");
}

// --- cloth ---------------------------------------------------------------------------------------
// fn_8008E580(object, pass) -> fn_80091A70(object, mode): a cloth sheet (a 16x16 grid of world-space
// points, CPU-built quads): cell n pairs with cell n of the same object and mode. (It was taken for
// the Poltergust hose at first; the hose is part of an S3D model: its matrices blend as one instance,
// fn_80059C54's tagGroup.)
HLE(fn_80091A70) {
    TagScope t(TagCat::Effect, c.r[3], 0x91A70, c.r[4]);
    callOriginal(c, "fn_80091A70");
}

// The room cloths: the room loop (fn_800118FC -> fn_80028974) walks a table of 0x388-byte items
// (11 slots in map 2) and draws the active ones with fn_80027F08(item): a CPU-simulated cloth mesh
// in an indexed F32 position array (149 vertices) the game rewrites every game frame, loaded with
// the item's own matrix. Only the 2F bathroom's shower curtain was active in the rooms checked
// (slot 10; the ballroom's drapes, the dining room's tablecloth and the others there are
// furniture models, blended by their matrices). It sways all the
// time (median 6, up to 37 units per game frame while the Poltergust pulls at it). Untagged, its
// array had no identity and the curtain stayed at the game frame's shape (user report: "this
// curtain did not get interpolated"). The item is its identity: its position array pairs with the
// same item's array of the game frame before and blends vertex by vertex (interp.cpp, replayPair).
HLE(fn_80027F08) {
    TagScope t(TagCat::Object, c.r[3], 0x27F08);
    callOriginal(c, "fn_80027F08");
}
// The other untagged position-array draw of the room loop: fn_80025FC8 walks 0x24C-byte items and
// draws each with fn_80025E00(item) (its own array and matrix). Tagged alike.
HLE(fn_80025E00) {
    TagScope t(TagCat::Object, c.r[3], 0x25E00);
    callOriginal(c, "fn_80025E00");
}

// --- ghosts' quads ---------------------------------------------------------------------------------
// fn_80061690 loops over the ghost list (0x803D48A0) and draws one CPU-built quad per ghost with
// fn_8005E110(.., ghost + 8 = its position, ..): its identity is the ghost (Bogmire's shadow ghosts,
// Chauncey's arena). Untagged, they were shown at the game frame's place.
HLE(fn_8005E110) {
    TagScope t(TagCat::Object, c.r[5] - 8, 0x5E110);
    callOriginal(c, "fn_8005E110");
}

// fn_80118D3C walks a table of 0xA8-byte items (five layers) and draws each one's CPU-built strip
// with fn_80118E44(item): the item is its identity.
HLE(fn_80118E44) {
    TagScope t(TagCat::Effect, c.r[3], 0x118E44);
    callOriginal(c, "fn_80118E44");
}

// The water spray (and the other element streams): fn_8011FB40 / fn_80120514 walk each stream's
// points (24 bytes each: position first) and draw every one as a GX point with fn_80120C30(point,
// size, colour). Untagged, the points were shown at the game frame's places while everything around
// them blended: the spray flickered.
HLE(fn_80120C30) {
    // LM_POINT_PEEK=<n> (test aid): log the 6 words of the first n points drawn per frame (finding an
    // age / birth field for the identity).
    static const int sPeek = std::getenv("LM_POINT_PEEK") ? std::atoi(std::getenv("LM_POINT_PEEK")) : 0;
    if (sPeek > 0) {
        static u32 frame = ~0u;
        static int n = 0;
        if (copyDispCount() != frame) frame = copyDispCount(), n = 0;
        if (n++ < sPeek)
            log("[point] f%u %08X lr %08X %08X %08X %08X %08X %08X %08X", frame, c.r[3], c.lr, rd32(c.r[3]), rd32(c.r[3] + 4),
                rd32(c.r[3] + 8), rd32(c.r[3] + 12), rd32(c.r[3] + 16), rd32(c.r[3] + 20));
    }
    // A point slot refilled from the nozzle is another particle: new generation, so it never pairs
    // with the slot's previous occupant (the idle ice mist's points paired across rebirths and were
    // shown at the game frame by the turn-back rule; sweep A/B). Two kinds of points are drawn here
    // (LM_POINT_LOG, nursery spray):
    //  - 24-byte points: position (+0) and velocity (+12); every game frame the velocity changes
    //    (gravity) and the point moves by the NEW velocity, exactly (p = last p + v: 92,095 of 92,095
    //    continuing points to the float);
    //  - points inside 0xF0-byte objects whose +12 is not their velocity (zero, or a stale value)
    //    while they move smoothly 20-35 units per game frame.
    // A point continues when it was drawn in the game frame before and either moved by its own
    // velocity (to 0.05 units) or kept its last step (to 3 units + a quarter of the step: gravity and
    // drag change it by ~1.2 per game frame), or, in the game frame after its birth, moved by up to 30
    // units; otherwise it was placed anew. (The old test, within half
    // the last velocity of last place + last velocity, took the 0xF0 points for new points every game
    // frame, so they never blended and stepped at 30 Hz, and let points re-placed near their slot's
    // last place pass as the same point.) A new point moves into its place along its step
    // (interp::tagBackStep): its velocity, when its slot is one that moves by its own velocity.
    // LM_POINT_OLD=1 (test aid): the old test, no back step.
    bool haveStep = false;
    float step[3] = {};
    if (interp::capturing()) {
        struct Last {
            u32 frame;
            float p[3], v[3], step[3];
            bool stepKnown, exactV;  // exactV: the slot was seen moving by its own velocity
            bool fresh;              // born in this game frame (frame == the record's)
            bool haveStep;
            float back[3];
        };
        static std::unordered_map<u32, Last> last;
        static const bool sOld = std::getenv("LM_POINT_OLD") != nullptr;
        const u32 frame = copyDispCount();
        float p[3], v[3];
        for (int k = 0; k < 3; k++) p[k] = flt(rd32(c.r[3] + 4 * k)), v[k] = flt(rd32(c.r[3] + 12 + 4 * k));
        auto it = last.find(c.r[3]);
        if (it != last.end() && it->second.frame == frame) {  // drawn again in another view this game frame
            haveStep = it->second.haveStep;
            std::copy(it->second.back, it->second.back + 3, step);
        } else {
            const bool seen = it != last.end();
            const bool consecutive = seen && it->second.frame + 1 == frame;
            float e1 = -1, e2 = -1, e3 = -1, s2 = 0, v2 = 0;  // e1 last p + last v, e2 last p + v, e3 last p + last step
            if (seen) {
                e1 = e2 = e3 = 0;
                for (int k = 0; k < 3; k++) {
                    const float lp = it->second.p[k];
                    e1 += (p[k] - lp - it->second.v[k]) * (p[k] - lp - it->second.v[k]);
                    e2 += (p[k] - lp - v[k]) * (p[k] - lp - v[k]);
                    e3 += (p[k] - lp - it->second.step[k]) * (p[k] - lp - it->second.step[k]);
                    s2 += it->second.step[k] * it->second.step[k];
                    v2 += it->second.v[k] * it->second.v[k];
                }
                e1 = std::sqrt(e1), e2 = std::sqrt(e2), e3 = std::sqrt(e3);
            }
            bool reborn = !consecutive;
            bool exactV = seen && it->second.exactV;
            if (consecutive) {
                if (sOld) {
                    reborn = !(e1 * e1 <= std::max(25.0f, 0.25f * v2));
                } else {
                    const bool byVel = e2 <= 0.05f + 1e-3f * std::sqrt(v2);
                    const bool byStep = it->second.stepKnown && e3 <= 3.0f + 0.25f * std::sqrt(s2);
                    // Its second game frame (no step known yet: born, or placed anew, last game frame):
                    // a short step continues it (the courtyard fountain's points keep a stale +12 and
                    // were taken for new points every game frame of their arc, as they never had a step).
                    float d2 = 0;
                    for (int k = 0; k < 3; k++) d2 += (p[k] - it->second.p[k]) * (p[k] - it->second.p[k]);
                    const bool byNear = !it->second.stepKnown && d2 <= 30.0f * 30.0f;
                    exactV |= byVel;
                    reborn = !byVel && !byStep && !byNear;
                }
            }
            Last rec{};
            rec.frame = frame;
            std::copy(p, p + 3, rec.p);
            std::copy(v, v + 3, rec.v);
            rec.stepKnown = consecutive && !reborn;
            if (rec.stepKnown)
                for (int k = 0; k < 3; k++) rec.step[k] = p[k] - it->second.p[k];
            rec.exactV = exactV;
            rec.fresh = reborn;
            if (reborn && !sOld && !exactV) {
                // A slot not seen moving yet: of the same kind as its neighbour in the 24-byte array.
                for (const u32 n : {c.r[3] - 0x18u, c.r[3] + 0x18u})
                    if (const auto nb = last.find(n); nb != last.end() && nb->second.exactV) exactV = true;
                rec.exactV = exactV;
            }
            if (reborn && !sOld) {
                // Born moving along its velocity (a slot that moves by its own velocity), else unknown.
                if (exactV && std::isfinite(v[0] + v[1] + v[2]) && v[0] * v[0] + v[1] * v[1] + v[2] * v[2] < 200.0f * 200.0f) {
                    haveStep = true;
                    std::copy(v, v + 3, step);
                }
            }
            rec.haveStep = haveStep;
            if (!reborn && consecutive && exactV) notePointStep(frame, p, v);
            std::copy(step, step + 3, rec.back);
            // LM_POINT_LOG=<file> (test aid): every point once per game frame: its slot, caller, place,
            // velocity, how far it is from last place + last velocity (e1), + its own velocity (e2) and
            // + its last step (e3), how many game frames since it was last drawn, the rebirth decision
            // and whether it is born moving along a step.
            static FILE* sPointLog = [] {
                const char* f = std::getenv("LM_POINT_LOG");
                return f && *f ? std::fopen(f, "w") : nullptr;
            }();
            if (sPointLog)
                std::fprintf(sPointLog, "f%u %08X lr %08X p %.2f %.2f %.2f v %.3f %.3f %.3f e1 %.3f e2 %.3f e3 %.3f last %d reborn %d back %d\n",
                             frame, c.r[3], c.lr, p[0], p[1], p[2], v[0], v[1], v[2], e1, e2, e3,
                             seen ? int(frame - it->second.frame) : -1, int(reborn), int(haveStep));
            if (reborn && seen) interp::tagGeneration(c.r[3]);
            if (last.size() > 8192) last.clear();
            last[c.r[3]] = rec;
        }
    }
    TagScope t(TagCat::Particle, c.r[3], 0x120C30);
    // (Its rebirth is detected above: a point turning back is bouncing. LM_POINT_OLD: as before.)
    static const bool sOldPoints = std::getenv("LM_POINT_OLD") != nullptr;
    if (!sOldPoints) interp::tagTracked();
    if (haveStep) interp::tagBackStep(step);
    callOriginal(c, "fn_80120C30");
}

// --- particles ---------------------------------------------------------------------------------
// A particle slot linked into or out of a group: its next use is another particle.
HLE(fn_80127A74) {
    if (c.r[5]) interp::tagGeneration(c.r[5]);
    callOriginal(c, "fn_80127A74");
}
HLE(fn_80127AA8) {
    if (c.r[3]) interp::tagGeneration(c.r[3]);
    if (c.r[4]) interp::tagGeneration(c.r[4]);
    if (c.r[5]) interp::tagGeneration(c.r[5]);
    callOriginal(c, "fn_80127AA8");
}

// --- J3D ---------------------------------------------------------------------------------------
// Shape packets belong to one model instance and one shape: the packet is the identity.
HLE(draw__14J3DShapePacketFv) {
    TagScope t(TagCat::Object, c.r[3], 0x13D);
    // The packets of one model share its matrix buffer (+0x14): one instance for switching.
    interp::tagGroup(rd32(c.r[3] + 0x14));
    callOriginal(c, "draw__14J3DShapePacketFv");
}

// Skinned shapes load the joint matrices of each matrix group (J3DShapeMtx::loadMtxIndx_*(this,
// slot, joint index)), skipping ones already in place, so the n-th load of a packet is not always
// the same joint: each load is tagged with its joint index.
#define J3D_MTX_LOAD(fn)                                       \
    HLE(fn) {                                                  \
        TagScope t(TagCat::Object, 0, 0x4D7, c.r[5] & 0xFFFF); \
        callOriginal(c, #fn);                                  \
    }
J3D_MTX_LOAD(loadMtxIndx_PNGP__11J3DShapeMtxCFiUs)
J3D_MTX_LOAD(loadMtxIndx_PCPU__11J3DShapeMtxCFiUs)
J3D_MTX_LOAD(loadMtxIndx_NCPU__11J3DShapeMtxCFiUs)
J3D_MTX_LOAD(loadMtxIndx_PNCPU__11J3DShapeMtxCFiUs)
#undef J3D_MTX_LOAD

// J3DSys::drawInit loads default matrices into every slot (10 position, normal and texture
// matrices) before each J3D draw phase: tagged so they pair (identical values; they used to count
// as ~480 untagged values per game frame).
HLE(drawInit__6J3DSysFv) {
    TagScope t(TagCat::Object, c.r[3], 0xD1);
    callOriginal(c, "drawInit__6J3DSysFv");
}

// A material packet loads its material (texture matrices among it) and then draws its shapes.
HLE(draw__12J3DMatPacketFv) {
    TagScope t(TagCat::Object, c.r[3], 0x13E);
    callOriginal(c, "draw__12J3DMatPacketFv");
}

// --- 2D ----------------------------------------------------------------------------------------
HLE(draw__7J2DPaneFiiPC14J2DGrafContextb) {
    TagScope t(TagCat::Hud, c.r[3], 0x2D);
    callOriginal(c, "draw__7J2DPaneFiiPC14J2DGrafContextb");
}

// --- text boxes: one layout ---------------------------------------------------------------------------
// Every text box is replayed as one 2D layout (interp::TextGroupScope; docs/INTERPOLATION.md "Text
// boxes at 120"): the box opens, closes and moves smoothly and its icon, text, arrow and choices move
// and scale with it, text that scrolls moves as one layer, and each letter fades in over the game
// frame that adds it. (Shown at the game frame before: a box that opened smoothly around letters
// appearing at the game rate made the text jump against it, user report after 4301b1e.)
// fn_80044138, from the frame loop fn_8000BCE8, draws every message window (fn_800433B4): the box
// picture (fn_8003B054, 480x120, grows open over 3-4 game frames), the speaker icon, the text, the
// "more" arrow and choices (a nine-piece box, fn_8003B390). All dialogue goes through it: Neville,
// Toad, E. Gadd in the lab and on the Game Boy Horror call, signs. (E. Gadd's call window on the GBH
// screen: widescreen.cpp fn_8003D634 / fn_8003BE9C / fn_80114E18; the room name: fn_80045BDC.)
namespace {
u32 sTextBoxFrame = ~0u;  // the game frame (GXCopyDisp count) a message window last drew in
}  // namespace
namespace tags {
// A message window is on screen (drawn in this game frame or the one before; the HUD is drawn
// before the windows). While one is, the event portrait belongs to the text box too
// (widescreen.cpp): E. Gadd's call is a message window next to his portrait on the GBH.
// How text boxes are replayed: 2 (default) one layout (interp::TextGroupScope), 1 shown at the game
// frame with the GBH and portrait (interp::FrameLockScope, the earlier behaviour), 0 nothing special
// (test aids LM_TEXTBOX_LOCK=1 / 0).
int textMode() {
    static const int sMode = [] {
        const char* v = std::getenv("LM_TEXTBOX_LOCK");
        return v && *v == '1' ? 1 : v && *v == '0' ? 0 : 2;
    }();
    return sMode;
}
bool textBoxShown() { return textMode() != 0 && sTextBoxFrame != ~0u && copyDispCount() - sTextBoxFrame <= 1; }
}  // namespace tags
HLE(fn_80044138) {
    if (!interp::capturing()) return callOriginal(c, "fn_80044138");
    const int mode = tags::textMode();
    interp::tagLogMark("textbox begin");
    if (mode == 1) interp::pushFrameLock();
    if (mode == 2) interp::pushTextGroup(0x80044138u);
    const u32 draws = interp::drawCount();
    callOriginal(c, "fn_80044138");
    if (interp::drawCount() != draws) sTextBoxFrame = copyDispCount();
    if (mode == 2) interp::popTextGroup();
    if (mode == 1) interp::popFrameLock();
    interp::tagLogMark("textbox end");
}

// --- skies ------------------------------------------------------------------------------------------
// The sky dome (a J3D model with texture SRT scroll / colour animations: map 2's vrball_M, the boss
// arenas' vrball_B on maps 10, 11 and 13, vrKoopa on map 9; docs/INTERPOLATION.md "Boss arena skies").
// fn_8002FA34, from the room loop fn_80011824 (per view, before the rooms), advances the sky's
// animations, places it at the camera (fn_800102B0, J3DSys's view matrix: view-space translation 0)
// and draws it (viewCalc, drawInit, its draw buffers): its matrices, texture matrices, projection,
// material / TEV / fog colours and its own light (fn_800AAF5C: constant J3D channel colours and GX
// light 7) are all set inside, so nothing scene-wide (Bogmire's lightning light 0-2, room ambient)
// reaches it. Its values are marked as the sky's (interp::pushSky): the dome's matrices follow the
// blended camera like any object, and its texture scrolls blend the short way when they wrap
// (blendTexWrap `sky`: the layers repeat 2-10 times, outside the general +-4 bound, and swept back
// through the whole texture in one game frame when a layer wrapped: the "jumpy" boss skies).
// LM_SKY_LOCK=1 (test aid): the boss arenas' sky (maps 9, 10, 11, 13) shown exactly as in its game
// frame (interp::FrameLockScope, like the text boxes; the camera's turns then step it at 30 Hz),
// =2: every map's sky.
namespace {
bool skyLocked() {
    static const int sMode = std::getenv("LM_SKY_LOCK") ? std::atoi(std::getenv("LM_SKY_LOCK")) : 0;
    if (sMode == 0) return false;
    if (sMode == 2) return true;
    const u32 map = rd32(VA(0x804D80C8));  // sCurMapNo
    return map == 9 || map == 10 || map == 11 || map == 13;
}
}  // namespace
HLE(fn_8002FA34) {
    // LM_SKY_HIDE=1 (test aid): the sky is not drawn at all (finding sky pixels: compare with a run
    // that draws it).
    static const bool sHide = std::getenv("LM_SKY_HIDE") != nullptr;
    if (sHide) return;
    if (!interp::capturing()) return callOriginal(c, "fn_8002FA34");
    interp::tagLogMark("sky begin");
    if (skyLocked()) {
        interp::FrameLockScope lock;
        callOriginal(c, "fn_8002FA34");
    } else {
        interp::pushSky();
        callOriginal(c, "fn_8002FA34");
        interp::popSky();
    }
    interp::tagLogMark("sky end");
}

// --- memory ------------------------------------------------------------------------------------
namespace {
bool heapLogAll() {
    static const bool on = [] {
        const char* v = std::getenv("LM_HEAP_LOG");
        return v && v[0] == '1';
    }();
    return on;
}
}  // namespace

// A freed heap block: objects that lived in it get new identities (a new actor allocated at the
// same address is not the old one).
HLE(free__10JKRExpHeapFPv) {
    const u32 p = c.r[4];
    if (heapLogAll()) log("[heap] free heap %08X %08X (lr %08X)", c.r[3], p, c.lr);
    if (p >= 0x80000010u && p < 0x84000000u) {
        u32 size = rd32(p - 0x10 + 4);
        if (size == 0 || size > (16u << 20)) size = 64;
        interp::tagFreed(p, size);
    }
    callOriginal(c, "free__10JKRExpHeapFPv");
}

// Every game heap allocation: a failure is logged with the heap's free total and largest block
// and the guest call chain (the retail game never fails one; up to 1.1 event01's did, from a
// 1 MiB smaller arena: hle_os.cpp OSInit). LM_HEAP_LOG=1 also logs every allocation and free
// (test aid: free space and fragmentation at a point of the game).
HLE(alloc__10JKRExpHeapFUli) {
    const u32 heap = c.r[3], size = c.r[4], align = c.r[5], lr = c.lr, sp = c.r[1];
    callOriginal(c, "alloc__10JKRExpHeapFUli");
    const u32 res = c.r[3];
    if (res != 0 && !heapLogAll()) return;
    u32 s = sp, chain[4] = {};
    for (int i = 0; i < 4 && s >= 0x80000000u && s < 0x81800000u; i++) {
        s = rd32(s);
        if (s < 0x80000000u || s >= 0x81800000u) break;
        chain[i] = rd32(s + 4);
    }
    const Ctx saved = c;
    static const u32 sTotal = guestSymbol("getTotalFreeSize__10JKRExpHeapFv");
    static const u32 sLargest = guestSymbol("getFreeSize__10JKRExpHeapFv");
    const u32 freeTotal = guestCall(c, sTotal, heap), largest = guestCall(c, sLargest, heap);
    c = saved;
    log("[heap] %salloc heap %08X size %X align %d -> %08X free %X largest %X (lr %08X %08X %08X %08X %08X)",
        res ? "" : "ERROR: failed ", heap, size, s32(align), res, freeTotal, largest, lr, chain[0], chain[1], chain[2],
        chain[3]);
}

}  // namespace recomp
