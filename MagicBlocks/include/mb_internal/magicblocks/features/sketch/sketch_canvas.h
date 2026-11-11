// MagicBlocks L4 —— 魔法涂鸦：画布 + 输入链路（R-SK-1 / R-SK-2 / D-SK-5）
//
// ============================ 这一层在解决什么问题 ============================
// 需求原文（`魔法功能预览.md:2`）：「2维图形（透明）转3D 挤出面。
// **只做简易画布与挤出面功能。**」
//
// 在补上本文件之前，"画布"这一环是**不存在的**：笔画坐标由 demo 生成器
// 直接写死在 CLI 里，`device::InputEvent` 从未被喂进任何代码。
// 也就是说，"用户在屏幕上画了一笔" → "文档里多了一笔"这一跳根本没有实现，
// 而它恰恰是需求里唯一没被裁掉的那个交互环节。
//
// 本文件把那条链补齐，并把它钉在**一个**换算点上：
//
//   device::InputEvent                （屏幕像素坐标，原点左上、y 向下）
//          │  map_input()             （逐字段映射，**不做**坐标变换）
//          ▼
//   primitives::StrokeEvent           （L3 画笔的事件类型）
//          │  Canvas::to_canvas_point()  ← 唯一的屏幕 → 画布换算
//          │      = ICamera::screen_to_ray(px,py)  ∩  画布平面
//          ▼
//   features::sketch::Stroke          （画布平面世界坐标）
//          │  SketchDocument::add_stroke()
//          ▼
//   文档（真源） → build_meshes() → 挤出体
//
// ============================ 为什么换算必须走 ICamera ============================
// D-SK-5 的裁定是"一律走 ICamera::screen_to_ray(px,py) ∩ Plane"。
// 两个被否掉的替代方案及其问题：
//   - 直接把像素线性映射到画布：像素与视口绑定，窗口一变文档就废，
//     而且透视画布下完全错误；
//   - 在 L4 里自己乘一遍投影矩阵的逆：那就是在 L4 复制一份投影数学，
//     两处一旦分叉（比如 y 翻转只翻一次）画面"看着对但方向反了"。
//     投影像素↔世界的正反变换**只**允许在 L3 `camera.cpp` 里存在一份。
//
// 本文件因此**零投影数学**：只用 `ICamera::screen_to_ray` 与
// `primitives::intersect_ray_plane`（两者都是 L3 已交付的公共能力）。
// 正交 / 透视两种相机下都自动正确 —— 这正是 A1 要求的"两种相机都正确"。
//
// ============================ 画布坐标系约定 ============================
// 画布平面 = 一个矩形面，带 4x4 世界变换（`CanvasParams::transform`，列主序）。
// 平面局部坐标 u,v ∈ [-w/2, +w/2] × [-h/2, +h/2]，挤出沿局部 +z。
//
// **屏幕坐标与画布坐标的 y 方向天然相反**：屏幕 y 向下，画布 y 向上。
// 这个翻转由 `screen_to_ray` ∩ 平面**自动**产生（射线打到画布下半部分，
// 交点就是较小的 y）—— 不需要、也**不允许**在这里手动再翻一次。
// `Canvas.ScreenUpMapsToLargerCanvasY` 用一条真实相机射线把这条钉住。
#pragma once

#include <cstdint>
#include <vector>

#include "mb_internal/magicblocks/device/events.h"
#include "mb_internal/magicblocks/features/sketch/sketch_document.h"
#include "mb_internal/magicblocks/primitives/camera.h"
#include "mb_internal/magicblocks/primitives/painter.h"
#include "mb_internal/magicblocks/primitives/types.h"

namespace magicblocks::features::sketch {

namespace pm = magicblocks::primitives;

// 画布参数（**场景常量**，进快照；不随帧变化）。
//
// 设计 §5.2 给出的四个字段全部保留原语义，另外补两个**位置**相关字段：
//   - `transform`：画布在世界里的姿态。没有它，"画布"就还是屏幕像素的别名 ——
//     设计 §5.2 默认画布贴在 XY 平面、看向 -Z，本文件用单位阵表示该默认值。
//   - `min_spacing`：采样点最小间距（画布单位）。设计 §7.3 的"采样点过密剔除"
//     原先被推到 `IPainter` 内部，但 `IPainter` 工作在像素空间，无法解释
//     "多近算过密"（世界单位才稳定）。所以这条规则放在画布坐标里，
//     与 `max_points` 一起构成采样策略。
struct CanvasParams {
  // 画布在世界单位下的宽/高（局部 x / y 方向）。
  float width = 2.0f;
  float height = 1.5f;
  // 沿画布法线（局部 +z）的挤出厚度。
  float extrude_thickness = 0.08f;
  // 笔画走出画布：true → 夹到边界；false → 允许（由参数决定，不静默）。
  bool clamp_to_canvas = true;
  // 画布 → 世界的变换（列主序 4x4，与 primitives::Mat4 同布局）。
  // 单位阵 = 画布中心在世界原点、法线朝 +z（设计 §5.2 的默认平面）。
  float transform[16] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                         0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
  // 单笔采样点上限（超出即停止追加，不静默"平滑掉"超出的部分）。
  std::uint32_t max_points_per_stroke = 4096;
  // 相邻采样点最小间距（画布单位）。<= 0 表示不剔除。
  float min_spacing = 0.0f;
};

// 一次"屏幕 → 画布"换算的结果。
struct CanvasHit {
  bool hit = false;
  // 画布局部坐标（u, v），单位与 width/height 一致。
  pm::Vec2 local{};
  // 世界坐标交点。
  pm::Vec3 world{};
};

// 画布。持有 CanvasParams，负责两件事：
//   1. 屏幕像素 → 画布平面求交（D-SK-5 的唯一换算点）；
//   2. 承接一次笔画的采样（Begin/Move/End/Cancel），产出 SketchDocument 的 Stroke。
//
// **不读时钟**：时间戳只从事件流取（设计 §9.2 第 1 条）。
class Canvas {
 public:
  explicit Canvas(const CanvasParams& params);

  const CanvasParams& params() const { return params_; }
  // 改参数：非法值（宽/高非正、厚度为负、变换奇异）→ 返回错误且**不改**参数。
  // 不静默夹到合法值 —— 那会让调用方以为自己的参数生效了。
  pm::Status set_params(const CanvasParams& params);

  // 画布平面的世界变换与法线（供调试/用例断言）。
  pm::Mat4 transform() const;
  pm::Vec3 normal() const;
  pm::Vec3 center() const;

  // 屏幕像素 → 画布平面求交。
  // 射线与平面平行（正交相机侧看）→ hit=false；交点在相机**背后** → hit=false。
  CanvasHit intersect_screen(const pm::ICamera& camera, float px, float py) const;

  // 画布局部坐标 → 世界坐标。
  pm::Vec3 local_to_world(const pm::Vec2& uv) const;

  // 夹取（`clamp_to_canvas` 为 false 时原样返回）。
  pm::Vec2 clamp_local(const pm::Vec2& uv) const;
  bool inside(const pm::Vec2& uv) const;

  // 采样过程中的统计 —— 用例据此断言"过密点确实被剔除 / 越界确实被夹"，
  // 而不是从结果反推。
  struct SampleStats {
    std::uint32_t samples_accepted = 0;
    std::uint32_t samples_dropped_spacing = 0;  // 间距过近
    std::uint32_t samples_dropped_limit = 0;    // 超过 max_points_per_stroke
    std::uint32_t samples_clamped = 0;          // 被夹到画布边界
    std::uint32_t samples_off_canvas_rejected = 0;  // 未夹取时落在画布外（**不**采样）
    std::uint32_t samples_missed_plane = 0;     // 射线打不到画布平面（平行/在背后）
    std::uint32_t illegal_transitions = 0;      // 非法转移（Idle 收到 Move 等）
  };

  struct StrokeOutcome {
    enum class Kind : std::uint8_t {
      Committed = 0,  // 已提交（points 非空）
      Cancelled = 1,  // Cancel：整笔丢弃，文档不变
      Empty = 2,      // 没有任何有效采样点（Down 之后没有可用 Move）
    };
    Kind kind = Kind::Empty;
    Stroke stroke{};  // Committed 时可直接 add_stroke()；否则为空骨
  };

  // 喂一个屏幕像素事件（Begin/Move/End/Cancel），推进笔画状态机。
  //
  // Begin → Move* → End 产出 Committed；Begin → Cancel 产出 Cancelled。
  // Idle 状态收到 Move → **忽略并计数**（不崩、不静默当成 Begin）。
  // 上一笔没收到 End 就来了新的 Begin → 视为隐式结束（把已采样部分提交）。
  StrokeOutcome feed(const pm::ICamera& camera, const pm::StrokeEvent& event,
                     Stroke stroke_template);

  // 丢弃进行中的笔画（不提交）。
  void cancel_current();

  bool drawing() const { return active_; }
  const SampleStats& stats() const { return stats_; }
  void reset_stats() { stats_ = SampleStats{}; }

 private:
  // 清空"进行中笔画"的状态（不动统计）。
  void reset_active();
  void append_sample(const pm::Vec2& local, float px, float py);
  // 屏幕 → 画布并（按策略）采样。返回 true 表示采到了点。
  // 命中语义与统计计数都在这一处，避免四个分支各写一遍、口径漂移。
  bool sample_screen(const pm::ICamera& camera, float px, float py);

  CanvasParams params_;
  bool active_ = false;
  std::uint32_t active_pointer_ = 0;
  std::vector<pm::Vec2> active_points_;
  std::vector<pm::Vec2> active_screen_;  // 供用例核对屏幕→画布的对应关系
  bool active_closed = false;
  SampleStats stats_{};
};

// ---------------------------------------------------------------- 事件映射

// `device::InputEvent` → `primitives::StrokeEvent`（D6：时间戳原样带过来）。
//
// 返回 false 表示这个事件**不是**绘制事件（KeyDown / Resize / Focus ...），
// 调用方应原样放行给别的消费者。**不做坐标变换**：
// 像素坐标原样传下去，换算只发生在 Canvas::intersect_screen。
bool map_input(const device::InputEvent& in, pm::StrokeEvent* out);

// 一批事件映射的计数（不画、只回答"这条流里有多少绘制事件"）。
struct InputMapStats {
  std::uint32_t total = 0;
  std::uint32_t mapped = 0;
  std::uint32_t ignored = 0;          // 非绘制事件
  std::uint32_t wrong_button = 0;     // 非主键/主触点
};
InputMapStats map_input_stream(const device::InputEvent* events, std::size_t count,
                               std::vector<pm::StrokeEvent>* out);

// ---------------------------------------------------------------- 与 L3 IPainter 的接线

// 把已映射好的 L3 笔画事件喂给 L3 画笔（R-SK-1 / R-SK-2 / A4）。
//
// 为什么还要走 `IPainter`：F-SK 之前**完全绕开**了它，导致 R-SK-2 要求的
// "Move 只追加、不重建" 这条增量语义在玩法里从来没有被执行过。
// 本函数把那条链接上，并把每个事件**同时**喂给画布采样器 ——
// 于是"增量几何"与"画布采样"看到的是同一串事件，不会有两份时间轴。
//
// 返回值：画布产出的已提交笔画（按提交顺序）。未提交的在 `feed` 里被丢弃。
struct PainterFeedResult {
  std::vector<Stroke> committed;
  std::uint32_t events_drawn = 0;
  // `IPainter::stats().segments_appended` 的增量：>0 证明 Move 真的追加了几何。
  std::uint32_t painter_segments_appended = 0;
};
PainterFeedResult feed_painter_and_canvas(pm::IPainter& painter, Canvas& canvas,
                                          const pm::ICamera& camera,
                                          const pm::StrokeEvent* events, std::size_t count,
                                          Stroke stroke_template_);

}  // namespace magicblocks::features::sketch
