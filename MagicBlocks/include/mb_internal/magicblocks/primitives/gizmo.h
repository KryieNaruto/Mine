#pragma once
// MagicBlocks L3 —— IGizmoSystem（spec §10 / M3 第 15 项）
//
// 要防的问题（用户点名的"补丁式代码"）：
//   input handler 里 `if (gizmo.hit_test(...))`、
//   renderer 里 `if (selected) draw_gizmo(...)`、
//   scene 里 `if (dragging_axis == X) ...` —— 三处补丁，处处耦合。
//
// 解耦方案：**事件总线 + 声明式轴 + 显式状态机**
//
//   InputEvent（D6，唯一事件源）
//        │
//        ▼
//   IGizmoSystem::on_input()   ← 唯一入口；内部 hit-test → 状态机 → 发事件
//        │
//        ▼  GizmoEvent（值类型，可进命令流）
//   IGizmoListener 订阅者       ← 玩法在这里响应；**gizmo 自己绝不改世界**
//
// 四条硬规则（写死，不留自由裁量）：
//   R1 **gizmo 不拥有世界**。它只发 HoverBegin/Drag/DragEnd… 事件，
//      由订阅者去改 world。本文件里不会出现任何玩法名词。
//   R2 **轴是数据不是分支**。轴由 `configure()` 声明，增删轴不改一行代码。
//   R3 **屏幕恒定尺寸是相机原语**。gizmo 只调 `ICamera::world_size_per_pixel()`，
//      自己不作任何"距离→大小"的推导 —— 那是这份代码最容易分叉的地方。
//   R4 **屏幕空间拾取是单点**。`pick_axis()` 返回 axis id，命中阈值是像素，
//      调用方不需要知道射线/阈值。

#include <cstdint>
#include <memory>
#include <vector>

#include "mb_internal/magicblocks/device/events.h"
#include "mb_internal/magicblocks/primitives/camera.h"
#include "mb_internal/magicblocks/primitives/overlay2d.h"
#include "mb_internal/magicblocks/primitives/types.h"

namespace magicblocks::primitives {

// ---------------------------------------------------------------- 模式与轴

enum class GizmoMode : std::uint8_t {
  Translate = 0,
  Rotate = 1,
  Scale = 2,
};

// 一根轴。**数据**，不是 case 分支 —— 想加一根斜轴只需 push 一项。
struct GizmoAxis {
  std::uint8_t id = 0;
  // 轴方向（模型/世界空间，见 GizmoTargetDesc::orientation）。
  float direction[3] = {1.0f, 0.0f, 0.0f};
  // 轴长基准（世界单位）。实际显示长度 = extent（屏幕恒定：由相机换算）。
  float extent = 1.0f;
  Color4 color{1.0f, 0.3f, 0.2f, 1.0f};
  // 旋转环模式下的半径倍率（相对 extent）。
  float ring_radius_scale = 1.0f;
};

struct GizmoTargetDesc {
  GizmoMode mode = GizmoMode::Translate;
  // 世界坐标中心。玩法每帧给最新值（拖动物体后中心会动）。
  float center[3] = {0.0f, 0.0f, 0.0f};
  // 3x3 朝向（行主序），nullptr = 世界轴。
  const float* orientation = nullptr;
  // 屏幕上的基准半径（像素）。**所有尺寸都从这里推**，与相机距离无关。
  float screen_radius_px = 80.0f;
  std::vector<GizmoAxis> axes;
};

// ---------------------------------------------------------------- 事件

struct GizmoEvent {
  enum class Kind : std::uint8_t {
    HoverBegin,
    HoverEnd,
    DragBegin,
    Drag,
    DragEnd,
    DragCancel,
  };
  Kind kind = Kind::HoverBegin;
  std::uint32_t target = 0;
  std::uint8_t axis = 0;
  // 本帧增量：平移是位移，旋转是弧度，缩放是比例增量。
  float delta[3] = {0.0f, 0.0f, 0.0f};
  // 自 DragBegin 起的累计。
  float total[3] = {0.0f, 0.0f, 0.0f};
  // 来自事件流，**绝不读时钟**（回放要求）。
  std::int64_t timestamp_us = 0;
};

class IGizmoListener {
 public:
  IGizmoListener() = default;
  IGizmoListener(const IGizmoListener&) = delete;
  IGizmoListener& operator=(const IGizmoListener&) = delete;
  virtual ~IGizmoListener() = default;
  virtual void on_gizmo(const GizmoEvent&) = 0;
};

// ---------------------------------------------------------------- 快照

// 命令流用：状态可快照/恢复（§11）。全部值类型。
struct GizmoSnapshot {
  std::uint32_t active_target = 0;
  std::uint8_t hovered_axis = 0;
  std::uint8_t dragging_axis = 0;
  bool is_dragging = false;
  float drag_total[3] = {0.0f, 0.0f, 0.0f};
  std::uint64_t event_counter = 0;

  bool operator==(const GizmoSnapshot& o) const {
    if (active_target != o.active_target) return false;
    if (hovered_axis != o.hovered_axis) return false;
    if (dragging_axis != o.dragging_axis) return false;
    if (is_dragging != o.is_dragging) return false;
    if (drag_total[0] != o.drag_total[0] || drag_total[1] != o.drag_total[1] ||
        drag_total[2] != o.drag_total[2])
      return false;
    return event_counter == o.event_counter;
  }
};

// ---------------------------------------------------------------- 接口

class IGizmoSystem {
 public:
  IGizmoSystem() = default;
  IGizmoSystem(const IGizmoSystem&) = delete;
  IGizmoSystem& operator=(const IGizmoSystem&) = delete;
  virtual ~IGizmoSystem() = default;

  // ---- 目标管理 ----

  // 声明一个 gizmo 目标。返回 handle（> 0）。轴来自 desc.axes。
  virtual std::uint32_t configure(const GizmoTargetDesc&) = 0;
  virtual void release(std::uint32_t target) = 0;
  // 更新中心/朝向/模式（拖动物体后每帧调）。
  virtual void update_target(std::uint32_t target, const GizmoTargetDesc&) = 0;
  virtual void set_active_target(std::uint32_t target) = 0;
  virtual std::uint32_t active_target() const = 0;

  // ---- 输入（唯一入口，R1）----

  // 返回 true = 这次事件被 gizmo 消费，玩法**不再**处理它。
  //
  // 注意：这里只改 gizmo 自己的状态并发事件，绝不碰 world。
  virtual bool on_input(const device::InputEvent&, const ICamera&) = 0;

  // 屏幕空间拾取单点（R4）。返回 axis id；`out_axis = kNoAxis` 表示没命中。
  static constexpr std::uint8_t kNoAxis = 0xFF;
  virtual std::uint8_t pick_axis(std::uint32_t target, float px, float py,
                                 const ICamera&) const = 0;

  virtual bool dragging() const = 0;
  virtual void cancel_drag() = 0;  // Esc / 失焦

  // ---- 订阅 ----

  virtual void subscribe(IGizmoListener*) = 0;
  virtual void unsubscribe(IGizmoListener*) = 0;

  // ---- 渲染 ----

  // gizmo 自己产出一组覆盖层图元，由渲染器统一提交。
  // **gizmo 不直接画**：它只往 IOverlay2D 里 push，提交权归渲染器。
  virtual Status build_draw(const ICamera&, IOverlay2D&) const = 0;

  // ---- 命令流 ----

  virtual GizmoSnapshot snapshot() const = 0;
  virtual Status restore(const GizmoSnapshot&) = 0;

  // 屏幕恒定尺寸自检：给定相机与轴，返回该轴在世界空间应有的长度。
  // 验收断言用它（"世界尺寸 ∝ world_size_per_pixel"）。
  virtual float axis_world_length(std::uint32_t target, std::uint8_t axis,
                                  const ICamera&) const = 0;
};

std::unique_ptr<IGizmoSystem> create_gizmo_system();

}  // namespace magicblocks::primitives
