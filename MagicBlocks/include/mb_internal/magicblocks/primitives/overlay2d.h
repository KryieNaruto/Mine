#pragma once
// MagicBlocks L3 —— IOverlay2D（spec §13.13 / M3 第 13 项）
//
// 屏幕空间的 2D 绘制原语：**屏幕恒宽线**、虚线、矩形。没有深度、没有变换 ——
// 顶点直接给像素坐标，shader 里一步转 NDC。
//
// 为什么单独一层而不是让调用方自己拼三角形：
//   1. **屏幕恒定宽度**是唯一必须集中处理的事。线宽 2 像素，在透视投影下
//      如果按世界空间给宽度，远处的线会细成一条缝、近处粗成一块砖。
//      这里所有宽度参数都是**像素**，由 shader 按视口尺寸换算。
//   2. gizmo（§10）、选框、网格辅助线、调试线全都需要同一套东西。
//      各写一份必然分叉 —— 这就是 "U6 解耦" 要防的补丁式代码。
//
// 坐标系（全引擎统一，与 ICamera::world_to_screen 一致）：
//   像素原点在**左上角**，+x 向右，+y 向下。因此这里给的 y 越大越靠下。
//
// 无状态收集、有状态提交的分离：
//   `IOverlay2D` 是**每帧收集器** —— 玩法/gizmo 只管往里 push 图元，
//   最后由渲染器一次性 `build()` 成顶点缓冲 + 一个 draw。
//   这样"谁画了什么"与"怎么提交"彻底解耦，gizmo 不需要知道渲染器存在。

#include <cstdint>
#include <memory>
#include <vector>

#include "mb_internal/magicblocks/primitives/alloc.h"
#include "mb_internal/magicblocks/primitives/types.h"

namespace magicblocks::primitives {

// ---------------------------------------------------------------- 图元描述

// 屏幕恒宽线段。宽度单位是**像素**，与相机距离无关。
struct LineDesc {
  Vec2 from{0.0f, 0.0f};
  Vec2 to{0.0f, 0.0f};
  float width_px = 1.0f;
  Color4 color{1.0f, 1.0f, 1.0f, 1.0f};
};

// 虚线：把线段按 dash/gap 切成小段。切分在**收集期**做，
// 因为它只依赖像素长度，不依赖任何 GPU 状态。
struct DashDesc {
  Vec2 from{0.0f, 0.0f};
  Vec2 to{0.0f, 0.0f};
  float width_px = 1.0f;
  float dash_px = 6.0f;
  float gap_px = 4.0f;
  Color4 color{1.0f, 1.0f, 1.0f, 1.0f};
};

// 屏幕矩形（轴对齐）。用于选框、高亮框、面板背景。
struct RectDesc {
  Vec2 min{0.0f, 0.0f};
  Vec2 max{0.0f, 0.0f};
  Color4 color{1.0f, 1.0f, 1.0f, 1.0f};
  // 空心框的线宽（像素）。0 = 实心填充。
  float border_px = 0.0f;
};

// 屏幕圆环。gizmo 的旋转环用它 —— 用折线逼近，段数由调用方给。
struct RingDesc {
  Vec2 center{0.0f, 0.0f};
  float radius_px = 24.0f;
  float width_px = 2.0f;
  std::uint32_t segments = 48;
  Color4 color{1.0f, 1.0f, 1.0f, 1.0f};
};

// 三角形（屏幕坐标）。填充用，gizmo 的箭头锥形体用它。
struct TriDesc {
  Vec2 a{0.0f, 0.0f};
  Vec2 b{0.0f, 0.0f};
  Vec2 c{0.0f, 0.0f};
  Color4 color{1.0f, 1.0f, 1.0f, 1.0f};
};

// ---------------------------------------------------------------- 统计

struct OverlayStats {
  std::uint32_t line_count = 0;
  std::uint32_t rect_count = 0;
  std::uint32_t ring_count = 0;
  std::uint32_t tri_count = 0;
  std::uint32_t vertex_count = 0;
  std::uint32_t index_count = 0;
  // 本帧 build() 里 vector 扩容次数。稳态必须为 0（G6 门控）。
  std::uint32_t reallocations = 0;
};

// ---------------------------------------------------------------- 顶点

// 覆盖层顶点。位置是**像素坐标**，不是 NDC —— 换算交给 shader，
// 这样 CPU 侧完全不依赖视口尺寸，viewport resize 不需要重建几何。
//
// 布局（24 字节，4 字节对齐）：
//   float position[2]  @0
//   uint8_t color[4]   @8   (RGBA8 归一化)
//   float _pad[3]      @12  → 补齐到 24，便于将来加 uv 而不破坏对齐
struct OverlayVertex {
  float position[2] = {0.0f, 0.0f};
  std::uint8_t color[4] = {255, 255, 255, 255};
  float _pad[3] = {0.0f, 0.0f, 0.0f};
};
static_assert(sizeof(OverlayVertex) == 24, "OverlayVertex 必须是 24 字节");

struct OverlayGeometry {
  std::vector<OverlayVertex> vertices;
  std::vector<std::uint32_t> indices;
};

// ---------------------------------------------------------------- 接口

class IOverlay2D {
 public:
  IOverlay2D() = default;
  IOverlay2D(const IOverlay2D&) = delete;
  IOverlay2D& operator=(const IOverlay2D&) = delete;
  virtual ~IOverlay2D() = default;

  // ---- 帧边界 ----

  // 清空收集缓冲（**不释放容量**）。每帧开头调一次。
  virtual void begin_frame() = 0;

  // 把已收集的图元三角化成顶点/索引。可重复调用（幂等）。
  virtual void build() = 0;

  // 视口尺寸。shader 需要它做像素→NDC；这里存一份供统计/裁剪用。
  virtual void set_viewport(std::uint32_t width, std::uint32_t height) = 0;
  virtual std::uint32_t viewport_width() const = 0;
  virtual std::uint32_t viewport_height() const = 0;

  // ---- 收集（每帧可调任意次）----

  virtual void push_line(const LineDesc&) = 0;
  virtual void push_dash(const DashDesc&) = 0;
  virtual void push_rect(const RectDesc&) = 0;
  virtual void push_ring(const RingDesc&) = 0;
  virtual void push_tri(const TriDesc&) = 0;

  // ---- 查询 ----

  virtual const OverlayGeometry& geometry() const = 0;
  virtual const OverlayStats& stats() const = 0;

  // 预留容量：调用方知道本帧大概要多少条线时先 reserve，
  // 避免 build() 期间扩容（G6 要求稳态零分配）。
  virtual void reserve(std::uint32_t lines, std::uint32_t rects) = 0;

  // 帧内缓冲来源。设了就从这个 arena 取，不设就用内部 vector。
  virtual void set_frame_arena(IFrameArena*) = 0;
};

std::unique_ptr<IOverlay2D> create_overlay2d();

}  // namespace magicblocks::primitives
