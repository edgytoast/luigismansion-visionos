#ifndef DOLPHIN_GXAURORA_H
#define DOLPHIN_GXAURORA_H

#include <dolphin/types.h>

#if __cplusplus
extern "C" {
#endif

//
// Subcommands for GX_AURORA.
//

/**
 * Sets the actual render viewport in native framebuffer coordinates.
 * Must be followed by six f32 values: left, top, width, height, nearz, farz.
 */
#define GX_AURORA_LOAD_VIEWPORT_RENDER 0x0001

/**
 * Sets the actual render scissor in native framebuffer coordinates.
 * Must be followed by four u32 values: left, top, width, height.
 */
#define GX_AURORA_LOAD_SCISSOR_RENDER 0x0002

/**
 * Loads a full 4x4 projection matrix, bypassing GXSetProjection's 6-parameter
 * hardware encoding. Must be followed by sixteen f32 values in row-major order.
 */
#define GX_AURORA_LOAD_PROJECTION_FULL 0x0003

/**
 * Aurora equivalent of CP_REG_ARRAYBASE_ID: sets the base address and size of a vertex array.
 * This command must be followed by a 64-bit memory address, 32-bit size, and 1-byte little-endian flag.
 * The index of the vertex array is given by the lowest 4 bits of the command ID,
 * e.g. writing GX_AURORA_LOAD_ARRAYBASE + 5 will set the vertex array for the sixth vertex attribute.
 * To set strides, use the normal CP_REG_ARRAYSTRIDE_ID register.
 */
#define GX_AURORA_LOAD_ARRAYBASE 0x0010

/**
 * Pushes a debug group to the backend graphics API. These may show in debugging tools such as RenderDoc.
 * Must be followed by a u16 string length and that many UTF-8 characters (no null terminator required).
 * It is considered an error to have unpopped debug groups at the end of the frame. They will be automatically cleared.
 */
#define GX_AURORA_DEBUG_GROUP_PUSH 0x0020

/**
 * Pops a previously pushed debug group.
 * Followed by nothing.
 */
#define GX_AURORA_DEBUG_GROUP_POP 0x0021

/**
 * Sends a debug marker to the backend graphics API.
 * Must be followed by a u16 string length and that many UTF-8 characters (no null terminator required).
 */
#define GX_AURORA_DEBUG_MARKER_INSERT 0x0022

#define GX_AURORA_LOAD_TEXOBJ 0x0030

#define GX_AURORA_LOAD_TLUT 0x0031

#define GX_AURORA_DESTROY_TEXOBJ 0x0032

#define GX_AURORA_DESTROY_TLUT 0x0033

#define GX_AURORA_DESTROY_COPY_TEX 0x0034

#define GX_AURORA_LOAD_COPY_SRC 0x0035

#define GX_AURORA_LOAD_COPY_DST 0x0036

#define GX_AURORA_LOAD_COPY_DEST 0x0037

#define GX_AURORA_REQUEST_DEPTH_SNAPSHOT 0x0038

#define GX_AURORA_BEGIN_OFFSCREEN 0x0039

#define GX_AURORA_END_OFFSCREEN 0x003A

/**
 * Draw primitives with the vertex count derived from a byte length, as written by
 * GXBegin(prim, fmt, GX_AUTO). Must be followed by a u8 draw opcode (vtxfmt|prim),
 * a u32 vertex data byte length, then that many bytes of vertex data. The byte length
 * must be a whole multiple of the current vertex size or zero (no draw).
 */
#define GX_AURORA_DRAW_SIZED 0x0040

/**
 * Draw pre-merged triangles with a prebuilt index buffer, as written by the display
 * list optimizer (aurora::gx::dl::optimize). Must be followed by a u8 draw opcode
 * (vtxfmt | GX_TRIANGLES), a u16 vertex count, a u32 index count, that many u16
 * indices, then vertex count * vertex size bytes of packed vertex data. Index data
 * is always host-endian regardless of stream endianness.
 */
#define GX_AURORA_DRAW_INDEXED 0x0041

/**
 * Wireframe override (debug viewers). Followed by a u8 enable flag and a u32 RGBA8 line colour.
 * While enabled, every triangle-type draw is rasterized as its triangle edges (a line list,
 * no culling) and every fragment gets the line colour. Lines and points are unaffected.
 */
#define GX_AURORA_SET_WIREFRAME 0x0050

/**
 * Back-face culling override (debug viewers). Followed by a u8: 0 = the game's cull mode,
 * 1 = draw both faces of every triangle (GX_CULL_ALL still drops the draw).
 */
#define GX_AURORA_SET_CULL_OVERRIDE 0x0051

/**
 * Enhanced lighting (include/aurora/enhanced.h): draw class of the following draws.
 * Followed by a u8 class (AURORA_DRAW_CLASS_*).
 */
#define GX_AURORA_SET_DRAW_CLASS 0x0052

/**
 * Enhanced lighting: extra point lights for this frame (include/aurora/lights.h).
 * Followed by a u8 count, then per light 8 f32 (pos xyz, color rgb, radius, intensity) and
 * 2 u32 (flags, id), all big-endian like the rest of the stream.
 */
#define GX_AURORA_SET_EXTRA_LIGHTS 0x0053
#define GX_AURORA_EXTRA_LIGHT_WORDS 22 // 8 f32 + 2 u32 + 12 f32 (room box, 0187) per light

/**
 * Enhanced lighting: end of the frame's 3D scene (screen-space effects are applied here).
 * Followed by nothing.
 */
#define GX_AURORA_SCENE_END 0x0054

/**
 * Enhanced lighting: material of the following draws. Followed by a u8 (AURORA_MATERIAL_*):
 * 0 world geometry, 1 characters (matte: the game's own vertex lighting, no glossy highlights,
 * soft wrap-around response to the extra lights).
 */
#define GX_AURORA_SET_MATERIAL_HINT 0x0055

/**
 * Replay state slots (frame interpolation): GX_AURORA_STATE_SAVE stores the processed GX state
 * (registers, matrices, vertex layout and arrays, loaded textures; not the copy-texture caches)
 * in a slot, GX_AURORA_STATE_RESTORE makes it current again. Followed by a u8 slot (0..3).
 * Lets a captured frame be replayed several times, each time from the state it started from.
 */
#define GX_AURORA_STATE_SAVE 0x0060
#define GX_AURORA_STATE_RESTORE 0x0061
/**
 * Replay copy generation (frame interpolation): followed by a u8, bits 0-1 the generation EFB
 * copies go into until the next one (copy textures and copy-to-RAM bytes are kept per
 * generation, so a replayed frame's copies do not overwrite the previous frame's, which the
 * frame's own early draws may still read), bit 7 set while a tape is replayed (copies are not
 * dropped for memory the CPU changed meanwhile: the guest is already on a later frame), bit 6 set
 * for a repeated replay of a tape (its copies are not read back into memory again). The state
 * slots also save/restore the copy textures and copy-to-RAM entries. Cleared by STATE_SAVE.
 */
#define GX_AURORA_COPY_GENERATION 0x0062

/**
 * Frame interpolation fades (spawns/despawns between two captured frames).
 * GX_AURORA_SET_FADE, u8 q: later draws lose q/255 of their pixels (ordered 4x4 dither, so it
 * works for opaque and blended draws alike and keeps depth correct); 0 = drawn as usual.
 * GX_AURORA_SET_REPLAY_MASK, u8 on: while on, draws, EFB copies, offscreen passes, depth
 * snapshots, extra lights and texture evictions are skipped (state changes still apply), so
 * a replay can run another frame's commands just to draw a few of its draws.
 * State slots 4..7 save/restore the GX state only (no copy textures or copy-to-RAM entries).
 */
#define GX_AURORA_SET_FADE 0x0063
#define GX_AURORA_SET_REPLAY_MASK 0x0064

/**
 * Draw probe (frame-interpolation judder detector, test builds of the game): GX_AURORA_PROBE_TAG,
 * u32 tag: later draws report this tag to the probe callback (AuroraSetDrawProbe). No effect on
 * rendering; ignored while no callback is set.
 */
#define GX_AURORA_PROBE_TAG 0x0068

#define GX2_SET_POLYGON_OFFSET 0x1000


/*
 * Hardware-accuracy options (take effect at the next draw; pipelines are rebuilt as needed).
 */

/** TEV evaluated with the hardware's integer arithmetic (8-bit inputs, s11 registers, per-stage rounding). */
#define GX_AURORA_ACCURACY_INT_TEV 0x0001
/** Pixel output quantized to the EFB pixel format (RGBA6 / RGB565) with the PE's ordered dither. */
#define GX_AURORA_ACCURACY_EFB_DEPTH 0x0002
/** Color texture copies (GXCopyTex) apply the vertical copy filter set by GXSetCopyFilter (BP 0x53/0x54). */
#define GX_AURORA_ACCURACY_COPY_FILTER 0x0004
/**
 * With GX_AURORA_ACCURACY_EFB_DEPTH: blended draws into an RGBA6 EFB are stored the way the PE stores
 * them, the blend result dithered and truncated to 6 bits (the color target is quantized after each
 * pass that has such draws; costs a full-target pass per affected EFB pass).
 */
#define GX_AURORA_ACCURACY_EFB_BLEND 0x0008
/**
 * Z compare before texturing (GXSetZCompLoc(GX_TRUE)) also writes the depth of pixels the alpha
 * test rejects, as the hardware updates Z before the TEV (Aurora 0170 splits such draws into a
 * depth-only pass and the colour pass). AURORA_EARLY_Z_SPLIT overrides it (0 off, 1 on, ab).
 */
#define GX_AURORA_ACCURACY_EARLY_Z 0x0010
/**
 * With GX_AURORA_ACCURACY_EFB_DEPTH (0190, enhancement): smooth color banding. The EFB format
 * quantization (RGBA6 / RGB565 with the PE dither) and the RGB565 / RGB5A3 texture-copy precision
 * output the local average of the hardware result instead of its steps: the same brightness and
 * tint as the 6/5-bit picture (the truncation's darkening is kept), without contour bands or
 * dither grain. Alpha keeps the exact 6-bit value; blended RGBA6 draws are not re-quantized
 * (GX_AURORA_ACCURACY_EFB_BLEND is ignored). Runtime switch: no pipeline changes.
 */
#define GX_AURORA_ACCURACY_EFB_SMOOTH 0x0020
/**
 * With GX_AURORA_ACCURACY_EFB_SMOOTH: the smoothed EFB color also gets a sub-LSB ordered noise
 * (+-0.5 of an 8-bit step, interleaved gradient noise by render pixel) before the 8-bit target
 * rounds it, so dark gradients do not show 8-bit steps either.
 */
#define GX_AURORA_ACCURACY_EFB_SMOOTH_NOISE 0x0040
/**
 * With GX_AURORA_ACCURACY_EARLY_Z: consecutive draws with the same state are merged into one
 * split pair (one depth-only pass, then one colour pass for all of them) instead of a pair per
 * draw. Much cheaper (a foyer frame: ~16,000 split draws -> a few hundred commands); differs from
 * the per-draw order only where the merged geometry overlaps itself with alpha-rejected pixels
 * (a few edge pixels of the Game Boy Horror hand in LM). AURORA_EARLY_Z_MERGE=0/1/ab overrides.
 */
#define GX_AURORA_ACCURACY_EARLY_Z_MERGE 0x0080

void GXSetAuroraAccuracy(u32 flags);
u32 GXGetAuroraAccuracy();

/**
 * What a TEV or indirect stage reads from a texture coordinate at or beyond GXSetNumTexGens:
 * 0 = (0, 0); 1 (default) = the coordinate its texgen registers describe, as if it were counted;
 * 2 = texture coordinate 0.
 */
#define GX_AURORA_TEXGEN_BEYOND_ZERO 0
#define GX_AURORA_TEXGEN_BEYOND_GENERATE 1
#define GX_AURORA_TEXGEN_BEYOND_TEXCOORD0 2
void GXSetAuroraTexGenBeyondCount(u32 mode);

/*
 * Debug marker stuff
 */

/**
 * Pushes a debug group to the backend graphics API. These may show in debugging tools such as RenderDoc.
 * It is considered an error to have unpopped debug groups at the end of the frame. They will be automatically cleared.
 */
void GXPushDebugGroup(const char* label);

/**
 * Pop a debug group previously pushed via GXPushDebugGroup().
 */
void GXPopDebugGroup();

/**
 * Sends a debug marker to the backend graphics API. These may show in debugging tools such as RenderDoc.
 */
void GXInsertDebugMarker(const char* label);

typedef enum _AuroraViewportPolicy {
  AURORA_VIEWPORT_FIT = 0,     // Preserve logical aspect in the content framebuffer
  AURORA_VIEWPORT_STRETCH = 1, // Match content framebuffer aspect to the native surface
  AURORA_VIEWPORT_NATIVE = 2,  // Use active framebuffer pixels directly
} AuroraViewportPolicy;

/**
 * Configures content framebuffer sizing and how GXSetViewport/GXSetScissor parameters are applied to rendering.
 * When AURORA_VIEWPORT_NATIVE is used, GXSetTexCopySrc/GXSetTexCopyDst will use native framebuffer resolution.
 */
void AuroraSetViewportPolicy(AuroraViewportPolicy policy);

/**
 * Retrieves the current content framebuffer size.
 */
void AuroraGetRenderSize(u32* width, u32* height);

/**
 * Flush pending GX state and wait for FIFO processing without signaling a draw-done callback.
 */
void AuroraGXSync(void);

/**
 * Sets the actual render viewport in native framebuffer coordinates.
 * Overrides the automatically scaled values set by the logical GXSetViewport.
 */
void GXSetViewportRender(f32 left, f32 top, f32 wd, f32 ht, f32 nearz, f32 farz);

/**
 * Sets the actual render scissor in native framebuffer coordinates.
 * Overrides the automatically scaled values set by the logical GXSetScissor.
 */
void GXSetScissorRender(u32 left, u32 top, u32 wd, u32 ht);

void GX2SetPolygonOffset(f32 mFrontOffset, f32 mFrontScale, f32 mBackOffset, f32 mBackScale, f32 mClamp);

/**
 * Wireframe override (see GX_AURORA_SET_WIREFRAME): triangles are drawn as their edges in
 * `rgba` (0xRRGGBBAA) until turned off again.
 */
void GXSetWireframeAurora(u8 enable, u32 rgba);

/**
 * Two-sided override (see GX_AURORA_SET_CULL_OVERRIDE).
 */
void GXSetCullOverrideAurora(u8 twoSided);

/**
 * Replay state slots (see GX_AURORA_STATE_SAVE): processed in FIFO order.
 */
void GXSaveStateAurora(u8 slot);
void GXRestoreStateAurora(u8 slot);
void GXSetCopyGenerationAurora(u8 generationAndFlags);
void GXSetFadeAurora(u8 q);
void GXSetReplayMaskAurora(u8 on);

/**
 * Load an arbitrary 4x4 projection matrix, avoiding the 6-parameter hardware encoding.
 */
void GXSetProjectionFull(const void* mtx);

/**
 * Create an offscreen framebuffer and switch rendering to it.
 * All subsequent GX rendering will target this framebuffer until GXRestoreFrameBuffer() is called.
 * Use GXCopyTex to resolve the offscreen content into a texture.
 */
void GXCreateFrameBuffer(u32 width, u32 height);

/**
 * Restore rendering to the main EFB framebuffer.
 * Must be called after GXCreateFrameBuffer() to resume normal rendering.
 */
void GXRestoreFrameBuffer(void);

/**
 * Command capture, for replaying a frame's commands later (e.g. frame interpolation).
 * While capturing, GX commands issued on the calling thread are written to `buf`
 * instead of the FIFO, and GXDrawDone/AuroraGXSync on that thread don't wait.
 * Other threads keep using the FIFO normally.
 */
void AuroraCaptureBegin(void* buf, u32 size);
/** Ends the capture on the calling thread; returns the bytes written (32-byte padded). */
u32 AuroraCaptureEnd(void);
/** Bytes written so far by the current capture on the calling thread. */
u32 AuroraCapturePos(void);
/** Submits captured commands to the FIFO (inside an open frame). */
void AuroraReplay(const void* data, u32 size);
/**
 * 0235: the calling thread's GX commands go into the capture (or display list) that another thread
 * began last, instead of the FIFO, while one is open (join = true); or into its own again. For
 * emulated threads that share one CPU FIFO and never write at the same time. Never join a thread
 * that submits to the FIFO concurrently (e.g. one that replays captures).
 */
void AuroraCaptureJoin(u8 join);
/** 0235: absolute FIFO stream position of the next byte the calling thread writes to the FIFO. */
u64 AuroraFifoStreamPos(void);

/**
 * 0235: malformed FIFO data. The command processor no longer stops on a bad command: it reports
 * it (rate-limited) through this callback, on the FIFO thread, skips to the next position where
 * the stream parses as valid commands again (or to the end of the submitted block) and goes on.
 * Only a stream that stays broken (AURORA_FIFO_ERROR_LIMIT errors within 10 s, default 300) is fatal.
 */
typedef struct AuroraFifoError {
  const char* what;        // "unknown opcode 0x1D", "draw vertex data overrun", ...
  u64 streamPos;           // absolute stream position of the bad command's first byte
  u32 blockOffset;         // its offset in the block being processed
  u32 blockSize;           // size of that block (one publish: a draw, a display list, a replay)
  u32 skipped;             // bytes skipped to resynchronize
  u8 resynced;             // false: skipped to the end of the block
  u32 count;               // errors so far in this session
  const u8* before;        // up to 64 bytes before the bad command
  u32 beforeLen;
  const u8* after;         // up to 32 bytes from the bad command on
  u32 afterLen;
} AuroraFifoError;
typedef void (*AuroraFifoErrorCallback)(const AuroraFifoError* error);
void AuroraSetFifoErrorCallback(AuroraFifoErrorCallback callback);

/**
 * Draw probe: while a callback is set, every draw the FIFO processes (except masked ones) is
 * reported with the matrix and projection it is drawn with and its first vertex's position, on
 * the FIFO thread. For measuring where things land on screen (frame interpolation judder).
 */
typedef struct AuroraDrawProbe {
  u32 tag;          /* the last GX_AURORA_PROBE_TAG */
  u8 prim, fmt;
  u16 vtxCount;
  u8 hasPos;        /* pos is the first vertex's model-space position (else 0,0,0) */
  u8 fadeQ;
  u8 mtxIdx;        /* position matrix slot (PNMTX index) of the first vertex */
  u8 _pad;
  f32 mtx[12];      /* position matrix (3x4 row-major) of the first vertex */
  f32 proj[16];     /* projection (4x4 row-major) */
  f32 pos[3];
  f32 ndcMin[2], ndcMax[2]; /* screen extent of all its vertices (normalized device coordinates; min > max: none) */
  f32 viewport[4];          /* logical viewport: left, top, width, height (640x480 frame) */
} AuroraDrawProbe;
typedef void (*AuroraDrawProbeFn)(const AuroraDrawProbe* probe);
void AuroraSetDrawProbe(AuroraDrawProbeFn fn);
void GXSetProbeTagAurora(u32 tag);

#if __cplusplus
}
#endif

#endif
