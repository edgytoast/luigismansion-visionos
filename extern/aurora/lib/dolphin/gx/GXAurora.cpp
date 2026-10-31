#include "dolphin/gx/GXAurora.h"

#include <aurora/enhanced.h>
#include <aurora/lights.h>

#include <algorithm>
#include <limits>

#include "__gx.h"
#include "gx.hpp"
#include "../../window.hpp"

#include "../../gx/command_processor.hpp"
#include "../../gx/fifo.hpp"

static void GXWriteString(const char* label) {
  auto length = strlen(label);

  if (length > std::numeric_limits<u16>::max()) {
    Log.warn("Debug marker size over u16 max, truncating");
    length = std::numeric_limits<u16>::max();
  }

  GX_WRITE_U16(length);
  GX_WRITE_DATA(label, length);
}

void GXPushDebugGroup(const char* label) {
  GX_WRITE_AURORA(GX_AURORA_DEBUG_GROUP_PUSH);
  GXWriteString(label);
}

void GXPopDebugGroup() { GX_WRITE_AURORA(GX_AURORA_DEBUG_GROUP_POP); }

void GXInsertDebugMarker(const char* label) {
  GX_WRITE_AURORA(GX_AURORA_DEBUG_MARKER_INSERT);
  GXWriteString(label);
}

void AuroraSetViewportPolicy(AuroraViewportPolicy policy) {
  aurora::gx::set_viewport_policy(policy);
}

void AuroraGetRenderSize(u32* width, u32* height) {
  const auto windowSize = aurora::window::get_window_size();
  if (width != nullptr) {
    *width = windowSize.fb_width;
  }
  if (height != nullptr) {
    *height = windowSize.fb_height;
  }
}

void AuroraCaptureBegin(void* buf, u32 size) {
  aurora::gx::fifo::begin_display_list(static_cast<u8*>(buf), size);
}

u32 AuroraCaptureEnd() { return aurora::gx::fifo::end_display_list(); }

u32 AuroraCapturePos() { return aurora::gx::fifo::display_list_pos(); }

void AuroraReplay(const void* data, u32 size) {
  aurora::gx::fifo::replay(static_cast<const u8*>(data), size);
}

void AuroraCaptureJoin(u8 join) { aurora::gx::fifo::join_recording(join != 0); }

u64 AuroraFifoStreamPos() { return aurora::gx::fifo::stream_position(); }

void AuroraSetFifoErrorCallback(AuroraFifoErrorCallback callback) { aurora::gx::fifo::set_error_callback(callback); }

void AuroraGXSync() {
  GXFlush();
  aurora::gx::fifo::drain();
}

void GXSetAuroraAccuracy(u32 flags) { aurora::gx::g_accuracyFlags.store(flags, std::memory_order_relaxed); }

u32 GXGetAuroraAccuracy() { return aurora::gx::g_accuracyFlags.load(std::memory_order_relaxed); }

void GXSetAuroraTexGenBeyondCount(u32 mode) { aurora::gx::g_texGenBeyondCount.store(mode, std::memory_order_relaxed); }

void GXSetViewportRender(f32 left, f32 top, f32 wd, f32 ht, f32 nearz, f32 farz) {
  GX_WRITE_AURORA(GX_AURORA_LOAD_VIEWPORT_RENDER);
  GX_WRITE_F32(left);
  GX_WRITE_F32(top);
  GX_WRITE_F32(wd);
  GX_WRITE_F32(ht);
  GX_WRITE_F32(nearz);
  GX_WRITE_F32(farz);
}

void GXSetScissorRender(u32 left, u32 top, u32 wd, u32 ht) {
  GX_WRITE_AURORA(GX_AURORA_LOAD_SCISSOR_RENDER);
  GX_WRITE_U32(left);
  GX_WRITE_U32(top);
  GX_WRITE_U32(wd);
  GX_WRITE_U32(ht);
}

void GXSetProjectionFull(const void* mtx) {
  const f32* values = reinterpret_cast<const f32*>(mtx);
  GX_WRITE_AURORA(GX_AURORA_LOAD_PROJECTION_FULL);
  for (int i = 0; i < 16; ++i) {
    GX_WRITE_F32(values[i]);
  }
}

void GXSetWireframeAurora(u8 enable, u32 rgba) {
  GX_WRITE_AURORA(GX_AURORA_SET_WIREFRAME);
  GX_WRITE_U8(enable);
  GX_WRITE_U32(rgba);
}

void GXSetCullOverrideAurora(u8 twoSided) {
  GX_WRITE_AURORA(GX_AURORA_SET_CULL_OVERRIDE);
  GX_WRITE_U8(twoSided);
}

void GXSaveStateAurora(u8 slot) {
  GX_WRITE_AURORA(GX_AURORA_STATE_SAVE);
  GX_WRITE_U8(slot);
}

void GXRestoreStateAurora(u8 slot) {
  GX_WRITE_AURORA(GX_AURORA_STATE_RESTORE);
  GX_WRITE_U8(slot);
}

void GXSetCopyGenerationAurora(u8 generationAndFlags) {
  GX_WRITE_AURORA(GX_AURORA_COPY_GENERATION);
  GX_WRITE_U8(generationAndFlags);
}

void GXSetFadeAurora(u8 q) {
  GX_WRITE_AURORA(GX_AURORA_SET_FADE);
  GX_WRITE_U8(q);
}

void GXSetReplayMaskAurora(u8 on) {
  GX_WRITE_AURORA(GX_AURORA_SET_REPLAY_MASK);
  GX_WRITE_U8(on);
}

void GXSetProbeTagAurora(u32 tag) {
  GX_WRITE_AURORA(GX_AURORA_PROBE_TAG);
  GX_WRITE_U32(tag);
}

void GX2SetPolygonOffset(f32 mFrontOffset, f32 mFrontScale, f32 mBackOffset, f32 mBackScale, f32 mClamp) {
  GX_WRITE_AURORA(GX2_SET_POLYGON_OFFSET);
  GX_WRITE_F32(mFrontOffset);
  GX_WRITE_F32(mFrontScale);
  GX_WRITE_F32(mBackOffset);
  GX_WRITE_F32(mBackScale);
  GX_WRITE_F32(mClamp);
}

void GXCreateFrameBuffer(u32 width, u32 height) {
  GX_WRITE_AURORA(GX_AURORA_BEGIN_OFFSCREEN);
  GX_WRITE_U32(width);
  GX_WRITE_U32(height);
  aurora::gx::fifo::publish();
}

void GXRestoreFrameBuffer() {
  GX_WRITE_AURORA(GX_AURORA_END_OFFSCREEN);
  aurora::gx::fifo::publish();
}

void GXSetDrawClassAurora(uint8_t drawClass) {
  GX_WRITE_AURORA(GX_AURORA_SET_DRAW_CLASS);
  GX_WRITE_U8(drawClass);
}

void GXSceneEndAurora(void) { GX_WRITE_AURORA(GX_AURORA_SCENE_END); }

void GXSetMaterialHintAurora(uint8_t hint) {
  GX_WRITE_AURORA(GX_AURORA_SET_MATERIAL_HINT);
  GX_WRITE_U8(hint);
}

namespace aurora::gfx {
void set_extra_lights(const ExtraLight* lights, uint32_t count) {
  count = std::min(count, MaxExtraLights);
  GX_WRITE_AURORA(GX_AURORA_SET_EXTRA_LIGHTS);
  GX_WRITE_U8(count);
  for (uint32_t i = 0; i < count; ++i) {
    const auto& l = lights[i];
    GX_WRITE_F32(l.pos[0]);
    GX_WRITE_F32(l.pos[1]);
    GX_WRITE_F32(l.pos[2]);
    GX_WRITE_F32(l.color[0]);
    GX_WRITE_F32(l.color[1]);
    GX_WRITE_F32(l.color[2]);
    GX_WRITE_F32(l.radius);
    GX_WRITE_F32(l.intensity);
    GX_WRITE_U32(l.flags);
    GX_WRITE_U32(l.id);
    for (float v : l.box) {
      GX_WRITE_F32(v);
    }
  }
}
} // namespace aurora::gfx
