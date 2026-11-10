#pragma once
// MagicBlocks L4 —— 魔法叠叠乐：颜色模式（设计 §11）
//
// 三种模式改变的是**同一个** `Block.color` 字段，不改变几何、不改变 stroke，
// 也不改变 `IBlockSet::hash` 的口径（hash 覆盖 color，所以改色会变 hash ——
// 那是**期望**的，改色是数据变更；不变的是"除了 color 字节，其它一个字节都不动"）。
//
// 为什么集中在这里而不是散在笔刷/渲染里：
//   ① 三段逻辑（选色、写 field、重刷实例）必须原子，散开必然出现
//      "改了一半"的中间态；
//   ② §15 A8 要求"改色前后实例缓冲里除 64-79 之外**逐字节相同**"。
//      只有把"写实例"这一步也收进来，才能保证没人顺手改了别的字段。

#include <cstdint>
#include <memory>
#include <vector>

#include "magicblocks/features/stack/block_set.h"
#include "magicblocks/features/stack/stack_types.h"

namespace magicblocks::features::stack {

enum class ColorMode : std::int32_t {
  Flat = 0,     // 整堆同色
  ByLayer = 1,  // 按 cell.y 取色
  ByStroke = 2, // 按 stroke_id 取色
};

const char* color_mode_name(ColorMode mode);

// 色板。空色板是**显式错误**（ColorOutOfRange），不是"全黑"。
using ColorPalette = std::vector<std::uint32_t>;

struct ColorApplyStats {
  std::uint32_t blocks_changed = 0;
  std::uint32_t blocks_total = 0;
  // 几何重建次数。§11 要求恒为 0；这里显式记录，让"零重建"可被断言而不是靠约定。
  std::uint32_t geometry_rebuilds = 0;
  ColorMode mode = ColorMode::Flat;
};

// 只改 `Block.color`。返回被改写块的个数；任一环节失败则**整堆回滚**，
// 不留下"改了一半"的堆（与 add_stroke 的全有或全无同一条纪律）。
//
// `palette` 语义按模式不同：
//   Flat     : palette[0] 即整堆颜色；尺寸必须 == 1
//   ByLayer  : palette[cell.y      % palette.size()]
//   ByStroke : palette[stroke_id   % palette.size()]
StackStatus apply_color_mode(IBlockSet& set, ColorMode mode, const ColorPalette& palette,
                             ColorApplyStats* out_stats);

// 把真源里的 color 写进实例属性数组的 base_color（偏移 64-79，4 个 float）。
// **只写**这 16 字节，其它字段原样保留 —— A8 的"逐字节相同"由本函数的
// 按字段写入范围保证，由用例的 memcmp 校验。
void write_instance_base_color(const Block& b, float out_base_color[4]);

// 确定性默认色板：与 F-SK 相同的黄金分割 hue 序列（设计 §11 末条）。
// 纯函数，不读时钟不取随机，同样输入必然同样输出 → golden 可复算。
ColorPalette make_default_palette(std::size_t count);

}  // namespace magicblocks::features::stack
