#pragma once
// High-frame-rate presentation by frame interpolation (docs/INTERPOLATION.md).
//
// The game keeps running at its own rate (30 Hz, paced by the emulated VI). While
// interpolation is on, the guest no longer draws into the host's Aurora frame:
// each guest frame's GX commands are captured into a "tape" (Aurora command
// capture, patch 0005), together with the stream offsets of every value that moves
// (matrices, projections, lights, CPU-built vertex positions) and copies of the
// vertex arrays its draws read. The host presents at display rate and replays the
// newest tape with those values blended toward the previous tape's, one game frame
// behind real time.
#include "recomp/ctx.h"

#include <string>
#include <memory>
#include <vector>

namespace recomp::interp {

enum class Kind : u32 { PosMtx, NrmMtx, TexMtx, Projection, Light, Verts, Color, Count };

// --- identity tags (docs/INTERPOLATION.md "Tagged interpolation") -----------------
// The game's own draw code labels what it draws: hooks on its draw entry points (tags.cpp) open a
// tag scope around each object, part, particle, HUD element or pass. A value noted inside gets a
// key = (the scope path, the GX call site, the n-th such value in that scope); values pair between
// game frames only by identical keys. Values noted outside any object scope are untagged: never
// paired (drawn from the newest frame, placed under the in-between camera when in the 3D view).
enum class TagCat : u8 {
    None,      // no scope
    View,      // structural: a view / sub-view / offscreen pass (camera identity, not an object)
    World,     // static map geometry: only the camera moves it between game frames
    Object,    // characters, furniture, props: model matrices blended
    Particle,  // one particle of an emitter
    Hud,       // 2D element
    Effect,    // CPU-built or other effect geometry owned by an object
    SubView,   // structural: a scene drawn with its OWN camera (event scenes: door close-ups, E. Gadd's
               // call portrait): its values are not factored by the game camera (blended as they are)
};
// Opens a scope: `object` is the guest address of the thing drawn (actor, model instance, particle,
// pane), `type` what it is (vtable, model data, type id; 0 = none), `sub` a part index. Nests; the
// identity of a scope is its parent's identity + these + the object's generation (bumped when its
// memory is freed or the slot is reused, tagGeneration) + its occurrence under that parent.
void pushTag(TagCat cat, u32 object, u32 type = 0, u32 sub = 0);
void popTag();
struct TagScope {
    TagScope(TagCat cat, u32 object, u32 type = 0, u32 sub = 0) { pushTag(cat, object, type, sub); }
    ~TagScope() { popTag(); }
    TagScope(const TagScope&) = delete;
    TagScope& operator=(const TagScope&) = delete;
};
// The memory at `addr` now holds a new object (freed, or a pool slot handed out again): scopes on
// that address get a new identity, so a reused slot never pairs with its previous occupant.
void tagGeneration(u32 addr);
// A heap block [addr, addr + size) was freed: every tagged object inside it gets a new generation.
void tagFreed(u32 addr, u32 size);
// The innermost open scope (and what is opened inside it) belongs to model instance id: its
// matrices switch together when one of them cannot blend (a skinned mesh drawn in several packets).
void tagGroup(u32 id);
// tagGroup only when the scope did not inherit an instance (a model's node or mesh drawn directly,
// not through its model draw).
void tagGroupIfNone(u32 id);
// The innermost open scope (and what is drawn inside it) takes a random orientation every game frame
// (the element streams' drops): its model matrices always blend the short way (never switched for a
// turn over ~150 degrees), their drawn centre moving in a straight line.
void tagTumbler();
// Marks the innermost scope as an element stream's drop for logs only (LM_SPRAY_LOG), not a tumbler.
void tagStreamItem();
// The innermost open scope moves by `step` (world units) per game frame: drawn without a partner in
// the game frame before (just born, or its slot re-placed), it is shown moving along that step into
// its place (at b - (1 - alpha) step) instead of standing still for the whole interval. Positions of
// CPU-built vertex blocks noted inside must be in world space.
void tagBackStep(const float step[3]);
// The innermost open scope's identity follows the thing itself (the hook detects when its slot holds a
// new one and gives it a new generation): a CPU-built block noted inside that turns back against its
// last step is bouncing (blended), not another particle in its slot (shown at the game frame).
void tagTracked();
// Every GX HLE call (hle_gx.cpp GXHLE): the guest call site and registers, for value keys and for
// call-site tags (a draw loop inside one game function, e.g. particles: tags.cpp registers the
// function's range and which register holds the item).
void gxCall(const Ctx& c);
struct SiteTag {
    u32 lo, hi;     // guest code range (US addresses; mapped with VA() at registration)
    TagCat cat;
    u8 objReg, typeReg;  // registers holding the item and its owner/type (0xFF: none)
    // A u16 in the item that counts its age (+1 per game frame) or remaining life (-1): its birth
    // frame becomes part of the identity, so a pool slot reused by a new particle is another item.
    s16 ageOffset = -1;
    s8 ageStep = 0;
};
void addSiteTag(const SiteTag& t);

// --- guest thread ---------------------------------------------------------------
bool capturing();
void noteResultsScreen(bool flat=false); // gallery 3D view vs ACCOUNT 2D layout
void noteDoorAnimation();
void notePlayerDemo(); // guest: dedicated non-phone player/event camera rendered
void noteSearchToggle(bool closing = false);
void noteGameBoyMap(); // guest: map renderer actually issued draws in this tape
// Before a GX call or raw FIFO write: while a frame is being captured, a guest thread other than the
// capturing one joins the capture (AuroraCaptureJoin, Aurora 0235), so its commands land in the
// tape in the order the guest issued them instead of in Aurora's FIFO beside the host's replays.
void joinCaptureSlow();
extern bool g_captureOpen;  // a tape is being captured (any guest thread)
extern u32 g_captureGen;    // the group of guest threads sharing the capture (new when its owner changes)
extern thread_local u32 t_captureGen;
inline void joinCapture() {
    if (g_captureOpen && t_captureGen != g_captureGen) [[unlikely]] joinCaptureSlow();
}
// The GX call site at the current tape position (malformed-FIFO reports, Aurora 0235).
void noteCallSite(u32 lr);
// Host/FIFO thread: which replayed tape (game frame, offset, GX calls before it) holds an absolute
// FIFO stream position; empty if none of the recent replays does.
std::string describeStreamPos(u64 streamPos);
void beginTape();                 // starts capturing the next guest frame
void endTape(u32 retraceCount);   // closes it and hands it to the host (LM_INTERP_TEST: and waits)

// Bracket an Aurora call that writes one XF block of `nFloats` floats (matrix,
// projection or light load): mark() before, note() after. `source` is the guest
// address of the matrix/light object (or 0); `slot` is the XF slot or light index.
// `first` is the index of the first blended float in the block.
u32 mark();
void note(u32 at, Kind kind, u32 source, u32 slot, int nFloats, int first = 0);
// While set, note() records nothing: the values are replayed exactly as captured (snapped to
// the tape's own game frame). Used for the shadow map: its caster pass and the receivers'
// shadow texture matrices must come from the same frame (shadows.cpp). Nests by count.
void pushNoBlend();
void popNoBlend();
// Shown exactly at its game frame: no value is noted inside (matrices, lights, colours, 2D vertex
// blocks and their colours, CPU position arrays). Used for the area-result screens (widescreen.cpp)
// and, with LM_TEXTBOX_LOCK=1, the text boxes (test aid; they are one layout otherwise:
// pushTextGroup). Nests by count.
void pushFrameLock();
void popFrameLock();
// Text boxes (tags.cpp): everything drawn inside is one 2D layout blended as a whole
// (docs/INTERPOLATION.md "Text boxes at 120"): the box picture's open/close animation gives the
// layout transform every block in it follows (box, speaker icon, text, arrow, choices); text that
// scrolls moves as one layer; letters new in a game frame fade in over the presents up to the next
// game frame. Nests (inner scopes join the outer group); `id` names the drawing function.
// oneLayout: everything in it is one picture moving as a whole (the Game Boy Horror's frame pieces,
// its call screen); otherwise every box picture in it (the message window, the choice box) moves
// on its own with the text drawn after it.
void pushTextGroup(u32 id, bool oneLayout = false);
// Draws inside join the last group `id` opened in this game frame (drawn apart from it: the Game
// Boy Horror's static noise belongs to the GBH's layout).
void pushTextGroupJoin(u32 id);
// Viewports and scissor boxes written inside a text group (Aurora's XF / BP writes at tape offset
// `at`) move and scale with its layout. The 2D layout's x becomes the pixel x f * x + c there
// (widescreen: centered / anchored 4:3). Outside a group, inside pushTextViewJoin(id), they belong
// to the last group `id` (E. Gadd's call portrait is drawn into the GBH screen's viewport).
void noteTextViewport(u32 at, float f, float c);
void noteTextScissor(u32 at, float f, float c);
void pushTextViewJoin(u32 id);
void popTextViewJoin();
void setCurrentMtx(u32 id);  // GXSetCurrentMtx (which position matrix 2D blocks are drawn with)
void popTextGroup();
// Capture-only element identity; does not change native interpolation/layout.
unsigned setHudElement(unsigned element);
struct HudElementScope {
    unsigned previous;
    explicit HudElementScope(unsigned element):previous(setHudElement(element)){}
    ~HudElementScope(){setHudElement(previous);}
    HudElementScope(const HudElementScope&)=delete;
    HudElementScope& operator=(const HudElementScope&)=delete;
};
struct TextGroupScope {
    explicit TextGroupScope(u32 id) { pushTextGroup(id); }
    ~TextGroupScope() { popTextGroup(); }
    TextGroupScope(const TextGroupScope&) = delete;
    TextGroupScope& operator=(const TextGroupScope&) = delete;
};
// LM_TAG_LOG: a marker line in the log (e.g. where a hooked draw begins and ends), when logging.
void tagLogMark(const char* text);
bool tagLogOn();  // LM_TAG_LOG is logging this game frame
// The sky dome (tags.cpp, fn_8002FA34): values noted inside are marked as the sky's.
void pushSky();
void popSky();
struct FrameLockScope {
    FrameLockScope() { pushFrameLock(); }
    ~FrameLockScope() { popFrameLock(); }
    FrameLockScope(const FrameLockScope&) = delete;
    FrameLockScope& operator=(const FrameLockScope&) = delete;
};
// While set, position matrices are blended linearly (element-wise, like projection-style texture
// matrices) instead of rigidly: the shadow map's caster pass, so the in-between shadow map matches
// the receivers' element-wise blended light projection (shadows.cpp).
void pushLinearPosMtx();
void popLinearPosMtx();

// Enhanced lighting extra lights (GX_AURORA_SET_EXTRA_LIGHTS written at `at`, `count`
// lights): blended between tapes by light id.
void noteExtraLights(u32 at, u32 count);

// Colors (channel ambient/material colors, light colors, TEV color/konst registers, fog color)
// written from tape offset `at` to now: each register is a value of its tag scope, blended with
// the same identity's register in the previous game frame unless it jumps (a blink, a flash, an
// on/off switch: shown at the game frame). 2D (HUD) colors are never blended.
// `lightObj`: the colours come from GXLoadLightObjImm of that light object (noted as a Light value
// just before): a light's colour takes the light's identity and blends while the light pairs.
void noteColors(u32 at, u32 lightObj = 0);

// Camera factoring: position/normal matrices and lights are view x world; they are blended as
// a blended camera times a blended world transform (objects move with the scene as one rigid
// whole instead of each product on its own path). The game's current view matrix (3x4) is read
// from guest memory at viewMatrixSource() (game-specific, e.g. shadows.cpp for LM; 0 = none)
// whenever a value is noted, so a pass drawn from another camera (the shadow light) factors by
// that camera.
u32 viewMatrixSource();
void noteVrPlayer(float x,float y,float z,float facingX,float facingZ,bool valid);

// Shadow maps (LM, shadows.cpp): the light pass's view LV and the receivers' projection K = P x LV
// and depth-ramp R x LV matrices of this frame. Receivers' shadow texture matrices noted between
// push/popShadowReceivers are K x W (TEXMTX: world W) or K x V^-1 (post-transform: view V); they
// are rebuilt from the blended light (P, LV) and the blended W or V, the same blend the caster
// pass is rendered with, so the shadow map and its lookup always agree.
void noteShadowLight(const float lightView[12], const float kProj[12], const float kRamp[12]);
void pushShadowReceivers();
// The pass being drawn (0 main; e.g. 1 the shadow caster pass): draws of different passes never pair.
void setPass(u32 pass);
// Values noted while an orthographic projection is current are 2D (HUD): not camera-factored,
// and a jump of more than 96 units (pixels) shows them unblended.
void setOrthographic(bool ortho);
void popShadowReceivers();

// LM_FRAME_DUMP's EFB copy (hle_gx GXCopyDisp), marked at `at`: LM_INTERP_TEST
// redirects it per replay.
void noteDump(u32 at, const void* target, u32 frameNo);

// Draws (interp_vtx.cpp). Values noted before a draw are paired between frames by
// what that draw is (display list, vertex layout), not by where they came from.
// Vertex arrays: a tape is replayed up to a game frame after it was captured, while
// the guest is already rewriting dynamic vertex data, so the array ranges each
// tape's draws index are copied into the tape at capture time.
void arrayBase(u32 at, u32 attr, u32 guestAddr, u32 stride);  // GXSetArray, marked at `at`
void invalidateVtxCache();                                    // GXInvalidateVtxCache
void vtxBegin(u32 prim, u32 fmt, u32 nVerts, bool perspective);  // GXBegin (before Aurora's header)
void vtxBytes(u32 v, int size);                               // raw FIFO write from game code (after it was written)
// A held immediate-mode draw (widescreen classification) is written after its GXBegin is replayed:
// its vertex data starts at `at` in the tape, not where the game's writes were first seen.
void vtxDataAt(u32 at);
void vtxEnd();  // gx::endPendingDraw: the immediate-mode draw is complete
u32 drawCount();  // draws captured so far (running count; tags.cpp: did a scope draw anything)
void vtxDisplayList(u32 guestAddr, const u8* data, u32 size);  // after GXCallDisplayList

// Draw-time vertex array snapshots without interpolation (interp_vtx.cpp; F1 > Visuals >
// Rendering "Snapshot vertex data at draw time", docs/RENDERING.md). Aurora's FIFO thread reads
// guest arrays when it processes a draw, later than the console's GPU would; game code that
// rewrites a buffer in between makes the draw use the wrong data. When active, the array ranges
// each draw indexes are copied on the guest thread at submit time and Aurora reads the copies.
// LM_VTX_RACE=1 counts guest stores into array ranges that a still-unprocessed draw reads live.
bool snapActive();  // enabled, not capturing a tape, not recording a display list
void snapDisplayList(const u8* data, u32 size);  // before GXCallDisplayList
void snapBegin(u32 fmt, u32 nVerts);  // before Aurora's GXBegin (immediate-mode draw)
void snapBytes(u32 v, int size);      // raw FIFO write inside that draw
void snapEnd();                       // before Aurora's GXEnd
void snapAfterDraw();                 // after the draw was written (race detector)
void snapEndFrame();                  // gx::endFrame
bool raceDetectorOn();
bool raceStore(u32 addr, int size);   // every guest store (true: log it) while the detector is on

// Textures and TLUTs (interp_tex.cpp): the data a load points at is copied at capture time.
void texLoaded(u32 at);   // after GXLoadTexObj, marked at `at`
u32 boundTexture(int map);  // what was last loaded into that texture map (while capturing; 0: none)
void tlutLoaded(u32 at);  // after GXLoadTlut, marked at `at`
void efbCopied(u32 guestDest, u32 size);  // GXCopyTex: textures over that memory are not copied
void efbWrittenBack(const void* dest, u32 size);  // a copy's bytes were written into memory

// The game recording its own display list suspends the capture around it.
void suspendForDisplayList();
void resumeAfterDisplayList();

// --- host thread -----------------------------------------------------------------
// Replays the newest tape into the open Aurora frame. Returns false if there is
// no tape yet. `final` replays the newest tape exactly (no blending).
bool replay(bool final = false);
bool replayStereoEye(); // host: same tape pair and alpha selected by replay(); no simulation tick
void reset();  // drop all tapes (after leaving interpolated mode)
// LM_INTERP_JUDDER=<file>: real-time pacing log for tools/interp/judder.py (host loop phases,
// per present the tapes and alpha shown, per tape its stamp and finish time). Host thread:
// 0 before aurora_begin_frame, 1 after it, 2 after the frame's replay, 3 after aurora_end_frame,
// 4 after frame::pace.
void hostMark(int phase);
bool judderLogOn();
void logPresentTiming(u64 frame, int64_t submitNs, int64_t presentNs, int64_t doneNs);  // P lines (pacing.cpp)
// The last replay: a running present number, the tape shown (b) and alpha. LM_FRAME_TAG=1 draws
// them as a bar code in the top-left corner (menu.cpp), so a screen capture tells which present
// (and which in-between) reached the screen (tools/interp/screen_judder.py --tag).
struct PresentInfo {
    u32 present = 0;
    u64 bSeq = 0;
    float alpha = 1.0f;
    // Pacing log (pacing.cpp, LM_PACING_LOG): the newest game frame (tape) the host had at this
    // present, when the guest finished it (steady clock seconds since its epoch), and whether the
    // shown game time was held at that newest tape (no newer game frame yet: a stall).
    u64 newestSeq = 0;
    double newestDone = 0;
    bool stalled = false;
};
PresentInfo lastPresent();

namespace detail {  // interp.cpp <-> interp_vtx.cpp
// A position array copied into a tape that was bound inside a tag scope (CPU-deformed geometry:
// cloth, morphs): blended between game frames when the same scope binds a same-sized array.
struct ArrayRec {
    u64 key;     // identity (tag scope, call site, occurrence), never 0
    u32 snap;    // index into the tape's array copies
    u32 ptrAt;   // tape offset of the load's host pointer (u64 BE; the u32 size follows)
    u16 stride;
    u8 cnt, type, frac;  // the position format of the first draw that used it
};
u64 arrayKey();  // the key of a position array bound now (0: untagged)
void vtxBeginTape();
// Copies are finished: points the tape's array loads at them (moved into `keep`).
void vtxEndTape(u8* tape, u32 size, std::vector<std::vector<u8>>& keep, u64& snapshotBytes, std::vector<ArrayRec>& recs);
// The tape bytes of the draw just noted (noteDraw): a display list's inlined commands, or an
// immediate-mode draw from before its setup (drawStart) to after its vertices (drawEnd).
void drawRange(u32 start, u32 end);
void drawStart(u32 start);
void drawEnd(u32 end);
// A draw with identity `id`: values noted since the previous draw belong to it.
void noteDraw(u64 id);
// The draw just noted reads indexed positions whose bounding box has centre `c` (model space).
void noteDrawCentre(const float c[3]);
// The same per position matrix slot (`mask`: the slots its vertices use by a direct PNMTXIDX).
void noteDrawSlotCentres(const float c[10][3], u16 mask);
// (unused since tagged interpolation; kept for interp_vtx.cpp)
inline void noteDrawAlt(u64) {}
// Immediate-mode vertices with direct f32 XYZ positions: `count` vertices of `stride`
// bytes starting at tape offset `at`, position at `posOffset` in each.
void noteVertices(u32 at, u32 count, u32 stride, u32 posOffset, u64 id, u32 clrOffset = 0xFFFF, u32 clrBytes = 0,
                  u32 posBytes2D = 0,  // posBytes2D: a 2D (orthographic) block of positions that size
                  // 2D only: 0x100 | component type | frac << 3 of the positions, and the direct texture
                  // coordinates (offset, bytes): a picture whose layout animates blends its positions.
                  u32 posFmt2D = 0, u32 uvOffset = 0xFFFF, u32 uvBytes = 0,
                  s32 projPatch = -1);  // 2D: the projection its data was written under (currentProjection)
s32 currentProjection();  // the last projection noted in this tape (index, -1 none)
void vertsBegin(bool ortho = false);  // at GXBegin of such a draw: its identity (tag scope, call site)
u64 mix(u64 h);
// A display list or stream wrote XF matrix memory directly (judder detector's slot table).
void xfMatrixWritten(u32 addr, u32 words);
// A color register written by a display list (BP write at tape offset `at`, the register byte):
// blended like noteColors.
void noteDrawColor(u32 at, u32 reg, u8 type);
void drawLog(const char* fmt, ...);  // LM_INTERP_DRAWS
u8* captureBytes();  // the capture buffer (tape offsets index it)
// This tape's texture/TLUT copies (kept alive by the tape) and the bytes newly copied.
void texEndTape(std::vector<std::shared_ptr<const std::vector<u8>>>& keep, u64& copiedBytes);
// Path check (LM_TRACK_LOG, game side: tags.cpp): world places of the player (0-2), the Poltergust
// (3-5) and its nozzle (6-8) at the end of the game frame; false when there is no player.
bool trackPoints(float out[9]);
}  // namespace detail

// LM_INTERP_DRAWS: whether this draw is being logged, and its guest call chain.
bool drawLogActive();
void drawLogCaller(const std::string& chain);

}  // namespace recomp::interp
