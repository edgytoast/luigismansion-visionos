#include "../gfx/beam_profile.hpp"
#include "../gfx/hash.hpp"
#include "../gfx/pipeline_cache.hpp"
#include "../gfx/types.hpp"

#include "../internal.hpp"
#include "../webgpu/gpu.hpp"
#include "gx.hpp"
#include "gx_fmt.hpp"
#include "shader_info.hpp"

#include <dolphin/gx/GXEnum.h>

#include <absl/container/flat_hash_set.h>
#include <cstdlib>
#include <mutex>
#include <string_view>
#include <utility>

#include "tracy/Tracy.hpp"

namespace aurora::gx {
using namespace fmt::literals;
using namespace std::string_literals;
using namespace std::string_view_literals;

namespace {
constexpr Module Log{"aurora::gfx::gx"};

std::string_view chan_comp(GXTevColorChan chan) noexcept {
  switch (chan) {
  case GX_CH_RED:
    return "r";
  case GX_CH_GREEN:
    return "g";
  case GX_CH_BLUE:
    return "b";
  case GX_CH_ALPHA:
    return "a";
  default:
    return "?";
  }
}

bool is_alpha_bump_channel(GXChannelID id) noexcept { return id == GX_ALPHA_BUMP || id == GX_ALPHA_BUMPN; }

std::string tev_mask_expr(const std::string& value, u32 mask) {
  // t_IndTexCoord is already expanded into the 0..255 indirect sample domain.
  return fmt::format("(f32(u32({}) & 0x{:X}u) / 255.0)", value, mask);
}

std::string alpha_bump_sel(size_t stageIdx, const ShaderConfig& config, const TevStage& stage) {
  if (stage.indTexStage >= config.numIndStages || stage.indTexAlphaSel == GX_ITBA_OFF) {
    return "0.0";
  }

  std::string baseCoord;
  switch (stage.indTexAlphaSel) {
    DEFAULT_FATAL("invalid indTexAlphaSel {} for stage {}", underlying(stage.indTexAlphaSel), stageIdx);
  case GX_ITBA_S:
    baseCoord = fmt::format("t_IndTexCoord{}.x", underlying(stage.indTexStage));
    break;
  case GX_ITBA_T:
    baseCoord = fmt::format("t_IndTexCoord{}.y", underlying(stage.indTexStage));
    break;
  case GX_ITBA_U:
    baseCoord = fmt::format("t_IndTexCoord{}.z", underlying(stage.indTexStage));
    break;
  case GX_ITBA_OFF:
    return "0.0";
  }

  switch (stage.indTexFormat) {
    DEFAULT_FATAL("invalid indirect format {} for stage {}", underlying(stage.indTexFormat), stageIdx);
  case GX_ITF_8:
    return tev_mask_expr(baseCoord, 0xF8u);
  case GX_ITF_5:
    return tev_mask_expr(baseCoord, 0xE0u);
  case GX_ITF_4:
    return tev_mask_expr(baseCoord, 0xF0u);
  case GX_ITF_3:
    return tev_mask_expr(baseCoord, 0xF8u);
  }
}

bool uses_texture_sample(const TevStage& stage) noexcept {
  if (stage.texMapId == GX_TEXMAP_NULL) {
    return false;
  }
  const auto& c = stage.colorPass;
  const auto& a = stage.alphaPass;
  return c.a == GX_CC_TEXC || c.a == GX_CC_TEXA || c.b == GX_CC_TEXC || c.b == GX_CC_TEXA || c.c == GX_CC_TEXC ||
         c.c == GX_CC_TEXA || c.d == GX_CC_TEXC || c.d == GX_CC_TEXA || a.a == GX_CA_TEXA || a.b == GX_CA_TEXA ||
         a.c == GX_CA_TEXA || a.d == GX_CA_TEXA;
}

std::string color_arg_reg(GXTevColorArg arg, size_t stageIdx, const ShaderConfig& config, const TevStage& stage) {
  switch (arg) {
    DEFAULT_FATAL("invalid color arg {}", underlying(arg));
  case GX_CC_CPREV:
    return "prev.rgb";
  case GX_CC_APREV:
    return "vec3f(prev.a)";
  case GX_CC_C0:
    return "tevreg0.rgb";
  case GX_CC_A0:
    return "vec3f(tevreg0.a)";
  case GX_CC_C1:
    return "tevreg1.rgb";
  case GX_CC_A1:
    return "vec3f(tevreg1.a)";
  case GX_CC_C2:
    return "tevreg2.rgb";
  case GX_CC_A2:
    return "vec3f(tevreg2.a)";
  case GX_CC_TEXC: {
    if (stage.texMapId == GX_TEXMAP_NULL) {
      return "vec3f(1.0)";
    }
    CHECK(stage.texMapId >= GX_TEXMAP0 && stage.texMapId <= GX_TEXMAP7, "invalid texture {} for stage {}",
          underlying(stage.texMapId), stageIdx);
    const auto& swap = config.tevSwapTable[stage.tevSwapTex];
    return fmt::format("sampled{}.{}{}{}", stageIdx, chan_comp(swap.red), chan_comp(swap.green), chan_comp(swap.blue));
  }
  case GX_CC_TEXA: {
    if (stage.texMapId == GX_TEXMAP_NULL) {
      return "vec3f(1.0)";
    }
    CHECK(stage.texMapId >= GX_TEXMAP0 && stage.texMapId <= GX_TEXMAP7, "invalid texture {} for stage {}",
          underlying(stage.texMapId), stageIdx);
    const auto& swap = config.tevSwapTable[stage.tevSwapTex];
    return fmt::format("vec3f(sampled{}.{})", stageIdx, chan_comp(swap.alpha));
  }
  case GX_CC_RASC: {
    // CHECK(stage.channelId != GX_COLOR_NULL, "unmapped color channel for stage {}", stageIdx);
    if (stage.channelId == GX_COLOR_ZERO || stage.channelId == GX_COLOR_NULL) {
      return "vec3f(0.0)";
    }
    if (is_alpha_bump_channel(stage.channelId)) {
      std::string alpha = alpha_bump_sel(stageIdx, config, stage);
      if (stage.channelId == GX_ALPHA_BUMPN) {
        alpha = fmt::format("({} * (255.0 / 248.0))", alpha);
      }
      return fmt::format("vec3f({})", alpha);
    }
    u32 idx = color_channel(stage.channelId);
    const auto& swap = config.tevSwapTable[stage.tevSwapRas];
    return fmt::format("rast{}.{}{}{}", idx, chan_comp(swap.red), chan_comp(swap.green), chan_comp(swap.blue));
  }
  case GX_CC_RASA: {
    // CHECK(stage.channelId != GX_COLOR_NULL, "unmapped color channel for stage {}", stageIdx);
    if (stage.channelId == GX_COLOR_ZERO || stage.channelId == GX_COLOR_NULL) {
      return "vec3f(0.0)";
    }
    if (is_alpha_bump_channel(stage.channelId)) {
      std::string alpha = alpha_bump_sel(stageIdx, config, stage);
      if (stage.channelId == GX_ALPHA_BUMPN) {
        alpha = fmt::format("({} * (255.0 / 248.0))", alpha);
      }
      return fmt::format("vec3f({})", alpha);
    }
    u32 idx = color_channel(stage.channelId);
    const auto& swap = config.tevSwapTable[stage.tevSwapRas];
    return fmt::format("vec3f(rast{}.{})", idx, chan_comp(swap.alpha));
  }
  case GX_CC_ONE:
    return "vec3f(1.0)";
  case GX_CC_HALF:
    return "vec3f(0.5)";
  case GX_CC_KONST: {
    switch (stage.kcSel) {
      DEFAULT_FATAL("invalid kcSel {}", underlying(stage.kcSel));
    case GX_TEV_KCSEL_8_8:
      return "vec3f(1.0)";
    case GX_TEV_KCSEL_7_8:
      return "vec3f(7.0/8.0)";
    case GX_TEV_KCSEL_6_8:
      return "vec3f(6.0/8.0)";
    case GX_TEV_KCSEL_5_8:
      return "vec3f(5.0/8.0)";
    case GX_TEV_KCSEL_4_8:
      return "vec3f(4.0/8.0)";
    case GX_TEV_KCSEL_3_8:
      return "vec3f(3.0/8.0)";
    case GX_TEV_KCSEL_2_8:
      return "vec3f(2.0/8.0)";
    case GX_TEV_KCSEL_1_8:
      return "vec3f(1.0/8.0)";
    case GX_TEV_KCSEL_K0:
      return "ubuf.kcolor0.rgb";
    case GX_TEV_KCSEL_K1:
      return "ubuf.kcolor1.rgb";
    case GX_TEV_KCSEL_K2:
      return "ubuf.kcolor2.rgb";
    case GX_TEV_KCSEL_K3:
      return "ubuf.kcolor3.rgb";
    case GX_TEV_KCSEL_K0_R:
      return "vec3f(ubuf.kcolor0.r)";
    case GX_TEV_KCSEL_K1_R:
      return "vec3f(ubuf.kcolor1.r)";
    case GX_TEV_KCSEL_K2_R:
      return "vec3f(ubuf.kcolor2.r)";
    case GX_TEV_KCSEL_K3_R:
      return "vec3f(ubuf.kcolor3.r)";
    case GX_TEV_KCSEL_K0_G:
      return "vec3f(ubuf.kcolor0.g)";
    case GX_TEV_KCSEL_K1_G:
      return "vec3f(ubuf.kcolor1.g)";
    case GX_TEV_KCSEL_K2_G:
      return "vec3f(ubuf.kcolor2.g)";
    case GX_TEV_KCSEL_K3_G:
      return "vec3f(ubuf.kcolor3.g)";
    case GX_TEV_KCSEL_K0_B:
      return "vec3f(ubuf.kcolor0.b)";
    case GX_TEV_KCSEL_K1_B:
      return "vec3f(ubuf.kcolor1.b)";
    case GX_TEV_KCSEL_K2_B:
      return "vec3f(ubuf.kcolor2.b)";
    case GX_TEV_KCSEL_K3_B:
      return "vec3f(ubuf.kcolor3.b)";
    case GX_TEV_KCSEL_K0_A:
      return "vec3f(ubuf.kcolor0.a)";
    case GX_TEV_KCSEL_K1_A:
      return "vec3f(ubuf.kcolor1.a)";
    case GX_TEV_KCSEL_K2_A:
      return "vec3f(ubuf.kcolor2.a)";
    case GX_TEV_KCSEL_K3_A:
      return "vec3f(ubuf.kcolor3.a)";
    }
  }
  case GX_CC_ZERO:
    return "vec3f(0.0)";
  }
}

std::string alpha_arg_reg(GXTevAlphaArg arg, size_t stageIdx, const ShaderConfig& config, const TevStage& stage) {
  switch (arg) {
    DEFAULT_FATAL("invalid alpha arg {}", underlying(arg));
  case GX_CA_APREV:
    return "prev.a";
  case GX_CA_A0:
    return "tevreg0.a";
  case GX_CA_A1:
    return "tevreg1.a";
  case GX_CA_A2:
    return "tevreg2.a";
  case GX_CA_TEXA: {
    if (stage.texMapId == GX_TEXMAP_NULL) {
      return "1.0";
    }
    CHECK(stage.texMapId >= GX_TEXMAP0 && stage.texMapId <= GX_TEXMAP7, "invalid texture {} for stage {}",
          underlying(stage.texMapId), stageIdx);
    const auto& swap = config.tevSwapTable[stage.tevSwapTex];
    return fmt::format("sampled{}.{}", stageIdx, chan_comp(swap.alpha));
  }
  case GX_CA_RASA: {
    // CHECK(stage.channelId != GX_COLOR_NULL, "unmapped color channel for stage {}", stageIdx);
    if (stage.channelId == GX_COLOR_ZERO || stage.channelId == GX_COLOR_NULL) {
      return "0.0";
    }
    if (is_alpha_bump_channel(stage.channelId)) {
      std::string alpha = alpha_bump_sel(stageIdx, config, stage);
      if (stage.channelId == GX_ALPHA_BUMPN) {
        alpha = fmt::format("({} * (255.0 / 248.0))", alpha);
      }
      return alpha;
    }
    u32 idx = color_channel(stage.channelId);
    const auto& swap = config.tevSwapTable[stage.tevSwapRas];
    return fmt::format("rast{}.{}", idx, chan_comp(swap.alpha));
  }
  case GX_CA_KONST: {
    switch (stage.kaSel) {
      DEFAULT_FATAL("invalid kaSel {}", underlying(stage.kaSel));
    case GX_TEV_KASEL_8_8:
      return "1.0";
    case GX_TEV_KASEL_7_8:
      return "(7.0/8.0)";
    case GX_TEV_KASEL_6_8:
      return "(6.0/8.0)";
    case GX_TEV_KASEL_5_8:
      return "(5.0/8.0)";
    case GX_TEV_KASEL_4_8:
      return "(4.0/8.0)";
    case GX_TEV_KASEL_3_8:
      return "(3.0/8.0)";
    case GX_TEV_KASEL_2_8:
      return "(2.0/8.0)";
    case GX_TEV_KASEL_1_8:
      return "(1.0/8.0)";
    case GX_TEV_KASEL_K0_R:
      return "ubuf.kcolor0.r";
    case GX_TEV_KASEL_K1_R:
      return "ubuf.kcolor1.r";
    case GX_TEV_KASEL_K2_R:
      return "ubuf.kcolor2.r";
    case GX_TEV_KASEL_K3_R:
      return "ubuf.kcolor3.r";
    case GX_TEV_KASEL_K0_G:
      return "ubuf.kcolor0.g";
    case GX_TEV_KASEL_K1_G:
      return "ubuf.kcolor1.g";
    case GX_TEV_KASEL_K2_G:
      return "ubuf.kcolor2.g";
    case GX_TEV_KASEL_K3_G:
      return "ubuf.kcolor3.g";
    case GX_TEV_KASEL_K0_B:
      return "ubuf.kcolor0.b";
    case GX_TEV_KASEL_K1_B:
      return "ubuf.kcolor1.b";
    case GX_TEV_KASEL_K2_B:
      return "ubuf.kcolor2.b";
    case GX_TEV_KASEL_K3_B:
      return "ubuf.kcolor3.b";
    case GX_TEV_KASEL_K0_A:
      return "ubuf.kcolor0.a";
    case GX_TEV_KASEL_K1_A:
      return "ubuf.kcolor1.a";
    case GX_TEV_KASEL_K2_A:
      return "ubuf.kcolor2.a";
    case GX_TEV_KASEL_K3_A:
      return "ubuf.kcolor3.a";
    }
  }
  case GX_CA_ZERO:
    return "0.0";
  }
}

bool tev_color_arg_is_normalized(GXTevColorArg arg, const std::array<bool, MaxTevRegs>& colorNormalized,
                                 const std::array<bool, MaxTevRegs>& alphaNormalized) {
  switch (arg) {
  case GX_CC_CPREV:
    return colorNormalized[GX_TEVPREV];
  case GX_CC_APREV:
    return alphaNormalized[GX_TEVPREV];
  case GX_CC_C0:
    return colorNormalized[GX_TEVREG0];
  case GX_CC_A0:
    return alphaNormalized[GX_TEVREG0];
  case GX_CC_C1:
    return colorNormalized[GX_TEVREG1];
  case GX_CC_A1:
    return alphaNormalized[GX_TEVREG1];
  case GX_CC_C2:
    return colorNormalized[GX_TEVREG2];
  case GX_CC_A2:
    return alphaNormalized[GX_TEVREG2];
  default:
    return true;
  }
}

bool tev_alpha_arg_is_normalized(GXTevAlphaArg arg, const std::array<bool, MaxTevRegs>& alphaNormalized) {
  switch (arg) {
  case GX_CA_APREV:
    return alphaNormalized[GX_TEVPREV];
  case GX_CA_A0:
    return alphaNormalized[GX_TEVREG0];
  case GX_CA_A1:
    return alphaNormalized[GX_TEVREG1];
  case GX_CA_A2:
    return alphaNormalized[GX_TEVREG2];
  default:
    return true;
  }
}

std::string tev_op(GXTevOp op, std::string_view bias, std::string_view scale, std::string_view a, std::string_view b,
                   std::string_view c, std::string_view d, std::string_view zero) {
  switch (op) {
    DEFAULT_FATAL("unimplemented tev op {}", underlying(op));
  case GX_TEV_ADD:
  case GX_TEV_SUB: {
    std::string_view neg = op == GX_TEV_SUB ? "-"sv : ""sv;
    return fmt::format("(({0}mix({1}, {2}, {3}) + {4}){5}){6}", neg, a, b, c, d, bias, scale);
  }
  case GX_TEV_COMP_R8_GT:
    return fmt::format("select({3}, {2}, round({0}.r * 255.0) > round({1}.r * 255.0)) + {4}", a, b, c, zero, d);
  case GX_TEV_COMP_R8_EQ:
    return fmt::format("select({3}, {2}, round({0}.r * 255.0) == round({1}.r * 255.0)) + {4}", a, b, c, zero, d);
  case GX_TEV_COMP_GR16_GT:
    return fmt::format(
        "select({3}, {2}, round(dot({0}.rg * 255.0, vec2(1.0, 256.0))) > round(dot({1}.rg * 255.0, vec2(1.0, 256.0))))"
        " + {4}",
        a, b, c, zero, d);
  case GX_TEV_COMP_GR16_EQ:
    return fmt::format(
        "select({3}, {2}, round(dot({0}.rg * 255.0, vec2(1.0, 256.0))) == round(dot({1}.rg * 255.0, vec2(1.0, 256.0))))"
        " + {4}",
        a, b, c, zero, d);
  case GX_TEV_COMP_BGR24_GT:
    return fmt::format(
        "select({3}, {2}, round(dot({0}.rgb * 255.0, vec3(1.0, 256.0, 65536.0))) > round(dot({1}.rgb * 255.0, "
        "vec3(1.0, 256.0, 65536.0)))) + {4}",
        a, b, c, zero, d);
  case GX_TEV_COMP_BGR24_EQ:
    return fmt::format(
        "select({3}, {2}, round(dot({0}.rgb * 255.0, vec3(1.0, 256.0, 65536.0))) == round(dot({1}.rgb * 255.0, "
        "vec3(1.0, 256.0, 65536.0)))) + {4}",
        a, b, c, zero, d);
  case GX_TEV_COMP_RGB8_GT:
    return fmt::format("select({3}, {2}, round({0} * 255.0) > round({1} * 255.0)) + {4}", a, b, c, zero, d);
  case GX_TEV_COMP_RGB8_EQ:
    return fmt::format("select({3}, {2}, round({0} * 255.0) == round({1} * 255.0)) + {4}", a, b, c, zero, d);
  }
}

std::string tev_color_op(GXTevOp op, std::string_view bias, std::string_view scale, bool clamp, std::string_view a,
                         std::string_view b, std::string_view c, std::string_view d) {
  std::string expr = tev_op(op, bias, scale, a, b, c, d, "vec3(0)"sv);
  return clamp ? fmt::format("clamp({}, vec3f(0.0), vec3f(1.0))", expr)
               : fmt::format("clamp({}, vec3f(-4.0), vec3f(4.0))", expr);
}

std::string tev_alpha_op(GXTevOp op, std::string_view bias, std::string_view scale, bool clamp, std::string_view a,
                         std::string_view b, std::string_view c, std::string_view d) {
  std::string expr = tev_op(op, bias, scale, a, b, c, d, "0.0"sv);
  return clamp ? fmt::format("clamp({}, 0.0, 1.0)", expr) : fmt::format("clamp({}, -4.0, 4.0)", expr);
}

std::string_view tev_bias(GXTevBias bias) {
  switch (bias) {
    DEFAULT_FATAL("invalid tev bias {}", underlying(bias));
  case GX_TB_ZERO:
    return ""sv;
  case GX_TB_ADDHALF:
    return " + 0.5"sv;
  case GX_TB_SUBHALF:
    return " - 0.5"sv;
  }
}

struct AlphaCompareExpr {
  std::string expr;
  int constant = -1;
};

AlphaCompareExpr alpha_compare_const(bool value) { return {value ? "true"s : "false"s, value ? 1 : 0}; }

AlphaCompareExpr alpha_compare_not(const AlphaCompareExpr& expr) {
  if (expr.constant != -1) {
    return alpha_compare_const(expr.constant == 0);
  }
  return {fmt::format("!{}", expr.expr), -1};
}

AlphaCompareExpr alpha_compare_and(const AlphaCompareExpr& lhs, const AlphaCompareExpr& rhs) {
  if (lhs.constant == 0 || rhs.constant == 0) {
    return alpha_compare_const(false);
  }
  if (lhs.constant == 1) {
    return rhs;
  }
  if (rhs.constant == 1) {
    return lhs;
  }
  return {fmt::format("({} && {})", lhs.expr, rhs.expr), -1};
}

AlphaCompareExpr alpha_compare_or(const AlphaCompareExpr& lhs, const AlphaCompareExpr& rhs) {
  if (lhs.constant == 1 || rhs.constant == 1) {
    return alpha_compare_const(true);
  }
  if (lhs.constant == 0) {
    return rhs;
  }
  if (rhs.constant == 0) {
    return lhs;
  }
  return {fmt::format("({} || {})", lhs.expr, rhs.expr), -1};
}

AlphaCompareExpr alpha_compare_xor(const AlphaCompareExpr& lhs, const AlphaCompareExpr& rhs) {
  if (lhs.constant != -1 && rhs.constant != -1) {
    return alpha_compare_const(lhs.constant != rhs.constant);
  }
  if (lhs.constant == 0) {
    return rhs;
  }
  if (rhs.constant == 0) {
    return lhs;
  }
  if (lhs.constant == 1) {
    return alpha_compare_not(rhs);
  }
  if (rhs.constant == 1) {
    return alpha_compare_not(lhs);
  }
  return {fmt::format("({} != {})", lhs.expr, rhs.expr), -1};
}

AlphaCompareExpr alpha_compare_xnor(const AlphaCompareExpr& lhs, const AlphaCompareExpr& rhs) {
  if (lhs.constant != -1 && rhs.constant != -1) {
    return alpha_compare_const(lhs.constant == rhs.constant);
  }
  if (lhs.constant == 0) {
    return alpha_compare_not(rhs);
  }
  if (rhs.constant == 0) {
    return alpha_compare_not(lhs);
  }
  if (lhs.constant == 1) {
    return rhs;
  }
  if (rhs.constant == 1) {
    return lhs;
  }
  return {fmt::format("({} == {})", lhs.expr, rhs.expr), -1};
}

AlphaCompareExpr alpha_compare(GXCompare comp, u8 ref) {
  const auto iref = static_cast<u32>(ref);
  switch (comp) {
    DEFAULT_FATAL("invalid alpha comp {}", underlying(comp));
  case GX_NEVER:
    return alpha_compare_const(false);
  case GX_LESS:
    if (ref == 0) {
      return alpha_compare_const(false);
    }
    return {fmt::format("(alphaCompare < {}u)", iref), -1};
  case GX_LEQUAL:
    if (ref == 255) {
      return alpha_compare_const(true);
    }
    return {fmt::format("(alphaCompare <= {}u)", iref), -1};
  case GX_EQUAL:
    return {fmt::format("(alphaCompare == {}u)", iref), -1};
  case GX_NEQUAL:
    return {fmt::format("(alphaCompare != {}u)", iref), -1};
  case GX_GEQUAL:
    if (ref == 0) {
      return alpha_compare_const(true);
    }
    return {fmt::format("(alphaCompare >= {}u)", iref), -1};
  case GX_GREATER:
    if (ref == 255) {
      return alpha_compare_const(false);
    }
    return {fmt::format("(alphaCompare > {}u)", iref), -1};
  case GX_ALWAYS:
    return alpha_compare_const(true);
  }
}

std::string_view tev_scale(GXTevScale scale) {
  switch (scale) {
    DEFAULT_FATAL("invalid tev scale {}", underlying(scale));
  case GX_CS_SCALE_1:
    return ""sv;
  case GX_CS_SCALE_2:
    return " * 2.0"sv;
  case GX_CS_SCALE_4:
    return " * 4.0"sv;
  case GX_CS_DIVIDE_2:
    return " / 2.0"sv;
  }
}

std::string vtx_attr(const ShaderConfig& config, GXAttr attr) {
  const auto type = config.attrs[attr].attrType;
  if (type == GX_NONE) {
    if (attr == GX_VA_PNMTXIDX) {
      return "(imm.current_pnmtx & 0xFFu)";
    }
    if (attr == GX_VA_NRM) {
      // Default normal
      return "vec3f(1.0, 0.0, 0.0)"s;
    }
    if (attr == GX_VA_CLR0 || attr == GX_VA_CLR1) {
      // No color in the vertex but a channel takes its material/ambient color from it. On
      // hardware the color of the last vertex that had one lingers; that history is not tracked,
      // so use white (the common emulator choice). Black made such materials vanish, e.g. the
      // core of Luigi's Mansion's flashlight beam (lit COLOR0 from a material without vertex
      // colors feeding a GX_TG_SRTG lookup).
      return "vec4f(1.0, 1.0, 1.0, 1.0)"s;
    }
    UNLIKELY FATAL("unmapped vtx attr {}", underlying(attr));
  }
  if (attr == GX_VA_POS) {
    return "in_pos"s;
  }
  if (attr == GX_VA_NRM) {
    return "in_nrm"s;
  }
  if (attr == GX_VA_CLR0 || attr == GX_VA_CLR1) {
    const auto idx = attr - GX_VA_CLR0;
    return fmt::format("in_clr{}", idx);
  }
  if (attr >= GX_VA_TEX0 && attr <= GX_VA_TEX7) {
    const auto idx = attr - GX_VA_TEX0;
    return fmt::format("in_tex{}_uv", idx);
  }
  if (attr == GX_VA_PNMTXIDX) {
    return "in_pnmtxidx"s;
  }
  if (attr >= GX_VA_TEX0MTXIDX && attr <= GX_VA_TEX7MTXIDX) {
    const auto idx = attr - GX_VA_TEX0MTXIDX;
    return fmt::format("in_texmtxidx{}", idx);
  }
  UNLIKELY FATAL("unhandled vtx attr {}", underlying(attr));
}

constexpr std::array<std::string_view, GX_CC_ZERO + 1> TevColorArgNames{
    "CPREV"sv, "APREV"sv, "C0"sv,   "A0"sv,   "C1"sv,  "A1"sv,   "C2"sv,    "A2"sv,
    "TEXC"sv,  "TEXA"sv,  "RASC"sv, "RASA"sv, "ONE"sv, "HALF"sv, "KONST"sv, "ZERO"sv,
};
constexpr std::array<std::string_view, GX_CA_ZERO + 1> TevAlphaArgNames{
    "APREV"sv, "A0"sv, "A1"sv, "A2"sv, "TEXA"sv, "RASA"sv, "KONST"sv, "ZERO"sv,
};

auto fetch_fixed16_attr(std::string_view fetchFn, const AttrConfig& mapping, std::string_view buf,
                        std::string_view offs, bool le) -> std::string {
  // Some Adreno drivers appear sensitive to generated shaders that route 2- and
  // 3-component fixed-16 vertex attributes through reusable vector fetch helpers.
  // Emitting scalar component fetches at the call site avoids observed artifacts.
  if (mapping.cnt == 2) {
    const auto comp0 = fmt::format("{}_1(&{}, {} + 0u, {}u, {})", fetchFn, buf, offs, mapping.frac, le);
    const auto comp1 = fmt::format("{}_1(&{}, {} + 2u, {}u, {})", fetchFn, buf, offs, mapping.frac, le);
    return fmt::format("vec2f({}, {})", comp0, comp1);
  }
  if (mapping.cnt == 3) {
    const auto comp0 = fmt::format("{}_1(&{}, {} + 0u, {}u, {})", fetchFn, buf, offs, mapping.frac, le);
    const auto comp1 = fmt::format("{}_1(&{}, {} + 2u, {}u, {})", fetchFn, buf, offs, mapping.frac, le);
    const auto comp2 = fmt::format("{}_1(&{}, {} + 4u, {}u, {})", fetchFn, buf, offs, mapping.frac, le);
    return fmt::format("vec3f({}, {}, {})", comp0, comp1, comp2);
  }
  return fmt::format("{}_{}(&{}, {}, {}u, {})", fetchFn, mapping.cnt, buf, offs, mapping.frac, le);
}

auto fetch_attr(const AttrConfig& mapping, std::string_view buf, std::string_view offs, bool le) -> std::string {
  switch (mapping.compType) {
  case GX_U8:
    return fmt::format("fetch_u8_{}(&{}, {}, {}, {})", mapping.cnt, buf, offs, mapping.frac, le);
  case GX_S8:
    return fmt::format("fetch_s8_{}(&{}, {}, {}, {})", mapping.cnt, buf, offs, mapping.frac, le);
  case GX_U16:
    return fetch_fixed16_attr("fetch_u16"sv, mapping, buf, offs, le);
  case GX_S16:
    return fetch_fixed16_attr("fetch_s16"sv, mapping, buf, offs, le);
  case GX_F32:
    return fmt::format("fetch_f32_{}(&{}, {}, {})", mapping.cnt, buf, offs, le);
  case GX_RGBA8:
    return fmt::format("unpack4x8unorm(load_u32_raw(&{}, {}))", buf, offs);
  default:
    Log.fatal("fetch_attr: Unimplemented {}", static_cast<GXCompType>(mapping.compType));
  }
}

auto fetch_color_attr(const AttrConfig& mapping, std::string_view buf, std::string_view offs, bool le) -> std::string {
  switch (mapping.compType) {
  case GX_RGB565:
    return fmt::format("fetch_rgb565(&{}, {}, {})", buf, offs, le);
  case GX_RGB8:
    return fmt::format("fetch_rgb8(&{}, {}, {})", buf, offs, le);
  case GX_RGBX8:
    return fmt::format("fetch_rgbx8(&{}, {}, {})", buf, offs, le);
  case GX_RGBA4:
    return fmt::format("fetch_rgba4(&{}, {}, {})", buf, offs, le);
  case GX_RGBA6:
    return fmt::format("fetch_rgba6(&{}, {}, {})", buf, offs, le);
  case GX_RGBA8:
    return fmt::format("fetch_rgba8(&{}, {}, {})", buf, offs, le);
  default:
    Log.fatal("fetch_color_attr: Unimplemented {}", static_cast<GXCompType>(mapping.compType));
  }
}

struct AttrAddress {
  std::string offs;
  std::string_view buf;
  bool le;
};

// Immediates cannot contain arrays, so array_start is packed as three vec4u.
std::string imm_array_start(GXAttr attr) noexcept {
  const u32 idx = attr - GX_VA_POS;
  return fmt::format("imm.array_start{}.{}", idx / 4, "xyzw"[idx % 4]);
}

auto attr_address(const AttrConfig& mapping, GXAttr attr, std::string_view vidx, u32 vtxStride, u32 dlExtra, u32 within)
    -> AttrAddress {
  const u32 dlOffset = mapping.offset + dlExtra;
  if (mapping.attrType == GX_INDEX8) {
    return {fmt::format("{} + raw_fetch_u8_1(&vbuf, imm.vtx_start + {} * {}u + {}u) * {}u + {}u", imm_array_start(attr),
                        vidx, vtxStride, dlOffset, mapping.stride, within),
            "abuf"sv, mapping.le};
  }
  if (mapping.attrType == GX_INDEX16) {
    return {fmt::format("{} + raw_fetch_u16_1(&vbuf, imm.vtx_start + {} * {}u + {}u, false) * {}u + {}u",
                        imm_array_start(attr), vidx, vtxStride, dlOffset, mapping.stride, within),
            "abuf"sv, mapping.le};
  }
  return {fmt::format("imm.vtx_start + {} * {}u + {}u", vidx, vtxStride, dlOffset + within), "vbuf"sv, false};
}

auto attr_load(const ShaderConfig& config, GXAttr attr, std::string_view vidx) -> std::string {
  const auto& mapping = config.attrs[attr];
  if (mapping.attrType == GX_NONE) {
    return vtx_attr(config, attr);
  }
  const auto [offs, buf, le] = attr_address(mapping, attr, vidx, config.vtxStride, 0u, 0u);
  switch (attr) {
  case GX_VA_PNMTXIDX:
    return fmt::format("(raw_fetch_u8_1(&{}, {}) / 3u)", buf, offs);
  case GX_VA_TEX0MTXIDX:
  case GX_VA_TEX1MTXIDX:
  case GX_VA_TEX2MTXIDX:
  case GX_VA_TEX3MTXIDX:
  case GX_VA_TEX4MTXIDX:
  case GX_VA_TEX5MTXIDX:
  case GX_VA_TEX6MTXIDX:
  case GX_VA_TEX7MTXIDX:
    return fmt::format("raw_fetch_u8_1(&{}, {})", buf, offs);
  case GX_VA_POS: {
    const auto posLoad = fetch_attr(mapping, buf, offs, le);
    if (mapping.cnt == 2) {
      return fmt::format("vec3f({}, 0.0)", posLoad);
    }
    return posLoad;
  }
  case GX_VA_NRM:
    // NBT: normal only here; binormal/tangent loaded via attr_load_nbt_slice
    if (mapping.cnt > 3) {
      auto nrmMapping = mapping;
      nrmMapping.cnt = 3;
      return fetch_attr(nrmMapping, buf, offs, le);
    }
    return fetch_attr(mapping, buf, offs, le);
  case GX_VA_CLR0:
  case GX_VA_CLR1:
    return fetch_color_attr(mapping, buf, offs, le);
  case GX_VA_TEX0:
  case GX_VA_TEX1:
  case GX_VA_TEX2:
  case GX_VA_TEX3:
  case GX_VA_TEX4:
  case GX_VA_TEX5:
  case GX_VA_TEX6:
  case GX_VA_TEX7: {
    const auto texLoad = fetch_attr(mapping, buf, offs, le);
    if (mapping.cnt == 1) {
      return fmt::format("vec2f({}, 0.0)", texLoad);
    }
    return texLoad;
  }
  default:
    Log.fatal("attr_load: Unimplemented {}", attr);
  }
}

enum class NbtSlice : u8 {
  N,
  B,
  T,
};

auto attr_load_nbt_slice(const ShaderConfig& config, NbtSlice slice, std::string_view vidx) -> std::string {
  const auto& mapping = config.attrs[GX_VA_NRM];
  if (mapping.attrType == GX_NONE || mapping.cnt != 9) {
    Log.fatal("attr_load_nbt_slice: GX_TG_BINRM/TANGENT requires GX_NRM_NBT or GX_NRM_NBT3");
  }
  const auto sliceIdx = static_cast<u32>(slice);
  const auto compsize = comp_type_size(GX_VA_NRM, static_cast<GXCompType>(mapping.compType));
  u32 dlExtra = 0;
  if (mapping.nbt3) {
    if (mapping.attrType == GX_INDEX8) {
      dlExtra = sliceIdx;
    } else if (mapping.attrType == GX_INDEX16) {
      dlExtra = sliceIdx * 2u;
    }
  }
  const u32 within = sliceIdx * 3u * compsize;
  const auto [offs, buf, le] = attr_address(mapping, GX_VA_NRM, vidx, config.vtxStride, dlExtra, within);
  auto sliceMapping = mapping;
  sliceMapping.cnt = 3;
  return fetch_attr(sliceMapping, buf, offs, le);
}

constexpr std::string_view nbt_slice_local(NbtSlice slice) noexcept {
  return slice == NbtSlice::B ? "in_binrm" : "in_tangent";
}

constexpr bool is_emboss_texgen(GXTexGenType type) noexcept { return type >= GX_TG_BUMP0 && type <= GX_TG_BUMP7; }

auto lighting_func(const ShaderConfig& config, const ColorChannelConfig& cc, u8 i, bool alpha) -> std::string {
  std::string_view swizzle = alpha ? ".a"sv : ""sv;
  std::string outVar;
  std::string_view posVar;
  if (UsePerPixelLighting) {
    outVar = fmt::format("rast{}", i);
    posVar = "in.mv_pos"sv;
  } else {
    outVar = fmt::format("out.cc{}", i);
    posVar = "mv_pos"sv;
  }
  std::string ambSrc, matSrc;
  if (cc.ambSrc == GX_SRC_VTX) {
    if (UsePerPixelLighting) {
      ambSrc = fmt::format("in.clr{}", i);
    } else {
      ambSrc = vtx_attr(config, static_cast<GXAttr>(GX_VA_CLR0 + i));
    }
  } else if (cc.ambSrc == GX_SRC_REG) {
    ambSrc = fmt::format("ubuf.cc{0}{1}_amb", i, alpha ? "a"sv : ""sv);
  }
  if (cc.matSrc == GX_SRC_VTX) {
    if (UsePerPixelLighting) {
      matSrc = fmt::format("in.clr{}", i);
    } else {
      matSrc = vtx_attr(config, static_cast<GXAttr>(GX_VA_CLR0 + i));
    }
  } else if (cc.matSrc == GX_SRC_REG) {
    matSrc = fmt::format("ubuf.cc{0}{1}_mat", i, alpha ? "a"sv : ""sv);
  }
  if (!cc.lightingEnabled) {
    return fmt::format("\n    {0}{2} = {1}{2};", outVar, matSrc, swizzle);
  }
  GXDiffuseFn diffFn = cc.diffFn;
  std::string lightAttnFn;
  if (cc.attnFn == GX_AF_NONE) {
    lightAttnFn = "attn = 1.0;"s;
  } else if (cc.attnFn == GX_AF_SPOT) {
    lightAttnFn = fmt::format(R"""(
          var cosine = max(0.0, dot(ldir, light.dir));
          var cos_attn = gx_attn_poly(light.cos_att, cosine);
          var dist_attn = gx_attn_poly(light.dist_att, dist);
          attn = max(0.0, cos_attn / dist_attn);)""");
  } else if (cc.attnFn == GX_AF_SPEC) {
    std::string_view normal = UsePerPixelLighting ? "in.mv_nrm"sv : "mv_nrm"sv;
    std::string dist_attn = diffFn != GX_DF_NONE
                                ? "max(0.0, dot(normalize(light.dist_att), vec3f(1.0, attn, attn * attn)));"
                                : "max(0.0, dot(light.dist_att, vec3f(1.0, attn, attn * attn)));";
    lightAttnFn = fmt::format(R"""(
          attn = select(0.0, max(0.0, dot({0}, light.dir)), dot({0}, ldir) >= 0.0);
          var cos_attn = gx_attn_poly(light.cos_att, attn);
          var dist_attn = {1};
          attn = max(0.0, cos_attn / dist_attn);)""",
                              normal, dist_attn);
  }
  std::string_view lightDiffFn;
  if (diffFn == GX_DF_NONE) {
    lightDiffFn = "1.0"sv;
  } else if (diffFn == GX_DF_SIGN) {
    if (UsePerPixelLighting) {
      lightDiffFn = "dot(ldir, in.mv_nrm)"sv;
    } else {
      lightDiffFn = "dot(ldir, mv_nrm)"sv;
    }
  } else if (diffFn == GX_DF_CLAMP) {
    if (UsePerPixelLighting) {
      lightDiffFn = "max(0.0, dot(ldir, in.mv_nrm))"sv;
    } else {
      lightDiffFn = "max(0.0, dot(ldir, mv_nrm))"sv;
    }
  }
  return fmt::format(R"""(
    {{
      var lighting = {5};
      for (var i = 0u; i < {1}u; i++) {{
          if ((ubuf.lightState{0}{9} & (1u << i)) == 0u) {{ continue; }}
          var light = ubuf.lights[i];
          var ldir = light.pos - {6};
          var dist2 = dot(ldir, ldir);
          var dist = sqrt(dist2);
          ldir = ldir / dist;
          var attn: f32;{2}
          var diff = {3};
          lighting = lighting + (attn * diff * light.color);
      }}
      {7}{8} = ({4} * clamp(lighting, vec4f(0.0), vec4f(1.0))){8};
    }})""",
                     i, GX::MaxLights, lightAttnFn, lightDiffFn, matSrc, ambSrc, posVar, outVar, swizzle,
                     alpha ? "a"sv : ""sv);
}

// ---- Enhanced lighting (include/aurora/enhanced.h) ---------------------------------------------
// World-geometry draws get a shader variant (ShaderConfig::enhClass) whose extra work is switched
// at runtime by ubuf.enh_flags (bit0 per-pixel lighting, bit1 extra lights, bit2 soft shadows,
// bit3 flashlight quality), so toggling a part never rebuilds pipelines.

// Per-pixel version of lighting_func: the same GX channel math, evaluated with the interpolated
// view-space position and normal (in.enh_pos / enh_n), writing rast{i}.
std::string enh_lighting_func(const ShaderConfig& config, const ColorChannelConfig& cc, u8 i, bool alpha) {
  std::string_view swizzle = alpha ? ".a"sv : ".rgb"sv;
  std::string ambSrc, matSrc;
  if (cc.ambSrc == GX_SRC_VTX) {
    ambSrc = fmt::format("in.enh_clr{}", i);
  } else {
    ambSrc = fmt::format("ubuf.cc{0}{1}_amb", i, alpha ? "a"sv : ""sv);
  }
  if (cc.matSrc == GX_SRC_VTX) {
    matSrc = fmt::format("in.enh_clr{}", i);
  } else {
    matSrc = fmt::format("ubuf.cc{0}{1}_mat", i, alpha ? "a"sv : ""sv);
  }
  if (!cc.lightingEnabled) {
    return fmt::format("\n      rast{0}{2} = {1}{2};", i, matSrc, swizzle);
  }
  std::string lightAttnFn;
  if (cc.attnFn == GX_AF_NONE) {
    lightAttnFn = "attn = 1.0;"s;
  } else if (cc.attnFn == GX_AF_SPOT) {
    // Spot cone with an optional smoothed penumbra (flashlight quality): the GX angular term is
    // remapped by smoothstep so the cone edge has no kink at the cutoff.
    // Only the color channel's cone edge (cos_attn below 1) is reshaped: alpha channels are
    // masks (the shadow light's ALPHA1 scales the shadow strength) and stay exact.
    lightAttnFn = alpha ? R"""(
            let cosine = max(0.0, dot(ldir, light.dir));
            let cos_attn = gx_attn_poly(light.cos_att, cosine);
            let dist_attn = gx_attn_poly(light.dist_att, dist);
            attn = max(0.0, cos_attn / dist_attn);)"""s
                        : R"""(
            let cosine = max(0.0, dot(ldir, light.dir));
            var cos_attn = gx_attn_poly(light.cos_att, cosine);
            if (cos_attn < 1.0) {
              let ca = clamp(cos_attn, 0.0, 1.0);
              cos_attn = mix(cos_attn, ca * ca * (3.0 - 2.0 * ca), ubuf.enh_flash_soft);
            }
            let dist_attn = gx_attn_poly(light.dist_att, dist);
            attn = max(0.0, cos_attn / dist_attn);
            // 0196: realistic flashlight footprint (bright spot cones only): the reflector profile
            // (hot centre, rings, soft edge, spill) over the cone the game defines, and an
            // inverse-square falloff (lens-size regularised) instead of the game's flat distance.
            if ((ubuf.enh_flags & 32u) != 0u && dot(light.color.rgb, vec3f(0.3333)) > 0.15) {
              let cut = enh_beam_cut(light.cos_att);
              if (cut > -1.0) {
                let bx = acos(clamp(cosine, -1.0, 1.0)) / max(acos(cut), 1e-3);
                let be = clamp((ubuf.enh_beam_dref2 + 1600.0) / (dist2 + 1600.0), ubuf.enh_beam_floor, 2.2);
                attn = enh_beam_profile(bx, ubuf.enh_beam_rings) * be * ubuf.enh_beam_gain;
              }
            })"""s;
  } else {
    const std::string dist_attn = cc.diffFn != GX_DF_NONE
                                      ? "max(0.0, dot(normalize(light.dist_att), vec3f(1.0, attn, attn * attn)))"
                                      : "max(0.0, dot(light.dist_att, vec3f(1.0, attn, attn * attn)))";
    lightAttnFn = fmt::format(R"""(
            attn = select(0.0, max(0.0, dot(enh_n, light.dir)), dot(enh_n, ldir) >= 0.0);
            let cos_attn = gx_attn_poly(light.cos_att, attn);
            let dist_attn = {};
            attn = max(0.0, cos_attn / dist_attn);)""",
                              dist_attn);
  }
  std::string_view lightDiffFn = "1.0"sv;
  if (cc.diffFn == GX_DF_SIGN) {
    lightDiffFn = "dot(ldir, enh_n)"sv;
  } else if (cc.diffFn == GX_DF_CLAMP) {
    lightDiffFn = "max(0.0, dot(ldir, enh_n))"sv;
  }
  // Flashlight specular (color channels with spot lights): a glossy highlight where the beam hits,
  // scaled by ubuf.enh_flash_spec (0 = the GX result only).
  const bool spec = !alpha && cc.attnFn == GX_AF_SPOT && cc.diffFn != GX_DF_NONE;
  const std::string_view specDecl =
      spec ? "\n        var spec = vec3f(0.0);\n        let enh_v = normalize(-in.enh_pos);"sv : ""sv;
  const std::string_view specAcc =
      spec ? "\n            if (dot(ldir, enh_n) > 0.0) { spec = spec + attn * pow(max(dot(enh_n, normalize(ldir + enh_v)), 0.0), 28.0) * light.color.rgb; }"sv
           : ""sv;
  const std::string specApply =
      spec ? fmt::format("\n        rast{0}.rgb = min(rast{0}.rgb + {1}.rgb * spec * ubuf.enh_flash_spec, vec3f(1.0));", i, matSrc)
           : std::string{};
  return fmt::format(R"""(
      {{
        var lighting = {4}{6};{9}
        for (var i = 0u; i < {1}u; i++) {{
            if ((ubuf.lightState{0}{5} & (1u << i)) == 0u) {{ continue; }}
            let light = ubuf.lights[i];
            var ldir = light.pos - in.enh_pos;
            let dist2 = dot(ldir, ldir);
            let dist = sqrt(dist2);
            ldir = ldir / dist;
            var attn: f32;{2}
            let diff = {3};
            lighting = lighting + (attn * diff * light.color){6};{10}
        }}
        rast{0}{6} = ({7}{6} * clamp(lighting, {8}(0.0), {8}(1.0)));{11}
      }})""",
                     i, GX::MaxLights, lightAttnFn, lightDiffFn, ambSrc, alpha ? "a"sv : ""sv, swizzle, matSrc,
                     alpha ? "f32"sv : "vec3f"sv, specDecl, specAcc, specApply);
}

// Material color expression of color channel i (for extra lights added to its lighting sum).
std::string enh_mat_src(const ColorChannelConfig& cc, u8 i) {
  if (cc.matSrc == GX_SRC_VTX) {
    return fmt::format("in.enh_clr{}.rgb", i);
  }
  return fmt::format("ubuf.cc{}_mat.rgb", i);
}

constexpr std::string_view EnhHelpers = R"""(
fn enh_ld(i: u32) -> f32 { return bitcast<f32>(abuf[i]); }
// Shadow of extra light `slot` at a fragment: occlusion (0 lit .. 1 shadowed) from this frame's
// mask (RGBA8, one channel per shadowed light, written into the storage buffer before the pass),
// bilinear from 2x2 taps for a softer edge.
// 0186: the fragment's window depth and its screen-space slope (set at the top of fs_main, in
// uniform control flow), for the depth match against the mask's pre-pass depth.
var<private> enh_fz: f32;
var<private> enh_dz: f32;
fn enh_mask(frag: vec2f, slot: u32) -> f32 {
  let dims = vec2i(i32(ubuf.enh_mask_dims & 0xFFFFu), i32(ubuf.enh_mask_dims >> 16u));
  if (dims.x == 0) { return 0.0; }
  // A 4x4 tent over the (half-resolution) mask around the pixel, so the ray march's per-pixel
  // sample pattern and the mask's blockiness disappear. 0186: each texel counts only if the
  // pre-pass depth stored with it is this fragment's surface (depth match within the depth slope
  // over the tap distance): occlusion and shadows of a surface behind (Luigi in front of a
  // cabinet) or in front never land on this one. No matching texel: unshadowed.
  let m = frag * ubuf.enh_mask_scale - vec2f(0.5);
  let base = vec2i(floor(m)) - vec2i(1);
  let plane = (slot >> 2u) * ubuf.enh_mask_pitch * u32(dims.y);
  let dplane = 5u * ubuf.enh_mask_pitch * u32(dims.y); // 0187: MaskDepthPlane
  let tol = enh_dz * (2.5 / max(ubuf.enh_mask_scale, 1e-3) + 1.0) + enh_fz * 0.004 + 1e-7;
  var acc = 0.0;
  var wsum = 0.0;
  for (var k = 0; k < 16; k++) {
    let o = vec2i(k & 3, k >> 2u);
    let c = base + o;
    let dd = vec2f(c) - m;
    let wt = max(2.0 - abs(dd.x), 0.0) * max(2.0 - abs(dd.y), 0.0);
    if (wt <= 0.0 || any(c < vec2i(0)) || any(c >= dims)) { continue; }
    let row = ubuf.enh_mask_base + u32(c.y) * ubuf.enh_mask_pitch + u32(c.x);
    let ds = bitcast<f32>(mbuf[row + dplane]);
    let dz = abs(ds - enh_fz);
    let wd = 1.0 - smoothstep(tol, tol * 2.0, dz);
    if (wd <= 0.0) { continue; }
    let word = mbuf[row + plane];
    let v = f32((word >> ((slot & 3u) * 8u)) & 0xFFu) / 255.0;
    acc = acc + v * wt * wd;
    wsum = wsum + wt * wd;
  }
  return select(0.0, acc / wsum, wsum > 1e-4);
}
// Extra point lights (aurora/lights.h), 24 words each in the storage buffer:
// pos xyz, radius, color rgb, intensity, flags, id, shadow slot (or ~0), 0, then (0187) the room
// box: centre relative to pos (3), half-axis vectors (3 x 3), all view space.
fn enh_extra_lights(p: vec3f, n: vec3f, ghost: bool, frag: vec2f) -> vec3f {
  var sum = vec3f(0.0);
  let cnt = min(ubuf.enh_light_count, 64u);
  let v = normalize(-p);
  for (var i = 0u; i < cnt; i++) {
    let b = ubuf.enh_light_base + i * 24u;
    // 0213: the reach test first (most lights miss a given pixel), then the flags.
    let d = vec3f(enh_ld(b), enh_ld(b + 1u), enh_ld(b + 2u)) - p;
    let radius = max(enh_ld(b + 3u), 1.0);
    let dist2 = dot(d, d);
    if (dist2 >= radius * radius) { continue; }
    let flags = abuf[b + 8u];
    if (ghost && (flags & 2u) == 0u) { continue; }
    let l = d * inverseSqrt(max(dist2, 1e-6));
    let x2 = dist2 / (radius * radius);
    let w = (1.0 - x2) * (1.0 - x2);
    var fall = w / (1.0 + 8.0 * x2);
    if ((flags & 16u) != 0u) {
      // 0187: limited to its room's box (fading out over 24 units past it): no light through
      // walls into the next room for lights without a shadow there.
      let q = -d - vec3f(enh_ld(b + 12u), enh_ld(b + 13u), enh_ld(b + 14u));
      var ex = vec3f(0.0);
      for (var k = 0u; k < 3u; k++) {
        let ax = vec3f(enh_ld(b + 15u + 3u * k), enh_ld(b + 16u + 3u * k), enh_ld(b + 17u + 3u * k));
        let al = max(length(ax), 1e-3);
        ex[k] = max(abs(dot(q, ax)) / al - al, 0.0);
      }
      fall = fall * (1.0 - smoothstep(0.0, 24.0, length(ex)));
      if (fall <= 0.0) { continue; }
    }
    var ndl = dot(n, l);
    ndl = select(max(ndl, 0.0), ndl * 0.5 + 0.5, ghost);
    // Characters (enh_material 1): matte wrap-around diffuse, so a colored light shapes the body
    // softly instead of a hard terminator; no highlight.
    ndl = select(ndl, clamp((dot(n, l) + 0.45) / 1.45, 0.0, 1.0) * 0.85, ubuf.enh_material > 0.5 && !ghost);
    var col = vec3f(enh_ld(b + 4u), enh_ld(b + 5u), enh_ld(b + 6u)) * enh_ld(b + 7u);
    if ((flags & 8u) != 0u) {
      // Fire (ExtraLightFire): yellow-white near the flame, deepening to red-orange with distance
      // (green and blue fall off faster than red), like the glow around a real fire.
      let x = sqrt(x2);
      col = col * vec3f(1.0, mix(1.2, 0.62, x), mix(1.5, 0.35, x));
    }
    let slot = abuf[b + 10u];
    if (slot < 16u) {
      // Shadow weight in flags bits 8..15 (0 = full): shadows fade in/out when the set of
      // shadow-casting lights changes.
      let sw = (flags >> 8u) & 0xFFu;
      col = col * (1.0 - enh_mask(frag, slot) * select(1.0, f32(sw) / 255.0, sw != 0u));
    }
    sum = sum + col * (ndl * fall);
    if ((flags & 4u) != 0u && ubuf.enh_material < 0.5) {
      let h = normalize(l + v);
      sum = sum + col * (fall * 0.35 * pow(max(dot(n, h), 0.0), 24.0));
    }
  }
  return sum * ubuf.enh_extra_scale;
}
// How much of the game's shadow darkening applies. The game lerps a shadowed pixel toward its
// shadow color whatever lights it; here only the share of the pixel's light the shadow stands for
// is removed: extra (colored) lights are never removed (they have their own shadows) and
// enh_shadow_keep of the ambient part is kept; enh_shadow_dark scales the result. lit < 0: the
// split is unknown (unlit channel 0), only the darkness applies. Exactly 1 (the game's darkening)
// when dark = 1, keep = 0 and no extra light reaches the pixel.
fn enh_shadow_scale(lit: f32, amb: f32, xl: f32) -> f32 {
  if (lit < 0.0) { return ubuf.enh_shadow_dark; }
  let total = lit + xl;
  if (total <= 1e-4) { return ubuf.enh_shadow_dark; }
  let removable = lit - min(amb, lit) * ubuf.enh_shadow_keep;
  return ubuf.enh_shadow_dark * clamp(removable / total, 0.0, 1.0);
}
// Interleaved gradient noise (per-pixel rotation of the filter kernels).
fn enh_ign(p: vec2f) -> f32 { return fract(52.9829189 * fract(dot(p, vec2f(0.06711056, 0.00583715)))); }
fn enh_key(t: vec4f, sr: u32, sg: u32) -> f32 {
  let v = round(clamp(t, vec4f(0.0), vec4f(1.0)) * 255.0);
  return v[sg] * 256.0 + v[sr];
}
fn enh_fetch_key(tex: texture_2d<f32>, uv: vec2f, off: vec2f, dims: vec2f, sr: u32, sg: u32) -> f32 {
  let c = clamp(vec2i(floor(uv * dims + off)), vec2i(0), vec2i(dims) - vec2i(1));
  return enh_key(textureLoad(tex, c, 0), sr, sg);
}
fn enh_pick_key(tev: f32, analytic: f32) -> f32 { return select(tev, analytic, abs(analytic - tev) < 768.0); }
fn enh_vogel(k: u32, n: u32, rot: f32) -> vec2f {
  let r = sqrt((f32(k) + 0.5) / f32(n));
  let a = f32(k) * 2.39996323 + rot;
  return vec2f(cos(a), sin(a)) * r;
}
// Soft version of the game's shadow-map compare (receiver key > map key, GR16 keys of the
// packed depth): the fraction of taps in shadow. PCF over a rotated Vogel disk; with PCSS the
// radius follows the receiver-blocker distance (contact hardening). `slope` is the receiver
// key's change per map texel (slope-scaled bias).
fn enh_shadow(tex: texture_2d<f32>, uv: vec2f, recv: f32, sr: u32, sg: u32, frag: vec2f, slope: f32) -> f32 {
  let dims = vec2f(textureDimensions(tex));
  let rot = enh_ign(frag) * 6.2831853;
  let q = ubuf.enh_shadow_taps;
  let taps = max(q & 0xFFFFu, 1u);
  var radius = ubuf.enh_shadow_radius * dims.x / 640.0;
  // Slope-scaled bias for the off-centre taps only, capped: with a grazing light (e.g. a ghost's
  // own light near the floor) a full receiver-plane bias would remove thin casters' shadows.
  let sl = clamp(slope * 0.75, 0.0, 96.0);
  // Early out: centre and an outer ring of 4 taps (at the widest search radius) agree -> the
  // pixel is fully lit or fully shadowed, no penumbra to filter (most pixels).
  {
    let ro = radius * select(1.0, 1.5, (q & 0x10000u) != 0u);
    var n = 0.0;
    for (var k = 0u; k < 5u; k++) {
      let a = rot + f32(k) * 1.5707963;
      let o = select(vec2f(cos(a), sin(a)) * ro, vec2f(0.0), k == 4u);
      let key = enh_fetch_key(tex, uv, o, dims, sr, sg);
      n = n + select(0.0, 1.0, recv - (3.0 + sl * length(o)) > key);
    }
    if (n == 0.0 || n == 5.0) { return n / 5.0; }
  }
  if ((q & 0x10000u) != 0u) {
    var bsum = 0.0;
    var bcnt = 0.0;
    let sr2 = radius * 1.5;
    for (var k = 0u; k < taps; k++) {
      let o = enh_vogel(k, taps, rot) * sr2;
      let key = enh_fetch_key(tex, uv, o, dims, sr, sg);
      if (recv - (3.0 + sl * length(o)) > key) {
        bsum = bsum + key;
        bcnt = bcnt + 1.0;
      }
    }
    if (bcnt == 0.0) { return 0.0; }
    let blk = bsum / bcnt;
    // Penumbra from the receiver-blocker key distance; capped at 1.5x the filter radius so thin
    // casters (spears, chair legs) keep a visible shadow.
    let pen = clamp((recv - blk) / max(65535.0 - blk, 256.0) * ubuf.enh_pcss * 0.35, 0.0, 1.0);
    radius = mix(max(radius * 0.3, 0.75), radius * 1.5, pen);
  }
  var s = 0.0;
  for (var k = 0u; k < taps; k++) {
    let o = enh_vogel(k, taps, rot) * radius;
    let key = enh_fetch_key(tex, uv, o, dims, sr, sg);
    s = s + select(0.0, 1.0, recv - (3.0 + sl * length(o)) > key);
  }
  return s / f32(taps);
}
)""";

u32 swap_index(GXTevColorChan c) noexcept {
  switch (c) {
  case GX_CH_RED:
    return 0;
  case GX_CH_GREEN:
    return 1;
  case GX_CH_BLUE:
    return 2;
  default:
    return 3;
  }
}

// Enhanced soft shadows: a stage doing the game's shadow-map compare (alpha GR16 GT of the
// color inputs A = receiver depth key, B = TEXC = packed map depth). Returns the WGSL that sets
// `enh_sf{idx}` (fraction of the stage's compare that is true) or "" when not applicable.
// `binaryCond` is the original compare condition (used when soft shadows are off).
std::string enh_shadow_stage(size_t idx, const ShaderConfig& config, const TevStage& stage,
                             std::string_view recvKeyExpr, std::string_view binaryCond) {
  const u32 map = underlying(stage.texMapId);
  const u32 coord = underlying(stage.texCoordId);
  const auto& swap = config.tevSwapTable[stage.tevSwapTex];
  // Receiver key: analytic from the depth-ramp lookup of the previous stage when it is the
  // game's ramp LUT (texel (x, y) = (A x, I y), s clamped, t = 256 s repeating: key =
  // floor(65536 s)), which avoids the LINEAR-filtered LUT's byte-wrap stripes; otherwise the
  // TEV's own key.
  std::string recv = fmt::format("f32({})", recvKeyExpr);
  static const bool sTevKey = std::getenv("AURORA_ENH_TEVKEY") != nullptr; // debug aid
  if (idx > 0 && !sTevKey) {
    const auto& prev = config.tevStages[idx - 1];
    if (prev.texMapId != GX_TEXMAP_NULL && prev.texCoordId != GX_TEXCOORD_NULL && prev.colorOp.outReg == GX_TEVPREV &&
        config.tcgs[prev.texCoordId].type == GX_TG_MTX3x4) {
      // Only where it agrees with the TEV's key (other materials map the ramp differently, e.g.
      // the room geometry in lit rooms; a wrong key loses every shadow).
      recv = fmt::format("enh_pick_key({}, min(floor(clamp(tex{}_uv.x, 0.0, 1.0) * 65536.0), 65535.0))", recv,
                         underlying(prev.texCoordId));
    }
  }
  return fmt::format(
      "\n    // Enhanced soft shadow (stage {0})"
      "\n    let enh_recv{0} = {1};"
      "\n    let enh_slope{0} = fwidth(enh_recv{0}) / max(length(fwidth(tex{2}_uv * vec2f(textureDimensions(tex{3})))), 1e-3);"
      "\n    var enh_sf{0} = select(0.0, 1.0, {4});"
      "\n    if ((ubuf.enh_flags & 4u) != 0u) {{"
      "\n      enh_sf{0} = enh_shadow(tex{3}, tex{2}_uv, enh_recv{0}, {5}u, {6}u, in.pos.xy, enh_slope{0});"
      "\n    }}"
      "\n    enh_sf{0} = enh_sf{0} * enh_shadow_scale(enh_s_lit, enh_s_amb, enh_s_xl);",
      idx, recv, coord, map, binaryCond, swap_index(swap.red), swap_index(swap.green));
}

bool enh_is_shadow_stage(const ShaderConfig& config, const TevStage& stage) noexcept {
  return config.enhClass != 0 && !config.lightDepth && !config.depthOnly && stage.alphaOp.op == GX_TEV_COMP_GR16_GT && stage.colorPass.b == GX_CC_TEXC &&
         stage.texMapId != GX_TEXMAP_NULL && stage.texCoordId != GX_TEXCOORD_NULL;
}

// ---- Hardware details (0180) ------------------------------------------------------------------
// Three Flipper details with their evidence in the ports' docs/ACCURACY.md. Each is on by default;
// an environment variable set to 0 restores the previous behaviour for A/B comparisons (read once).
bool hw_env_on(const char* name) noexcept {
  const char* v = std::getenv(name);
  return v == nullptr || v[0] != '0';
}
// Setup unit: vertex positions snap down to the 1/12-pixel subsample grid (AURORA_PIXEL_SNAP).
bool hw_pixel_snap() noexcept {
  static const bool on = hw_env_on("AURORA_PIXEL_SNAP");
  return on;
}
// PE dither: the 4x4 Bayer matrix (AURORA_DITHER_4X4; 0 = the 2x2 matrix).
bool hw_dither_4x4() noexcept {
  static const bool on = hw_env_on("AURORA_DITHER_4X4");
  return on;
}
// TEV GX_CS_DIVIDE_2 without a rounding constant (AURORA_TEV_DIV2_TRUNC; 0 = round to nearest).
bool hw_tev_div2_trunc() noexcept {
  static const bool on = hw_env_on("AURORA_TEV_DIV2_TRUNC");
  return on;
}

// ---- Integer TEV (GX_AURORA_ACCURACY_INT_TEV) -------------------------------------------------
// The hardware combiner works on integers: A/B/C inputs are 8 bits (the low byte of a register),
// D and the registers are signed 11-bit, C is widened to 0..256 (C + C>>7) for the lerp, and every
// stage rounds its result to an integer and clamps to 0..255 or -1024..1023.

constexpr std::string_view IntTevHelpers = R"(
fn itev_lerp3(a: vec3i, b: vec3i, c: vec3i) -> vec3i {
  let cc = c + (c >> vec3u(7u));
  return a * (vec3i(256) - cc) + b * cc;
}
fn itev_lerp1(a: i32, b: i32, c: i32) -> i32 {
  let cc = c + (c >> 7u);
  return a * (256 - cc) + b * cc;
}
fn itev_clamp3(r: vec3i, clamp8: bool) -> vec3i {
  if (clamp8) { return clamp(r, vec3i(0), vec3i(255)); }
  return clamp(r, vec3i(-1024), vec3i(1023));
}
fn itev_clamp1(r: i32, clamp8: bool) -> i32 {
  if (clamp8) { return clamp(r, 0, 255); }
  return clamp(r, -1024, 1023);
}
// v carries 8 fractional bits; scale 0..3 = x1, x2, x4, /2.
fn itev_finish3(v: vec3i, scale: u32, clamp8: bool) -> vec3i {
  var r: vec3i;
  if (scale == 3u) { r = (v + vec3i(256)) >> vec3u(9u); }
  else { r = (v * i32(1u << scale) + vec3i(128)) >> vec3u(8u); }
  return itev_clamp3(r, clamp8);
}
fn itev_finish1(v: i32, scale: u32, clamp8: bool) -> i32 {
  var r: i32;
  if (scale == 3u) { r = (v + 256) >> 9u; }
  else { r = (v * i32(1u << scale) + 128) >> 8u; }
  return itev_clamp1(r, clamp8);
}
// GX_CS_DIVIDE_2 (0180): no rounding constant; the lerp is truncated to an integer (floor of
// lerp / 256, negated for SUB), added to D and the bias, and the sum halved with an arithmetic
// shift (floor), then clamped. Dolphin's hardware test (hwtests gxtest/tev.cpp, random A/B/C,
// D = 0, all scales/biases/ops) expects exactly this.
fn itev_div2_3(l: vec3i, d: vec3i, sub: bool, bias: i32, clamp8: bool) -> vec3i {
  let lf = l >> vec3u(8u);
  return itev_clamp3((d + vec3i(bias) + select(lf, -lf, sub)) >> vec3u(1u), clamp8);
}
fn itev_div2_1(l: i32, d: i32, sub: bool, bias: i32, clamp8: bool) -> i32 {
  let lf = l >> 8u;
  return itev_clamp1((d + bias + select(lf, -lf, sub)) >> 1u, clamp8);
}
fn itev_op3(a: vec3i, b: vec3i, c: vec3i, d: vec3i, sub: bool, bias: i32, scale: u32, clamp8: bool) -> vec3i {
  let l = itev_lerp3(a, b, c);
  if (scale == 3u && itev_div2_trunc) { return itev_div2_3(l, d, sub, bias, clamp8); }
  return itev_finish3(d * 256 + vec3i(bias * 256) + select(l, -l, sub), scale, clamp8);
}
fn itev_op1(a: i32, b: i32, c: i32, d: i32, sub: bool, bias: i32, scale: u32, clamp8: bool) -> i32 {
  let l = itev_lerp1(a, b, c);
  if (scale == 3u && itev_div2_trunc) { return itev_div2_1(l, d, sub, bias, clamp8); }
  return itev_finish1(d * 256 + bias * 256 + select(l, -l, sub), scale, clamp8);
}
fn itev_f2i3(v: vec3f) -> vec3i { return vec3i(round(clamp(v, vec3f(0.0), vec3f(1.0)) * 255.0)); }
fn itev_f2i1(v: f32) -> i32 { return i32(round(clamp(v, 0.0, 1.0) * 255.0)); }
)";

// Hardware values of the fractional konstant selections (k/8 of 256, 8/8 = 255).
int konst_fraction(u32 sel) noexcept { return sel == 0 ? 255 : static_cast<int>(8 - sel) * 32; }

std::string_view int_reg_name(u32 reg) noexcept {
  static constexpr std::array names{"iprev"sv, "ireg0"sv, "ireg1"sv, "ireg2"sv};
  return names[reg];
}

// Integer color input. `full` = the D input (signed 11-bit); A/B/C see the low 8 bits.
std::string int_color_arg(GXTevColorArg arg, size_t stageIdx, const ShaderConfig& config, const TevStage& stage,
                          bool full) {
  const auto reg = [&](u32 r, bool alpha) {
    std::string v = alpha ? fmt::format("vec3i({}.a)", int_reg_name(r)) : fmt::format("{}.rgb", int_reg_name(r));
    return full ? v : fmt::format("({} & vec3i(255))", v);
  };
  switch (arg) {
  case GX_CC_CPREV:
    return reg(0, false);
  case GX_CC_APREV:
    return reg(0, true);
  case GX_CC_C0:
    return reg(1, false);
  case GX_CC_A0:
    return reg(1, true);
  case GX_CC_C1:
    return reg(2, false);
  case GX_CC_A1:
    return reg(2, true);
  case GX_CC_C2:
    return reg(3, false);
  case GX_CC_A2:
    return reg(3, true);
  case GX_CC_ONE:
    return "vec3i(255)";
  case GX_CC_HALF:
    return "vec3i(128)";
  case GX_CC_ZERO:
    return "vec3i(0)";
  case GX_CC_KONST:
    if (stage.kcSel <= GX_TEV_KCSEL_1_8) {
      return fmt::format("vec3i({})", konst_fraction(stage.kcSel));
    }
    [[fallthrough]];
  default:
    return fmt::format("itev_f2i3({})", color_arg_reg(arg, stageIdx, config, stage));
  }
}

std::string int_alpha_arg(GXTevAlphaArg arg, size_t stageIdx, const ShaderConfig& config, const TevStage& stage,
                          bool full) {
  const auto reg = [&](u32 r) {
    std::string v = fmt::format("{}.a", int_reg_name(r));
    return full ? v : fmt::format("({} & 255)", v);
  };
  switch (arg) {
  case GX_CA_APREV:
    return reg(0);
  case GX_CA_A0:
    return reg(1);
  case GX_CA_A1:
    return reg(2);
  case GX_CA_A2:
    return reg(3);
  case GX_CA_ZERO:
    return "0";
  case GX_CA_KONST:
    if (stage.kaSel <= GX_TEV_KASEL_1_8) {
      return fmt::format("{}", konst_fraction(stage.kaSel));
    }
    [[fallthrough]];
  default:
    return fmt::format("itev_f2i1({})", alpha_arg_reg(arg, stageIdx, config, stage));
  }
}

// Compare key of an 8-bit-per-channel color for the R8/GR16/BGR24 compare modes.
std::string int_cmp_key(GXTevOp op, std::string_view v) {
  switch (op) {
  case GX_TEV_COMP_R8_GT:
  case GX_TEV_COMP_R8_EQ:
    return fmt::format("({}).r", v);
  case GX_TEV_COMP_GR16_GT:
  case GX_TEV_COMP_GR16_EQ:
    return fmt::format("(({0}).g * 256 + ({0}).r)", v);
  default:
    return fmt::format("(({0}).b * 65536 + ({0}).g * 256 + ({0}).r)", v);
  }
}

bool tev_cmp_is_gt(GXTevOp op) noexcept {
  return op == GX_TEV_COMP_R8_GT || op == GX_TEV_COMP_GR16_GT || op == GX_TEV_COMP_BGR24_GT ||
         op == GX_TEV_COMP_RGB8_GT;
}

std::string build_int_tev_stage(size_t idx, const ShaderConfig& config, const TevStage& stage) {
  std::string out = fmt::format("\n    // TEV stage {} (integer)", idx);
  const auto bias = [](GXTevBias b) { return b == GX_TB_ADDHALF ? 128 : b == GX_TB_SUBHALF ? -128 : 0; };
  // Color
  {
    const auto& cp = stage.colorPass;
    const auto& op = stage.colorOp;
    const auto a = int_color_arg(cp.a, idx, config, stage, false);
    const auto b = int_color_arg(cp.b, idx, config, stage, false);
    const auto c = int_color_arg(cp.c, idx, config, stage, false);
    const auto d = int_color_arg(cp.d, idx, config, stage, true);
    std::string expr;
    if (op.op == GX_TEV_ADD || op.op == GX_TEV_SUB) {
      expr = fmt::format("itev_op3({}, {}, {}, {}, {}, {}, {}u, {})", a, b, c, d, op.op == GX_TEV_SUB, bias(op.bias),
                         underlying(op.scale), op.clamp);
    } else {
      std::string cond;
      const std::string_view rel = tev_cmp_is_gt(op.op) ? ">"sv : "=="sv;
      if (op.op == GX_TEV_COMP_RGB8_GT || op.op == GX_TEV_COMP_RGB8_EQ) {
        cond = fmt::format("({} {} {})", a, rel, b);
      } else {
        cond = fmt::format("vec3<bool>({} {} {})", int_cmp_key(op.op, a), rel, int_cmp_key(op.op, b));
      }
      expr = fmt::format("itev_clamp3({} + select(vec3i(0), {}, {}), {})", d, c, cond, op.clamp);
    }
    // The color and alpha units read their inputs before either writes its result (the alpha
    // unit's R8/GR16/BGR24 compares read the stage's color inputs, and color inputs may read
    // alpha registers), so both results go through temporaries and are stored together below.
    out += fmt::format("\n    let tevc{} = {};", idx, expr);
  }
  // Alpha
  {
    const auto& ap = stage.alphaPass;
    const auto& op = stage.alphaOp;
    const auto c = int_alpha_arg(ap.c, idx, config, stage, false);
    const auto d = int_alpha_arg(ap.d, idx, config, stage, true);
    std::string expr;
    if (op.op == GX_TEV_ADD || op.op == GX_TEV_SUB) {
      const auto a = int_alpha_arg(ap.a, idx, config, stage, false);
      const auto b = int_alpha_arg(ap.b, idx, config, stage, false);
      expr = fmt::format("itev_op1({}, {}, {}, {}, {}, {}, {}u, {})", a, b, c, d, op.op == GX_TEV_SUB, bias(op.bias),
                         underlying(op.scale), op.clamp);
    } else {
      const std::string_view rel = tev_cmp_is_gt(op.op) ? ">"sv : "=="sv;
      std::string cond;
      if (op.op == GX_TEV_COMP_A8_GT || op.op == GX_TEV_COMP_A8_EQ) {
        cond = fmt::format("({} {} {})", int_alpha_arg(ap.a, idx, config, stage, false), rel,
                           int_alpha_arg(ap.b, idx, config, stage, false));
      } else {
        // R8/GR16/BGR24 compare the stage's color inputs A and B, even in the alpha unit.
        const auto a = int_color_arg(stage.colorPass.a, idx, config, stage, false);
        const auto b = int_color_arg(stage.colorPass.b, idx, config, stage, false);
        cond = fmt::format("({} {} {})", int_cmp_key(op.op, a), rel, int_cmp_key(op.op, b));
        if (enh_is_shadow_stage(config, stage)) {
          out += enh_shadow_stage(idx, config, stage, int_cmp_key(op.op, a), cond);
          expr = fmt::format("itev_clamp1({} + i32(round(f32({}) * enh_sf{})), {})", d, c, idx, op.clamp);
        }
      }
      if (expr.empty()) {
        expr = fmt::format("itev_clamp1({} + select(0, {}, {}), {})", d, c, cond, op.clamp);
      }
    }
    out += fmt::format("\n    let teva{} = {};", idx, expr);
  }
  const auto colorReg = int_reg_name(stage.colorOp.outReg);
  out += fmt::format("\n    {0} = vec4i(tevc{1}, {0}.a);", colorReg, idx);
  out += fmt::format("\n    {}.a = teva{};", int_reg_name(stage.alphaOp.outReg), idx);
  return out;
}

// Quantize the pixel to the EFB pixel format the way the PE stores it (optionally with its 2x2
// ordered dither), then expand back to 8 bits as copies and the display read it.
std::string efb_quantize(const ShaderConfig& config) {
  const bool rgb565 = config.efbFmt == 2;
  // 0190 (GX_AURORA_ACCURACY_EFB_SMOOTH, immediate bit 16; bit 17 = noise): instead of the stored
  // 6/5-bit value, the average of what the PE stores over the sub-step positions of a smooth input:
  // with dither the mean of the dithered result is t = (c - (c >> bits)) / step levels (the dither
  // adds 0..step-1 uniformly), without dither the truncation's mean is t = (c - (step - 1) / 2) / step
  // for the 8-bit TEV output. The output is the widened level ramp (k << (8 - bits)) | (k >> (2 bits - 8))
  // interpolated at t: no steps, same mean brightness and tint as the quantized picture. Alpha (dst
  // alpha / blend factor) keeps its exact 6-bit value.
  std::string out = "\n    // EFB pixel format quantization"
                    "\n    if ((imm.current_pnmtx & 0x10000u) != 0u) {"
                    "\n      let sv = clamp(prev.rgb, vec3f(0.0), vec3f(1.0)) * 255.0;";
  out += fmt::format("\n      let slev = {};", rgb565 ? "vec3f(32.0, 64.0, 32.0)"sv : "vec3f(64.0)"sv);
  out += "\n      let sstep = vec3f(256.0) / slev;";
  if (config.dither) {
    out += "\n      let st = (sv - floor(sv / slev)) / sstep;";
  } else {
    out += "\n      let st = (sv - (sstep - 1.0) * 0.5) / sstep;";
  }
  out += "\n      let sk = clamp(floor(st), vec3f(0.0), slev - 1.0);"
         "\n      let sk1 = min(sk + 1.0, slev - 1.0);"
         "\n      let sdiv = slev * slev / 256.0;"
         "\n      var so = mix(sk * sstep + floor(sk / sdiv), sk1 * sstep + floor(sk1 / sdiv), clamp(st - sk, vec3f(0.0), vec3f(1.0)));"
         "\n      if ((imm.current_pnmtx & 0x20000u) != 0u) {"
         "\n        let sn = fract(52.9829189 * fract(dot(in.pos.xy, vec2f(0.06711056, 0.00583715))));"
         "\n        so = so + (sn - 0.5);"
         "\n      }";
  if (!rgb565 && !config.efbBlend) {
    out += "\n      let sa = u32(round(clamp(prev.a, 0.0, 1.0) * 255.0)) >> 2u;"
           "\n      prev = vec4f(so / 255.0, f32((sa << 2u) | (sa >> 4u)) / 255.0);";
  } else {
    out += "\n      prev = vec4f(so / 255.0, prev.a);";
  }
  out += "\n    } else {"
         "\n      let q8 = vec4i(round(clamp(prev, vec4f(0.0), vec4f(1.0)) * 255.0));"
         "\n      var rgb = q8.rgb;";
  // Per-channel bit depth: RGBA6 = 6/6/6, RGB565 = 5/6/5.
  const std::string_view bits = rgb565 ? "vec3u(5u, 6u, 5u)"sv : "vec3u(6u)"sv;
  out += fmt::format("\n      let bits = {};", bits);
  if (config.dither && hw_dither_4x4()) {
    // 4x4 ordered (Bayer) dither (0180): index B = 4 * B2(x, y) + B2(x >> 1, y >> 1) with
    // B2 = 0 2 / 3 1, in sixteenths of one output step, by EFB pixel. A channel that drops 2 bits
    // (RGBA6, RGB565's green) sees floor(B / 4) = the 2x2 matrix exactly; RGB565's 5-bit red and
    // blue drop 3 bits and see 8 levels of the 4x4 pattern.
    out += "\n      let dp = vec2u(in.pos.xy);"
           "\n      let d0 = dp & vec2u(1u);"
           "\n      let d1 = (dp >> vec2u(1u)) & vec2u(1u);"
           "\n      let dq = i32(4u * (((d0.x ^ d0.y) << 1u) + d0.y) + ((d1.x ^ d1.y) << 1u) + d1.y);"
           "\n      let step = vec3i(1) << (vec3u(8u) - bits);"
           "\n      rgb = rgb - (rgb >> bits) + (step * dq) / 16;";
  } else if (config.dither) {
    // 2x2 ordered (Bayer) dither: thresholds 0 2 / 3 1 in quarters of one output step.
    out += "\n      let dp = vec2u(in.pos.xy) & vec2u(1u);"
           "\n      let dq = i32(select(select(0u, 2u, dp.x == 1u), select(3u, 1u, dp.x == 1u), dp.y == 1u));"
           "\n      let step = vec3i(1) << (vec3u(8u) - bits);"
           "\n      rgb = rgb - (rgb >> bits) + (step * dq) / 4;";
  }
  out += "\n      let qn = rgb >> (vec3u(8u) - bits);"
         "\n      rgb = (qn << (vec3u(8u) - bits)) | (qn >> (bits + bits - vec3u(8u)));";
  if (!rgb565 && !config.efbBlend) {
    out += "\n      let qa = q8.a >> 2u;"
           "\n      prev = vec4f(vec4i(rgb, (qa << 2u) | (qa >> 4u))) / 255.0;";
  } else {
    out += "\n      prev = vec4f(vec3f(rgb) / 255.0, prev.a);";
  }
  out += "\n    }";
  return out;
}

absl::flat_hash_set<gfx::ShaderRef> s_seenShaders;
} // namespace

std::string build_shader_source(const ShaderConfig& config, DstAlphaMode dstAlphaMode,
                                uint32_t normalAttachment) noexcept {
  ZoneScoped;
  const auto hash = xxh3_hash(dstAlphaMode, xxh3_hash(normalAttachment, xxh3_hash(config)));
  const auto info = build_shader_info(config);
  if (EnableDebugPrints && !s_seenShaders.contains(hash)) {
    s_seenShaders.insert(hash);

    Log.info("Shader config (hash {:x}):", hash);
    {
      for (int i = 0; i < config.tevStageCount; ++i) {
        const auto& stage = config.tevStages[i];
        Log.info("  tevStages[{}]:", i);
        Log.info("    color_a: {}", TevColorArgNames[stage.colorPass.a]);
        Log.info("    color_b: {}", TevColorArgNames[stage.colorPass.b]);
        Log.info("    color_c: {}", TevColorArgNames[stage.colorPass.c]);
        Log.info("    color_d: {}", TevColorArgNames[stage.colorPass.d]);
        Log.info("    alpha_a: {}", TevAlphaArgNames[stage.alphaPass.a]);
        Log.info("    alpha_b: {}", TevAlphaArgNames[stage.alphaPass.b]);
        Log.info("    alpha_c: {}", TevAlphaArgNames[stage.alphaPass.c]);
        Log.info("    alpha_d: {}", TevAlphaArgNames[stage.alphaPass.d]);
        Log.info("    color_op_clamp: {}", stage.colorOp.clamp);
        Log.info("    color_op_op: {}", stage.colorOp.op);
        Log.info("    color_op_bias: {}", stage.colorOp.bias);
        Log.info("    color_op_scale: {}", stage.colorOp.scale);
        Log.info("    color_op_reg_id: {}", stage.colorOp.outReg);
        Log.info("    alpha_op_clamp: {}", stage.alphaOp.clamp);
        Log.info("    alpha_op_op: {}", stage.alphaOp.op);
        Log.info("    alpha_op_bias: {}", stage.alphaOp.bias);
        Log.info("    alpha_op_scale: {}", stage.alphaOp.scale);
        Log.info("    alpha_op_reg_id: {}", stage.alphaOp.outReg);
        Log.info("    kc_sel: {}", stage.kcSel);
        Log.info("    ka_sel: {}", stage.kaSel);
        Log.info("    texCoordId: {}", stage.texCoordId);
        Log.info("    texMapId: {}", stage.texMapId);
        Log.info("    channelId: {}", stage.channelId);
        Log.info("    tevSwapRas: {}", stage.tevSwapRas);
        Log.info("    tevSwapTex: {}", stage.tevSwapTex);
        Log.info("    indTexStage: {}", stage.indTexStage);
        Log.info("    indTexFormat: {}", stage.indTexFormat);
        Log.info("    indTexBiasSel: {}", stage.indTexBiasSel);
        Log.info("    indTexAlphaSel: {}", stage.indTexAlphaSel);
        Log.info("    indTexMtxId: {}", stage.indTexMtxId);
        Log.info("    indTexWrapS: {}", stage.indTexWrapS);
        Log.info("    indTexWrapT: {}", stage.indTexWrapT);
        Log.info("    indTexUseOrigLOD: {}", stage.indTexUseOrigLOD);
        Log.info("    indTexAddPrev: {}", stage.indTexAddPrev);
      }
      Log.info("  numIndStages: {}", config.numIndStages);
      for (u32 i = 0; i < config.numIndStages; ++i) {
        const auto& stage = config.indStages[i];
        Log.info("  indStages[{}]: texCoordId {} texMapId {} scaleS {} scaleT {}", i, stage.texCoordId, stage.texMapId,
                 stage.scaleS, stage.scaleT);
      }
      for (int i = 0; i < config.colorChannels.size(); ++i) {
        const auto& chan = config.colorChannels[i];
        Log.info("  colorChannels[{}]: enabled {} mat {} amb {}", static_cast<GXChannelID>(i), chan.lightingEnabled,
                 chan.matSrc, chan.ambSrc);
      }
      for (int i = 0; i < config.tcgs.size(); ++i) {
        const auto& tcg = config.tcgs[i];
        if (tcg.src != GX_MAX_TEXGENSRC) {
          Log.info("  tcg[{}]: src {} mtx {} post {} type {} norm {}", i, tcg.src, tcg.mtx, tcg.postMtx, tcg.type,
                   tcg.normalize);
        }
      }
      Log.info("  alphaCompare: comp0 {} ref0 {} op {} comp1 {} ref1 {}", config.alphaCompare.comp0,
               config.alphaCompare.ref0, config.alphaCompare.op, config.alphaCompare.comp1, config.alphaCompare.ref1);
      Log.info("  fogType: {} logicOut: {}", unsigned(config.fogType), unsigned(config.logicOut));
      Log.info("  fogRangeEnabled: {}", config.fogRangeEnabled);
    }
  }

  std::string uniformPre;
  std::string uniBufAttrs;
  std::string texBindings;
  std::string vtxOutAttrs;
  std::string vtxInAttrs;
  std::string vtxXfrAttrsPre;
  std::string vtxXfrAttrs;
  size_t vtxOutIdx = 0;

  // Load points for line/point expansion
  std::string_view vidxAttr = "vidx"sv;
  if (config.lightDepth && config.lineMode == 0) {
    vtxInAttrs += ",\n    @builtin(instance_index) iidx: u32";
  }
  if (config.lineMode != 0) {
    vtxInAttrs += ",\n    @builtin(instance_index) iidx: u32";
    uniBufAttrs +=
        "\n    line_width: f32,"
        "\n    line_aspect_y: f32,"
        "\n    line_tex_offset: f32,"
        "\n    line_texcoord_mask: u32,";
    if (config.lineMode == 3) {
      // GX_POINTS: each instance = one vertex, expand to quad
      vtxXfrAttrsPre += fmt::format(
          "\n    let in_vidx = iidx;"
          "\n    let in_pos = {};"
          "\n    let in_pnmtxidx = {};"
          "\n    let mv_pos = vec4f(in_pos, 1.0) * ubuf.postex_mtx[in_pnmtxidx];",
          attr_load(config, GX_VA_POS, "in_vidx"sv), attr_load(config, GX_VA_PNMTXIDX, "in_vidx"sv));
    } else {
      // GX_LINES / GX_LINESTRIP: each instance = two vertices, expand to quad
      vtxXfrAttrsPre += fmt::format(
          "\n    let use_b = vidx >= 2u;"
          "\n    let vidx_a = iidx * {}u;"
          "\n    let vidx_b = vidx_a + 1u;"
          "\n    let in_vidx = select(vidx_a, vidx_b, use_b);"
          "\n    let pos_a = {};"
          "\n    let pos_b = {};"
          "\n    let in_pos = select(pos_a, pos_b, use_b);"
          "\n    let pnmtxidx_a = {};"
          "\n    let pnmtxidx_b = {};"
          "\n    let in_pnmtxidx = select(pnmtxidx_a, pnmtxidx_b, use_b);"
          "\n    let mv_pos_a = vec4f(pos_a, 1.0) * ubuf.postex_mtx[pnmtxidx_a];"
          "\n    let mv_pos_b = vec4f(pos_b, 1.0) * ubuf.postex_mtx[pnmtxidx_b];"
          "\n    let mv_pos = select(mv_pos_a, mv_pos_b, use_b);",
          config.lineMode == 1 ? 2 : 1, attr_load(config, GX_VA_POS, "vidx_a"sv),
          attr_load(config, GX_VA_POS, "vidx_b"sv), attr_load(config, GX_VA_PNMTXIDX, "vidx_a"sv),
          attr_load(config, GX_VA_PNMTXIDX, "vidx_b"sv));
    }
    vidxAttr = "in_vidx"sv;
  } else if (config.attrs[GX_VA_PNMTXIDX].attrType == GX_NONE) {
    vtxXfrAttrsPre += "\n    let in_pnmtxidx = imm.current_pnmtx & 0xFFu;";
  }

  // Load vertex attributes
  for (GXAttr attr = GX_VA_PNMTXIDX; attr <= GX_VA_TEX7; attr = static_cast<GXAttr>(attr + 1)) {
    const auto attrType = config.attrs[attr].attrType;
    if (attrType == GX_NONE) {
      continue;
    }
    // in_pnmtxidx and in_pos written above for line mode
    if ((attr != GX_VA_PNMTXIDX && attr != GX_VA_POS) || config.lineMode == 0) {
      vtxXfrAttrsPre += fmt::format("\n    let {} = {};", vtx_attr(config, attr), attr_load(config, attr, vidxAttr));
    }
  }
  bool needsBinrm = false;
  bool needsTangent = false;
  for (int i = 0; i < info.sampledTexCoords.size(); ++i) {
    if (!info.sampledTexCoords.test(i)) {
      continue;
    }
    const bool emboss = is_emboss_texgen(config.tcgs[i].type);
    needsBinrm = needsBinrm || config.tcgs[i].src == GX_TG_BINRM || emboss;
    needsTangent = needsTangent || config.tcgs[i].src == GX_TG_TANGENT || emboss;
  }
  if (needsBinrm) {
    vtxXfrAttrsPre += fmt::format("\n    let {} = {};", nbt_slice_local(NbtSlice::B),
                                  attr_load_nbt_slice(config, NbtSlice::B, vidxAttr));
  }
  if (needsTangent) {
    vtxXfrAttrsPre += fmt::format("\n    let {} = {};", nbt_slice_local(NbtSlice::T),
                                  attr_load_nbt_slice(config, NbtSlice::T, vidxAttr));
  }

  if (config.lineMode == 0) {
    vtxXfrAttrsPre += fmt::format(
        "\n    let mv_pos = vec4f({}, 1.0) * ubuf.postex_mtx[in_pnmtxidx];"
        "\n    out.pos = vec4f(mv_pos, 1.0) * ubuf.proj;",
        vtx_attr(config, GX_VA_POS));
    if (config.lightDepth) {
      // 0028 shadow-map caster: instance = (light, cube face); the record holds the view -> atlas
      // clip matrix (rows) and the face's tile rectangle in atlas pixels.
      vtxXfrAttrsPre +=
          "\n    let enh_lt = imm._pad + iidx * 24u;"
          "\n    let enh_lp = vec4f(mv_pos, 1.0);"
          "\n    out.pos = vec4f(dot(enh_lp, enh_ldv(enh_lt)), dot(enh_lp, enh_ldv(enh_lt + 4u)), dot(enh_lp, enh_ldv(enh_lt + 8u)),"
          "\n                    dot(enh_lp, enh_ldv(enh_lt + 12u)));"
          "";
      if (webgpu::g_clipDistancesSupported) {
        // Clip each instance to its face's tile (atlas NDC rectangle): no rasterization outside it.
        vtxXfrAttrsPre +=
            "\n    let enh_tb = enh_ldv(enh_lt + 16u);"
            "\n    out.enh_clip[0] = out.pos.x - enh_tb.x * out.pos.w;"
            "\n    out.enh_clip[1] = enh_tb.z * out.pos.w - out.pos.x;"
            "\n    out.enh_clip[2] = out.pos.y - enh_tb.y * out.pos.w;"
            "\n    out.enh_clip[3] = enh_tb.w * out.pos.w - out.pos.y;";
        vtxOutAttrs += "\n    @builtin(clip_distances) enh_clip: array<f32, 4>,";
      } else {
        vtxXfrAttrsPre += "\n    out.enh_tile = enh_ldv(enh_lt + 20u);";
        vtxOutAttrs += fmt::format("\n    @location({}) @interpolate(flat) enh_tile: vec4f,", vtxOutIdx++);
      }
      uniformPre += "\nfn enh_ldv(i: u32) -> vec4f {\n  return vec4f(bitcast<f32>(abuf[i]), bitcast<f32>(abuf[i + 1u]), "
                    "bitcast<f32>(abuf[i + 2u]), bitcast<f32>(abuf[i + 3u]));\n}\n";
    }
  } else if (config.lineMode == 3) {
    // GX_POINTS: expand single vertex to axis-aligned screen-space square
    vtxXfrAttrsPre +=
        "\n    let clip = vec4f(mv_pos, 1.0) * ubuf.proj;"
        "\n    let viewport_scale = ubuf.render_viewport_size / max(ubuf.logical_viewport_size, vec2f(1.0));"
        "\n    let point_size = ubuf.line_width * min(viewport_scale.x, viewport_scale.y);"
        "\n    let x_sign = select(-1.0, 1.0, (vidx & 1u) != 0u);"
        "\n    let y_sign = select(-1.0, 1.0, vidx >= 2u);"
        "\n    let offset_px = vec2f(x_sign, y_sign) * (point_size / 2.0);"
        "\n    let offset_ndc = (offset_px * 2.0) / ubuf.render_viewport_size;"
        "\n    out.pos = vec4f(clip.xy + offset_ndc * clip.w, clip.zw);";
  } else {
    // GX_LINES / GX_LINESTRIP: expand line segment perpendicular to direction
    vtxXfrAttrsPre +=
        "\n    let clip_a = vec4f(mv_pos_a, 1.0) * ubuf.proj;"
        "\n    let clip_b = vec4f(mv_pos_b, 1.0) * ubuf.proj;"
        "\n    let ndc_a = clip_a.xy / clip_a.w;"
        "\n    let ndc_b = clip_b.xy / clip_b.w;"
        "\n    let viewport_scale = ubuf.render_viewport_size / max(ubuf.logical_viewport_size, vec2f(1.0));"
        "\n    let delta_px = (ndc_b - ndc_a) / 2.0 * ubuf.render_viewport_size;"
        "\n    let dir_px = select(vec2f(1.0, 0.0), normalize(delta_px), dot(delta_px, delta_px) > 1e-10);"
        "\n    let perp_dir = vec2f(-dir_px.y, dir_px.x);"
        // The GX rasterizer widens a line along its minor axis (vertical for a mostly
        // horizontal line, horizontal otherwise): the ends are horizontal/vertical cuts and a
        // sloped line is thinner than its width measured across it.
        "\n    let perp_px = select(vec2f(sign(perp_dir.x), 0.0), vec2f(0.0, sign(perp_dir.y)),"
        " abs(delta_px.x) >= abs(delta_px.y));"
        "\n    let line_width = ubuf.line_width * min(viewport_scale.x, viewport_scale.y);"
        "\n    let offset_px = perp_px * (line_width / 2.0) * select(-1.0, 1.0, (vidx & 1u) != 0u);"
        "\n    let offset_ndc = (offset_px * 2.0) / ubuf.render_viewport_size;"
        "\n    let clip_base = select(clip_a, clip_b, use_b);"
        "\n    out.pos = vec4f(clip_base.xy + offset_ndc * clip_base.w, clip_base.zw);";
  }
  vtxXfrAttrsPre += fmt::format(
      "\n    let nrm_tmp = vec4f({}, 0.0) * ubuf.nrm_mtx[in_pnmtxidx];"
      "\n    let mv_nrm = select(nrm_tmp, normalize(nrm_tmp), dot(nrm_tmp, nrm_tmp) > 1e-10);",
      vtx_attr(config, GX_VA_NRM));
  if constexpr (EnableNormalVisualization) {
    vtxOutAttrs += fmt::format("\n    @location({}) nrm: vec3f,", vtxOutIdx++);
    vtxXfrAttrsPre += "\n    out.nrm = mv_nrm;";
  }
  const bool useNormalTarget = normalAttachment != UINT32_MAX && config.attrs[GX_VA_NRM].attrType != GX_NONE;
  if (useNormalTarget && !(UsePerPixelLighting && info.lightingEnabled)) {
    vtxOutAttrs += fmt::format("\n    @location({}) mv_nrm: vec3f,", vtxOutIdx++);
    vtxXfrAttrsPre += "\n    out.mv_nrm = mv_nrm;";
  }

  if (config.wireframe) {
    uniBufAttrs += "\n    wire_r: f32,\n    wire_g: f32,\n    wire_b: f32,\n    wire_a: f32,";
  }
  uniBufAttrs += "\n    proj: mat4x4f,";
  uniBufAttrs += fmt::format("\n    postex_mtx: array<mat3x4f, {}>,", MaxPnMtx + MaxTexMtx);
  uniBufAttrs += fmt::format("\n    nrm_mtx: array<mat3x4f, {}>,", MaxPnMtx);
  std::string fragmentFnPre;
  std::string fragmentFn;

  static std::array regName{"prev"sv, "tevreg0"sv, "tevreg1"sv, "tevreg2"sv};
  std::array<bool, MaxTevRegs> colorNormalized{};
  std::array<bool, MaxTevRegs> alphaNormalized{};
  for (u32 idx = 0; idx < config.tevStageCount; ++idx) {
    const auto& stage = config.tevStages[idx];
    if (config.intTev) {
      fragmentFn += build_int_tev_stage(idx, config, stage);
      continue;
    }
    {
      const auto color_arg = [&](GXTevColorArg arg) {
        auto value = color_arg_reg(arg, idx, config, stage);
        if (tev_color_arg_is_normalized(arg, colorNormalized, alphaNormalized)) {
          return fmt::format("vec3f({})", value);
        }
        return fmt::format("tev_overflow_vec3f({})", value);
      };
      std::string op = tev_color_op(stage.colorOp.op, tev_bias(stage.colorOp.bias), tev_scale(stage.colorOp.scale),
                                    stage.colorOp.clamp, color_arg(stage.colorPass.a), color_arg(stage.colorPass.b),
                                    color_arg(stage.colorPass.c), color_arg_reg(stage.colorPass.d, idx, config, stage));
      // Stored after the alpha unit has read its inputs (see build_int_tev_stage).
      fragmentFn += fmt::format("\n    // TEV stage {1}\n    let tevc{1} = {0};", op, idx);
      colorNormalized[stage.colorOp.outReg] = stage.colorOp.clamp;
    }
    {
      const auto alpha_arg = [&](GXTevAlphaArg arg) {
        auto value = alpha_arg_reg(arg, idx, config, stage);
        if (tev_alpha_arg_is_normalized(arg, alphaNormalized)) {
          return value;
        }
        return fmt::format("tev_overflow_f32({})", value);
      };
      std::string_view outReg = regName[stage.alphaOp.outReg];
      // The R8/GR16/BGR24 compare modes compare the stage's *color* inputs A and B even
      // in the alpha unit; only the last mode (A8) compares the alpha inputs.
      const auto aop = stage.alphaOp.op;
      const bool colorCompare = aop >= GX_TEV_COMP_R8_GT && aop <= GX_TEV_COMP_BGR24_EQ;
      const auto cmp_arg = [&](GXTevColorArg carg, GXTevAlphaArg aarg) {
        if (!colorCompare) return alpha_arg(aarg);
        auto value = color_arg_reg(carg, idx, config, stage);
        return fmt::format("vec3f({})", value);
      };
      std::string op;
      if (enh_is_shadow_stage(config, stage)) {
        const auto ka = fmt::format("round(dot({}.rg * 255.0, vec2(1.0, 256.0)))", cmp_arg(stage.colorPass.a, stage.alphaPass.a));
        const auto kb = fmt::format("round(dot({}.rg * 255.0, vec2(1.0, 256.0)))", cmp_arg(stage.colorPass.b, stage.alphaPass.b));
        fragmentFn += enh_shadow_stage(idx, config, stage, ka, fmt::format("{} > {}", ka, kb));
        const auto expr = fmt::format("{} * enh_sf{} + {}", alpha_arg(stage.alphaPass.c), idx,
                                      alpha_arg_reg(stage.alphaPass.d, idx, config, stage));
        op = stage.alphaOp.clamp ? fmt::format("clamp({}, 0.0, 1.0)", expr) : fmt::format("clamp({}, -4.0, 4.0)", expr);
      } else {
        op = tev_alpha_op(stage.alphaOp.op, tev_bias(stage.alphaOp.bias), tev_scale(stage.alphaOp.scale),
                          stage.alphaOp.clamp, cmp_arg(stage.colorPass.a, stage.alphaPass.a),
                          cmp_arg(stage.colorPass.b, stage.alphaPass.b), alpha_arg(stage.alphaPass.c),
                          alpha_arg_reg(stage.alphaPass.d, idx, config, stage));
      }
      fragmentFn += fmt::format("\n    let teva{0} = {1};", idx, op);
      fragmentFn += fmt::format("\n    {0} = vec4f(tevc{1}, {0}.a);", regName[stage.colorOp.outReg], idx);
      fragmentFn += fmt::format("\n    {0}.a = teva{1};", outReg, idx);
      alphaNormalized[stage.alphaOp.outReg] = stage.alphaOp.clamp;
    }
  }

  const auto& lastStage = config.tevStages[config.tevStageCount - 1];
  const bool prevColorNormalized = colorNormalized[lastStage.colorOp.outReg];
  const bool prevAlphaNormalized = alphaNormalized[lastStage.alphaOp.outReg];
  if (config.intTev) {
    if (lastStage.colorOp.outReg != 0) {
      fragmentFn += fmt::format("\n    iprev = vec4i({0}.rgb, iprev.a);", int_reg_name(lastStage.colorOp.outReg));
    }
    if (lastStage.alphaOp.outReg != 0) {
      fragmentFn += fmt::format("\n    iprev.a = {0}.a;", int_reg_name(lastStage.alphaOp.outReg));
    }
    // The PE takes the low 8 bits of the last stage's output.
    fragmentFn += "\n    prev = vec4f(iprev & vec4i(255)) / 255.0;";
    uniformPre += hw_tev_div2_trunc() ? "\nconst itev_div2_trunc = true;"sv : "\nconst itev_div2_trunc = false;"sv;
    uniformPre += IntTevHelpers;
    if (info.loadsTevReg.test(0)) {
      fragmentFnPre += "\n    var iprev = vec4i(round(ubuf.tevprev * 255.0));";
    } else {
      fragmentFnPre += "\n    var iprev = vec4i(0);";
    }
    for (int i = 1; i < info.loadsTevReg.size(); ++i) {
      if (info.loadsTevReg.test(i)) {
        fragmentFnPre += fmt::format("\n    var ireg{0} = vec4i(round(ubuf.tevreg{0} * 255.0));", i - 1);
      } else if (info.writesTevReg.test(i)) {
        fragmentFnPre += fmt::format("\n    var ireg{0} = vec4i(0);", i - 1);
      }
    }
  } else {
    if (lastStage.colorOp.outReg != 0) {
      fragmentFn += fmt::format("\n    prev = vec4f({0}.rgb, prev.a);", regName[lastStage.colorOp.outReg]);
    }
    if (lastStage.alphaOp.outReg != 0) {
      fragmentFn += fmt::format("\n    prev.a = {0}.a;", regName[lastStage.alphaOp.outReg]);
    }
  }

  if (info.loadsTevReg.test(0)) {
    uniBufAttrs += "\n    tevprev: vec4f,";
    fragmentFnPre += "\n    var prev = ubuf.tevprev;";
  } else {
    fragmentFnPre += "\n    var prev: vec4f;";
  }
  for (int i = 1 /* Skip TEVPREV */; i < info.loadsTevReg.size(); ++i) {
    if (info.loadsTevReg.test(i)) {
      uniBufAttrs += fmt::format("\n    tevreg{}: vec4f,", i - 1);
      fragmentFnPre += fmt::format("\n    var tevreg{0} = ubuf.tevreg{0};", i - 1);
    } else if (info.writesTevReg.test(i)) {
      fragmentFnPre += fmt::format("\n    var tevreg{0}: vec4f;", i - 1);
    }
  }

  if (info.lightingEnabled) {
    uniBufAttrs += fmt::format(FMT_STRING(R"""(
    lights: array<Light, {}>,
    lightState0: u32,
    lightState1: u32,
    lightState0a: u32,
    lightState1a: u32,)"""),
                               GX::MaxLights);
    uniformPre +=
        "\n"
        "struct Light {\n"
        "    pos: vec3f,\n"
        "    dir: vec3f,\n"
        "    color: vec4f,\n"
        "    cos_att: vec3f,\n"
        "    dist_att: vec3f,\n"
        "};";
    if (UsePerPixelLighting) {
      vtxOutAttrs += fmt::format("\n    @location({}) mv_pos: vec3f,", vtxOutIdx++);
      vtxOutAttrs += fmt::format("\n    @location({}) mv_nrm: vec3f,", vtxOutIdx++);
      vtxXfrAttrs += fmt::format(FMT_STRING(R"""(
    out.mv_pos = mv_pos;
    out.mv_nrm = mv_nrm;)"""));
    }
  }

  // Enhanced lighting variant: view-space position/normal for per-pixel work.
  const bool enh = config.enhClass != 0 && !config.lightDepth && !config.depthOnly;
  if (enh) {
    vtxOutAttrs += fmt::format("\n    @location({}) enh_pos: vec3f,", vtxOutIdx++);
    vtxXfrAttrs += "\n    out.enh_pos = mv_pos;";
    fragmentFnPre += "\n    enh_fz = in.pos.z;"
                     "\n    enh_dz = fwidth(in.pos.z);";
    if (config.attrs[GX_VA_NRM].attrType != GX_NONE) {
      vtxOutAttrs += fmt::format("\n    @location({}) enh_nrm: vec3f,", vtxOutIdx++);
      vtxXfrAttrs += "\n    out.enh_nrm = mv_nrm;";
      fragmentFnPre += "\n    let enh_nl = dot(in.enh_nrm, in.enh_nrm);"
                       "\n    let enh_n = select(vec3f(0.0, 0.0, 1.0), in.enh_nrm * inverseSqrt(max(enh_nl, 1e-20)), enh_nl > 1e-10);";
    } else {
      // No normals: the face normal from the position derivatives, facing the camera.
      fragmentFnPre += "\n    let enh_c = cross(dpdx(in.enh_pos), dpdy(in.enh_pos));"
                       "\n    let enh_cl = dot(enh_c, enh_c);"
                       "\n    var enh_n = select(vec3f(0.0, 0.0, 1.0), enh_c * inverseSqrt(max(enh_cl, 1e-30)), enh_cl > 1e-20);"
                       "\n    enh_n = select(enh_n, -enh_n, dot(enh_n, in.enh_pos) > 0.0);";
    }
    fragmentFnPre += fmt::format("\n    var enh_xl = vec3f(0.0);"
                                 "\n    if ((ubuf.enh_flags & 2u) != 0u) {{ enh_xl = enh_extra_lights(in.enh_pos, enh_n, {}, in.pos.xy); }}"
                                 // Light shares of color channel 0 for the shadow darkening (enh_shadow_scale):
                                 // lit = the channel's GX lighting result, amb = its ambient part, xl = extra lights.
                                 "\n    var enh_s_lit = -1.0;"
                                 "\n    var enh_s_amb = 0.0;"
                                 "\n    var enh_s_xl = 0.0;",
                                 config.enhClass == 2 ? "true"sv : "false"sv);
    uniformPre += EnhHelpers;
    uniformPre += gfx::BeamProfileWgsl;
  }
  // Color channels feeding a color texgen (GX_TG_SRTG: the flashlight beam's lookup) keep
  // their exact lighting result: extra lights would move the lookup.
  std::bitset<2> srtgChannels;
  for (int i = 0; i < info.sampledTexCoords.size(); ++i) {
    if (info.sampledTexCoords.test(i) && config.tcgs[i].type == GX_TG_SRTG) {
      srtgChannels.set(config.tcgs[i].src == GX_TG_COLOR1 ? 1 : 0);
    }
  }

  for (int i = 0; i < info.sampledColorChannels.size(); ++i) {
    if (!info.sampledColorChannels.test(i)) {
      continue;
    }

    const auto& cc = config.colorChannels[i];
    const auto& cca = config.colorChannels[i + GX_ALPHA0];
    if (enh && ((cc.lightingEnabled && cc.ambSrc == GX_SRC_VTX) || cc.matSrc == GX_SRC_VTX ||
                (cca.lightingEnabled && cca.ambSrc == GX_SRC_VTX) || cca.matSrc == GX_SRC_VTX)) {
      vtxOutAttrs += fmt::format("\n    @location({}) enh_clr{}: vec4f,", vtxOutIdx++, i);
      vtxXfrAttrs += fmt::format("\n    out.enh_clr{} = {};", i, vtx_attr(config, static_cast<GXAttr>(GX_VA_CLR0 + i)));
    }
    if (cc.lightingEnabled && cc.ambSrc == GX_SRC_REG) {
      uniBufAttrs += fmt::format("\n    cc{0}_amb: vec4f,", i);
    }
    if (cc.matSrc == GX_SRC_REG) {
      uniBufAttrs += fmt::format("\n    cc{0}_mat: vec4f,", i);
    }
    if (cca.lightingEnabled && cca.ambSrc == GX_SRC_REG) {
      uniBufAttrs += fmt::format("\n    cc{0}a_amb: vec4f,", i);
    }
    if (cca.matSrc == GX_SRC_REG) {
      uniBufAttrs += fmt::format("\n    cc{0}a_mat: vec4f,", i);
    }

    // Output vertex color if necessary
    if (UsePerPixelLighting) {
      if ((cc.lightingEnabled && cc.ambSrc == GX_SRC_VTX) || cc.matSrc == GX_SRC_VTX ||
          (cca.lightingEnabled && cca.ambSrc == GX_SRC_VTX) || cca.matSrc == GX_SRC_VTX) {
        vtxOutAttrs += fmt::format("\n    @location({}) clr{}: vec4f,", vtxOutIdx++, i);
        vtxXfrAttrs += fmt::format("\n    out.clr{} = {};", i, vtx_attr(config, static_cast<GXAttr>(GX_VA_CLR0 + i)));
      }
    }

    if (UsePerPixelLighting) {
      fragmentFnPre += fmt::format("\n    var rast{}: vec4f;", i);
      fragmentFnPre += lighting_func(config, cc, i, false);
      fragmentFnPre += lighting_func(config, cca, i, true);
    } else {
      vtxOutAttrs += fmt::format("\n    @location({}) cc{}: vec4f,", vtxOutIdx++, i);
      vtxXfrAttrs += lighting_func(config, cc, i, false);
      vtxXfrAttrs += lighting_func(config, cca, i, true);
      if (config.intTev) {
        // The transform unit hands the rasterizer 8-bit vertex colors.
        vtxXfrAttrs += fmt::format("\n    out.cc{0} = round(clamp(out.cc{0}, vec4f(0.0), vec4f(1.0)) * 255.0) / 255.0;", i);
      }
      fragmentFnPre += fmt::format("\n    var rast{0} = in.cc{0};", i);
      // Ghost-pass draws (enhClass 2) keep the GX vertex lighting: the ghost layer is composited
      // from its own copy, and invisible ghosts must stay black there.
      if (enh && config.enhClass == 1 && info.lightingEnabled && (cc.lightingEnabled || cca.lightingEnabled) &&
          !srtgChannels.test(i)) {
        const bool spot = (cc.lightingEnabled && cc.attnFn == GX_AF_SPOT) || (cca.lightingEnabled && cca.attnFn == GX_AF_SPOT);
        fragmentFnPre += fmt::format("\n    if ((ubuf.enh_flags & {}u) != 0u) {{", spot ? 9 : 1);
        fragmentFnPre += enh_lighting_func(config, cc, i, false);
        fragmentFnPre += enh_lighting_func(config, cca, i, true);
        fragmentFnPre += "\n    }";
      }
      if (enh && i == 0 && cc.lightingEnabled && !srtgChannels.test(i)) {
        const std::string amb = cc.ambSrc == GX_SRC_REG ? fmt::format("{} * ubuf.cc0_amb.rgb", enh_mat_src(cc, 0))
                                                        : std::string{"vec3f(0.0)"};
        fragmentFnPre += fmt::format(
            "\n    enh_s_lit = dot(rast0.rgb, vec3f(0.299, 0.587, 0.114));"
            "\n    enh_s_amb = dot(clamp({}, vec3f(0.0), vec3f(1.0)), vec3f(0.299, 0.587, 0.114));"
            "\n    if ((ubuf.enh_flags & 2u) != 0u) {{ enh_s_xl = dot({} * enh_xl, vec3f(0.299, 0.587, 0.114)); }}",
            amb, enh_mat_src(cc, 0));
      }
      if (enh && cc.lightingEnabled && cc.diffFn != GX_DF_NONE && !srtgChannels.test(i)) {
        // Extra lights join the channel's lighting sum (approximately: mat * extra added after
        // the clamp), so the game's material and TEV decide how the surface responds.
        fragmentFnPre += fmt::format(
            "\n    if ((ubuf.enh_flags & 2u) != 0u) {{"
            "\n      rast{0} = vec4f(clamp(rast{0}.rgb + {1} * enh_xl, vec3f(0.0), vec3f(1.0)), rast{0}.a);"
            "\n    }}",
            i, enh_mat_src(cc, i));
      }
      if (enh && config.intTev) {
        fragmentFnPre += fmt::format("\n    rast{0} = round(clamp(rast{0}, vec4f(0.0), vec4f(1.0)) * 255.0) / 255.0;", i);
      }
    }
  }
  for (int i = 0; i < info.sampledKColors.size(); ++i) {
    if (info.sampledKColors.test(i)) {
      uniBufAttrs += fmt::format("\n    kcolor{}: vec4f,", i);
    }
  }
  for (int i = 0; i < info.sampledTexCoords.size(); ++i) {
    if (!info.sampledTexCoords.test(i)) {
      continue;
    }
    const auto& tcg = config.tcgs[i];
    if (tcg.type == GX_TG_MTX3x4) {
      vtxOutAttrs += fmt::format("\n    @location({}) tex{}_uvw: vec3f,", vtxOutIdx++, i);
    } else {
      vtxOutAttrs += fmt::format("\n    @location({}) tex{}_uv: vec2f,", vtxOutIdx++, i);
    }
    if (is_emboss_texgen(tcg.type)) {
      // Emboss bump: offset the source texcoord by the light projected onto tangent/binormal
      const u32 lightIdx = tcg.type - GX_TG_BUMP0;
      vtxXfrAttrs += fmt::format(
          "\n    let bump_ldir{0} = normalize(ubuf.lights[{1}].pos - mv_pos);"
          "\n    let bump_tan{0} = vec4f(in_tangent, 0.0) * ubuf.nrm_mtx[in_pnmtxidx];"
          "\n    let bump_bin{0} = vec4f(in_binrm, 0.0) * ubuf.nrm_mtx[in_pnmtxidx];"
          "\n    out.tex{0}_uv = tc{2}_proj.xy + vec2f(dot(bump_ldir{0}, bump_tan{0}), dot(bump_ldir{0}, "
          "bump_bin{0}));",
          i, lightIdx, tcg.embossSrc);
      fragmentFnPre += fmt::format("\n    var tex{0}_uv = in.tex{0}_uv.xy;", i);
      continue;
    }
    if (tcg.type == GX_TG_SRTG) {
      // Color texgen (XF texgen type 2/3, GX_TG_SRTG): s,t = R,G of the color channel's
      // *lighting result* (COLOR0 or COLOR1), not the raw vertex color. Used for lookups indexed
      // by light intensity (e.g. Luigi's Mansion's flashlight cone, which samples a gradient with
      // the output of a spot light). The texture matrix, post-transform matrix and normalization
      // only apply to regular texgens.
      const u32 chan = tcg.src == GX_TG_COLOR1 ? 1 : 0;
      if (UsePerPixelLighting) {
        vtxXfrAttrs += fmt::format("\n    out.tex{0}_uv = vec2f(0.0);", i);
        fragmentFnPre += fmt::format("\n    var tex{0}_uv = rast{1}.rg;", i, chan);
        continue;
      }
      vtxXfrAttrs += fmt::format("\n    var tc{0}_proj = vec3f(out.cc{1}.rg, 1.0);", i, chan);
    } else if (tcg.src >= GX_TG_TEX0 && tcg.src <= GX_TG_TEX7) {
      vtxXfrAttrs += fmt::format("\n    var tc{} = vec4f({}, 1.0, 1.0);", i,
                                 vtx_attr(config, GXAttr(GX_VA_TEX0 + (tcg.src - GX_TG_TEX0))));
    } else if (tcg.src == GX_TG_POS) {
      vtxXfrAttrs += fmt::format("\n    var tc{} = vec4f({}, 1.0);", i, vtx_attr(config, GX_VA_POS));
    } else if (tcg.src == GX_TG_NRM) {
      vtxXfrAttrs += fmt::format("\n    var tc{} = vec4f({}, 1.0);", i, vtx_attr(config, GX_VA_NRM));
    } else if (tcg.src == GX_TG_COLOR0) {
      vtxXfrAttrs += fmt::format("\n    var tc{} = {};", i, vtx_attr(config, GX_VA_CLR0));
    } else if (tcg.src == GX_TG_COLOR1) {
      vtxXfrAttrs += fmt::format("\n    var tc{} = {};", i, vtx_attr(config, GX_VA_CLR1));
    } else if (tcg.src == GX_TG_BINRM) {
      vtxXfrAttrs += fmt::format("\n    var tc{} = vec4f({}, 1.0);", i, nbt_slice_local(NbtSlice::B));
    } else if (tcg.src == GX_TG_TANGENT) {
      vtxXfrAttrs += fmt::format("\n    var tc{} = vec4f({}, 1.0);", i, nbt_slice_local(NbtSlice::T));
    } else if (tcg.src == GX_MAX_TEXGENSRC) {
      // A TEV stage samples a texcoord that has no texgen (beyond numTexGens). Hardware yields
      // undefined coordinates rather than failing; use (0, 0) and warn once.
      static bool sWarned = false;
      if (!sWarned) {
        sWarned = true;
        Log.warn("TEV samples texcoord {} without a texgen; using (0, 0)", i);
      }
      vtxXfrAttrs += fmt::format("\n    var tc{} = vec4f(0.0, 0.0, 1.0, 1.0);", i);
    } else
      UNLIKELY FATAL("unhandled tcg src {}", underlying(tcg.src));
    if (tcg.type == GX_TG_MTX2x4 || tcg.type == GX_TG_MTX3x4) {
      if (info.indexAttr.test(GX_VA_TEX0MTXIDX + i)) {
        vtxXfrAttrs += fmt::format("\n    var tc{0}_tmp = tc{0} * ubuf.postex_mtx[in_texmtxidx{0} / 3u];", i);
      } else if (tcg.mtx == GX_IDENTITY) {
        vtxXfrAttrs += fmt::format("\n    var tc{0}_tmp = tc{0}.xyz;", i);
      } else {
        u32 texMtxIdx = (tcg.mtx) / 3;
        vtxXfrAttrs += fmt::format("\n    var tc{0}_tmp = tc{0} * ubuf.postex_mtx[{1}];", i, texMtxIdx);
      }
      if (tcg.type == GX_TG_MTX2x4) {
        vtxXfrAttrs += fmt::format("\n    tc{0}_tmp.z = 1.0f;", i);
      }
    }
    if (tcg.type != GX_TG_SRTG && tcg.normalize) {
      vtxXfrAttrs += fmt::format("\n    tc{0}_tmp = normalize(tc{0}_tmp);", i);
    }
    if (tcg.type == GX_TG_SRTG) {
      // tc{i}_proj was set from the color channel above
    } else if (tcg.postMtx == GX_PTIDENTITY) {
      vtxXfrAttrs += fmt::format("\n    var tc{0}_proj = tc{0}_tmp;", i);
    } else {
      u32 postMtxIdx = (tcg.postMtx - GX_PTTEXMTX0) / 3;
      vtxXfrAttrs +=
          fmt::format("\n    var tc{0}_proj = vec4f(tc{0}_tmp.xyz, 1.0) * ubuf.postmtx[{1}];", i, postMtxIdx);
    }
    // Apply line/point tex offset
    if (config.lineMode == 3) {
      // GX_POINTS: offset S for right columns, T for bottom rows
      vtxXfrAttrs += fmt::format(
          "\n    if ((ubuf.line_texcoord_mask & (1u << {0})) != 0u) {{"
          "\n        if ((vidx & 1u) != 0u) {{ tc{0}_proj.x += ubuf.line_tex_offset; }}"
          "\n        if (vidx >= 2u) {{ tc{0}_proj.y += ubuf.line_tex_offset; }}"
          "\n    }}",
          i);
    } else if (config.lineMode != 0) {
      // GX_LINES / GX_LINESTRIP: offset one axis for perpendicular side
      vtxXfrAttrs += fmt::format(
          "\n    if ((ubuf.line_texcoord_mask & (1u << {0})) != 0u && (vidx & 1u) != 0u) {{"
          "\n        tc{0}_proj.y += ubuf.line_tex_offset;"
          "\n    }}",
          i);
    }
    if (tcg.type == GX_TG_MTX3x4) {
      vtxXfrAttrs += fmt::format("\n    out.tex{0}_uvw = tc{0}_proj.xyz;", i);
      fragmentFnPre += fmt::format("\n    var tex{0}_uv = in.tex{0}_uvw.xy / in.tex{0}_uvw.z;", i);
    } else {
      vtxXfrAttrs += fmt::format("\n    out.tex{0}_uv = tc{0}_proj.xy;", i);
      fragmentFnPre += fmt::format("\n    var tex{0}_uv = in.tex{0}_uv.xy;", i);
    }
  }
  // Multiple TEV stages may reference the same indirect stage,
  // so we sample each indirect texture only once.
  const auto ind_scale = [](const GXIndTexScale s) -> std::string_view {
    switch (s) {
    case GX_ITS_1:
      return "1.0"sv;
    case GX_ITS_2:
      return "(1.0 / 2.0)"sv;
    case GX_ITS_4:
      return "(1.0 / 4.0)"sv;
    case GX_ITS_8:
      return "(1.0 / 8.0)"sv;
    case GX_ITS_16:
      return "(1.0 / 16.0)"sv;
    case GX_ITS_32:
      return "(1.0 / 32.0)"sv;
    case GX_ITS_64:
      return "(1.0 / 64.0)"sv;
    case GX_ITS_128:
      return "(1.0 / 128.0)"sv;
    case GX_ITS_256:
      return "(1.0 / 256.0)"sv;
    default:
      FATAL("unhandled indirect scale {}", underlying(s));
    }
  };
  for (int i = 0; i < info.usedIndStages.size(); ++i) {
    if (!info.usedIndStages.test(i)) {
      continue;
    }
    const auto& indStage = config.indStages[i];
    const u32 texCoordId = underlying(indStage.texCoordId);
    const u32 texMapId = underlying(indStage.texMapId);
    // GX applies the SU texture-coordinate scale before the indirect stage scale.
    // The shader carries normalized UVs, so convert that texel-space result back
    // into normalized coordinates for the indirect texture sample.
    const auto scaleExpr =
        fmt::format("tex{0}_uv * ubuf.texcoord_scale[{0}].xy * vec2f({1}, {2}) / ubuf.tex{3}_size_bias.xy", texCoordId,
                    ind_scale(indStage.scaleS), ind_scale(indStage.scaleT), texMapId);
    fragmentFnPre += fmt::format(
        "\n    // Indirect stage {0}"
        "\n    var t_IndTexCoord{0} = 255.0 * textureSampleBias(tex{1}, tex{1}_samp, {2}, "
        "ubuf.tex{1}_size_bias.z).abg;",
        i, texMapId, scaleExpr);
    if (config.intTev) {
      // The indirect unit receives the filtered texel as 8-bit integers.
      fragmentFnPre += fmt::format("\n    t_IndTexCoord{0} = round(t_IndTexCoord{0});", i);
    }
  }
  if (info.usedIndStages.any()) {
    fragmentFnPre += "\n    var t_TexCoord = vec2f(0.0);";
  }
  for (int i = 0; i < config.tevStageCount; ++i) {
    const auto& stage = config.tevStages[i];
    const bool needsIndirectCoord = stage.indTexMtxId != GX_ITM_OFF;
    const bool hasIndirectStage = stage.indTexStage < config.numIndStages;
    const bool needsTevTexCoord =
        needsIndirectCoord || stage.indTexWrapS != GX_ITW_OFF || stage.indTexWrapT != GX_ITW_OFF || stage.indTexAddPrev;
    // The Z texture is the last stage's texel, whether or not the TEV uses it
    const bool needsTextureSample =
        uses_texture_sample(stage) || (info.usesZTex && i == static_cast<int>(config.tevStageCount) - 1);
    if (!needsTevTexCoord && !needsTextureSample) {
      continue;
    }
    const bool hasBaseTexCoord = stage.texCoordId != GX_TEXCOORD_NULL;
    const bool hasBaseTexture = stage.texMapId != GX_TEXMAP_NULL;
    const bool hasBaseCoord = hasBaseTexCoord && hasBaseTexture;
    std::string uvIn;
    if (needsTevTexCoord) {
      fragmentFnPre += fmt::format("\n    // TEV stage {} indirect", i);

      // Apply indirect texture matrix (produces a texel-space offset)
      std::string indirectOffsetTexel;
      if (needsIndirectCoord && hasIndirectStage) {
        // ITF_5/4/3 offsets are the low bits of each component; the high bits are the bump alpha
        // (alpha_bump_sel masks 0xE0/0xF0/0xF8).
        u32 fmtMask = 0;
        switch (stage.indTexFormat) {
        case GX_ITF_8:
          break;
        case GX_ITF_5:
          fmtMask = 0x1F;
          break;
        case GX_ITF_4:
          fmtMask = 0x0F;
          break;
        case GX_ITF_3:
          fmtMask = 0x07;
          break;
        default:
          FATAL("unhandled indirect format {}", underlying(stage.indTexFormat));
        }
        if (fmtMask == 0) {
          fragmentFnPre += fmt::format("\n    var ind{0}_coord = t_IndTexCoord{1};", i, underlying(stage.indTexStage));
        } else {
          fragmentFnPre +=
              fmt::format("\n    var ind{0}_coord = vec3f(vec3u(round(t_IndTexCoord{1})) & vec3u(0x{2:X}u));", i,
                          underlying(stage.indTexStage), fmtMask);
        }

        if (stage.indTexBiasSel != GX_ITB_NONE) {
          auto bias = stage.indTexFormat == GX_ITF_8 ? "-128.0"sv : "1.0"sv;
          auto biasS = "0.0"sv, biasT = "0.0"sv, biasU = "0.0"sv;
          if (stage.indTexBiasSel == GX_ITB_S || stage.indTexBiasSel == GX_ITB_ST || stage.indTexBiasSel == GX_ITB_SU ||
              stage.indTexBiasSel == GX_ITB_STU) {
            biasS = "1.0"sv;
          }
          if (stage.indTexBiasSel == GX_ITB_T || stage.indTexBiasSel == GX_ITB_ST || stage.indTexBiasSel == GX_ITB_TU ||
              stage.indTexBiasSel == GX_ITB_STU) {
            biasT = "1.0"sv;
          }
          if (stage.indTexBiasSel == GX_ITB_U || stage.indTexBiasSel == GX_ITB_SU || stage.indTexBiasSel == GX_ITB_TU ||
              stage.indTexBiasSel == GX_ITB_STU) {
            biasU = "1.0"sv;
          }
          fragmentFnPre += fmt::format("\n    ind{0}_coord = ind{0}_coord + vec3f({1}, {2}, {3}) * {4};", i, biasS,
                                       biasT, biasU, bias);
        }

        if (stage.indTexMtxId >= GX_ITM_0 && stage.indTexMtxId <= GX_ITM_2) {
          // Static 2x3 matrix: dot(mat_row, vec3(S,T,U)) * scale
          u32 mtxIdx = stage.indTexMtxId - GX_ITM_0;
          fragmentFnPre += fmt::format(
              "\n    let ind{0}_c0 = ubuf.ind_mtx[{1}][0];"
              "\n    let ind{0}_c1 = ubuf.ind_mtx[{1}][1];",
              i, mtxIdx);
          if (config.intTev) {
            // Hardware: s1.10 matrix x s8 offsets, >> 3 into texel units with 7 fractional bits, then
            // the scale shift (right shifts truncate toward -inf).
            fragmentFnPre += fmt::format("\n    let ind{0}_raw = round(1024.0 * vec2f("
                                         "dot(vec3f(ind{0}_c0.xz, ind{0}_c1.x), ind{0}_coord), "
                                         "dot(vec3f(ind{0}_c0.yw, ind{0}_c1.y), ind{0}_coord)));",
                                         i);
            indirectOffsetTexel = fmt::format("(floor(floor(ind{0}_raw / 8.0) * ind{0}_c1.z) / 128.0)", i);
          } else {
            indirectOffsetTexel = fmt::format(
                "vec2f("
                "dot(vec3f(ind{0}_c0.xz, ind{0}_c1.x), ind{0}_coord), "
                "dot(vec3f(ind{0}_c0.yw, ind{0}_c1.y), ind{0}_coord)"
                ") * ind{0}_c1.z",
                i);
          }
        } else if (stage.indTexMtxId >= GX_ITM_S0 && stage.indTexMtxId <= GX_ITM_S2 && hasBaseCoord) {
          // Dynamic S: result = scaled texcoord * ind_coord.x * scale / 256
          u32 mtxIdx = stage.indTexMtxId - GX_ITM_S0;
          u32 regTexCoord = underlying(stage.texCoordId);
          indirectOffsetTexel =
              config.intTev
                  ? fmt::format("(floor(floor(floor(tex{1}_uv * ubuf.texcoord_scale[{1}].xy * 128.0) * ind{0}_coord.x / "
                                "256.0) * ubuf.ind_mtx[{2}][1][2]) / 128.0)",
                                i, regTexCoord, mtxIdx)
                  : fmt::format("tex{1}_uv * ubuf.texcoord_scale[{1}].xy * ind{0}_coord.x"
                                " * ubuf.ind_mtx[{2}][1][2] / 256.0",
                                i, regTexCoord, mtxIdx);
        } else if (stage.indTexMtxId >= GX_ITM_T0 && stage.indTexMtxId <= GX_ITM_T2 && hasBaseCoord) {
          // Dynamic T: result = scaled texcoord * ind_coord.y * scale / 256
          u32 mtxIdx = stage.indTexMtxId - GX_ITM_T0;
          u32 regTexCoord = underlying(stage.texCoordId);
          indirectOffsetTexel =
              config.intTev
                  ? fmt::format("(floor(floor(floor(tex{1}_uv * ubuf.texcoord_scale[{1}].xy * 128.0) * ind{0}_coord.y / "
                                "256.0) * ubuf.ind_mtx[{2}][1][2]) / 128.0)",
                                i, regTexCoord, mtxIdx)
                  : fmt::format("tex{1}_uv * ubuf.texcoord_scale[{1}].xy * ind{0}_coord.y"
                                " * ubuf.ind_mtx[{2}][1][2] / 256.0",
                                i, regTexCoord, mtxIdx);
        }
      }

      // Don't convert to/from texel space if we can avoid it
      const bool useSimpleCoords = stage.indTexMtxId == GX_ITM_OFF && !stage.indTexAddPrev;

      // Wrap base coord and combine with the indirect translation.
      auto wrap_comp = [](GXIndTexWrap wrap, std::string&& coord) -> std::string {
        switch (wrap) {
        case GX_ITW_OFF:
          return std::move(coord);
        case GX_ITW_256:
          return fmt::format("({0} - 256.0 * floor({0} / 256.0))", coord);
        case GX_ITW_128:
          return fmt::format("({0} - 128.0 * floor({0} / 128.0))", coord);
        case GX_ITW_64:
          return fmt::format("({0} - 64.0 * floor({0} / 64.0))", coord);
        case GX_ITW_32:
          return fmt::format("({0} - 32.0 * floor({0} / 32.0))", coord);
        case GX_ITW_16:
          return fmt::format("({0} - 16.0 * floor({0} / 16.0))", coord);
        case GX_ITW_0:
          return "0.0";
        default:
          FATAL("unhandled indirect wrap {}", underlying(wrap));
        }
      };
      std::string baseCoordExpr;
      if (hasBaseCoord) {
        u32 texCoordId = underlying(stage.texCoordId);
        if (useSimpleCoords) {
          baseCoordExpr = fmt::format("tex{}_uv", texCoordId);
        } else {
          // The TEV's texture coordinates are fixed point (texel units, 7 fractional bits).
          if (config.intTev) {
            fragmentFnPre += fmt::format(
                "\n    var ind{0}_texel = floor(tex{1}_uv * ubuf.texcoord_scale[{1}].xy * 128.0) / 128.0;", i,
                texCoordId);
          } else {
            fragmentFnPre +=
                fmt::format("\n    var ind{0}_texel = tex{1}_uv * ubuf.texcoord_scale[{1}].xy;", i, texCoordId);
          }
          baseCoordExpr = fmt::format("ind{}_texel", i);
        }
      }
      std::string wrappedExpr = baseCoordExpr;
      if (!baseCoordExpr.empty() && (stage.indTexWrapS != GX_ITW_OFF || stage.indTexWrapT != GX_ITW_OFF)) {
        wrappedExpr = fmt::format("vec2f({}, {})", wrap_comp(stage.indTexWrapS, fmt::format("{}.x", baseCoordExpr)),
                                  wrap_comp(stage.indTexWrapT, fmt::format("{}.y", baseCoordExpr)));
      }

      std::string finalCoord;
      if (!wrappedExpr.empty() && !indirectOffsetTexel.empty()) {
        finalCoord = fmt::format("{} + ({})", wrappedExpr, indirectOffsetTexel);
      } else if (!wrappedExpr.empty()) {
        finalCoord = wrappedExpr;
      } else {
        finalCoord = indirectOffsetTexel;
      }

      if (info.usedIndStages.any() && !finalCoord.empty()) {
        if (stage.indTexAddPrev) {
          fragmentFnPre += fmt::format("\n    t_TexCoord += {};", finalCoord);
        } else {
          fragmentFnPre += fmt::format("\n    t_TexCoord = {};", finalCoord);
        }

        if (needsTextureSample && hasBaseTexture) {
          u32 texMapId = underlying(stage.texMapId);
          if (useSimpleCoords) {
            fragmentFnPre += fmt::format("\n    var ind{0}_uv = t_TexCoord;", i);
          } else {
            fragmentFnPre += fmt::format("\n    var ind{0}_uv = t_TexCoord / ubuf.tex{1}_size_bias.xy;", i, texMapId);
          }
          uvIn = fmt::format("ind{0}_uv", i);
        }
      }
    }
    if (!needsTextureSample) {
      continue;
    }

    CHECK(stage.texMapId != GX_TEXMAP_NULL, "unmapped texture for stage {}", i);
    CHECK(stage.texCoordId != GX_TEXCOORD_NULL, "unmapped texcoord for stage {}", i);
    if (uvIn.empty()) {
      // No indirect texturing. The setup unit scales each texture coordinate to texels with its
      // own size registers (BP 0x30-0x3F, normally the size of the texture the TEV order binds it
      // to); the texture unit then addresses the map in texels. When a coordinate's scale and the
      // sampled map's size differ (a coordinate shared by maps of different sizes, a texture
      // swapped under a display list that set the scale, GXSetTexCoordScaleManually), the map is
      // sampled at uv * scale / size. Identical to plain uv when they match.
      uvIn = fmt::format("(tex{0}_uv * (ubuf.texcoord_scale[{0}].xy / ubuf.tex{1}_size_bias.xy))",
                         underlying(stage.texCoordId), underlying(stage.texMapId));
    }
    fragmentFnPre +=
        fmt::format("\n    var sampled{0} = textureSampleBias(tex{1}, tex{1}_samp, {2}, ubuf.tex{1}_size_bias.z);", i,
                    underlying(stage.texMapId), uvIn);
  }
  if (info.usesPTTexMtx.any()) {
    uniBufAttrs += fmt::format("\n    postmtx: array<mat3x4f, {}>,", MaxPTTexMtx);
  }
  if (info.usesFog) {
    uniformPre +=
        "\n"
        "struct Fog {\n"
        "    color: vec4f,\n"
        "    a: f32,\n"
        "    b: f32,\n"
        "    c: f32,\n"
        "    range_center: f32,\n"
        "    range_k: array<vec4f, 3>,\n"
        "}";
    uniBufAttrs += "\n    fog: Fog,";

    const std::string_view fogDepth = UseReversedZ ? "(1.0 - in.pos.z)" : "in.pos.z";
    if ((config.fogType & 0x08) != 0) {
      fragmentFn += fmt::format("\n    // Orthographic fog\n    var fogBase = ubuf.fog.a * {};", fogDepth);
    } else {
      fragmentFn +=
          fmt::format("\n    // Perspective fog\n    var fogBase = ubuf.fog.a / (ubuf.fog.b - {});", fogDepth);
    }
    if (config.fogRangeEnabled) {
      fragmentFn += "\n        fogBase *= bitcast<f32>(abuf[imm.fog_range_base + u32(in.pos.x)]);";
    }
    fragmentFn += "\n    var fogF = clamp(fogBase - ubuf.fog.c, 0.0, 1.0);";
    switch (config.fogType) {
      DEFAULT_FATAL("invalid fog type {}", unsigned(config.fogType));
    case GX_FOG_PERSP_LIN:
    case GX_FOG_ORTHO_LIN:
      fragmentFn += "\n    var fogZ = fogF;";
      break;
    case GX_FOG_PERSP_EXP:
    case GX_FOG_ORTHO_EXP:
      fragmentFn += "\n    var fogZ = 1.0 - exp2(-8.0 * fogF);";
      break;
    case GX_FOG_PERSP_EXP2:
    case GX_FOG_ORTHO_EXP2:
      fragmentFn += "\n    var fogZ = 1.0 - exp2(-8.0 * fogF * fogF);";
      break;
    case GX_FOG_PERSP_REVEXP:
    case GX_FOG_ORTHO_REVEXP:
      fragmentFn += "\n    var fogZ = exp2(-8.0 * (1.0 - fogF));";
      break;
    case GX_FOG_PERSP_REVEXP2:
    case GX_FOG_ORTHO_REVEXP2:
      fragmentFn +=
          "\n    fogF = 1.0 - fogF;"
          "\n    var fogZ = exp2(-8.0 * fogF * fogF);";
      break;
    }
    fragmentFn += "\n    prev = vec4f(mix(prev.rgb, ubuf.fog.color.rgb, clamp(fogZ, 0.0, 1.0)), prev.a);";
    if (config.intTev) {
      // The fog blender works on 8-bit color.
      fragmentFn += "\n    prev = vec4f(round(prev.rgb * 255.0) / 255.0, prev.a);";
    }
  }
  uniBufAttrs += fmt::format("\n    texcoord_scale: array<vec4f, {}>,", MaxTexCoord);
  if (info.usedIndTexMtxs.any()) {
    uniBufAttrs += fmt::format("\n    ind_mtx: array<mat2x4f, {}>,", MaxIndTexMtxs);
  }
  for (int i = 0; i < info.sampledTextures.size(); ++i) {
    if (!info.sampledTextures.test(i)) {
      continue;
    }
    uniBufAttrs += fmt::format("\n    tex{}_size_bias: vec4f,", i);
    texBindings += fmt::format(
        "\n@group(2) @binding({1})\n"
        "var tex{0}: texture_2d<f32>;\n"
        "@group(2) @binding({2})\n"
        "var tex{0}_samp: sampler;",
        i, i * 2, i * 2 + 1);
  }
  if (info.usesZTex) {
    uniBufAttrs += "\n    ztex_bias: vec4u,";
  }
  if (config.enhClass != 0) {
    // Must stay last (fill_uniform appends them last).
    uniBufAttrs +=
        "\n    enh_flags: u32,"
        "\n    enh_light_base: u32,"
        "\n    enh_light_count: u32,"
        "\n    enh_shadow_taps: u32,"
        "\n    enh_extra_scale: f32,"
        "\n    enh_shadow_radius: f32,"
        "\n    enh_flash_soft: f32,"
        "\n    enh_pcss: f32,"
        "\n    enh_mask_base: u32,"
        "\n    enh_mask_pitch: u32,"
        "\n    enh_mask_dims: u32,"
        "\n    enh_mask_scale: f32,"
        "\n    enh_flash_spec: f32,"
        "\n    enh_shadow_dark: f32,"
        "\n    enh_shadow_keep: f32,"
        "\n    enh_material: f32,"
        // 0196: realistic flashlight footprint (enh_flags bit 5): gain, reference distance^2,
        // minimum distance falloff, reflector ring amount.
        "\n    enh_beam_gain: f32,"
        "\n    enh_beam_dref2: f32,"
        "\n    enh_beam_floor: f32,"
        "\n    enh_beam_rings: f32,";
  }
  if (config.intTev) {
    // Already wrapped to 8 bits after the last stage.
  } else if (!prevColorNormalized && !prevAlphaNormalized) {
    fragmentFn += "\n    prev = tev_overflow_vec4f(prev);";
  } else if (!prevColorNormalized) {
    fragmentFn += "\n    prev = vec4f(tev_overflow_vec3f(prev.rgb), prev.a);";
  } else if (!prevAlphaNormalized) {
    fragmentFn += "\n    prev.a = tev_overflow_f32(prev.a);";
  }
  if (enh) {
    // 0032: forward ambient occlusion (opaque world draws: enh_flags bit 16; mask slot 16 since 0187).
    fragmentFn += "\n    if ((ubuf.enh_flags & 16u) != 0u) { prev = vec4f(prev.rgb * (1.0 - enh_mask(in.pos.xy, 16u)), prev.a); }"
                  // 0186 debug (AURORA_ENH_AO_SHOW=1): the occlusion each surface receives, white = none.
                  "\n    if ((ubuf.enh_flags & 0x30000u) == 0x30000u) { prev = vec4f(vec3f(1.0 - enh_mask(in.pos.xy, 16u)), prev.a); }";
  }
  if (config.wireframe) {
    // GX_AURORA_SET_WIREFRAME: edges in the override colour (before alpha compare, so
    // alpha-tested surfaces keep all their edges).
    fragmentFn += "\n    prev = vec4f(ubuf.wire_r, ubuf.wire_g, ubuf.wire_b, ubuf.wire_a);";
  }
  if (config.alphaCompare) {
    const auto comp0 = alpha_compare(config.alphaCompare.comp0, config.alphaCompare.ref0);
    const auto comp1 = alpha_compare(config.alphaCompare.comp1, config.alphaCompare.ref1);
    AlphaCompareExpr pass;
    switch (config.alphaCompare.op) {
      DEFAULT_FATAL("invalid alpha compare op {}", underlying(config.alphaCompare.op));
    case GX_AOP_AND:
      pass = alpha_compare_and(comp0, comp1);
      break;
    case GX_AOP_OR:
      pass = alpha_compare_or(comp0, comp1);
      break;
    case GX_AOP_XOR:
      pass = alpha_compare_xor(comp0, comp1);
      break;
    case GX_AOP_XNOR:
      pass = alpha_compare_xnor(comp0, comp1);
      break;
    }
    const auto discard = alpha_compare_not(pass);
    if (discard.constant == 1) {
      fragmentFn += "\n    // Alpha compare\n    discard;";
    } else if (discard.constant != 0) {
      fragmentFn +=
          "\n    // Alpha compare"
          "\n    let alphaCompare = u32(round(clamp(prev.a, 0.0, 1.0) * 255.0));";
      fragmentFn += fmt::format("\n    if ({}) {{ discard; }}", discard.expr);
    }
  }
  if constexpr (EnableNormalVisualization) {
    fragmentFn += "\n    prev = vec4f(in.nrm, prev.a);";
  }

  if (info.usesZTex) {
    // Z texture (GXSetZTexture): the texel of the last TEV stage's lookup, read as an unsigned
    // depth value (Z8 = I8 texel, Z16 = IA8 texel with A as the high byte, Z24X8 = RGBA8 texel
    // with R,G,B from high to low), plus the 24-bit bias, either replaces the rasterized depth or
    // is added to it. The sum wraps at 24 bits.
    const u32 lastStage = config.tevStageCount - 1;
    fragmentFn += fmt::format(
        "\n    // Z texture"
        "\n    let ztex_t = vec4u(round(clamp(sampled{}, vec4f(0.0), vec4f(1.0)) * 255.0));",
        lastStage);
    switch (config.zTexFmt) {
    case 0:
      fragmentFn += "\n    var ztex_z = ztex_t.r;";
      break;
    case 1:
      fragmentFn += "\n    var ztex_z = (ztex_t.a << 8u) | ztex_t.r;";
      break;
    default:
      fragmentFn += "\n    var ztex_z = (ztex_t.r << 16u) | (ztex_t.g << 8u) | ztex_t.b;";
      break;
    }
    fragmentFn += "\n    ztex_z = ztex_z + ubuf.ztex_bias.x;";
    if (config.zTexOp == GX_ZT_ADD) {
      fragmentFn += fmt::format("\n    ztex_z = ztex_z + u32(round(clamp({}, 0.0, 1.0) * 16777215.0));",
                                UseReversedZ ? "1.0 - in.pos.z"sv : "in.pos.z"sv);
    }
    fragmentFn += "\n    ztex_z = ztex_z & 0xFFFFFFu;";
    fragmentFn += fmt::format("\n    let ztex_depth = {};",
                              UseReversedZ ? "1.0 - f32(ztex_z) / 16777215.0"sv : "f32(ztex_z) / 16777215.0"sv);
  }

  if (config.efbFmt != 0) {
    fragmentFn += efb_quantize(config);
  }

  // Logic ops (GX_BM_LOGIC) that need ~src or a constant source: the blend unit gets white
  // (INV = 1 - dst, SET) or the inverted color (INVCOPY, NOR, INVOR); see to_blend_state.
  if (config.logicOut == 1) {
    fragmentFn += "\n    // Logic op source\n    prev = vec4f(1.0);";
  } else if (config.logicOut == 2) {
    fragmentFn += "\n    // Logic op source\n    prev = vec4f(1.0) - clamp(prev, vec4f(0.0), vec4f(1.0));";
  }

  if (dstAlphaMode == DstAlphaMode::Replace) {
    // GX destination alpha is set via the blend constant
    fragmentFn += "\n    prev.a = 1.0;";
  }

  // GX_AURORA_SET_FADE (frame interpolation): drop q/255 of the pixels in a 4x4 ordered dither.
  fragmentFn +=
      "\n    let fade_q = (imm.current_pnmtx >> 8u) & 0xFFu;"
      "\n    if (fade_q != 0u) {"
      "\n        var fade_bayer = array<u32, 16>(0u, 8u, 2u, 10u, 12u, 4u, 14u, 6u, 3u, 11u, 1u, 9u, 15u, 7u, 13u, 5u);"
      "\n        let fade_p = vec2u(in.pos.xy) & vec2u(3u);"
      "\n        if ((f32(fade_bayer[fade_p.y * 4u + fade_p.x]) + 0.5) * 16.0 < f32(fade_q)) { discard; }"
      "\n    }";

  std::string fragmentOutput;
  std::string_view fragmentOutputType = "@location(0) vec4f"sv;
  std::string fragmentReturn = "\n    return prev;"s;
  if (normalAttachment != UINT32_MAX) {
    fragmentOutput = fmt::format(
        "\nstruct FragmentOutput {{\n"
        "    @location(0) color: vec4f,\n"
        "    @location({}) normal: vec4f,\n"
        "}};\n",
        normalAttachment);
    fragmentOutputType = "FragmentOutput"sv;
    fragmentReturn = "\n    var out: FragmentOutput;\n    out.color = prev;";
    if (useNormalTarget) {
      fragmentReturn +=
          "\n    let nrm_len_sq = dot(in.mv_nrm, in.mv_nrm);"
          "\n    let unit_nrm = select(vec3f(0.0), normalize(in.mv_nrm), nrm_len_sq > 1e-10);"
          "\n    out.normal = vec4f(unit_nrm * 0.5 + 0.5, select(0.0, 1.0, nrm_len_sq > 1e-10));";
    } else {
      fragmentReturn += "\n    out.normal = vec4f(0.5, 0.5, 0.5, 0.0);";
    }
    fragmentReturn += "\n    return out;";
  }

  if (dstAlphaMode == DstAlphaMode::DualSource) {
    fragmentOutput =
        "\nstruct FragmentOutput {\n"
        "    @location(0) @blend_src(0) color: vec4f,\n"
        "    @location(0) @blend_src(1) blend: vec4f,\n"
        "};\n";
    fragmentOutputType = "FragmentOutput"sv;
    fragmentReturn = "\n    return FragmentOutput(vec4f(prev.rgb, 1.0), prev);";
  }

  if (info.usesZTex) {
    // Add the depth output to whichever output form was chosen above.
    if (dstAlphaMode == DstAlphaMode::DualSource) {
      fragmentOutput =
          "\nstruct FragmentOutput {\n"
          "    @location(0) @blend_src(0) color: vec4f,\n"
          "    @location(0) @blend_src(1) blend: vec4f,\n"
          "    @builtin(frag_depth) depth: f32,\n"
          "};\n";
      fragmentReturn = "\n    return FragmentOutput(vec4f(prev.rgb, 1.0), prev, ztex_depth);";
    } else if (normalAttachment != UINT32_MAX) {
      fragmentOutput = fmt::format(
          "\nstruct FragmentOutput {{\n"
          "    @location(0) color: vec4f,\n"
          "    @location({}) normal: vec4f,\n"
          "    @builtin(frag_depth) depth: f32,\n"
          "}};\n",
          normalAttachment);
      const auto ret = fragmentReturn.rfind("\n    return out;");
      fragmentReturn.insert(ret, "\n    out.depth = ztex_depth;");
    } else {
      fragmentOutput =
          "\nstruct FragmentOutput {\n"
          "    @location(0) color: vec4f,\n"
          "    @builtin(frag_depth) depth: f32,\n"
          "};\n";
      fragmentReturn = "\n    return FragmentOutput(prev, ztex_depth);";
    }
    fragmentOutputType = "FragmentOutput"sv;
  }

  if (config.lightDepth && config.lineMode == 0 && !webgpu::g_clipDistancesSupported) {
    fragmentFnPre.insert(0, "\n    if (any((in.pos.xy < in.enh_tile.xy)) || any((in.pos.xy >= in.enh_tile.zw))) { discard; }");
  }
  if (config.lineMode == 0 && !config.lightDepth && hw_pixel_snap()) {
    // 0180: Flipper's setup unit takes screen positions on a 1/12-pixel grid (the subsample grid
    // of GXSetCopyFilter's sample patterns), dropping the rest (floor; positions are positive
    // thanks to the SDK's +340 viewport offset), and samples non-AA pixels at 6/12 (the SDK's
    // centred pattern). A left edge at x covers the pixel's sample iff x < 7/12 (Dolphin hwtests
    // rasterization.cpp: the "7/12 pixel centre"), while geometry on whole pixels is sampled at
    // exactly the texel centres (1:1 linear blits stay sharp on hardware). Snapped here in the
    // logical (EFB) pixel grid, so coverage and attribute interpolation follow the snapped
    // positions at any render scale. The 1/256 tolerance keeps float noise on exact grid
    // positions from dropping a step.
    vtxXfrAttrs +=
        "\n    if (out.pos.w > 0.0 && all(abs(ubuf.logical_viewport_size) >= vec2f(1.0))) {"
        "\n      let hw_vs = ubuf.logical_viewport_size;"
        "\n      let hw_ndc = out.pos.xy / out.pos.w;"
        "\n      let hw_win = ubuf.logical_viewport_origin.xy + vec2f(hw_ndc.x * 0.5 + 0.5, 0.5 - hw_ndc.y * 0.5) * hw_vs;"
        "\n      let hw_d = (floor(hw_win * 12.0 + 1.0 / 256.0) / 12.0 - hw_win) * 2.0 / hw_vs;"
        "\n      out.pos = vec4f(out.pos.xy + vec2f(hw_d.x, -hw_d.y) * out.pos.w, out.pos.zw);"
        "\n    }";
  }
  auto shaderSource =
      fmt::format(R"""(
fn bswap32(v: u32, le: bool) -> u32 {{
  if (le) {{
    return v;
  }}
  return ((v & 0x000000FFu) << 24u) |
         ((v & 0x0000FF00u) << 8u) |
         ((v & 0x00FF0000u) >> 8u) |
         ((v & 0xFF000000u) >> 24u);
}}

fn bswap16(v: u32, le: bool) -> u32 {{
  return select(((v & 0xFFu) << 8u) | (v >> 8u), v, le);
}}

fn load_word(p: ptr<storage, array<u32>>, word_idx: u32) -> u32 {{
  // This guard is not expected to handle routine out-of-bounds accesses.
  // It appears to discourage some Adreno drivers/optimizers from storage buffer
  // optimizations that can cause visual artifacts, including vertex explosions
  // in Dusklight.
  if (word_idx < arrayLength(p)) {{
    return p[word_idx];
  }}
  return 0u;
}}

fn load_u8(p: ptr<storage, array<u32>>, byte_off: u32) -> u32 {{
  let word = load_word(p, byte_off / 4u);
  let shift = (byte_off & 3u) * 8u;
  return (word >> shift) & 0xFFu;
}}

fn load_u32_raw(p: ptr<storage, array<u32>>, byte_off: u32) -> u32 {{
  let word_idx = byte_off >> 2u;
  let sub = byte_off & 3u;
  let lo = load_word(p, word_idx);
  if (sub == 0u) {{
    return lo;
  }}
  let hi = load_word(p, word_idx + 1u);
  let shift = sub * 8u;
  return (lo >> shift) | (hi << (32u - shift));
}}

fn load_u16(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> u32 {{
  let word_idx = byte_off >> 2u;
  let sub = byte_off & 3u;
  let word = load_word(p, word_idx);
  if (sub <= 2u) {{
    return bswap16(extractBits(word, sub * 8u, 16u), le);
  }}
  let next = load_word(p, word_idx + 1u);
  let raw = extractBits(word, 24u, 8u) | (extractBits(next, 0u, 8u) << 8u);
  return bswap16(raw, le);
}}

fn load_u24(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> u32 {{
  let raw = load_u32_raw(p, byte_off) & 0x00FFFFFFu;
  if (le) {{
    return raw;
  }}
  return ((raw & 0x0000FFu) << 16u) |
         (raw & 0x00FF00u) |
         ((raw & 0xFF0000u) >> 16u);
}}

fn load_u32(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> u32 {{
  return bswap32(load_u32_raw(p, byte_off), le);
}}

fn load_f32(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> f32 {{
  return bitcast<f32>(load_u32(p, byte_off, le));
}}

fn raw_fetch_u8_1(p: ptr<storage, array<u32>>, byte_off: u32) -> u32 {{
  return load_u8(p, byte_off);
}}

fn raw_fetch_u8_2(p: ptr<storage, array<u32>>, byte_off: u32) -> vec2u {{
  let word_idx = byte_off >> 2u;
  let sub = byte_off & 3u;
  let word = load_word(p, word_idx);
  if (sub <= 2u) {{
    let shift = sub * 8u;
    return vec2u(
      extractBits(word, shift + 0u, 8u),
      extractBits(word, shift + 8u, 8u),
    );
  }}
  let next = load_word(p, word_idx + 1u);
  return vec2u(
    extractBits(word, 24u, 8u),
    extractBits(next, 0u, 8u),
  );
}}

fn raw_fetch_u8_3(p: ptr<storage, array<u32>>, byte_off: u32) -> vec3u {{
  let raw = load_u32_raw(p, byte_off);
  return vec3u(
    extractBits(raw, 0u, 8u),
    extractBits(raw, 8u, 8u),
    extractBits(raw, 16u, 8u),
  );
}}

fn raw_fetch_u8_4(p: ptr<storage, array<u32>>, byte_off: u32) -> vec4u {{
  let raw = load_u32_raw(p, byte_off);
  return vec4u(
    extractBits(raw, 0u, 8u),
    extractBits(raw, 8u, 8u),
    extractBits(raw, 16u, 8u),
    extractBits(raw, 24u, 8u),
  );
}}

fn raw_fetch_u16_1(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> u32 {{
  return load_u16(p, byte_off, le);
}}

fn raw_fetch_u16_2(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> vec2u {{
  return vec2u(
    load_u16(p, byte_off + 0u, le),
    load_u16(p, byte_off + 2u, le),
  );
}}

fn raw_fetch_u16_3(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> vec3u {{
  return vec3u(
    load_u16(p, byte_off + 0u, le),
    load_u16(p, byte_off + 2u, le),
    load_u16(p, byte_off + 4u, le),
  );
}}

fn raw_fetch_u16_4(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> vec4u {{
  return vec4u(
    load_u16(p, byte_off + 0u, le),
    load_u16(p, byte_off + 2u, le),
    load_u16(p, byte_off + 4u, le),
    load_u16(p, byte_off + 6u, le),
  );
}}

fn raw_fetch_f32_1(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> f32 {{
  return load_f32(p, byte_off, le);
}}

fn raw_fetch_f32_2(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> vec2f {{
  return vec2f(
    load_f32(p, byte_off + 0u, le),
    load_f32(p, byte_off + 4u, le),
  );
}}

fn raw_fetch_f32_3(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> vec3f {{
  return vec3f(
    load_f32(p, byte_off + 0u, le),
    load_f32(p, byte_off + 4u, le),
    load_f32(p, byte_off + 8u, le),
  );
}}

fn raw_fetch_f32_4(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> vec4f {{
  return vec4f(
    load_f32(p, byte_off + 0u, le),
    load_f32(p, byte_off + 4u, le),
    load_f32(p, byte_off + 8u, le),
    load_f32(p, byte_off + 12u, le),
  );
}}

fn fetch_u8_1(p: ptr<storage, array<u32>>, byte_off: u32, frac: u32, le: bool) -> f32 {{
  let v = raw_fetch_u8_1(p, byte_off);
  return f32(v) / f32(1u << frac);
}}

fn fetch_s8_1(p: ptr<storage, array<u32>>, byte_off: u32, frac: u32, le: bool) -> f32 {{
  let v = (bitcast<i32>(raw_fetch_u8_1(p, byte_off)) << 24) >> 24;
  return f32(v) / f32(1u << frac);
}}

fn fetch_u8_2(p: ptr<storage, array<u32>>, byte_off: u32, frac: u32, le: bool) -> vec2f {{
  let v = raw_fetch_u8_2(p, byte_off);
  return vec2f(v) / f32(1u << frac);
}}

fn fetch_s8_2(p: ptr<storage, array<u32>>, byte_off: u32, frac: u32, le: bool) -> vec2f {{
  let v = (bitcast<vec2i>(raw_fetch_u8_2(p, byte_off)) << vec2u(24u)) >> vec2u(24u);
  return vec2f(v) / f32(1u << frac);
}}

fn fetch_u8_3(p: ptr<storage, array<u32>>, byte_off: u32, frac: u32, le: bool) -> vec3f {{
  let v = raw_fetch_u8_3(p, byte_off);
  return vec3f(v) / f32(1u << frac);
}}

fn fetch_s8_3(p: ptr<storage, array<u32>>, byte_off: u32, frac: u32, le: bool) -> vec3f {{
  let v = (bitcast<vec3i>(raw_fetch_u8_3(p, byte_off)) << vec3u(24u)) >> vec3u(24u);
  return vec3f(v) / f32(1u << frac);
}}

fn fetch_u8_4(p: ptr<storage, array<u32>>, byte_off: u32, frac: u32, le: bool) -> vec4f {{
  let v = raw_fetch_u8_4(p, byte_off);
  return vec4f(v) / f32(1u << frac);
}}

fn fetch_s8_4(p: ptr<storage, array<u32>>, byte_off: u32, frac: u32, le: bool) -> vec4f {{
  let v = (bitcast<vec4i>(raw_fetch_u8_4(p, byte_off)) << vec4u(24u)) >> vec4u(24u);
  return vec4f(v) / f32(1u << frac);
}}

fn fetch_u16_1(p: ptr<storage, array<u32>>, byte_off: u32, frac: u32, le: bool) -> f32 {{
  let v = raw_fetch_u16_1(p, byte_off, le);
  return f32(v) / f32(1u << frac);
}}

fn fetch_s16_1(p: ptr<storage, array<u32>>, byte_off: u32, frac: u32, le: bool) -> f32 {{
  let v = bitcast<i32>(raw_fetch_u16_1(p, byte_off, le) << 16u) >> 16;
  return f32(v) / f32(1u << frac);
}}

fn fetch_u16_2(p: ptr<storage, array<u32>>, byte_off: u32, frac: u32, le: bool) -> vec2f {{
  let v = raw_fetch_u16_2(p, byte_off, le);
  return vec2f(v) / f32(1u << frac);
}}

fn fetch_s16_2(p: ptr<storage, array<u32>>, byte_off: u32, frac: u32, le: bool) -> vec2f {{
  let v = (bitcast<vec2i>(raw_fetch_u16_2(p, byte_off, le)) << vec2u(16u)) >> vec2u(16u);
  return vec2f(v) / f32(1u << frac);
}}

fn fetch_u16_3(p: ptr<storage, array<u32>>, byte_off: u32, frac: u32, le: bool) -> vec3f {{
  let v = raw_fetch_u16_3(p, byte_off, le);
  return vec3f(v) / f32(1u << frac);
}}

fn fetch_s16_3(p: ptr<storage, array<u32>>, byte_off: u32, frac: u32, le: bool) -> vec3f {{
  let v = (bitcast<vec3i>(raw_fetch_u16_3(p, byte_off, le)) << vec3u(16u)) >> vec3u(16u);
  return vec3f(v) / f32(1u << frac);
}}

fn fetch_u16_4(p: ptr<storage, array<u32>>, byte_off: u32, frac: u32, le: bool) -> vec4f {{
  let v = raw_fetch_u16_4(p, byte_off, le);
  return vec4f(v) / f32(1u << frac);
}}

fn fetch_s16_4(p: ptr<storage, array<u32>>, byte_off: u32, frac: u32, le: bool) -> vec4f {{
  let v = (bitcast<vec4i>(raw_fetch_u16_4(p, byte_off, le)) << vec4u(16u)) >> vec4u(16u);
  return vec4f(v) / f32(1u << frac);
}}

fn fetch_f32_1(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> f32 {{
  return raw_fetch_f32_1(p, byte_off, le);
}}

fn fetch_f32_2(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> vec2f {{
  return raw_fetch_f32_2(p, byte_off, le);
}}

fn fetch_f32_3(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> vec3f {{
  return raw_fetch_f32_3(p, byte_off, le);
}}

fn fetch_f32_4(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> vec4f {{
  return raw_fetch_f32_4(p, byte_off, le);
}}

fn fetch_rgb565(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> vec4f {{
  let v = load_u16(p, byte_off, le);
  return vec4f(
    f32((v >> 11u) & 0x1Fu) / f32(0x1Fu),
    f32((v >>  5u) & 0x3Fu) / f32(0x3Fu),
    f32((v >>  0u) & 0x1Fu) / f32(0x1Fu),
    1.0,
  );
}}

fn fetch_rgb8(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> vec4f {{
  let v = raw_fetch_u8_3(p, byte_off);
  return vec4f(f32(v.x), f32(v.y), f32(v.z), 255.0) / 255.0;
}}

fn fetch_rgbx8(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> vec4f {{
  let v = raw_fetch_u8_4(p, byte_off);
  return vec4f(f32(v.x), f32(v.y), f32(v.z), 255.0) / 255.0;
}}

fn fetch_rgba4(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> vec4f {{
  let v = load_u16(p, byte_off, le);
  return vec4f(
    f32((v >> 12u) & 0x0Fu) / f32(0x0Fu),
    f32((v >>  8u) & 0x0Fu) / f32(0x0Fu),
    f32((v >>  4u) & 0x0Fu) / f32(0x0Fu),
    f32((v >>  0u) & 0x0Fu) / f32(0x0Fu),
  );
}}

fn fetch_rgba6(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> vec4f {{
  let v = load_u24(p, byte_off, le);
  return vec4f(
    f32((v >> 18u) & 0x3Fu) / f32(0x3Fu),
    f32((v >> 12u) & 0x3Fu) / f32(0x3Fu),
    f32((v >>  6u) & 0x3Fu) / f32(0x3Fu),
    f32((v >>  0u) & 0x3Fu) / f32(0x3Fu),
  );
}}

fn fetch_rgba8(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> vec4f {{
  let v = raw_fetch_u8_4(p, byte_off);
  return vec4f(v) / 255.0;
}}

fn tev_overflow_f32(in: f32) -> f32 {{
  let byte_space = in * 255.0;
  return (byte_space - floor(byte_space / 256.0) * 256.0) / 255.0;
}}

fn tev_overflow_vec3f(in: vec3f) -> vec3f {{
  let byte_space = in * 255.0;
  return (byte_space - floor(byte_space / 256.0) * 256.0) / 255.0;
}}

fn tev_overflow_vec4f(in: vec4f) -> vec4f {{
  let byte_space = in * 255.0;
  return (byte_space - floor(byte_space / 256.0) * 256.0) / 255.0;
}}

// Light attenuation polynomial c.x + c.y * x + c.z * x^2: a term whose coefficient is zero is
// zero, whatever x is. Games leave the direction of lights loaded with GXInitLightPos only
// uninitialized (stack garbage, e.g. 3e36) and switch the spot terms off with zero
// coefficients instead; 0 * inf would give NaN and flicker from frame to frame.
fn gx_attn_poly(c: vec3f, x: f32) -> f32 {{
  return c.x + select(c.y * x, 0.0, c.y == 0.0) + select(c.z * x * x, 0.0, c.z == 0.0);
}}

{8}

struct Immediate {{
    vtx_start: u32,
    current_pnmtx: u32,
    fog_range_base: u32,
    _pad: u32,
    array_start0: vec4u,
    array_start1: vec4u,
    array_start2: vec4u,
}};
var<immediate> imm: Immediate;

struct Uniform {{
    render_viewport_size: vec2f,
    logical_viewport_size: vec2f,
    logical_viewport_origin: vec4f,{0}
}};
@group(0) @binding(0)
var<storage, read> vbuf: array<u32>;
@group(0) @binding(1)
var<storage, read> abuf: array<u32>;{12}
@group(1) @binding(0)
var<uniform> ubuf: Uniform;{1}

struct VertexOutput {{
    @builtin(position) pos: vec4f,{2}
}};

@vertex
fn vs_main(
    @builtin(vertex_index) vidx: u32{3}
) -> VertexOutput {{
    var out: VertexOutput;{7}{4}
    return out;
}}

{9}
@fragment
fn fs_main(in: VertexOutput) -> {10} {{{6}{5}{11}
}}
)""",
                  uniBufAttrs, texBindings, vtxOutAttrs, vtxInAttrs, vtxXfrAttrs, fragmentFn, fragmentFnPre,
                  vtxXfrAttrsPre, uniformPre, fragmentOutput, fragmentOutputType, fragmentReturn,
                  enh ? "\n@group(0) @binding(2)\nvar<storage, read> mbuf: array<u32>;"sv : ""sv);
  if (dstAlphaMode == DstAlphaMode::DualSource) {
    shaderSource.insert(0, "enable dual_source_blending;\n");
  }
  if (config.lightDepth && config.lineMode == 0 && webgpu::g_clipDistancesSupported) {
    // clip_distances is a vertex output only: the fragment stage reads a copy of the struct
    // without it.
    const std::string clipField = "\n    @builtin(clip_distances) enh_clip: array<f32, 4>,";
    const auto structPos = shaderSource.find("struct VertexOutput {");
    const auto structEnd = shaderSource.find("};", structPos);
    if (structPos != std::string::npos && structEnd != std::string::npos) {
      std::string fragStruct = shaderSource.substr(structPos, structEnd + 2 - structPos);
      fragStruct.replace(0, std::string_view("struct VertexOutput").size(), "struct FragmentInput");
      if (const auto c = fragStruct.find(clipField); c != std::string::npos) {
        fragStruct.erase(c, clipField.size());
      }
      shaderSource.insert(structEnd + 2, "\n" + fragStruct);
      if (const auto f = shaderSource.find("fn fs_main(in: VertexOutput)"); f != std::string::npos) {
        shaderSource.replace(f, std::string_view("fn fs_main(in: VertexOutput)").size(), "fn fs_main(in: FragmentInput)");
      }
    }
    shaderSource.insert(0, "enable clip_distances;\n");
  }
  if (EnableDebugPrints) {
    Log.info("Generated shader (hash {:x}): {}", hash, shaderSource);
  }
  // Enhanced lighting debug aid: AURORA_SHADER_DUMP=<dir> writes every generated shader.
  static const char* sDumpDir = std::getenv("AURORA_SHADER_DUMP");
  if (sDumpDir != nullptr) {
    const auto path = fmt::format("{}/{:016x}_e{}{}.wgsl", sDumpDir, hash, int(config.enhClass), config.lightDepth ? "L" : "");
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
      std::fwrite(shaderSource.data(), 1, shaderSource.size(), f);
      std::fclose(f);
    }
  }

  return shaderSource;
}

static uint32_t normal_attachment(const gfx::RenderTargetLayout& layout) noexcept {
  uint32_t normalAttachment = UINT32_MAX;
  for (uint32_t i = 0; i < layout.colorAttachmentCount; ++i) {
    if (layout.colorAttachments[i].semantic == gfx::ColorAttachmentSemantic::Normal) {
      normalAttachment = i;
    }
  }
  return normalAttachment;
}

HashType shader_module_key(const ShaderConfig& config, const gfx::RenderTargetLayout& layout,
                           DstAlphaMode dstAlphaMode) noexcept {
  return xxh3_hash(dstAlphaMode, xxh3_hash(normal_attachment(layout), xxh3_hash(config)));
}

wgpu::ShaderModule build_shader(const ShaderConfig& config, const gfx::RenderTargetLayout& layout,
                                DstAlphaMode dstAlphaMode) noexcept {
  ZoneScoped;
  uint32_t normalAttachment = UINT32_MAX;
  for (uint32_t i = 0; i < layout.colorAttachmentCount; ++i) {
    if (layout.colorAttachments[i].semantic == gfx::ColorAttachmentSemantic::Normal) {
      normalAttachment = i;
    }
  }
  const uint64_t t0 = gfx::now_ns(); // 0226
  const auto shaderSource = build_shader_source(config, dstAlphaMode, normalAttachment);
  const uint64_t t1 = gfx::now_ns();
  gfx::add_create_time(gfx::CreateStage::Wgsl, t1 - t0);
  const auto hash = xxh3_hash(dstAlphaMode, xxh3_hash(normalAttachment, xxh3_hash(config)));
  wgpu::ShaderSourceWGSL wgslDescriptor{};
  wgslDescriptor.code = shaderSource.c_str();
  const auto label = fmt::format("GX Shader {:x}", hash);
  const auto shaderDescriptor = wgpu::ShaderModuleDescriptor{
      .nextInChain = &wgslDescriptor,
      .label = label.c_str(),
  };
  auto module = webgpu::g_device.CreateShaderModule(&shaderDescriptor);
  gfx::add_create_time(gfx::CreateStage::Module, gfx::now_ns() - t1);
  return module;
}
} // namespace aurora::gx
