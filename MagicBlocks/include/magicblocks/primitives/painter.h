// MagicBlocks L3 —— 画笔（spec §6，U2：先完成"画"这个简易功能）
//
// 最小闭环：**收到输入事件 → 生成笔迹几何 → 渲染出来**，且能录制回放。
//
// 三个刻意的边界：
//   1. 不做布尔合并、不做挤出 —— 那些是 L4 的组合（§6.4）；
//      `extract_outline()` 只负责把笔迹还原成轮廓，交给 L4 去挤出。
//   2. 不接触 GPU —— IPainter 产出**顶点与索引**（屏幕像素空间），
//      上传与 draw 由 IPrimitiveRenderer 负责。这样画笔逻辑可以完全离线单测，
//      命令行无头出图也不需要真实设备参与几何生成。
//   3. 不读时钟 —— 事件自带 timestamp_us，画笔内部只做纯几何运算。
//      这是命令回放确定性的前提：同样的输入序列必然产出同样的几何。
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "magicblocks/primitives/types.h"

namespace magicblocks::primitives {

using device::u32;
using device::u64;

// 笔画事件种类。Begin/Move/End 是一笔的完整生命周期，
// Cancel 用于"按下后发现是手势而非画线"（例如拖动相机）——
// 此时已产生的几何必须整笔丢弃，不能留下半截笔迹。
enum class StrokeEventKind : std::uint8_t {
  Begin = 0,
  Move = 1,
  End = 2,
  Cancel = 3,
};

// 由 device::InputEvent 一一映射而来（D6：字段与语义对齐，时间戳原样带过来）。
//
// 坐标是**像素、原点左上、y 向下**（D-L3-2），与 InputEvent::position 同口径。
// 不做任何坐标变换 —— 变换在投影矩阵那一层已经做完，这里再转一次必然出偏移。
struct StrokeEvent {
  StrokeEventKind kind = StrokeEventKind::Move;
  std::uint32_t pointer_id = 0;   // 多点触控区分不同手指
  float x = 0.0f;
  float y = 0.0f;
  std::int64_t timestamp_us = 0;
};

// 画笔参数。
struct StrokeDesc {
  float width_px = 6.0f;
  Color4 color{1.0f, 1.0f, 1.0f, 1.0f};
  bool round_cap = true;
  // 中心线平滑：Catmull-Rom 细分到相邻点间距 ≤ 2px。
  // 关掉后是折线（点少时能看出棱角），但省 CPU。
  bool smoothing = true;
  std::int32_t max_points_per_stroke = 4096;
};

// 笔迹顶点：屏幕空间 2D + RGBA。
//
// 为什么不用 mesh.h 的 Vertex（3D + 法线 + uv）：
//   笔迹是屏幕空间的，没有法线也没有 uv，用 48 字节的 3D 顶点浪费一半带宽。
//   32 字节的 2D 顶点让同样大小的实例缓冲能多装 50% 的笔迹。
//   两者由不同的管线消费（stroke_ribbon vs std_pbr），不会混用。
struct StrokeVertex {
  float position[2] = {0.0f, 0.0f};  // 像素坐标，原点左上   [0,8)
  float _pad[2] = {0.0f, 0.0f};      // 补到 16B            [8,16)
  std::uint8_t color[4] = {255, 255, 255, 255};  //            [16,20)
  float _pad2[3] = {0.0f, 0.0f, 0.0f};  // 补到 32B             [20,32)
};
static_assert(sizeof(StrokeVertex) == 32, "StrokeVertex 必须是 32 字节");

// 一笔的输出几何。
struct StrokeGeometry {
  std::vector<StrokeVertex> vertices;
  std::vector<std::uint32_t> indices;
  u32 stroke_id = 0;
  bool closed = false;  // 该笔是否已结束（End/Cancel 之后不再增长）

  void clear() {
    vertices.clear();
    indices.clear();
    closed = false;
  }
};

// 画笔统计 —— 用于验证"增量"确实生效。
struct PainterStats {
  u32 strokes_started = 0;
  u32 strokes_finished = 0;
  u32 strokes_cancelled = 0;
  u32 segments_appended = 0;   // 累计生成的段数
  u32 points_accepted = 0;     // 接受的笔迹点
  u32 points_dropped = 0;      // 因 max_points_per_stroke 或过近丢弃的点
  u32 geometry_vertices_added = 0;
};

class IPainter {
 public:
  IPainter() = default;
  IPainter(const IPainter&) = delete;
  IPainter& operator=(const IPainter&) = delete;
  virtual ~IPainter() = default;

  virtual void set_desc(const StrokeDesc& desc) = 0;
  virtual const StrokeDesc& desc() const = 0;

  // 喂一个事件。**增量**：Move 只追加一个新段（4~6 个顶点），不重算已有段。
  // 这是"画"能不能跑 60fps 的关键（§6.2.1）。
  virtual void feed(const StrokeEvent& event) = 0;

  // 批量喂（从事件队列一次性灌入）。
  virtual void feed_batch(const StrokeEvent* events, std::size_t count) = 0;

  // 当前帧的几何（所有进行中 + 已完成的笔迹）。
  // 返回的引用在下一次 feed 之前有效。
  virtual const StrokeGeometry& geometry() const = 0;

  // 已完成的笔迹数量（用于 UI 显示 / 撤销）。
  virtual u32 completed_stroke_count() const = 0;

  // 取消当前进行中的笔迹（丢弃已生成几何）。
  virtual void cancel_current() = 0;

  // 清空所有笔迹。
  virtual void clear() = 0;

  // 取出某笔的轮廓（中心线附近的左右边界），供 L4 的 FromOutline 挤出。
  // 返回值：每条折线一条，含孔时按 L4 约定处理（L3 只返回中心线轮廓）。
  virtual std::vector<std::vector<Vec2>> extract_outline() const = 0;

  virtual const PainterStats& stats() const = 0;

  // ---- 确定性校验（§11 回放）----
  // 当前所有几何的指纹。同样的输入序列必须得到同样的 hash。
  virtual std::uint64_t geometry_hash() const = 0;

  // 预热：预分配内部缓冲，避免首笔触发分配。
  virtual void reserve(u32 strokes, u32 points_per_stroke) = 0;
};

std::unique_ptr<IPainter> create_painter();

}  // namespace magicblocks::primitives
