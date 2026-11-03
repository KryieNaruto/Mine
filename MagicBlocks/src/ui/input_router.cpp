// MagicBlocks UI 消费层 —— 输入路由与手势识别实现(spec §7)
//
// 仲裁顺序只看三处,且**顺序写死在代码里、不读配置**(spec §7 是硬规定):
//     ① HUD 命中  →  ② 工具激活  →  ③ 相机/场景
//
// 归属模型(整段交互只有一个 owner):
//   - PointerDown 时做一次仲裁,结果写入该指针的 owner;
//   - Move/Up/Cancel **沿用它**,不再重新仲裁 —— 否则一根手指画到一半跳槽到相机,
//     是经典的手势撕裂 bug;
//   - 归属 Unrouted 的指针也算一个明确结果(没人要),计入 dropped 供排查。
//
// 手势在"抬起"那一刻成型,结果**自包含**(只含数值,不含指针/时间引用),
// 可直接进命令流;原始时间戳同时保留,满足 spec §7「手势结果 + 原始事件」。
#include "magicblocks/ui/input.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include "magicblocks/ui/hud.h"

namespace magicblocks::ui {

const char* to_string(InputOwner owner) {
  switch (owner) {
    case InputOwner::Unrouted: return "Unrouted";
    case InputOwner::Hud: return "Hud";
    case InputOwner::Tool: return "Tool";
    case InputOwner::Camera: return "Camera";
  }
  return "Unknown";
}

const char* to_string(GestureKind kind) {
  switch (kind) {
    case GestureKind::None: return "None";
    case GestureKind::Tap: return "Tap";
    case GestureKind::Drag: return "Drag";
    case GestureKind::Pinch: return "Pinch";
    case GestureKind::TwoFingerPan: return "TwoFingerPan";
    case GestureKind::LongPress: return "LongPress";
  }
  return "Unknown";
}

namespace {

constexpr f32 kEps = 1e-6f;

f32 dist(Vec2 a, Vec2 b) {
  const f32 dx = a.x - b.x;
  const f32 dy = a.y - b.y;
  return std::sqrt(dx * dx + dy * dy);
}

// 单指追踪。
struct PointerTrack {
  Vec2 start{};
  Vec2 last{};
  std::int64_t start_us = 0;
  bool moved_past_threshold = false;
  InputOwner owner = InputOwner::Unrouted;
};

}  // namespace

struct InputRouter::Impl {
  InputRouterConfig cfg{};
  InputRouteStats stats{};
  Hud* hud = nullptr;
  IToolSink* tool = nullptr;
  ICameraSink* camera = nullptr;

  // 所有按下的指针(抬起即移除)。无序容器:遍历只用于聚合,不参与判定顺序。
  std::unordered_map<u32, PointerTrack> pointers;

  bool two_finger = false;
  Vec2 pinch_start_centroid{};
  Vec2 pinch_last_centroid{};
  f32 pinch_start_distance = 0.f;
  // 最后一次两指都在场时观察到的间距(end_two_finger 的唯一正确读数)。
  f32 pinch_last_distance = 0.f;
  std::vector<std::int64_t> two_finger_ts{};

  std::vector<Gesture> gestures;

  // -------------------------------------------------- 仲裁(§7 顺序)

  InputOwner arbitrate(const device::InputEvent& ev) {
    // ① HUD 命中 → 消费,不再下发
    if (hud && (ev.kind == device::InputEventKind::PointerDown ||
                ev.kind == device::InputEventKind::PointerMove ||
                ev.kind == device::InputEventKind::PointerUp ||
                ev.kind == device::InputEventKind::PointerCancel)) {
      if (hud->on_input(ev)) return InputOwner::Hud;
    }
    // ② 工具激活
    if (tool && tool->tool_active() && tool->on_input(ev)) return InputOwner::Tool;
    // ③ 相机/场景
    if (camera && camera->on_input(ev)) return InputOwner::Camera;
    return InputOwner::Unrouted;
  }

  // 按 owner 把事件接力给已委派的下游(不改变 owner)。
  void forward(const device::InputEvent& ev, InputOwner owner) {
    switch (owner) {
      case InputOwner::Hud: if (hud) hud->on_input(ev); break;
      case InputOwner::Tool: if (tool) tool->on_input(ev); break;
      case InputOwner::Camera: if (camera) camera->on_input(ev); break;
      case InputOwner::Unrouted: break;
    }
  }

  u32 count_active() const { return static_cast<u32>(pointers.size()); }

  Vec2 centroid() const {
    Vec2 c{};
    if (pointers.empty()) return c;
    for (const auto& kv : pointers) {
      c.x += kv.second.last.x;
      c.y += kv.second.last.y;
    }
    const f32 n = static_cast<f32>(pointers.size());
    return {c.x / n, c.y / n};
  }

  f32 pair_distance() const {
    if (pointers.size() < 2) return 0.f;
    auto it = pointers.begin();
    Vec2 a = it->second.last;
    ++it;
    Vec2 b = it->second.last;
    return dist(a, b);
  }

  void begin_two_finger() {
    two_finger = true;
    pinch_start_centroid = centroid();
    pinch_last_centroid = pinch_start_centroid;
    pinch_start_distance = pair_distance();

    pinch_last_distance = pinch_start_distance;
    two_finger_ts.clear();
    for (const auto& kv : pointers) two_finger_ts.push_back(kv.second.start_us);
  }

  void end_two_finger(Vec2 last_centroid) {
    Gesture g;
    g.pointer_count = 2;
    g.start = pinch_start_centroid;
    g.end = last_centroid;
    g.delta = {last_centroid.x - pinch_start_centroid.x,
               last_centroid.y - pinch_start_centroid.y};
    // **必须用最后一次观察到的双指距离**,不能用"此刻"的 pair_distance():
    // 抬手时 pointers 里已经少了手指,当场算出来的距离会骤降甚至为 0,
    // 于是每一次 pinch 都会被判成"没缩放"。pinch_last_distance 在每次 Move
    // 且两指都在场时更新,是唯一正确的读数。
    const f32 cur = pinch_last_distance;
    g.scale = pinch_start_distance > kEps ? (cur / pinch_start_distance) : 1.f;
    g.owner = InputOwner::Unrouted;
    g.raw_timestamps_us = two_finger_ts;
    g.kind = (std::fabs(g.scale - 1.f) >= cfg.pinch_min_scale_delta) ? GestureKind::Pinch
                                                                     : GestureKind::TwoFingerPan;
    // 双指手势也算"相机/场景交互"的一种;由 camera 认领过任何一次即为 Camera。
    for (auto& kv : pointers) {
      if (kv.second.owner == InputOwner::Camera) {
        g.owner = InputOwner::Camera;
        break;
      }
    }
    gestures.push_back(std::move(g));
    two_finger = false;
  }

  // -------------------------------------------------- 事件处理

  void on_down(const device::InputEvent& ev) {
    // 第二根手指按下:进入双指模式(单指已确定的那段作废为双指手势的一部分)
    if (count_active() >= 1 && !two_finger) {
      PointerTrack t{from_device(ev.position), from_device(ev.position), ev.timestamp_us, false, InputOwner::Unrouted};
      pointers[ev.pointer_id] = t;
      begin_two_finger();
      return;
    }
    PointerTrack t{from_device(ev.position), from_device(ev.position), ev.timestamp_us, false, InputOwner::Unrouted};
    pointers[ev.pointer_id] = t;
    if (two_finger) return;  // 三指以上:并入双指,不单独仲裁
    // 单指:仲裁一次,结果写进本指针
    pointers[ev.pointer_id].owner = arbitrate(ev);
  }

  void on_move(const device::InputEvent& ev) {
    auto it = pointers.find(ev.pointer_id);
    if (it == pointers.end()) return;
    PointerTrack& t = it->second;
    t.last = from_device(ev.position);

    if (two_finger) {
      pinch_last_centroid = centroid();
      // 两指都在场时刷新间距:这是 pinch 结束时唯一可信的读数(见 end_two_finger)。
      if (pointers.size() >= 2) pinch_last_distance = pair_distance();
      forward(ev, InputOwner::Camera);  // 双指手势统一喂给相机层
      return;
    }
    const f32 threshold = cfg.drag_threshold_dp;
    if (!t.moved_past_threshold && dist(t.start, t.last) > threshold) {
      t.moved_past_threshold = true;
    }
    forward(ev, t.owner);
  }

  void on_up(const device::InputEvent& ev) {
    auto it = pointers.find(ev.pointer_id);
    if (it == pointers.end()) return;
    const PointerTrack t = it->second;
    pointers.erase(it);

    if (two_finger) {
      forward(ev, InputOwner::Camera);
      // 抬起后仍剩一根 = 双指结束,成型一次;两根都抬起时不再重复成型。
      if (count_active() == 1) end_two_finger(t.last);
      else if (count_active() == 0) {
        // 同时抬起(cancel 等):用当前双指状态成型
        end_two_finger(pinch_last_centroid);
      }
      return;
    }

    forward(ev, t.owner);

    Gesture g;
    g.owner = t.owner;
    g.start = t.start;
    g.end = from_device(ev.position);
    g.delta = {ev.position.x - t.start.x, ev.position.y - t.start.y};
    g.duration_us = ev.timestamp_us - t.start_us;
    g.pointer_count = 1;
    g.raw_timestamps_us = {t.start_us, ev.timestamp_us};
    if (t.moved_past_threshold) {
      g.kind = GestureKind::Drag;
    } else if (g.duration_us >= cfg.long_press_us) {
      g.kind = GestureKind::LongPress;
    } else {
      g.kind = GestureKind::Tap;
    }
    gestures.push_back(std::move(g));
  }

  void on_cancel(const device::InputEvent& ev) {
    auto it = pointers.find(ev.pointer_id);
    if (it == pointers.end()) return;
    forward(ev, it->second.owner);
    pointers.erase(it);
    if (two_finger && count_active() == 0) two_finger = false;
  }

  void tally(InputOwner owner) {
    switch (owner) {
      case InputOwner::Hud: ++stats.routed_hud; break;
      case InputOwner::Tool: ++stats.routed_tool; break;
      case InputOwner::Camera: ++stats.routed_camera; break;
      case InputOwner::Unrouted: ++stats.unrouted; break;
    }
  }
};

InputRouter::InputRouter() : impl_(std::make_unique<Impl>()) {}
InputRouter::~InputRouter() = default;

void InputRouter::set_config(const InputRouterConfig& cfg) { impl_->cfg = cfg; }
const InputRouterConfig& InputRouter::config() const { return impl_->cfg; }

void InputRouter::set_hud(Hud* hud) { impl_->hud = hud; }
void InputRouter::set_tool(IToolSink* tool) { impl_->tool = tool; }
void InputRouter::set_camera(ICameraSink* camera) { impl_->camera = camera; }

InputOwner InputRouter::route(const device::InputEvent& ev) {
  ++impl_->stats.events_total;
  InputOwner owner = InputOwner::Unrouted;

  switch (ev.kind) {
    case device::InputEventKind::PointerDown:
      impl_->on_down(ev);
      owner = impl_->pointers.count(ev.pointer_id) ? impl_->pointers[ev.pointer_id].owner
                                                   : InputOwner::Unrouted;
      break;
    case device::InputEventKind::PointerMove: {
      // 先取归属(App::on_move 会把 last 更新到新位置,但 owner 不变)
      auto it = impl_->pointers.find(ev.pointer_id);
      owner = it != impl_->pointers.end() ? it->second.owner : InputOwner::Unrouted;
      impl_->on_move(ev);
      break;
    }
    case device::InputEventKind::PointerUp: {
      auto it = impl_->pointers.find(ev.pointer_id);
      const InputOwner prev = it != impl_->pointers.end() ? it->second.owner : InputOwner::Unrouted;
      impl_->on_up(ev);
      owner = prev;
      break;
    }
    case device::InputEventKind::PointerCancel: {
      auto it = impl_->pointers.find(ev.pointer_id);
      const InputOwner prev = it != impl_->pointers.end() ? it->second.owner : InputOwner::Unrouted;
      impl_->on_cancel(ev);
      owner = prev;
      break;
    }
    case device::InputEventKind::Resize:
    case device::InputEventKind::Focus: {
      // 尺寸/焦点变化不是输入仲裁项,但它们会打断手势(直接丢弃未完成的手势)
      impl_->pointers.clear();
      impl_->two_finger = false;
      break;
    }
    default:
      break;
  }

  // 归属统计:每个被路由的指针事件都恰好计一次(spec §7 要求不丢事件)。
  const bool is_pointer = ev.kind == device::InputEventKind::PointerDown ||
                          ev.kind == device::InputEventKind::PointerMove ||
                          ev.kind == device::InputEventKind::PointerUp ||
                          ev.kind == device::InputEventKind::PointerCancel;
  if (is_pointer) {
    impl_->tally(owner);
    if (owner == InputOwner::Unrouted) ++impl_->stats.dropped;
  }
  return owner;
}

const std::vector<Gesture>& InputRouter::gestures() const { return impl_->gestures; }

void InputRouter::end_frame() { impl_->stats.gestures_emitted = static_cast<u32>(impl_->gestures.size()); }

const InputRouteStats& InputRouter::stats() const { return impl_->stats; }

}  // namespace magicblocks::ui
