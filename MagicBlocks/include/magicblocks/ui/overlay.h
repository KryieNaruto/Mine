// MagicBlocks UI 消费层 —— L3 覆盖层绘制接口的桩(spec §8 UI 框架选型、§6 G3/G5)
//
// 对齐 reference 设计 §4 L3 的 P5 `IOverlay2D`(屏幕空间线/虚线/矩形,屏幕恒宽)。
//
// 本轮状态:SDK 的 L3 尚未落地(见 sdk_stub.h)。这里给出 UI 层**所有绘制调用**
// 的唯一出口,目的有三个:
//
//   1. **合批口径固化**(spec §6 G3):UI 每帧只允许按"纹理 → 管线"分组提交,
//      调用方按顺序 push 图元,由后端在 `end_frame` 前统一合批。UI 层不允许
//      逐图元 drawcall。
//   2. **零分配口径固化**(spec §6 G5):顶点/索引写进**预分配**大 buffer
//      (VMA 分页偏移),稳态帧 VMA alloc/free = 0。因此 `IOverlay2D` 的接口
//      只有"追加图元",没有"创建资源"。
//   3. **可替换**:demo 期只有"矩形 + 线段"两种图元(不做圆角/渐变),未来换
//      轻量即时模式 UI 库时,换的是这个接口的实现,不是 UI 层布局代码。
//
// 坐标:像素、左上原点、y 向下(与 types.h 的 Vec2 同系)。到 NDC 的换算
// 只发生在 overlay 实现内部一处。
#pragma once

#include <cstdint>

#include "magicblocks/ui/sdk_stub.h"
#include "magicblocks/ui/types.h"

namespace magicblocks::ui::stub {

// 一条描边(屏幕恒定宽度,不随相机缩放 —— reference 设计 P5 的要求)。
struct StrokeStyle {
  f32 width_px = 1.f;
  bool dashed = false;
  f32 dash_on_px = 6.f;
  f32 dash_off_px = 4.f;
};

// 纹理句柄:图集/字体图集都在 SDK 侧,UI 层只持有不透明的 0 号句柄。
// 本轮无文本图集(见 §9 待办),因此实际只用 kNoTexture。
using TextureHandle = std::uint32_t;
inline constexpr TextureHandle kNoTexture = 0;

// L3 `IOverlay2D` 桩(UI 层的唯一绘制出口)。
class IOverlay2D {
 public:
  virtual ~IOverlay2D() = default;

  // 一帧的图元录制窗口。一个 overlay pass = begin → push* → end。
  virtual void begin(Rect viewport_px) = 0;

  // 实心矩形(带 alpha 混合;HUD 底与色块用)。
  virtual void fill_rect(Rect r, Rgba8 color) = 0;

  // 描边矩形(1px 边框类);线宽与虚线只走 line/stroke 接口,矩形边框恒 1px。
  virtual void stroke_rect(Rect r, Rgba8 color) = 0;

  // 线段(屏幕恒定宽度)。
  virtual void line(Vec2 a, Vec2 b, Rgba8 color, const StrokeStyle& style) = 0;

  // 纹理矩形(本轮恒 kNoTexture;色表/图集落地后启用)。
  virtual void blit(Rect dst, TextureHandle texture, Rect src_uv, Rgba8 tint) = 0;

  // 结束录制:后端在此做"按纹理/管线合批"并产出本帧 drawcall。
  virtual void end() = 0;

  // 本帧统计(G3 门槛:drawcall 上限、顶点上限由 PerfGovernor 判)。
  struct Stats {
    u32 draw_calls = 0;
    u32 vertices = 0;
    u32 batches = 0;
  };
  virtual Stats stats() const = 0;
};

}  // namespace magicblocks::ui::stub
