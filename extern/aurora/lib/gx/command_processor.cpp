#include "../gfx/enhanced.hpp"
#include "command_processor.hpp"

#include <optional>
#include <unordered_map>

#include "../gfx/depth_peek.hpp"
#include "../gfx/efb_ram.hpp"
#include "../gfx/recording.hpp"
#include "../internal.hpp"
#include "dolphin/gd/GDGeometry.h"
#include "dolphin/gx/GXAurora.h"
#include "gx.hpp"
#include "mirror.hpp"
#include "pipeline.hpp"
#include "regs.hpp"
#include "shader_info.hpp"
#include "texture.hpp"

#include <tracy/Tracy.hpp>

#include <absl/container/flat_hash_set.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <array>
#include <chrono>
#include <string>
#include <vector>

namespace aurora::gx::fifo {
namespace {
constexpr Module Log{"aurora::gx::fifo"};

// GX_AURORA_SET_WIREFRAME: the edges of a triangle list as a line list.
u32 wire_edges(ByteBuffer& out, const u16* tri, u32 count) noexcept {
  out.reserve_extra((count / 3) * 6 * sizeof(u16));
  for (u32 i = 0; i + 2 < count; i += 3) {
    const u16 a = tri[i], b = tri[i + 1], c = tri[i + 2];
    out.append(a);
    out.append(b);
    out.append(b);
    out.append(c);
    out.append(c);
    out.append(a);
  }
  return (count / 3) * 6;
}

bool wire_prim(GXPrimitive prim) noexcept {
  return g_gxState.wireframe && prim != GX_LINES && prim != GX_LINESTRIP && prim != GX_POINTS;
}

u16 prepare_idx_buffer(ByteBuffer& buf, GXPrimitive prim, u16 vtxStart, u16 vtxCount) noexcept {
  u16 numIndices = 0;
  if (prim == GX_QUADS) {
    buf.reserve_extra((vtxCount / 4) * 6 * sizeof(u16));

    for (u16 v = 0; v < vtxCount; v += 4) {
      u16 idx0 = vtxStart + v;
      u16 idx1 = vtxStart + v + 1;
      u16 idx2 = vtxStart + v + 2;
      u16 idx3 = vtxStart + v + 3;

      buf.append(idx0);
      buf.append(idx1);
      buf.append(idx2);
      numIndices += 3;

      buf.append(idx2);
      buf.append(idx3);
      buf.append(idx0);
      numIndices += 3;
    }
  } else if (prim == GX_TRIANGLES) {
    buf.reserve_extra(vtxCount * sizeof(u16));
    for (u16 v = 0; v < vtxCount; ++v) {
      const u16 idx = vtxStart + v;
      buf.append(idx);
      ++numIndices;
    }
  } else if (prim == GX_TRIANGLEFAN) {
    buf.reserve_extra(((u32(vtxCount) - 3) * 3 + 3) * sizeof(u16));
    for (u16 v = 0; v < vtxCount; ++v) {
      const u16 idx = vtxStart + v;
      if (v < 3) {
        buf.append(idx);
        ++numIndices;
        continue;
      }
      buf.append(std::array{vtxStart, static_cast<u16>(idx - 1), idx});
      numIndices += 3;
    }
  } else if (prim == GX_TRIANGLESTRIP) {
    buf.reserve_extra(((static_cast<u32>(vtxCount) - 3) * 3 + 3) * sizeof(u16));
    for (u16 v = 0; v < vtxCount; ++v) {
      const u16 idx = vtxStart + v;
      if (v < 3) {
        buf.append(idx);
        ++numIndices;
        continue;
      }
      if ((v & 1) == 0) {
        buf.append(std::array{static_cast<u16>(idx - 2), static_cast<u16>(idx - 1), idx});
      } else {
        buf.append(std::array{static_cast<u16>(idx - 1), static_cast<u16>(idx - 2), idx});
      }
      numIndices += 3;
    }
  } else if (prim == GX_LINES || prim == GX_LINESTRIP || prim == GX_POINTS) {
    buf.reserve_extra(6 * sizeof(u16));
    buf.append<u16>(0);
    buf.append<u16>(1);
    buf.append<u16>(3);
    buf.append<u16>(3);
    buf.append<u16>(2);
    buf.append<u16>(0);
    numIndices = 6;
  } else
    UNLIKELY FATAL("unsupported primitive type {}", static_cast<u32>(prim));
  return numIndices;
}

// GX FIFO opcodes - use CP_ prefix to avoid clashing with GXCommandList.h macros
constexpr u8 CP_CMD_NOP = GX_NOP;
constexpr u8 CP_CMD_LOAD_CP_REG = GX_LOAD_CP_REG;
constexpr u8 CP_CMD_LOAD_XF_REG = GX_LOAD_XF_REG;
constexpr u8 CP_CMD_LOAD_INDX_A = GX_LOAD_INDX_A;
constexpr u8 CP_CMD_LOAD_INDX_B = GX_LOAD_INDX_B;
constexpr u8 CP_CMD_LOAD_INDX_C = GX_LOAD_INDX_C;
constexpr u8 CP_CMD_LOAD_INDX_D = GX_LOAD_INDX_D;
constexpr u8 CP_CMD_CALL_DL = GX_CMD_CALL_DL;
constexpr u8 CP_CMD_INVAL_VTX = GX_CMD_INVL_VC;
constexpr u8 CP_CMD_LOAD_BP_REG = GX_LOAD_BP_REG & GX_OPCODE_MASK;

// Primitive type mask
constexpr u8 CP_OPCODE_MASK = GX_OPCODE_MASK;
constexpr u8 CP_VAT_MASK = GX_VAT_MASK;

struct FogRangeLutKey {
  std::array<u16, 10> rangeK;
  f32 rangeCenter;
  f32 renderWidth;
  u32 targetWidth;

  bool operator==(const FogRangeLutKey&) const = default;
};

struct FogRangeLutEntry {
  FogRangeLutKey key;
  std::vector<f32> factors;
};

constexpr size_t MaxFogRangeLuts = 32;
std::vector<FogRangeLutEntry> sFogRangeLuts;

struct DrawCache {
  uint64_t targetLayoutKey = 0;
  PipelineConfig config{};
  ShaderInfo shaderInfo{};
  gfx::PipelineRef pipelineRef{};
  GXBindGroups bindGroups{};
  uint64_t bindGeneration = 0;
  GXVtxFmt fmt = GX_MAX_VTXFMT;
  u8 lineMode = 0;
  bool hasPipeline = false;
  gfx::Range uniformRange{};
  gfx::Range fogRange{};
  FogRangeLutKey fogRangeKey{};
  bool hasFogRange = false;
  GXVtxFmt lastDrawFmt = GX_MAX_VTXFMT;
  u8 efbBlend6 = 0; // g_drawEfbBlend6 of the cached pipeline
  // Early-Z split (0170/0182): decision and the two pipelines, for the cached pipeline.
  gfx::PipelineRef splitFor{};
  uint64_t splitTargetKey = 0;
  u32 splitFlags = UINT32_MAX;
  bool splitValid = false;
  bool split = false;
  gfx::PipelineRef splitDepth{};
  gfx::PipelineRef splitColor{};
};
DrawCache sDrawCache;

FogRangeLutKey fog_range_lut_key() noexcept {
  const auto& state = g_gxState.fog;
  const f32 logicalWidth = static_cast<f32>(std::max(logical_fb_size().x, 1u));
  const u32 targetWidth = gfx::get_render_target_size().x;
  return {
      .rangeK = state.rangeK,
      .rangeCenter = static_cast<f32>(state.rangeCenter),
      // EFB pixels per render-target pixel
      .renderWidth = logicalWidth / static_cast<f32>(std::max(targetWidth, 1u)),
      .targetWidth = targetWidth,
  };
}

// Range adjustment (GXSetFogRangeAdj): the ten 12-bit table entries are 256x the eye-depth
// correction factor sqrt(1 + x^2/z^2) at 32, 64, ... 320 EFB pixels from the center column
// (that is how GXInitFogAdjTable fills them). The factor for a pixel is interpolated from the
// table by its horizontal distance from the center, 1.0 at the center, the last entry beyond.
std::vector<f32> build_fog_range_lut(const FogRangeLutKey& key) {
  std::vector<f32> lut(key.targetWidth);
  for (u32 x = 0; x < key.targetWidth; ++x) {
    const f32 efbX = (static_cast<f32>(x) + 0.5f) * key.renderWidth;
    const f32 pos = std::abs(efbX - key.rangeCenter) / 32.f; // 1.0 = first table entry
    f32 factor;
    if (pos >= 10.f) {
      factor = static_cast<f32>(key.rangeK[9]) / 256.f;
    } else {
      const u32 hi = static_cast<u32>(pos);
      const f32 frac = pos - static_cast<f32>(hi);
      const f32 lower = hi == 0 ? 1.f : static_cast<f32>(key.rangeK[hi - 1]) / 256.f;
      const f32 upper = static_cast<f32>(key.rangeK[hi]) / 256.f;
      factor = lower + (upper - lower) * frac;
    }
    lut[x] = std::max(factor, 1.f / 256.f);
  }
  return lut;
}

const std::vector<f32>& resolve_fog_range_lut(const FogRangeLutKey& key) {
  for (const auto& entry : sFogRangeLuts) {
    if (entry.key == key) {
      return entry.factors;
    }
  }
  if (sFogRangeLuts.size() == MaxFogRangeLuts) {
    sFogRangeLuts.erase(sFogRangeLuts.begin());
  }
  sFogRangeLuts.emplace_back(FogRangeLutEntry{key, build_fog_range_lut(key)});
  return sFogRangeLuts.back().factors;
}

gfx::Range push_fog_range_lut(const FogRangeLutKey& key) {
  const auto& lut = resolve_fog_range_lut(key);
  return gfx::push_storage(reinterpret_cast<const u8*>(lut.data()), lut.size() * sizeof(f32));
}

u8 line_mode_for_prim(GXPrimitive prim) noexcept {
  switch (prim) {
  case GX_LINES:
    return 1;
  case GX_LINESTRIP:
    return 2;
  case GX_POINTS:
    return 3;
  default:
    return 0;
  }
}
} // namespace

static void handle_draw(u8 cmd, ByteReader& reader) noexcept;
static void handle_aurora(ByteReader& reader) noexcept;
static u32 calc_vtx_size(GXVtxFmt fmt) noexcept;

// ---- Malformed FIFO data (0235) -----------------------------------------------------------------
// A bad command used to be fatal ("command_processor: unknown opcode 0x1D at pos 11946", issue #17).
// Now it is reported (rate-limited, with the bytes around it and through the application's
// callback), the processor skips to the next position where the stream parses as valid commands
// again (or to the end of the block it was given: one publish, so the next block starts on a command
// boundary) and goes on. Only a stream that stays broken is fatal (AURORA_FIFO_ERROR_LIMIT errors
// within 10 s, default 300).
namespace {
std::atomic<AuroraFifoErrorCallback> sErrorCallback{nullptr};
u64 sBlockBase = 0;   // absolute stream position of the block being processed (FIFO thread)
u32 sErrorCount = 0;  // errors this session
std::array<std::chrono::steady_clock::time_point, 4096> sErrorTimes{};
std::chrono::steady_clock::time_point sLastVerbose{};
u32 sSuppressed = 0;

u16 be16(const u8* p) noexcept { return static_cast<u16>(p[0] << 8 | p[1]); }
u32 be32(const u8* p) noexcept { return u32(p[0]) << 24 | u32(p[1]) << 16 | u32(p[2]) << 8 | p[3]; }

bool valid_draw_prim(u8 cmd) noexcept {
  switch (cmd & CP_OPCODE_MASK) {
  case GX_DRAW_QUADS:
  case GX_DRAW_TRIANGLES:
  case GX_DRAW_TRIANGLE_STRIP:
  case GX_DRAW_TRIANGLE_FAN:
  case GX_DRAW_LINES:
  case GX_DRAW_LINE_STRIP:
  case GX_DRAW_POINTS:
    return true;
  default:
    return false;
  }
}

// Bytes a GX_AURORA command takes after its 2-byte subcommand; -1 when it does not fit in `avail`
// or carries an impossible draw, -2 for a subcommand not listed here (a new subcommand should be
// added, or resynchronization will not accept it). `p` points after the subcommand.
s64 aurora_payload_len(u16 subCmd, const u8* p, size_t avail) noexcept {
  const auto fixed = [avail](s64 n) -> s64 { return static_cast<size_t>(n) <= avail ? n : -1; };
  if (subCmd >= GX_AURORA_LOAD_ARRAYBASE && subCmd <= (GX_AURORA_LOAD_ARRAYBASE | 0x0f)) {
    return fixed(13);
  }
  switch (subCmd) {
  case GX_AURORA_LOAD_VIEWPORT_RENDER:
    return fixed(24);
  case GX_AURORA_LOAD_SCISSOR_RENDER:
  case GX_AURORA_LOAD_COPY_SRC:
    return fixed(16);
  case GX_AURORA_LOAD_PROJECTION_FULL:
    return fixed(64);
  case GX_AURORA_DEBUG_GROUP_PUSH:
  case GX_AURORA_DEBUG_MARKER_INSERT:
    return avail >= 2 ? fixed(2 + be16(p)) : -1;
  case GX_AURORA_DEBUG_GROUP_POP:
  case GX_AURORA_SCENE_END:
  case GX_AURORA_REQUEST_DEPTH_SNAPSHOT:
  case GX_AURORA_END_OFFSCREEN:
    return 0;
  case GX_AURORA_LOAD_TEXOBJ:
    return avail >= 1 && p[0] < MaxTextures ? fixed(42) : -1;
  case GX_AURORA_LOAD_TLUT:
    return avail >= 1 && p[0] < MaxTluts ? fixed(23) : -1;
  case GX_AURORA_DESTROY_TEXOBJ:
  case GX_AURORA_DESTROY_TLUT:
  case GX_AURORA_PROBE_TAG:
    return fixed(4);
  case GX_AURORA_DESTROY_COPY_TEX:
  case GX_AURORA_LOAD_COPY_DEST:
  case GX_AURORA_BEGIN_OFFSCREEN:
  case GX_AURORA_MIRROR_MARK:
    return fixed(8);
  case GX_AURORA_LOAD_COPY_DST:
    return fixed(13);
  case GX_AURORA_SET_WIREFRAME:
    return fixed(5);
  case GX_AURORA_SET_CULL_OVERRIDE:
  case GX_AURORA_SET_DRAW_CLASS:
  case GX_AURORA_SET_MATERIAL_HINT:
  case GX_AURORA_STATE_SAVE:
  case GX_AURORA_STATE_RESTORE:
  case GX_AURORA_COPY_GENERATION:
  case GX_AURORA_SET_FADE:
  case GX_AURORA_SET_REPLAY_MASK:
    return fixed(1);
  case GX_AURORA_SET_EXTRA_LIGHTS:
    return avail >= 1 && p[0] <= gfx::MaxExtraLights ? fixed(1 + s64(p[0]) * GX_AURORA_EXTRA_LIGHT_WORDS * 4) : -1;
  case GX2_SET_POLYGON_OFFSET:
    return fixed(20);
  case GX_AURORA_DRAW_SIZED: {
    if (avail < 5 || !valid_draw_prim(p[0])) {
      return -1;
    }
    const u32 byteLen = be32(p + 1);
    if (byteLen != 0) {
      const u32 vtxSize = calc_vtx_size(static_cast<GXVtxFmt>(p[0] & CP_VAT_MASK));
      if (vtxSize == 0 || byteLen % vtxSize != 0 || byteLen / vtxSize > 0xFFFF) {
        return -1;
      }
    }
    return fixed(5 + s64(byteLen));
  }
  case GX_AURORA_DRAW_INDEXED: {
    if (avail < 7 || (p[0] & CP_OPCODE_MASK) != GX_DRAW_TRIANGLES) {
      return -1;
    }
    const u32 vtxSize = calc_vtx_size(static_cast<GXVtxFmt>(p[0] & CP_VAT_MASK));
    return fixed(7 + s64(be32(p + 3)) * 2 + s64(be16(p + 1)) * vtxSize);
  }
  default:
    return -2;
  }
}

// Length of the command at `pos` if it is a plausible one, else 0. Stricter than the processor (exact
// opcodes, known CP/XF addresses, sane counts): used to find where a broken stream is whole again.
// `counts` is false for commands that prove nothing (NOPs, empty draws).
size_t plausible_command_len(const u8* data, size_t size, size_t pos, bool& counts) noexcept {
  const u8 cmd = data[pos];
  const size_t avail = size - pos - 1;
  const u8* p = data + pos + 1;
  counts = true;
  switch (cmd) {
  case GX_NOP:
  case GX_CMD_INVL_VC:
    counts = cmd != GX_NOP;
    return 1;
  case GX_LOAD_BP_REG:
    return avail >= 4 ? 5 : 0;
  case GX_LOAD_CP_REG: {
    if (avail < 5) {
      return 0;
    }
    const u8 hi = p[0] >> 4, lo = p[0] & 0xF;
    const bool known = ((hi == 3 || hi == 4 || hi == 5 || hi == 6) && lo == 0) ||
                       ((hi == 7 || hi == 8 || hi == 9) && lo < 8) || ((hi == 0xA || hi == 0xB) && lo < 12);
    return known ? 6 : 0;
  }
  case GX_LOAD_XF_REG: {
    if (avail < 4) {
      return 0;
    }
    const u32 header = be32(p);
    const u32 count = ((header >> 16) & 0xFFFF) + 1;
    const u32 addr = header & 0xFFFF;
    const bool known = count <= 64 && (addr + count <= 0x500 || (addr >= 0x1000 && addr + count <= 0x1060));
    return known && avail >= 4 + size_t(count) * 4 ? 5 + size_t(count) * 4 : 0;
  }
  case GX_LOAD_INDX_A:
  case GX_LOAD_INDX_B:
  case GX_LOAD_INDX_C:
  case GX_LOAD_INDX_D:
    return avail >= 4 ? 5 : 0;
  case GX_CMD_CALL_DL:
    return avail >= 8 ? 9 : 0;
  case GX_AURORA: {
    if (avail < 2) {
      return 0;
    }
    const s64 len = aurora_payload_len(be16(p), p + 2, avail - 2);
    return len >= 0 ? 3 + static_cast<size_t>(len) : 0;
  }
  default:
    break;
  }
  if (!valid_draw_prim(cmd) || avail < 2) {
    return 0;
  }
  const u16 nVerts = be16(p);
  const u32 vtxSize = calc_vtx_size(static_cast<GXVtxFmt>(cmd & CP_VAT_MASK));
  const size_t bytes = size_t(nVerts) * vtxSize;
  counts = nVerts != 0 && vtxSize != 0;
  return avail - 2 >= bytes ? 3 + bytes : 0;
}

// The first position at or after `from` where at least 8 commands that count parse in a row, or
// commands parse up to the exact end of the block; `size` if there is none.
size_t find_resync(const u8* data, size_t size, size_t from) noexcept {
  for (size_t q = from; q < size; ++q) {
    size_t p = q;
    int good = 0;
    bool ok = true;
    while (p < size && good < 8) {
      bool counts = false;
      const size_t len = plausible_command_len(data, size, p, counts);
      if (len == 0) {
        ok = false;
        break;
      }
      good += counts ? 1 : 0;
      p += len;
    }
    if (ok && good >= 1) {
      return q;
    }
  }
  return size;
}

u32 error_limit() noexcept {
  static const u32 limit = [] {
    const char* env = std::getenv("AURORA_FIFO_ERROR_LIMIT");
    const long v = env != nullptr ? std::strtol(env, nullptr, 10) : 300;
    return static_cast<u32>(std::clamp<long>(v, 1, static_cast<long>(sErrorTimes.size())));
  }();
  return limit;
}

// Reports the bad command at `cmdPos` and returns where processing continues.
size_t bad_command(const u8* data, size_t size, size_t cmdPos, std::string what) noexcept {
  const size_t next = find_resync(data, size, cmdPos + 1);
  ++sErrorCount;
  const auto now = std::chrono::steady_clock::now();
  const u32 limit = error_limit();
  sErrorTimes[sErrorCount % limit] = now;
  const bool verbose = sErrorCount <= 10 || now - sLastVerbose >= std::chrono::seconds(1);
  if (verbose) {
    sLastVerbose = now;
    const size_t beforeStart = cmdPos > 64 ? cmdPos - 64 : 0;
    const size_t afterEnd = std::min(size, cmdPos + 32);
    std::string hex;
    for (size_t i = beforeStart; i < afterEnd; ++i) {
      hex += i == cmdPos ? fmt::format("[{:02x}]", data[i]) : fmt::format(" {:02x}", data[i]);
    }
    Log.error("FIFO: {} at stream position {} (offset {} of a {}-byte block); {} bytes skipped, {} (error {}{})",
              what, sBlockBase + cmdPos, cmdPos, size, next - cmdPos,
              next < size ? "resynchronized" : "rest of the block dropped", sErrorCount,
              sSuppressed != 0 ? fmt::format(", {} not shown", sSuppressed) : std::string{});
    Log.error("FIFO:   bytes {}..{}:{}", sBlockBase + beforeStart, sBlockBase + afterEnd - 1, hex);
    sSuppressed = 0;
    if (const auto cb = sErrorCallback.load(std::memory_order_acquire); cb != nullptr) {
      const AuroraFifoError error{
          .what = what.c_str(),
          .streamPos = sBlockBase + cmdPos,
          .blockOffset = static_cast<u32>(cmdPos),
          .blockSize = static_cast<u32>(size),
          .skipped = static_cast<u32>(next - cmdPos),
          .resynced = static_cast<u8>(next < size ? 1 : 0),
          .count = sErrorCount,
          .before = data + beforeStart,
          .beforeLen = static_cast<u32>(cmdPos - beforeStart),
          .after = data + cmdPos,
          .afterLen = static_cast<u32>(afterEnd - cmdPos),
      };
      cb(&error);
    }
  } else {
    ++sSuppressed;
  }
  // Hopeless: the limit's worth of errors within 10 s (the oldest of them is recent).
  if (sErrorCount >= limit && now - sErrorTimes[(sErrorCount + 1) % limit] < std::chrono::seconds(10)) {
    FATAL("The game's graphics command stream stays corrupted ({} malformed commands within 10 seconds, the "
          "last: {}). The log has the details.",
          limit, what);
  }
  return next;
}
} // namespace

void set_error_callback(AuroraFifoErrorCallback callback) noexcept {
  sErrorCallback.store(callback, std::memory_order_release);
}

ProcessResult process(const u8* data, u32 size, u64 streamBase) noexcept {
  ZoneScoped;
  sBlockBase = streamBase;
  ByteReader reader{{data, size}};
  // Continues at `pos` after a bad command (0235).
  const auto resume = [&](size_t pos) {
    reader = ByteReader{{data, size}};
    reader.skip(pos);
  };

  while (!reader.empty()) {
    const size_t cmdPos = reader.offset();
    const u8 cmd = reader.read<u8>();
    u8 opcode = cmd & CP_OPCODE_MASK;
    const size_t avail = reader.remaining();

    switch (opcode) {
    case CP_CMD_NOP:
      continue;

    case CP_CMD_LOAD_BP_REG: {
      if (avail < 4) {
        resume(bad_command(data, size, cmdPos, "truncated BP register load"));
        break;
      }
      const u32 value = reader.read<u32>();
      handle_bp(value);
      if (reg_get(value, 8, 24) == GX_BP_REG_DRAWDONE) {
        return {static_cast<u32>(reader.offset()), true};
      }
      break;
    }

    case CP_CMD_LOAD_CP_REG: {
      if (avail < 5) {
        resume(bad_command(data, size, cmdPos, "truncated CP register load"));
        break;
      }
      const u8 addr = reader.read<u8>();
      handle_cp(addr, reader.read<u32>());
      break;
    }

    case CP_CMD_LOAD_XF_REG: {
      const u32 count = avail >= 4 ? ((be32(data + cmdPos + 1) >> 16) & 0xFFFF) + 1 : 0;
      if (avail < 4 || avail - 4 < size_t(count) * sizeof(u32)) {
        resume(bad_command(data, size, cmdPos, fmt::format("XF load of {} words past the end of the block", count)));
        break;
      }
      const u32 header = reader.read<u32>();
      const u16 addr = header & 0xFFFF;
      handle_xf(addr, reader.take(count * sizeof(u32)));
      break;
    }

    case CP_CMD_LOAD_INDX_A:
    case CP_CMD_LOAD_INDX_B:
    case CP_CMD_LOAD_INDX_C:
    case CP_CMD_LOAD_INDX_D: {
      ZoneScopedN("LOAD_INDX");
      if (avail < 4) {
        resume(bad_command(data, size, cmdPos, "truncated indexed XF load"));
        break;
      }
      const u32 arrayType = GX_POS_MTX_ARRAY + (opcode - CP_CMD_LOAD_INDX_A) / 0x08;
      const u16 srcArrayIdx = reader.read<u16>();
      const u16 addrLen = reader.read<u16>();

      const u16 len = (addrLen >> 12) + 1;
      const u16 dstAddr = addrLen & 0x0FFF;
      auto const& array = g_gxState.arrays[arrayType];
      const u32 srcOffset = static_cast<u32>(srcArrayIdx) * array.stride;
      const u32 srcSize = static_cast<u32>(len) * sizeof(u32);
      if (array.data == nullptr ||
          (array.size != 0 && (srcOffset > array.size || srcSize > array.size - srcOffset))) {
        // A whole command that cannot be carried out: skipped, the stream stays in step.
        bad_command(data, size, cmdPos,
                    fmt::format("indexed XF load outside array {} (offset {}, size {}, array {} bytes{})", arrayType,
                                srcOffset, srcSize, array.size, array.data == nullptr ? ", unmapped" : ""));
        resume(cmdPos + 5);
        break;
      }
      auto const* srcData = static_cast<const u8*>(array.data) + srcOffset;
      if (!copy_xf_data(dstAddr, srcData, len, array.le ? std::endian::little : std::endian::big)) {
#ifndef NDEBUG
        Log.debug("Unimplemented indexed XF load (opcode 0x{:02X}, dstAddr=%04x)", opcode, dstAddr);
#endif
      }
      break;
    }

    case CP_CMD_CALL_DL: {
      if (avail < 8) {
        resume(bad_command(data, size, cmdPos, "truncated display list call"));
        break;
      }
      // Call display list: 8 bytes (address + size)
      Log.warn("Ignoring nested GX_CMD_CALL_DL");
      reader.skip(8);
      break;
    }

    case CP_CMD_INVAL_VTX: {
      for (auto& array : g_gxState.arrays) {
        array.cachedRange = {};
      }
      g_gxState.dirty |= DirtyImmediates;
      break;
    }

    case GX_AURORA: {
      if (avail < 2) {
        resume(bad_command(data, size, cmdPos, "truncated Aurora command"));
        break;
      }
      const u16 subCmd = be16(data + cmdPos + 1);
      const s64 len = aurora_payload_len(subCmd, data + cmdPos + 3, avail - 2);
      if (len == -1) {
        // A known subcommand that does not fit (or carries an impossible draw) would stop the
        // processor; unknown ones (another patch's) are left to handle_aurora as before.
        resume(bad_command(data, size, cmdPos, fmt::format("malformed Aurora command {:04X}", subCmd)));
        break;
      }
      handle_aurora(reader);
      break;
    }

    default: {
      if (cmd < 0x80 || !valid_draw_prim(cmd)) {
        resume(bad_command(data, size, cmdPos, fmt::format("unknown opcode 0x{:02X}", cmd)));
        break;
      }
      if (avail < 2) {
        resume(bad_command(data, size, cmdPos, "truncated draw"));
        break;
      }
      const u16 nVerts = be16(data + cmdPos + 1);
      const auto fmt = static_cast<GXVtxFmt>(cmd & CP_VAT_MASK);
      const u32 vtxSize = g_gxState.lastVtxFmt == fmt ? g_gxState.lastVtxSize : calc_vtx_size(fmt);
      const size_t bytes = size_t(nVerts) * vtxSize;
      if (avail - 2 < bytes) {
        resume(bad_command(data, size, cmdPos,
                           fmt::format("draw vertex data overrun (0x{:02X}: {} vertices of {} bytes, {} left)", cmd,
                                       nVerts, vtxSize, avail - 2)));
        break;
      }
      const auto prim = static_cast<GXPrimitive>(cmd & CP_OPCODE_MASK);
      if (nVerts == 0 || ((prim == GX_TRIANGLESTRIP || prim == GX_TRIANGLEFAN) && nVerts < 3)) {
        reader.skip(2 + bytes);  // nothing to draw (and no index buffer can be built for it)
        break;
      }
      handle_draw(cmd, reader);
      break;
    }
    }
  }
  return {size, false};
}

[[noreturn]] static void handle_draw_overrun(size_t totalVtxBytes, const ByteReader& reader) noexcept {
  // Hex dump around the draw command for debugging
  const size_t pos = reader.offset();
  const size_t size = reader.size();
  const u8* data = reader.data();
  size_t cmdPos = pos - 2 - 1; // opcode byte position (before vtxCount and pos++)
  size_t dumpStart = (cmdPos > 16) ? cmdPos - 16 : 0;
  size_t dumpEnd = (cmdPos + 32 < size) ? cmdPos + 32 : size;
  std::string hex;
  for (size_t i = dumpStart; i < dumpEnd; i++) {
    if (i == cmdPos)
      hex += fmt::format("[{:02x}]", data[i]);
    else
      hex += fmt::format(" {:02x}", data[i]);
  }
  Log.error("  hex dump around draw cmd (pos {}-{}):{}", dumpStart, dumpEnd - 1, hex);
  FATAL("draw vertex data overrun: need {} bytes at pos {}, have {}", totalVtxBytes, pos, reader.remaining());
}

static u32 calc_vtx_size(GXVtxFmt fmt) noexcept {
  u32 vtxSize = 0;
  const auto& vtxFmt = g_gxState.vtxFmts[fmt];
  for (int i = GX_VA_PNMTXIDX; i <= GX_VA_TEX7; ++i) {
    const auto& attrFmt = vtxFmt.attrs[i];
    switch (g_gxState.vtxDesc[i]) {
    case GX_NONE:
      break;
    case GX_DIRECT: {
      const auto attr = static_cast<GXAttr>(i);
      vtxSize += comp_type_size(attr, attrFmt.type) * comp_cnt_count(attr, attrFmt.cnt);
      break;
    }
    case GX_INDEX8:
      vtxSize += i == GX_VA_NRM && attrFmt.cnt == GX_NRM_NBT3 ? 3 : 1;
      break;
    case GX_INDEX16:
      vtxSize += i == GX_VA_NRM && attrFmt.cnt == GX_NRM_NBT3 ? 6 : 2;
      break;
    }
  }
  g_gxState.lastVtxFmt = fmt;
  g_gxState.lastVtxSize = vtxSize;
  return vtxSize;
}

// GX feature audit (AURORA_GX_AUDIT=1): logs each distinct hardware feature the first time a
// draw uses it, to see which parts of the GX pipeline a game exercises.
static void audit_tag(std::string tag) noexcept {
  static absl::flat_hash_set<std::string> seen;
  if (seen.insert(tag).second) {
    Log.info("[gx-audit] {}", tag);
  }
}

static void audit_pipeline(const PipelineConfig& pc, GXPrimitive prim) noexcept {
  static const bool enabled = [] {
    const char* env = std::getenv("AURORA_GX_AUDIT");
    return env != nullptr && env[0] == '1';
  }();
  if (!enabled) {
    return;
  }
  const auto& sc = pc.shaderConfig;
  const auto& st = g_gxState;
  const u32 cmode0 = st.bpRegCache[0x41];
  audit_tag(fmt::format("pixfmt={} zfmt={} dither={}", underlying(st.pixelFmt), underlying(st.zFmt),
                        (cmode0 >> 2) & 1));
  audit_tag(fmt::format("blend mode={} src={} dst={} logic={} dstAlpha={} alphaUpd={}", underlying(pc.blendMode),
                        underlying(pc.blendFacSrc), underlying(pc.blendFacDst),
                        pc.blendMode == GX_BM_LOGIC ? int(pc.blendOp) : -1, pc.dstAlpha != UINT32_MAX ? 1 : 0,
                        pc.alphaUpdate ? 1 : 0));
  if (sc.alphaCompare) {
    audit_tag(fmt::format("alphacmp {} {} op={} {} {} zcompBeforeTex={} zupd={}", underlying(sc.alphaCompare.comp0),
                          sc.alphaCompare.ref0, underlying(sc.alphaCompare.op), underlying(sc.alphaCompare.comp1),
                          sc.alphaCompare.ref1, st.zCompLocBeforeTex ? 1 : 0, pc.depthUpdate ? 1 : 0));
  }
  if (sc.lineMode != 0) {
    audit_tag(fmt::format("prim={} lineWidth={} pointSize={}", underlying(prim), st.lineWidth, st.pointSize));
  }
  audit_tag(fmt::format("tevstages={}", sc.tevStageCount));
  for (u32 i = 0; i < sc.tevStageCount; ++i) {
    const auto& s = sc.tevStages[i];
    audit_tag(fmt::format("tev color op={} bias={} scale={} clamp={} out={}", underlying(s.colorOp.op),
                          underlying(s.colorOp.bias), underlying(s.colorOp.scale), s.colorOp.clamp ? 1 : 0,
                          underlying(s.colorOp.outReg)));
    audit_tag(fmt::format("tev alpha op={} bias={} scale={} clamp={} out={}", underlying(s.alphaOp.op),
                          underlying(s.alphaOp.bias), underlying(s.alphaOp.scale), s.alphaOp.clamp ? 1 : 0,
                          underlying(s.alphaOp.outReg)));
    audit_tag(fmt::format("tev color in a={} b={} c={} d={}", underlying(s.colorPass.a), underlying(s.colorPass.b),
                          underlying(s.colorPass.c), underlying(s.colorPass.d)));
    audit_tag(fmt::format("tev alpha in a={} b={} c={} d={}", underlying(s.alphaPass.a), underlying(s.alphaPass.b),
                          underlying(s.alphaPass.c), underlying(s.alphaPass.d)));
    const auto usesC = [&](GXTevColorArg a) {
      return s.colorPass.a == a || s.colorPass.b == a || s.colorPass.c == a || s.colorPass.d == a;
    };
    const auto usesA = [&](GXTevAlphaArg a) {
      return s.alphaPass.a == a || s.alphaPass.b == a || s.alphaPass.c == a || s.alphaPass.d == a;
    };
    if (usesC(GX_CC_KONST)) {
      audit_tag(fmt::format("tev kcsel={}", underlying(s.kcSel)));
    }
    if (usesA(GX_CA_KONST)) {
      audit_tag(fmt::format("tev kasel={}", underlying(s.kaSel)));
    }
    if (usesC(GX_CC_RASC) || usesC(GX_CC_RASA) || usesA(GX_CA_RASA)) {
      if (sc.tevSwapTable[s.tevSwapRas]) {
        audit_tag(fmt::format("tev swap ras sel={}", underlying(s.tevSwapRas)));
      }
    }
    if (usesC(GX_CC_TEXC) || usesC(GX_CC_TEXA) || usesA(GX_CA_TEXA)) {
      if (sc.tevSwapTable[s.tevSwapTex]) {
        const auto& sw = sc.tevSwapTable[s.tevSwapTex];
        audit_tag(fmt::format("tev swap tex sel={} ({}{}{}{})", underlying(s.tevSwapTex), underlying(sw.red),
                              underlying(sw.green), underlying(sw.blue), underlying(sw.alpha)));
      }
    }
    if (s.indTexMtxId != GX_ITM_OFF || s.indTexWrapS != GX_ITW_OFF || s.indTexWrapT != GX_ITW_OFF ||
        s.indTexAddPrev || s.indTexAlphaSel != GX_ITBA_OFF) {
      audit_tag(fmt::format("ind stage fmt={} bias={} alpha={} mtx={} wrap={},{} addPrev={} origLOD={}",
                            underlying(s.indTexFormat), underlying(s.indTexBiasSel), underlying(s.indTexAlphaSel),
                            underlying(s.indTexMtxId), underlying(s.indTexWrapS), underlying(s.indTexWrapT),
                            s.indTexAddPrev ? 1 : 0, s.indTexUseOrigLOD ? 1 : 0));
    }
  }
  for (u32 i = 0; i < sc.numIndStages; ++i) {
    audit_tag(fmt::format("ind scale {} {}", underlying(sc.indStages[i].scaleS), underlying(sc.indStages[i].scaleT)));
  }
  for (u32 i = 0; i < MaxColorChannels; ++i) {
    const auto& cc = sc.colorChannels[i];
    if (cc.lightingEnabled) {
      audit_tag(fmt::format("light chan={} diff={} attn={} amb={} mat={}", i, underlying(cc.diffFn),
                            underlying(cc.attnFn), underlying(cc.ambSrc), underlying(cc.matSrc)));
    }
  }
  for (u32 i = 0; i < st.numTexGens; ++i) {
    const auto& t = sc.tcgs[i];
    audit_tag(fmt::format("texgen type={} src={} norm={} post={}", underlying(t.type), underlying(t.src),
                          t.normalize ? 1 : 0, t.postMtx != GX_PTIDENTITY ? 1 : 0));
  }
  if (sc.fogType != GX_FOG_NONE) {
    audit_tag(fmt::format("fog type={} range={}", unsigned(sc.fogType), sc.fogRangeEnabled ? 1 : 0));
  }
}

// Per-draw debug log (AURORA_DRAW_LOG=<file>, AURORA_DRAW_LOG_FRAMES=<first>,<last>): every draw
// of those recorded frames with its full pixel pipeline state (TEV stages and registers, texture
// maps, texgens, channels, blend/Z/alpha compare, masks). AURORA_DRAW_SKIP=<i>[,<i>...] drops
// those draw ordinals (per frame) in the same frames, to find which draw makes which pixels.
static std::atomic<u32> sDbgFrame{0};
static u32 sDbgDraw = 0;
struct DrawDebug {
  FILE* file = nullptr;
  u32 first = 0, last = UINT32_MAX;
  std::vector<u32> skip;
  std::vector<std::vector<std::string>> match;
  bool any = false;
};
static const DrawDebug& draw_debug() noexcept {
  static const DrawDebug d = [] {
    DrawDebug r;
    if (const char* p = std::getenv("AURORA_DRAW_LOG")) {
      r.file = std::fopen(p, "w");
    }
    if (const char* p = std::getenv("AURORA_DRAW_LOG_FRAMES")) {
      std::sscanf(p, "%u,%u", &r.first, &r.last);
    }
    if (const char* p = std::getenv("AURORA_DRAW_SKIP")) {
      for (const char* s = p; *s;) {
        char* end = nullptr;
        const unsigned long v = std::strtoul(s, &end, 10);
        if (end == s) {
          break;
        }
        u32 hi = static_cast<u32>(v);
        if (*end == '-') { // a range a-b
          const char* t = end + 1;
          hi = static_cast<u32>(std::strtoul(t, &end, 10));
        }
        for (u32 i = static_cast<u32>(v); i <= hi; ++i) {
          r.skip.push_back(i);
        }
        s = *end ? end + 1 : end;
      }
    }
    // AURORA_DRAW_SKIP_MATCH=<text>[|<text>...]: drop draws whose log entry contains all the
    // '&'-separated parts of any '|' alternative (e.g. "dst=2 lop=4&s2 "), whatever their ordinal.
    if (const char* p = std::getenv("AURORA_DRAW_SKIP_MATCH")) {
      std::string all = p;
      size_t pos = 0;
      while (pos <= all.size()) {
        const size_t bar = std::min(all.find('|', pos), all.size());
        std::vector<std::string> parts;
        const std::string alt = all.substr(pos, bar - pos);
        size_t q = 0;
        while (q <= alt.size()) {
          const size_t amp = std::min(alt.find('&', q), alt.size());
          if (amp > q) {
            parts.push_back(alt.substr(q, amp - q));
          }
          q = amp + 1;
        }
        if (!parts.empty()) {
          r.match.push_back(std::move(parts));
        }
        pos = bar + 1;
      }
    }
    r.any = r.file != nullptr || !r.skip.empty() || !r.match.empty();
    return r;
  }();
  return d;
}
static std::string dbg_color(const Vec4<float>& c) {
  return fmt::format("({},{},{},{})", int(std::round(c.x() * 255.f)), int(std::round(c.y() * 255.f)),
                     int(std::round(c.z() * 255.f)), int(std::round(c.w() * 255.f)));
}
// Returns true when the draw is to be skipped.
static bool draw_debug_note(const PipelineConfig& pc, GXPrimitive prim, u16 vtxCount) noexcept {
  const auto& dbg = draw_debug();
  if (!dbg.any) {
    return false;
  }
  const u32 frame = sDbgFrame.load(std::memory_order_relaxed);
  if (frame < dbg.first || frame > dbg.last) {
    return false;
  }
  const u32 idx = sDbgDraw++;
  bool skip = std::find(dbg.skip.begin(), dbg.skip.end(), idx) != dbg.skip.end();
  if (dbg.file == nullptr && dbg.match.empty()) {
    return skip;
  }
  const auto& sc = pc.shaderConfig;
  const auto& st = g_gxState;
  std::string s = fmt::format("F{} D{} prim={} n={} vp=({:.0f},{:.0f} {:.0f}x{:.0f}) sc=({},{} {}x{})", frame, idx,
                              underlying(prim), vtxCount, st.logicalViewport.left,
                              st.logicalViewport.top, st.logicalViewport.width, st.logicalViewport.height,
                              st.logicalScissor.x, st.logicalScissor.y, st.logicalScissor.width,
                              st.logicalScissor.height);
  s += fmt::format("\n  blend={} src={} dst={} lop={} z={}/{}/{} cupd={} aupd={} dstA={} acmp={} {} op{} {} {} zloc={} "
                   "cull={} pix={} dither={} fog={}",
                   underlying(pc.blendMode), underlying(pc.blendFacSrc), underlying(pc.blendFacDst),
                   underlying(pc.blendOp), pc.depthCompare ? 1 : 0, underlying(pc.depthFunc), pc.depthUpdate ? 1 : 0,
                   pc.colorUpdate ? 1 : 0, pc.alphaUpdate ? 1 : 0,
                   st.dstAlpha == UINT32_MAX ? -1 : int(st.dstAlpha), underlying(sc.alphaCompare.comp0),
                   sc.alphaCompare.ref0, underlying(sc.alphaCompare.op), underlying(sc.alphaCompare.comp1),
                   sc.alphaCompare.ref1, st.zCompLocBeforeTex ? 1 : 0, underlying(st.cullMode),
                   underlying(st.pixelFmt), st.dither ? 1 : 0, unsigned(sc.fogType));
  for (u32 i = 0; i < sc.tevStageCount; ++i) {
    const auto& t = sc.tevStages[i];
    s += fmt::format("\n  s{} tc={} map={} ch={} C[{} {} {} {} op{} b{} s{} cl{} ->{}] A[{} {} {} {} op{} b{} s{} cl{} ->{}] "
                     "kc={} ka={} swap={}/{}",
                     i, int(t.texCoordId), int(t.texMapId), int(t.channelId), underlying(t.colorPass.a),
                     underlying(t.colorPass.b), underlying(t.colorPass.c), underlying(t.colorPass.d),
                     underlying(t.colorOp.op), underlying(t.colorOp.bias), underlying(t.colorOp.scale),
                     t.colorOp.clamp ? 1 : 0, underlying(t.colorOp.outReg), underlying(t.alphaPass.a),
                     underlying(t.alphaPass.b), underlying(t.alphaPass.c), underlying(t.alphaPass.d),
                     underlying(t.alphaOp.op), underlying(t.alphaOp.bias), underlying(t.alphaOp.scale),
                     t.alphaOp.clamp ? 1 : 0, underlying(t.alphaOp.outReg), underlying(t.kcSel),
                     underlying(t.kaSel), underlying(t.tevSwapRas), underlying(t.tevSwapTex));
    if (t.indTexMtxId != GX_ITM_OFF || t.indTexWrapS != GX_ITW_OFF || t.indTexWrapT != GX_ITW_OFF ||
        t.indTexAddPrev || t.indTexAlphaSel != GX_ITBA_OFF) {
      s += fmt::format(" ind[st{} f{} b{} m{} w{},{} add{} a{}]", underlying(t.indTexStage),
                       underlying(t.indTexFormat), underlying(t.indTexBiasSel), underlying(t.indTexMtxId),
                       underlying(t.indTexWrapS), underlying(t.indTexWrapT), t.indTexAddPrev ? 1 : 0,
                       underlying(t.indTexAlphaSel));
    }
  }
  s += fmt::format("\n  regs prev={} r0={} r1={} r2={} k0={} k1={} k2={} k3={}", dbg_color(st.colorRegs[0]),
                   dbg_color(st.colorRegs[1]), dbg_color(st.colorRegs[2]), dbg_color(st.colorRegs[3]),
                   dbg_color(st.kcolors[0]), dbg_color(st.kcolors[1]), dbg_color(st.kcolors[2]),
                   dbg_color(st.kcolors[3]));
  for (u32 i = 0; i < 2 && i < st.numChans; ++i) {
    const auto& c = sc.colorChannels[i];
    const auto& a = sc.colorChannels[i + GX_ALPHA0];
    s += fmt::format("\n  chan{} light={} mat={} amb={} diff={} attn={} | alpha light={} mat={} amb={} | mat={} amb={} "
                     "amat={} aamb={} mask={:02X}",
                     i, c.lightingEnabled ? 1 : 0, underlying(c.matSrc), underlying(c.ambSrc), underlying(c.diffFn),
                     underlying(c.attnFn), a.lightingEnabled ? 1 : 0, underlying(a.matSrc), underlying(a.ambSrc),
                     dbg_color(st.colorChannelState[i].matColor), dbg_color(st.colorChannelState[i].ambColor),
                     dbg_color(st.colorChannelState[i + GX_ALPHA0].matColor),
                     dbg_color(st.colorChannelState[i + GX_ALPHA0].ambColor),
                     static_cast<unsigned>(st.colorChannelState[i].lightMask.to_ulong()));
  }
  for (u32 i = 0; i < st.numTexGens; ++i) {
    const auto& t = sc.tcgs[i];
    s += fmt::format("\n  tg{} type={} src={} mtx={} post={} norm={} su={}x{}", i, underlying(t.type),
                     underlying(t.src), underlying(t.mtx), underlying(t.postMtx), t.normalize ? 1 : 0,
                     st.texCoordScales[i].scaleS + 1, st.texCoordScales[i].scaleT + 1);
  }
  for (u32 i = 0; i < MaxTextures; ++i) {
    const auto& tex = st.textures[i];
    if (tex) {
      s += fmt::format("\n  tex{} {}x{} fmt={} wrap={},{} filt={},{} lod={:.2f}..{:.2f} bias={:.2f} edge={} mips={}/{}{}", i,
                       tex.texObj.width(), tex.texObj.height(), tex.texObj.format(), underlying(tex.texObj.wrap_s()),
                       underlying(tex.texObj.wrap_t()), underlying(tex.texObj.min_filter()),
                       underlying(tex.texObj.mag_filter()), tex.texObj.min_lod(), tex.texObj.max_lod(),
                       tex.texObj.lod_bias(), tex.texObj.do_edge_lod() ? 1 : 0, tex.texObj.mip_count(),
                       tex.ref ? tex.ref->mipCount : 0, tex.ref && tex.ref->hasArbitraryMips ? " arb" : "");
    }
  }
  for (u32 i = 0; i < sc.numIndStages; ++i) {
    s += fmt::format("\n  indstage{} tc={} map={} scale={},{}", i, underlying(sc.indStages[i].texCoordId),
                     underlying(sc.indStages[i].texMapId), underlying(sc.indStages[i].scaleS),
                     underlying(sc.indStages[i].scaleT));
  }
  for (const auto& alt : dbg.match) {
    if (std::all_of(alt.begin(), alt.end(), [&](const std::string& part) { return s.find(part) != std::string::npos; })) {
      skip = true;
    }
  }
  if (dbg.file != nullptr) {
    std::fprintf(dbg.file, "%s%s\n", skip ? "SKIPPED " : "", s.c_str());
  }
  return skip;
}

// Z compare before texturing (GXSetZCompLoc(GX_TRUE)) tests *and updates* Z before the TEV and
// the alpha test, so a pixel the alpha test rejects still writes its depth (SDK: "when alpha
// compare is used, Z buffering must be done after texturing"; the Z-texture patent: the alpha
// result masks Z writes only with late Z; Dolphin forces early fragment tests for it). WebGPU
// never writes depth for a discarded fragment, so such draws are split in two: a depth-only pass
// without the alpha test (the depth this draw writes on the console), then the colour pass with
// the alpha test against that depth (no Z write; LESS/GREATER widened by equality, which the
// fragment's own depth now has). AURORA_EARLY_Z_SPLIT=0 turns it off (A/B).
static bool alpha_compare_passes(const AlphaCompare& ac, u32 a) noexcept {
  const auto cmp = [a](GXCompare c, u32 ref) {
    switch (c) {
    case GX_NEVER: return false;
    case GX_LESS: return a < ref;
    case GX_EQUAL: return a == ref;
    case GX_LEQUAL: return a <= ref;
    case GX_GREATER: return a > ref;
    case GX_NEQUAL: return a != ref;
    case GX_GEQUAL: return a >= ref;
    default: return true;
    }
  };
  const bool p0 = cmp(ac.comp0, ac.ref0), p1 = cmp(ac.comp1, ac.ref1);
  switch (ac.op) {
  case GX_AOP_AND: return p0 && p1;
  case GX_AOP_OR: return p0 || p1;
  case GX_AOP_XOR: return p0 != p1;
  default: return p0 == p1; // XNOR
  }
}
static bool early_z_alpha_split(const PipelineConfig& pc) noexcept {
  // AURORA_EARLY_Z_SPLIT=0 off, =ab only in odd frames (A/B within one run)
  // 0179: unset, the application's GX_AURORA_ACCURACY_EARLY_Z flag decides (=1 forces it on).
  static const int mode = [] {
    const char* e = std::getenv("AURORA_EARLY_Z_SPLIT");
    return e == nullptr ? 3 : e[0] == '0' ? 0 : e[0] == 'a' ? 2 : 1;
  }();
  const bool enabled =
      mode == 1 || (mode == 2 && (sDbgFrame.load(std::memory_order_relaxed) & 1) != 0) ||
      (mode == 3 && (g_accuracyFlags.load(std::memory_order_relaxed) & GX_AURORA_ACCURACY_EARLY_Z) != 0);
  if (!enabled || !g_gxState.zCompLocBeforeTex || !pc.depthCompare || !pc.depthUpdate ||
      !pc.shaderConfig.alphaCompare || pc.shaderConfig.lineMode != 0 || pc.shaderConfig.wireframe) {
    return false;
  }
  for (u32 a = 0; a < 256; ++a) {
    if (!alpha_compare_passes(pc.shaderConfig.alphaCompare, a)) {
      return true;
    }
  }
  return false;
}
static bool early_z_merge() noexcept {
  // GX_AURORA_ACCURACY_EARLY_Z_MERGE; AURORA_EARLY_Z_MERGE=0 off, 1 on, ab: on in odd frames (A/B).
  static const int mode = [] {
    const char* e = std::getenv("AURORA_EARLY_Z_MERGE");
    return e == nullptr ? 3 : e[0] == '0' ? 0 : e[0] == 'a' ? 2 : 1;
  }();
  return mode == 1 || (mode == 2 && (sDbgFrame.load(std::memory_order_relaxed) & 1) != 0) ||
         (mode == 3 && (g_accuracyFlags.load(std::memory_order_relaxed) & GX_AURORA_ACCURACY_EARLY_Z_MERGE) != 0);
}
static void push_early_z_split(const DrawCache& cache, const DrawData& data) noexcept {
  static const bool sLog = std::getenv("AURORA_EARLY_Z_LOG") != nullptr;
  if (sLog) {
    static u32 sCount = 0, sLastFrame = 0;
    ++sCount;
    if (const u32 f = sDbgFrame.load(std::memory_order_relaxed); f >= sLastFrame + 300) {
      Log.info("[early-z] {} split draws in frames {}..{}", sCount, sLastFrame, f);
      sCount = 0;
      sLastFrame = f;
    }
  }
  DrawData first = data;
  first.pipeline = cache.splitDepth;
  first.dstAlpha = UINT32_MAX;
  // 0179: the next draw must not be merged into one half of the split (draw_prim extends the last
  // draw command with the following draw when the GX state is unchanged): merged geometry then
  // got only the colour pass without a depth write, or only the depth pass without colour.
  // With the cache's format reset, the next draw is pushed on its own (and split again).
  // 0182: with AURORA_EARLY_Z_MERGE=1 the next draw is merged into both halves instead.
  if (!early_z_merge()) {
    sDrawCache.lastDrawFmt = GX_MAX_VTXFMT;
  }
  gfx::push_draw_command(first);
  DrawData second = data;
  second.pipeline = cache.splitColor;
  gfx::push_draw_command(second);
}

// 0182: the split decision and its two pipelines are derived from the cached pipeline config, so
// they are computed once per pipeline change instead of for every draw (two pipeline lookups
// hashing the whole config per draw; ~16,000 split draws per foyer frame).
static bool split_variants_impl(const PipelineConfig& pc, PipelineConfig& depthOnly, PipelineConfig& color) noexcept;
static bool early_z_split_cached(DrawCache& cache) noexcept {
  const u32 flags = g_accuracyFlags.load(std::memory_order_relaxed) | (g_gxState.zCompLocBeforeTex ? 0x80000000u : 0u);
  const auto targetKey = gfx::get_render_target_layout().key;
  if (cache.splitValid && cache.splitFor == cache.pipelineRef && cache.splitFlags == flags &&
      cache.splitTargetKey == targetKey) {
    return cache.split;
  }
  cache.splitValid = true;
  cache.splitFor = cache.pipelineRef;
  cache.splitFlags = flags;
  cache.splitTargetKey = targetKey;
  cache.split = early_z_alpha_split(cache.config);
  if (!cache.split) {
    return false;
  }
  PipelineConfig depthOnly, color;
  split_variants_impl(cache.config, depthOnly, color);
  cache.splitDepth = gfx::pipeline_ref(depthOnly);
  cache.splitColor = gfx::pipeline_ref(color);
  return true;
}

static bool split_variants_impl(const PipelineConfig& pc, PipelineConfig& depthOnly, PipelineConfig& color) noexcept {
  if (!pc.depthCompare || !pc.depthUpdate || !pc.shaderConfig.alphaCompare || pc.shaderConfig.lineMode != 0 ||
      pc.shaderConfig.wireframe) {
    return false;
  }
  bool rejects = false;
  for (u32 a = 0; a < 256 && !rejects; ++a) {
    rejects = !alpha_compare_passes(pc.shaderConfig.alphaCompare, a);
  }
  if (!rejects) {
    return false;
  }
  depthOnly = pc;
  depthOnly.shaderConfig.alphaCompare = AlphaCompare{GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0};
  depthOnly.colorUpdate = false;
  depthOnly.alphaUpdate = false;
  depthOnly.blendMode = GX_BM_NONE;
  depthOnly.blendFacSrc = GX_BL_ONE;
  depthOnly.blendFacDst = GX_BL_ZERO;
  depthOnly.blendOp = GX_LO_COPY;
  depthOnly.dstAlpha = UINT32_MAX;
  color = pc;
  color.depthUpdate = false;
  switch (color.depthFunc) {
  case GX_LESS: color.depthFunc = GX_LEQUAL; break;
  case GX_GREATER: color.depthFunc = GX_GEQUAL; break;
  case GX_NEQUAL: color.depthFunc = GX_EQUAL; break;
  default: break;
  }
  return true;
}

static void push_gx_draw(GXPrimitive prim, GXVtxFmt fmt, u16 vtxCount, gfx::Range vertRange, gfx::Range idxRange,
                         u32 numIndices) noexcept {
  auto& state = g_gxState;
  auto& cache = sDrawCache;

  // GX_CULL_ALL rejects every triangle; lines and points are not culled.
  if (state.cullMode == GX_CULL_ALL && prim != GX_LINES && prim != GX_LINESTRIP && prim != GX_POINTS) {
    return;
  }
  if (state.replayMask) {
    return;
  }

  // Bits 8..15: GX_AURORA_SET_FADE (the matrix index stays in the low byte). Bit 16: smooth EFB
  // quantization, bit 17: with sub-LSB noise (0190, GX_AURORA_ACCURACY_EFB_SMOOTH / _NOISE).
  const u32 smoothFlags = g_accuracyFlags.load(std::memory_order_relaxed);
  const u32 smoothBits = (smoothFlags & GX_AURORA_ACCURACY_EFB_SMOOTH) == 0 ? 0u
                         : (smoothFlags & GX_AURORA_ACCURACY_EFB_SMOOTH_NOISE) != 0 ? 0x30000u
                                                                                     : 0x10000u;
  DrawImmediateData immediates{.vtxStart = vertRange.offset,
                               .currentPnMtx = state.currentPnMtx | u32(state.fadeQ) << 8 | smoothBits};
  for (int i = GX_VA_POS; i <= GX_VA_TEX7; ++i) {
    if (state.vtxDesc[i] != GX_INDEX8 && state.vtxDesc[i] != GX_INDEX16) {
      continue;
    }
    auto& array = state.arrays[i];
    if (array.cachedRange.size == 0 && array.size != 0) {
      array.cachedRange = gfx::push_storage(static_cast<const uint8_t*>(array.data), array.size);
    }
    immediates.arrayStart[i - GX_VA_POS] = array.cachedRange.offset;
  }

  static u32 sLastAccuracy = UINT32_MAX;
  if (const u32 accuracy = g_accuracyFlags.load(std::memory_order_relaxed); accuracy != sLastAccuracy) {
    sLastAccuracy = accuracy;
    state.dirty |= DirtyPipeline;
  }
  if (gfx::enhanced::sync_config()) {
    state.dirty |= DirtyPipeline;
  }
  const u8 lineMode = line_mode_for_prim(prim);
  const auto targetLayoutKey = gfx::get_render_target_layout().key;
  const bool pipelineValid = cache.hasPipeline && (state.dirty & DirtyPipeline) == 0 && cache.fmt == fmt &&
                             cache.lineMode == lineMode && cache.targetLayoutKey == targetLayoutKey;
  if (!pipelineValid) {
    const bool hadPipeline = cache.hasPipeline;
    const auto prevSampledTextures = cache.shaderInfo.sampledTextures;
    const auto prevSampledIndTextures = cache.shaderInfo.sampledIndTextures;
    populate_pipeline_config(cache.config, prim, fmt);
    cache.efbBlend6 = g_drawEfbBlend6;
    audit_pipeline(cache.config, prim);
    cache.shaderInfo = build_shader_info(cache.config.shaderConfig);
    cache.pipelineRef = gfx::pipeline_ref(cache.config);
    cache.targetLayoutKey = targetLayoutKey;
    cache.fmt = fmt;
    cache.lineMode = lineMode;
    cache.hasPipeline = true;
    state.dirty = (state.dirty & ~DirtyPipeline) | DirtyUniform;
    if (!hadPipeline || prevSampledTextures != cache.shaderInfo.sampledTextures ||
        prevSampledIndTextures != cache.shaderInfo.sampledIndTextures) {
      cache.bindGeneration = 0;
    }
  }

  const bool bindGroupsValid =
      (state.dirty & DirtyTextures) == 0 && cache.bindGeneration == texture::current_bind_generation();
  if (!bindGroupsValid) {
    const auto prevBindGroup = cache.bindGroups.textureBindGroup;
    resolve_sampled_textures(cache.shaderInfo);
    cache.bindGroups = build_bind_groups(cache.shaderInfo);
    cache.bindGeneration = texture::current_bind_generation();
    state.dirty &= ~DirtyTextures;
    // For texture_size_bias uniform
    if (cache.bindGroups.textureBindGroup != prevBindGroup) {
      state.dirty |= DirtyUniform;
    }
  }

  const bool uniformValid = (state.dirty & DirtyUniform) == 0 && cache.uniformRange.size != 0;
  if (!uniformValid) {
    cache.uniformRange = build_uniform(cache.shaderInfo);
    state.dirty &= ~DirtyUniform;
  }
  if (cache.config.shaderConfig.fogRangeEnabled) {
    const auto key = fog_range_lut_key();
    if (!cache.hasFogRange || cache.fogRangeKey != key) {
      cache.fogRange = push_fog_range_lut(key);
      cache.fogRangeKey = key;
      cache.hasFogRange = true;
    }
  }
  immediates.fogRangeBase = cache.fogRange.offset / sizeof(u32);

  state.dirty &= ~DirtyImmediates;

  uint32_t instanceCount = 1;
  if (prim == GX_LINES) {
    instanceCount = vtxCount / 2;
  } else if (prim == GX_LINESTRIP) {
    instanceCount = vtxCount - 1;
  } else if (prim == GX_POINTS) {
    instanceCount = vtxCount;
  }
  cache.lastDrawFmt = fmt;
  const DrawData drawData{
      .pipeline = cache.pipelineRef,
      .bindGroups = cache.bindGroups,
      .vertRange = vertRange,
      .idxRange = idxRange,
      .uniformRange = cache.uniformRange,
      .immediateData = immediates,
      .vtxCount = vtxCount,
      .indexCount = numIndices,
      .instanceCount = instanceCount,
      .dstAlpha = state.dstAlpha,
  };
  if (draw_debug_note(cache.config, prim, vtxCount)) [[unlikely]] {
    return;
  }
  // 0190: smoothed EFB (bit 16) leaves the blend result unquantized (no 6-bit resolve of the pass).
  if (cache.efbBlend6 != 0 && smoothBits == 0 && (cache.config.colorUpdate || cache.config.alphaUpdate)) {
    gfx::note_efb_blend6(cache.efbBlend6 == 3);
  }
  if (gfx::enhanced::config().enabled) {
    if (gfx::enhanced::note_draw(cache.config, state.drawClass, vtxCount, drawData)) {
      return; // debug: hidden
    }
  }
  if (early_z_split_cached(cache)) [[unlikely]] {
    push_early_z_split(cache, drawData);
    return;
  }
  gfx::push_draw_command(drawData);
}

static void handle_draw_unmerged(GXPrimitive prim, GXVtxFmt fmt, u16 vtxCount, gfx::Range vertRange) noexcept {
  ZoneScoped;
  u32 numIndices = 0;
  gfx::Range idxRange;

  if (wire_prim(prim)) {
    static ByteBuffer triBuf, lineBuf;
    const u32 tris = prepare_idx_buffer(triBuf, prim, 0, vtxCount);
    numIndices = wire_edges(lineBuf, reinterpret_cast<const u16*>(triBuf.data()), tris);
    idxRange = gfx::push_indices(lineBuf.data(), lineBuf.size(), 4);
    triBuf.clear();
    lineBuf.clear();
    if (numIndices != 0) {
      push_gx_draw(prim, fmt, vtxCount, vertRange, idxRange, numIndices);
    }
    return;
  }

  if (prim != GX_TRIANGLES) {
    ZoneScopedN("build idx buffer");
    static ByteBuffer idxBuf;
    numIndices = prepare_idx_buffer(idxBuf, prim, 0, vtxCount);
    idxRange = gfx::push_indices(idxBuf.data(), idxBuf.size(), 4);
    idxBuf.clear();
  }

  push_gx_draw(prim, fmt, vtxCount, vertRange, idxRange, numIndices);
}

// Arrays registered with size 0 have an unknown extent (e.g. set by recompiled
// console code, which never passes sizes). Size them from the largest index the
// draw actually references, growing the uploaded range when a later draw needs more.
static std::unordered_map<const void*, gfx::Range> sDynamicUploads;

void reset_dynamic_arrays() noexcept {
  sDynamicUploads.clear();
  // Once per recorded frame: the draw debug log's frame boundary.
  sDbgFrame.fetch_add(1, std::memory_order_relaxed);
  if (sDbgDraw != 0 && draw_debug().file != nullptr) {
    std::fprintf(draw_debug().file, "---- end of frame %u (%u draws)\n", sDbgFrame.load() - 1, sDbgDraw);
    std::fflush(draw_debug().file);
  }
  sDbgDraw = 0;
}

static void size_dynamic_arrays(GXVtxFmt fmt, const u8* vtx, u16 vtxCount, u32 vtxSize) noexcept {
  const auto& vtxFmt = g_gxState.vtxFmts[fmt];
  u32 offset = 0;
  for (int i = GX_VA_PNMTXIDX; i <= GX_VA_TEX7; ++i) {
    const auto& attrFmt = vtxFmt.attrs[i];
    const auto type = g_gxState.vtxDesc[i];
    if (type == GX_NONE) {
      continue;
    }
    if (type == GX_DIRECT) {
      const auto attr = static_cast<GXAttr>(i);
      offset += comp_type_size(attr, attrFmt.type) * comp_cnt_count(attr, attrFmt.cnt);
      continue;
    }
    const bool nbt3 = i == GX_VA_NRM && attrFmt.cnt == GX_NRM_NBT3;
    const u32 idxSize = type == GX_INDEX16 ? 2 : 1;
    const u32 width = nbt3 ? idxSize * 3 : idxSize;
    auto& array = g_gxState.arrays[i];
    if (array.size == 0 && array.data != nullptr) {
      u32 maxIdx = 0;
      bool any = false;
      for (u32 v = 0; v < vtxCount; ++v) {
        const u8* p = vtx + v * vtxSize + offset;
        for (u32 k = 0; k < width; k += idxSize) {
          const u32 idx = idxSize == 2 ? (u32(p[k]) << 8 | p[k + 1]) : p[k];
          if (idx == (idxSize == 2 ? 0xFFFFu : 0xFFu)) {
            continue;
          }
          maxIdx = std::max(maxIdx, idx);
          any = true;
        }
      }
      const u32 stride = array.stride != 0 ? array.stride : 1;
      const u32 needed = any ? (maxIdx + 1) * stride + (nbt3 ? 2 * stride : 0) : 0;
      if (needed > array.cachedRange.size) {
        // Share uploads between arrays/draws using the same memory this frame, and
        // grow geometrically so a slowly increasing index range isn't re-uploaded per draw.
        auto& shared = sDynamicUploads[array.data];
        if (needed > shared.size) {
          u32 size = 256;
          while (size < needed) size *= 2;
          shared = gfx::push_storage(static_cast<const uint8_t*>(array.data), size);
        }
        array.cachedRange = shared;
        g_gxState.dirty |= DirtyImmediates;
      }
    }
    offset += width;
  }
}

// Draw probe (GXAurora.h AuroraSetDrawProbe): where each draw is drawn, for judder measurements.
std::atomic<AuroraDrawProbeFn> sDrawProbe{nullptr};
u32 sProbeTag = ~0u;

// The position (model space) and position-matrix slot of the vertex at `vtx`.
static bool probe_vertex(GXVtxFmt fmt, const u8* vtx, float pos[3], u32& mtxIdx) noexcept {
  const auto& vtxFmt = g_gxState.vtxFmts[fmt];
  mtxIdx = g_gxState.currentPnMtx;
  u32 offset = 0;
  for (int i = GX_VA_PNMTXIDX; i <= GX_VA_POS; ++i) {
    const auto attr = static_cast<GXAttr>(i);
    const auto& attrFmt = vtxFmt.attrs[i];
    const auto type = g_gxState.vtxDesc[i];
    if (type == GX_NONE) {
      continue;
    }
    if (i == GX_VA_PNMTXIDX && type == GX_DIRECT) {
      mtxIdx = vtx[offset] / 3;
    }
    if (i == GX_VA_POS) {
      const u8* src = nullptr;
      bool le = false;
      if (type == GX_DIRECT) {
        src = vtx + offset;
      } else {
        const u32 idx = type == GX_INDEX16 ? (u32(vtx[offset]) << 8 | vtx[offset + 1]) : vtx[offset];
        const auto& array = g_gxState.arrays[GX_VA_POS];
        if (array.data != nullptr && (array.size == 0 || idx * array.stride + 12 <= array.size)) {
          src = static_cast<const u8*>(array.data) + idx * array.stride;
          le = array.le;
        }
      }
      if (src == nullptr) {
        return false;
      }
      const u32 cnt = comp_cnt_count(attr, attrFmt.cnt);
      const u32 csize = comp_type_size(attr, attrFmt.type);
      const float scale = attrFmt.type == GX_F32 ? 1.0f : 1.0f / float(1u << attrFmt.frac);
      pos[0] = pos[1] = pos[2] = 0.0f;
      for (u32 c = 0; c < cnt && c < 3; ++c) {
        const u8* p = src + c * csize;
        u32 raw = 0;
        for (u32 b = 0; b < csize; ++b) {
          raw = le ? raw | (u32(p[b]) << (8 * b)) : (raw << 8) | p[b];
        }
        float v = 0.0f;
        switch (attrFmt.type) {
        case GX_F32: v = std::bit_cast<float>(raw); break;
        case GX_S16: v = float(static_cast<s16>(raw)) * scale; break;
        case GX_U16: v = float(static_cast<u16>(raw)) * scale; break;
        case GX_S8: v = float(static_cast<s8>(raw)) * scale; break;
        default: v = float(static_cast<u8>(raw)) * scale; break;
        }
        pos[c] = v;
      }
      return true;
    }
    if (type == GX_DIRECT) {
      offset += comp_type_size(attr, attrFmt.type) * comp_cnt_count(attr, attrFmt.cnt);
    } else {
      offset += type == GX_INDEX16 ? 2 : 1;
    }
  }
  return false;
}

static void probe_draw(GXPrimitive prim, GXVtxFmt fmt, u16 vtxCount, const u8* vtx, u32 vtxSize) noexcept {
  const AuroraDrawProbeFn fn = sDrawProbe.load(std::memory_order_relaxed);
  if (fn == nullptr || g_gxState.replayMask || vtxCount == 0) {
    return;
  }
  AuroraDrawProbe pr{};
  pr.tag = sProbeTag;
  if ((pr.tag & 0xFFFFF) == 0xFFFFF) {
    return;  // not judged: nothing to measure
  }
  pr.prim = static_cast<u8>(prim);
  pr.fmt = static_cast<u8>(fmt);
  pr.vtxCount = vtxCount;
  pr.fadeQ = g_gxState.fadeQ;
  u32 mtxIdx = 0;
  const bool posOk = probe_vertex(fmt, vtx, pr.pos, mtxIdx);
  pr.hasPos = posOk ? 1 : 0;
  pr.mtxIdx = static_cast<u8>(mtxIdx < MaxPnMtx ? mtxIdx : 0);
  if (mtxIdx >= MaxPnMtx) {
    mtxIdx = 0;
  }
  const auto& m = g_gxState.pnMtx[mtxIdx].pos;
  const Vec4<float>* rows[3] = {&m.m0, &m.m1, &m.m2};
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 4; ++c) {
      pr.mtx[4 * r + c] = (*rows[r])[c];
    }
  }
  const auto& P = g_gxState.proj;
  const Vec4<float>* prow[4] = {&P.m0, &P.m1, &P.m2, &P.m3};
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      pr.proj[4 * r + c] = (*prow[r])[c];
    }
  }
  // Screen extent: every vertex (up to 4096) through its own position matrix and the projection,
  // in normalized device coordinates (vertices behind the camera are left out).
  pr.ndcMin[0] = pr.ndcMin[1] = 1e30f;
  pr.ndcMax[0] = pr.ndcMax[1] = -1e30f;
  const u32 n = std::min<u32>(vtxCount, 4096u);
  for (u32 v = 0; v < n && vtxSize != 0; ++v) {
    float p[3];
    u32 mi = 0;
    if (!probe_vertex(fmt, vtx + v * vtxSize, p, mi)) {
      continue;
    }
    if (mi >= MaxPnMtx) {
      mi = 0;
    }
    const auto& M = g_gxState.pnMtx[mi].pos;
    const float e[3] = {M.m0[0] * p[0] + M.m0[1] * p[1] + M.m0[2] * p[2] + M.m0[3],
                        M.m1[0] * p[0] + M.m1[1] * p[1] + M.m1[2] * p[2] + M.m1[3],
                        M.m2[0] * p[0] + M.m2[1] * p[1] + M.m2[2] * p[2] + M.m2[3]};
    const float x = pr.proj[0] * e[0] + pr.proj[1] * e[1] + pr.proj[2] * e[2] + pr.proj[3];
    const float y = pr.proj[4] * e[0] + pr.proj[5] * e[1] + pr.proj[6] * e[2] + pr.proj[7];
    const float w = pr.proj[12] * e[0] + pr.proj[13] * e[1] + pr.proj[14] * e[2] + pr.proj[15];
    if (!(w > 1e-4f) || !std::isfinite(x) || !std::isfinite(y)) {
      continue;
    }
    pr.ndcMin[0] = std::min(pr.ndcMin[0], x / w), pr.ndcMax[0] = std::max(pr.ndcMax[0], x / w);
    pr.ndcMin[1] = std::min(pr.ndcMin[1], y / w), pr.ndcMax[1] = std::max(pr.ndcMax[1], y / w);
  }
  const auto& vp = g_gxState.logicalViewport;
  pr.viewport[0] = vp.left, pr.viewport[1] = vp.top, pr.viewport[2] = vp.width, pr.viewport[3] = vp.height;
  fn(&pr);
}

static void draw_prim(GXPrimitive prim, GXVtxFmt fmt, u16 vtxCount, ByteReader& reader) noexcept {
  ZoneScoped;
  u32 vtxSize;
  if (g_gxState.lastVtxFmt == fmt)
    LIKELY { vtxSize = g_gxState.lastVtxSize; }
  else
    UNLIKELY { vtxSize = calc_vtx_size(fmt); }

  u32 totalVtxBytes = vtxCount * vtxSize;
  if (totalVtxBytes > reader.remaining())
    UNLIKELY { handle_draw_overrun(totalVtxBytes, reader); }
  if (sDrawProbe.load(std::memory_order_relaxed) != nullptr)
    UNLIKELY { probe_draw(prim, fmt, vtxCount, reader.data() + reader.offset(), vtxSize); }

  size_dynamic_arrays(fmt, reader.data() + reader.offset(), vtxCount, vtxSize);

  const bool cleanState = g_gxState.dirty == 0 && !g_gxState.wireframe && fmt == sDrawCache.lastDrawFmt &&
                          sDrawCache.lineMode == 0 &&
                          prim != GX_LINES && prim != GX_LINESTRIP && prim != GX_POINTS;
  auto* lastDraw = cleanState ? gfx::get_last_draw_command<DrawData>() : nullptr;
  const bool canMerge = lastDraw != nullptr && lastDraw->instanceCount == 1;

  // Push raw vertex data to buffer. Merged draws must remain contiguous with the previous range.
  const auto vertexData = reader.take(totalVtxBytes);
  gfx::Range vertRange = gfx::push_verts(vertexData.data(), vertexData.size(), canMerge ? 0 : 4);

  // Try to merge with previous draw call
  if (canMerge) {
    u32 numIndices = 0;
    gfx::Range idxRange;
    static ByteBuffer idxBuf;
    const bool hadIndexRange = lastDraw->idxRange.size != 0;
    if (lastDraw->indexCount == 0 && prim != GX_TRIANGLES) {
      // Generate triangle index buffer for previous draw
      lastDraw->indexCount = prepare_idx_buffer(idxBuf, GX_TRIANGLES, 0, lastDraw->vtxCount);
    }
    if (lastDraw->indexCount != 0) {
      numIndices += prepare_idx_buffer(idxBuf, prim, lastDraw->vtxCount, vtxCount);
      idxRange = gfx::push_indices(idxBuf.data(), idxBuf.size(), hadIndexRange ? 0 : 4);
      idxBuf.clear();
    }
    CHECK(lastDraw->vertRange.offset + lastDraw->vertRange.size == vertRange.offset,
          "Non-consecutive vertex ranges ({} < {})", lastDraw->vertRange.offset + lastDraw->vertRange.size,
          vertRange.offset);
    if (hadIndexRange) {
      CHECK(lastDraw->idxRange.offset + lastDraw->idxRange.size == idxRange.offset,
            "Non-consecutive index ranges ({} < {})", lastDraw->idxRange.offset + lastDraw->idxRange.size,
            idxRange.offset);
    }
    lastDraw->vertRange.size += vertRange.size;
    if (lastDraw->idxRange.size == 0) {
      lastDraw->idxRange = idxRange;
    } else {
      lastDraw->idxRange.size += idxRange.size;
    }
    lastDraw->vtxCount += vtxCount;
    lastDraw->indexCount += numIndices;
    // 0182: the last command is the colour half of an early-Z split pair: its depth half (the
    // command before it, same geometry) is extended alike.
    if (sDrawCache.split && sDrawCache.splitValid && lastDraw->pipeline == sDrawCache.splitColor) {
      if (auto* depthHalf = gfx::get_draw_command_before_last();
          depthHalf != nullptr && depthHalf->pipeline == sDrawCache.splitDepth) {
        depthHalf->vertRange = lastDraw->vertRange;
        depthHalf->idxRange = lastDraw->idxRange;
        depthHalf->vtxCount = lastDraw->vtxCount;
        depthHalf->indexCount = lastDraw->indexCount;
      }
    }
    gfx::detail::increment_merged_draw_count();
    if (mirror::recording() && !g_gxState.replayMask) {
      mirror::record(prim, sDrawCache.config.shaderConfig, vtxCount, vertexData.data(), nullptr, 0, true);
    }
    return;
  }

  handle_draw_unmerged(prim, fmt, vtxCount, vertRange);
  if (mirror::recording() && !g_gxState.replayMask) {
    mirror::record(prim, sDrawCache.config.shaderConfig, vtxCount, vertexData.data(), nullptr, 0);
  }
}

static void handle_draw(u8 cmd, ByteReader& reader) noexcept {
  const auto fmt = static_cast<GXVtxFmt>(cmd & CP_VAT_MASK);
  const auto prim = static_cast<GXPrimitive>(cmd & CP_OPCODE_MASK);
  draw_prim(prim, fmt, reader.read<u16>(), reader);
}

void handle_aurora(ByteReader& reader) noexcept {
  ZoneScoped;
  const u16 subCmd = reader.read<u16>();

  if (subCmd == GX_AURORA_LOAD_VIEWPORT_RENDER) {
    const f32 left = reader.read<f32>();
    const f32 top = reader.read<f32>();
    const f32 width = reader.read<f32>();
    const f32 height = reader.read<f32>();
    const f32 nearZ = reader.read<f32>();
    const f32 farZ = reader.read<f32>();
    set_render_viewport({
        .left = left,
        .top = top,
        .width = width,
        .height = height,
        .znear = nearZ,
        .zfar = farZ,
    });
  } else if (subCmd == GX_AURORA_LOAD_SCISSOR_RENDER) {
    const s32 left = reader.read<s32>();
    const s32 top = reader.read<s32>();
    const s32 width = reader.read<s32>();
    const s32 height = reader.read<s32>();
    set_render_scissor({left, top, width, height});
  } else if (subCmd == GX_AURORA_LOAD_PROJECTION_FULL) {
    auto& proj = g_gxState.proj;
    for (int r = 0; r < 4; ++r) {
      for (int c = 0; c < 4; ++c) {
        proj[r][c] = reader.read<f32>();
      }
    }
    // Invalidate projection XF regs
    for (u32 reg = 0x20; reg <= 0x26; ++reg) {
      g_gxState.xfRegValid.reset(reg);
    }
    g_gxState.dirty |= DirtyUniform;
  } else if (subCmd >= GX_AURORA_LOAD_ARRAYBASE && subCmd <= (GX_AURORA_LOAD_ARRAYBASE | 0x0f)) {
    const u32 attrIdx = subCmd - GX_AURORA_LOAD_ARRAYBASE + GX_VA_POS;
    const u64 arrayAddr = reader.read<u64>();
    const u32 arraySize = reader.read<u32>();
    const bool le = reader.read<u8>() == 1;

    auto& array = g_gxState.arrays[attrIdx];
    const auto newData = reinterpret_cast<void*>(arrayAddr);
    if (array.data != newData || array.size != arraySize || array.le != le) {
      if (array.le != le) {
        // Endianness is baked into the shader
        g_gxState.dirty |= DirtyPipeline;
      }
      array.data = newData;
      array.size = arraySize;
      array.le = le;
      array.cachedRange = {};
      g_gxState.dirty |= DirtyImmediates;
    }
  } else if (subCmd == GX_AURORA_LOAD_TEXOBJ) {
    const auto texMapId = reader.read<u8>();
    CHECK(texMapId < MaxTextures, "invalid texture map id {}", texMapId);
    auto& slot = g_gxState.loadedTextures[texMapId];
    const auto newData = reinterpret_cast<const void*>(reader.read<u64>());
    const u32 newWidth = reader.read<u32>();
    const u32 newHeight = reader.read<u32>();
    auto newFormat = static_cast<GXTexFmt>(reader.read<u32>());
    // Z texture formats only exist on the CPU side: the texture unit reads Z8/Z16/Z24X8 data as
    // I8/IA8/RGBA8 (the hardware format field is the low 4 bits); GXSetZTexture interprets it.
    switch (newFormat) {
    case GX_TF_Z8:
      newFormat = GX_TF_I8;
      break;
    case GX_TF_Z16:
      newFormat = GX_TF_IA8;
      break;
    case GX_TF_Z24X8:
      newFormat = GX_TF_RGBA8;
      break;
    default:
      break;
    }
    const auto newTlut = static_cast<GXTlut>(reader.read<u32>());
    u8 newFlags = slot.flags & ~0x80u; // Reset no-cache flag
    if (reader.read<u8>() != 0) {
      newFlags |= 1u;
    } else {
      newFlags &= ~1u;
    }
    const u32 newTexObjId = reader.read<u32>();
    const u32 newTexDataVersion = reader.read<u32>();
    const auto newContent = reinterpret_cast<const void*>(reader.read<u64>());
    if (slot.data != newData || slot.mWidth != newWidth || slot.mHeight != newHeight ||
        slot.mFormat != static_cast<u32>(newFormat) || slot.tlut != newTlut || slot.flags != newFlags ||
        slot.texObjId != newTexObjId || slot.texDataVersion != newTexDataVersion ||
        g_gxState.loadedTextureContent[texMapId] != newContent) {
      g_gxState.loadedTextureContent[texMapId] = newContent;
      slot.data = newData;
      slot.mWidth = newWidth;
      slot.mHeight = newHeight;
      slot.mFormat = newFormat;
      slot.tlut = newTlut;
      slot.flags = newFlags;
      slot.texObjId = newTexObjId;
      slot.texDataVersion = newTexDataVersion;
      g_gxState.dirty |= DirtyTextures;
    }
  } else if (subCmd == GX_AURORA_LOAD_TLUT) {
    const auto idx = reader.read<u8>();
    CHECK(idx < MaxTluts, "invalid tlut slot {}", idx);
    auto& slot = g_gxState.loadedTluts[idx];
    const auto newData = reinterpret_cast<const void*>(reader.read<u64>());
    const auto newFormat = static_cast<GXTlutFmt>(reader.read<u32>());
    const u16 newNumEntries = reader.read<u16>();
    const u32 newTlutObjId = reader.read<u32>();
    const u32 newTlutDataVersion = reader.read<u32>();
    const u8 newFlags = slot.flags & ~0x80u; // Reset no-cache flag
    if (slot.data != newData || slot.format != newFormat || slot.numEntries != newNumEntries ||
        slot.tlutObjId != newTlutObjId || slot.tlutDataVersion != newTlutDataVersion || slot.flags != newFlags) {
      if (slot.tlutObjId != newTlutObjId || slot.tlutDataVersion != newTlutDataVersion) {
        texture::invalidate_bindings();
      }
      slot.data = newData;
      slot.format = newFormat;
      slot.numEntries = newNumEntries;
      slot.tlutObjId = newTlutObjId;
      slot.tlutDataVersion = newTlutDataVersion;
      slot.flags = newFlags;
      g_gxState.dirty |= DirtyTextures;
    }
  } else if (subCmd == GX_AURORA_SET_WIREFRAME) {
    const bool enable = reader.read<u8>() != 0;
    const u32 color = reader.read<u32>();
    if (g_gxState.wireframe != enable) {
      g_gxState.wireframe = enable;
      g_gxState.dirty |= DirtyPipeline;
    }
    if (g_gxState.wireColor != color) {
      g_gxState.wireColor = color;
      g_gxState.dirty |= DirtyUniform;
    }
  } else if (subCmd == GX_AURORA_SET_CULL_OVERRIDE) {
    const bool twoSided = reader.read<u8>() != 0;
    if (g_gxState.twoSided != twoSided) {
      g_gxState.twoSided = twoSided;
      g_gxState.dirty |= DirtyPipeline;
    }
  } else if (subCmd == GX_AURORA_SET_DRAW_CLASS) {
    const u8 drawClass = reader.read<u8>();
    if (g_gxState.drawClass != drawClass) {
      g_gxState.drawClass = drawClass;
      static const bool sNoDirty = std::getenv("AURORA_ENH_NODIRTY") != nullptr; // debug aid
      if (gfx::enhanced::config().enabled && !sNoDirty) {
        g_gxState.dirty |= DirtyPipeline;
      }
    }
  } else if (subCmd == GX_AURORA_SET_MATERIAL_HINT) {
    const u8 hint = reader.read<u8>();
    if (g_gxState.materialHint != hint) {
      g_gxState.materialHint = hint;
      g_gxState.dirty |= DirtyUniform;
    }
  } else if (subCmd == GX_AURORA_SET_EXTRA_LIGHTS) {
    const u32 count = std::min<u32>(reader.read<u8>(), gfx::MaxExtraLights);
    std::array<gfx::ExtraLight, gfx::MaxExtraLights> lights{};
    for (u32 i = 0; i < count; ++i) {
      auto& l = lights[i];
      for (float& v : l.pos) v = reader.read<f32>();
      for (float& v : l.color) v = reader.read<f32>();
      l.radius = reader.read<f32>();
      l.intensity = reader.read<f32>();
      l.flags = reader.read<u32>();
      l.id = reader.read<u32>();
      for (float& v : l.box) v = reader.read<f32>();
    }
    if (!g_gxState.replayMask) {
      gfx::enhanced::set_lights(lights.data(), count);
    }
  } else if (subCmd == GX_AURORA_SCENE_END) {
    if (!g_gxState.replayMask) {
      gfx::enhanced::scene_end();
    }
  } else if (subCmd == GX2_SET_POLYGON_OFFSET) {
    const f32 frontOffset = reader.read<f32>();
    const f32 frontScale = reader.read<f32>();
    const f32 backOffset = reader.read<f32>();
    const f32 backScale = reader.read<f32>();
    const f32 clamp = reader.read<f32>();
    if (g_gxState.frontOffset != frontOffset || g_gxState.frontScale != frontScale ||
        g_gxState.backOffset != backOffset || g_gxState.backScale != backScale || g_gxState.clamp != clamp) {
      g_gxState.frontOffset = frontOffset;
      g_gxState.frontScale = frontScale;
      g_gxState.backOffset = backOffset;
      g_gxState.backScale = backScale;
      g_gxState.clamp = clamp;
      g_gxState.dirty |= DirtyPipeline;
    }
  } else if (subCmd == GX_AURORA_LOAD_COPY_SRC) {
    const s32 left = reader.read<s32>();
    const s32 top = reader.read<s32>();
    const s32 width = reader.read<s32>();
    const s32 height = reader.read<s32>();
    g_gxState.texCopySrc = {left, top, width, height};
  } else if (subCmd == GX_AURORA_LOAD_COPY_DST) {
    g_gxState.texCopyDstWidth = reader.read<u32>();
    g_gxState.texCopyDstHeight = reader.read<u32>();
    g_gxState.texCopyFmt = static_cast<GXTexFmt>(reader.read<u32>());
    reader.skip(1); // mipmap is not implemented, but remains part of the command payload
    g_gxState.texCopyDstWide = true;
  } else if (subCmd == GX_AURORA_LOAD_COPY_DEST) {
    g_gxState.texCopyDest = reinterpret_cast<const void*>(reader.read<u64>());
  } else if (subCmd == GX_AURORA_REQUEST_DEPTH_SNAPSHOT) {
    if (!g_gxState.replayMask) {
      gfx::depth_peek::request_snapshot();
    }
  } else if (subCmd == GX_AURORA_BEGIN_OFFSCREEN) {
    const u32 width = reader.read<u32>();
    const u32 height = reader.read<u32>();
    if (!g_gxState.replayMask) {
      gfx::begin_offscreen(width, height);
      mirror::offscreen(true);
    }
  } else if (subCmd == GX_AURORA_END_OFFSCREEN) {
    if (!g_gxState.replayMask) {
      gfx::end_offscreen();
      mirror::offscreen(false);
    }
  } else if (subCmd == GX_AURORA_DESTROY_TEXOBJ) {
    const u32 id = reader.read<u32>();
    if (!g_gxState.replayMask) {
      evict_texture_object(id);
    }
  } else if (subCmd == GX_AURORA_DESTROY_TLUT) {
    const u32 id = reader.read<u32>();
    if (!g_gxState.replayMask) {
      evict_tlut_object(id);
    }
  } else if (subCmd == GX_AURORA_DESTROY_COPY_TEX) {
    const auto* ptr = reinterpret_cast<const void*>(reader.read<u64>());
    if (!g_gxState.replayMask) {
      evict_copy_texture(ptr);
    }
  } else if (subCmd == GX_AURORA_SET_FADE) {
    const u8 q = reader.read<u8>();
    if (g_gxState.fadeQ != q) {
      g_gxState.fadeQ = q;
      g_gxState.dirty |= DirtyImmediates;  // no merging across a fade change
    }
  } else if (subCmd == GX_AURORA_SET_REPLAY_MASK) {
    g_gxState.replayMask = reader.read<u8>() != 0;
  } else if (subCmd == GX_AURORA_PROBE_TAG) {
    sProbeTag = reader.read<u32>();
  } else if (subCmd == GX_AURORA_DRAW_SIZED) {
    const u8 cmd = reader.read<u8>();
    const u32 byteLen = reader.read<u32>();
    const GXVtxFmt fmt = static_cast<GXVtxFmt>(cmd & CP_VAT_MASK);
    const GXPrimitive prim = static_cast<GXPrimitive>(cmd & CP_OPCODE_MASK);
    if (byteLen != 0) {
      u32 vtxSize;
      if (g_gxState.lastVtxFmt == fmt) {
        vtxSize = g_gxState.lastVtxSize;
      } else {
        vtxSize = calc_vtx_size(fmt);
      }
      AURORA_ASSERT(vtxSize != 0 && byteLen % vtxSize == 0,
                    "GX_AURORA_DRAW_SIZED: {} bytes is not a whole number of size-{} vertices", byteLen, vtxSize);
      u32 vtxCount = byteLen / vtxSize;
      AURORA_ASSERT(vtxCount <= 0xFFFF, "GX_AURORA_DRAW_SIZED: too many vertices ({})", vtxCount);
      draw_prim(prim, fmt, static_cast<u16>(vtxCount), reader);
    }
  } else if (subCmd == GX_AURORA_DRAW_INDEXED) {
    ZoneScopedN("DRAW_INDEXED");
    const u8 cmd = reader.read<u8>();
    const u16 vtxCount = reader.read<u16>();
    const u32 indexCount = reader.read<u32>();
    const GXVtxFmt fmt = static_cast<GXVtxFmt>(cmd & CP_VAT_MASK);
    const GXPrimitive prim = static_cast<GXPrimitive>(cmd & CP_OPCODE_MASK);
    AURORA_ASSERT(prim == GX_TRIANGLES, "GX_AURORA_DRAW_INDEXED: primitive must be GX_TRIANGLES, got {}",
                  static_cast<u32>(prim));
    const size_t idxBytes = static_cast<size_t>(indexCount) * sizeof(u16);
    // Index data is always host-endian; push it to the GPU buffer as-is
    const auto indexData = reader.take(idxBytes);
    gfx::Range idxRange;
    u32 drawIndexCount = indexCount;
    if (wire_prim(prim)) {
      static ByteBuffer lineBuf;
      drawIndexCount = wire_edges(lineBuf, reinterpret_cast<const u16*>(indexData.data()), indexCount);
      idxRange = gfx::push_indices(lineBuf.data(), lineBuf.size(), 4);
      lineBuf.clear();
    } else {
      idxRange = gfx::push_indices(indexData.data(), indexData.size(), 4);
    }
    u32 vtxSize;
    if (g_gxState.lastVtxFmt == fmt) {
      vtxSize = g_gxState.lastVtxSize;
    } else {
      vtxSize = calc_vtx_size(fmt);
    }
    const u32 totalVtxBytes = vtxCount * vtxSize;
    const auto vertexData = reader.take(totalVtxBytes);
    if (sDrawProbe.load(std::memory_order_relaxed) != nullptr)
      UNLIKELY { probe_draw(prim, fmt, vtxCount, vertexData.data(), vtxSize); }
    const gfx::Range vertRange = gfx::push_verts(vertexData.data(), vertexData.size(), 4);
    if (drawIndexCount != 0) {
      push_gx_draw(prim, fmt, vtxCount, vertRange, idxRange, drawIndexCount);
      if (mirror::recording() && !g_gxState.replayMask && !wire_prim(prim)) {
        mirror::record(prim, sDrawCache.config.shaderConfig, vtxCount, vertexData.data(),
                       reinterpret_cast<const u16*>(indexData.data()), indexCount);
      }
    }
  } else if (subCmd == GX_AURORA_MIRROR_MARK) {
    const u32 mark = reader.read<u32>();
    const f32 value = reader.read<f32>();
    mirror::mark(mark, value);
  } else if (subCmd == GX_AURORA_STATE_SAVE || subCmd == GX_AURORA_STATE_RESTORE) {
    // Replay state slots: everything but the copy-texture caches (copies made since stay valid)
    // and the per-frame cache state (array upload ranges, texture binds), which is rebuilt.
    // Slots 4..7 (GX_AURORA_SET_REPLAY_MASK passes) hold the GX state only.
    static std::array<std::unique_ptr<GXState>, 16> sSlots;
    // The copy textures each slot's frame ended with (by generation, so they stay as they were).
    static std::array<std::optional<decltype(GXState::copyTextures)>, 8> sCopySlots;
    const u8 slot = reader.read<u8>() & 15;
    // 0..3: left/flat history; 4..7: temporary mask passes; 8..11: right history.
    const bool stateOnly = (slot >= 4 && slot < 8) || slot >= 12;
    const u8 copySlot = slot >= 8 && slot < 12 ? slot - 4 : slot;
    auto& state = g_gxState;
    const u8 generation = state.copyGeneration;
    if (stateOnly) {
      // (state only: copies, copy-to-RAM entries and generation flags stay as they are)
    } else if (subCmd == GX_AURORA_STATE_SAVE) {
      sCopySlots[copySlot] = state.copyTextures;
      gfx::efb_ram::save_slot(copySlot);
      gfx::efb_ram::set_generation(generation, false, false);
    } else if (sCopySlots[copySlot]) {
      state.copyTextures = *sCopySlots[copySlot];
      gfx::efb_ram::restore_slot(copySlot);
      texture::invalidate_bindings();
    }
    auto copies = std::move(state.copyTextures);
    auto copyCache = std::move(state.copyTextureCache);
    auto binds = std::move(state.textures);
    if (subCmd == GX_AURORA_STATE_SAVE) {
      if (!sSlots[slot]) sSlots[slot] = std::make_unique<GXState>();
      *sSlots[slot] = state;
    } else if (sSlots[slot]) {
      state = *sSlots[slot];
      for (auto& array : state.arrays) {
        array.cachedRange = {};
      }
      state.clearVtxSizeCache();
      state.dirty = DirtyAll;
      clear_draw_cache();
    }
    state.copyTextures = std::move(copies);
    state.copyTextureCache = std::move(copyCache);
    state.textures = std::move(binds);
    state.copyGeneration = generation;
  } else if (subCmd == GX_AURORA_COPY_GENERATION) {
    const u8 v = reader.read<u8>();
    g_gxState.copyGeneration = v & 7;
    gfx::efb_ram::set_generation(v & 7, (v & 0x80) != 0, (v & 0x40) != 0);
  } else if (subCmd == GX_AURORA_DEBUG_GROUP_PUSH) {
    auto label = reader.read_string();
    mirror::debug_group(&label);
    gfx::push_debug_group(std::move(label));
  } else if (subCmd == GX_AURORA_DEBUG_GROUP_POP) {
    mirror::debug_group(nullptr);
    pop_debug_group();
  } else if (subCmd == GX_AURORA_DEBUG_MARKER_INSERT) {
    auto label = reader.read_string();
    gfx::insert_debug_marker(std::move(label));
  }

  else {
    Log.error("Unknown Aurora subcommand: {:04X}", subCmd);
  }
}

void clear_draw_cache() noexcept {
  sDrawCache.bindGeneration = 0;
  sDrawCache.uniformRange = {};
  sDrawCache.fogRange = {};
  sDrawCache.hasFogRange = false;
}

} // namespace aurora::gx::fifo

// 0217: the split pair for the pipeline precache (lib/gfx/pipeline_cache.cpp).
bool aurora::gx::early_z_split_variants(const PipelineConfig& pc, PipelineConfig& depthOnly,
                                        PipelineConfig& color) noexcept {
  return fifo::split_variants_impl(pc, depthOnly, color);
}

void AuroraSetDrawProbe(AuroraDrawProbeFn fn) { aurora::gx::fifo::sDrawProbe.store(fn, std::memory_order_relaxed); }
