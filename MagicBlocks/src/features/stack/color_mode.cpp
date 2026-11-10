// MagicBlocks L4 —— 颜色模式实现（设计 §11）
#include "magicblocks/features/stack/color_mode.h"

#include <cmath>

namespace magicblocks::features::stack {

namespace {

void unpack_rgba8(std::uint32_t c, float out[4]) {
  // 定死 0xRRGGBBAA（与 stack_render.cpp 的实例解包、write_png 字节序一致）。
  out[0] = static_cast<float>((c >> 24) & 0xFFu) / 255.0f;
  out[1] = static_cast<float>((c >> 16) & 0xFFu) / 255.0f;
  out[2] = static_cast<float>((c >> 8) & 0xFFu) / 255.0f;
  out[3] = static_cast<float>(c & 0xFFu) / 255.0f;
}

std::uint32_t pick_color(ColorMode mode, const ColorPalette& palette, const Block& b) {
  switch (mode) {
    case ColorMode::Flat:
      return palette[0];
    case ColorMode::ByLayer: {
      const std::uint32_t idx = static_cast<std::uint32_t>(b.cell.y) % static_cast<std::uint32_t>(palette.size());
      return palette[idx];
    }
    case ColorMode::ByStroke: {
      const std::uint32_t idx = b.stroke_id % static_cast<std::uint32_t>(palette.size());
      return palette[idx];
    }
  }
  return palette[0];
}

}  // namespace

const char* color_mode_name(ColorMode mode) {
  switch (mode) {
    case ColorMode::Flat:
      return "flat";
    case ColorMode::ByLayer:
      return "by-layer";
    case ColorMode::ByStroke:
      return "by-stroke";
  }
  return "unknown";
}

StackStatus apply_color_mode(IBlockSet& set, ColorMode mode, const ColorPalette& palette,
                             ColorApplyStats* out_stats) {
  if (palette.empty()) {
    return StackStatus::Fail(StackCode::ColorOutOfRange, "色板为空：改色需要至少一个颜色");
  }
  if (mode == ColorMode::Flat && palette.size() != 1u) {
    return StackStatus::Fail(StackCode::ColorOutOfRange,
                             "Flat 模式需要恰好 1 个颜色，实际 " +
                                 std::to_string(palette.size()) + " 个");
  }
  // 越界取值模式：Flat 已经在上面挡掉；ByLayer/ByStroke 用取模，任何尺寸都合法
  // （这也是设计表的写法：palette[cell.y % palette.size()]）。
  const std::size_t n = set.size();

  // 先把目标颜色全部算出来再一次性写。这样任一环节要失败时，
  // 真源还一个字节都没动 —— 回滚成本为 0（与 add_stroke 的全有或全无同源）。
  std::vector<std::uint32_t> target(n, 0u);
  for (std::size_t i = 0; i < n; ++i) {
    const Block* b = set.at(i);
    if (b == nullptr) {
      return StackStatus::Fail(StackCode::OutOfGrid, "改色时真源下标 " + std::to_string(i) + " 为空");
    }
    target[i] = pick_color(mode, palette, *b);
  }

  std::uint32_t changed = 0;
  for (std::size_t i = 0; i < n; ++i) {
    if (set.set_color(i, target[i]).ok()) {
      ++changed;
    } else {
      return StackStatus::Fail(StackCode::OutOfGrid, "写 color 失败于下标 " + std::to_string(i));
    }
  }

  if (out_stats != nullptr) {
    out_stats->blocks_changed = changed;
    out_stats->blocks_total = static_cast<std::uint32_t>(n);
    out_stats->geometry_rebuilds = 0;  // 本路径不碰几何，恒为 0
    out_stats->mode = mode;
  }
  return StackStatus::Ok_();
}

void write_instance_base_color(const Block& b, float out_base_color[4]) {
  unpack_rgba8(b.color, out_base_color);
}

ColorPalette make_default_palette(std::size_t count) {
  // 黄金分割色相步进（与 F-SK 的 cos 相位版本同源）。
  //
  // ⚠️ 这里**不能**用 t = i/(count-1)：那样 t 在 [0,1] 上完整走一圈 360°，
  // 于是 **首尾必然同色**（实测 count=6 时第 0 项与第 5 项都是 (255,64,64)，
  // 12 项只剩 5 种颜色）。按层/按笔取色一旦撞色，"不同层不同色"这个
  // 视觉契约就静默失效 —— 而且不报错。
  // 改用黄金分割步进 kGOLDEN_STEP：任意 count 下相邻项都把色相推
  // 0.618 圈，项与项之间始终最大程度错开，也不会绕回起点。
  constexpr float kGoldenStep = 0.6180339887f;
  ColorPalette out;
  out.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    const float t = static_cast<float>(i) * kGoldenStep;
    const float r = 0.5f + 0.5f * std::cos(6.2831853f * t);
    const float g = 0.5f + 0.5f * std::cos(6.2831853f * (t + 0.333f));
    const float b = 0.5f + 0.5f * std::cos(6.2831853f * (t + 0.667f));
    const std::uint32_t ir = static_cast<std::uint32_t>(r * 255.0f + 0.5f);
    const std::uint32_t ig = static_cast<std::uint32_t>(g * 255.0f + 0.5f);
    const std::uint32_t ib = static_cast<std::uint32_t>(b * 255.0f + 0.5f);
    out.push_back((ir << 24) | (ig << 16) | (ib << 8) | 0xFFu);
  }
  return out;
}

}  // namespace magicblocks::features::stack
