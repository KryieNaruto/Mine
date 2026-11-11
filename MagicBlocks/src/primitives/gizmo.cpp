// MagicBlocks L3 —— IGizmoSystem 实现（spec §10）
//
// 显式状态机（可断言，见 §10.4）：
//
//   Idle ──hit──► Hovered ──down──► Dragging ──up──► Idle
//     ▲              │                  │
//     └── leave ─────┘                  └── cancel ──► Idle（发 DragCancel）
//
// 全部状态集中在 `state_` 一个结构里，任何时刻都能 `snapshot()` 出来，
// 因此这个状态机是可测试、可回放的 —— 而不是散在若干 bool 里。

#include "mb_internal/magicblocks/primitives/gizmo.h"

#include <algorithm>
#include <cmath>

namespace magicblocks::primitives {
namespace {

constexpr float kPi = 3.14159265358979323846f;

struct Target {
  std::uint32_t handle = 0;
  GizmoTargetDesc desc{};
};

struct State {
  std::uint32_t active_target = 0;
  std::uint8_t hovered_axis = IGizmoSystem::kNoAxis;
  std::uint8_t dragging_axis = IGizmoSystem::kNoAxis;
  bool is_dragging = false;
  float drag_total[3] = {0.0f, 0.0f, 0.0f};
  std::uint64_t event_counter = 0;
};

// 把屏幕点投到过 gizmo 中心的平面上，拿世界点 —— 拖动的统一算法。
// 三种模式都靠它：平移取位移、旋转取角度、缩放取投影长度比。
bool project_to_plane(const ICamera& cam, const Vec3& plane_center, const Vec3& plane_normal,
                      float px, float py, Vec3* out) {
  const Ray ray = cam.screen_to_ray(px, py);
  const float denom = dot(plane_normal, ray.direction);
  if (std::fabs(denom) < 1e-6f) return false;  // 射线与平面平行
  const float t = dot(plane_normal, plane_center - ray.origin) / denom;
  if (t <= 0.0f) return false;  // 平面在相机后面
  *out = ray.origin + ray.direction * t;
  return true;
}

class GizmoSystem final : public IGizmoSystem {
 public:
  // 基类删了拷贝构造，隐式默认构造因此也被删除 —— 显式写出来。
  GizmoSystem() = default;

  // ---- 目标 ----

  std::uint32_t configure(const GizmoTargetDesc& desc) override {
    Target t;
    t.handle = next_handle_++;
    t.desc = desc;
    targets_.push_back(t);
    if (state_.active_target == 0) state_.active_target = t.handle;
    return t.handle;
  }

  void release(std::uint32_t target) override {
    targets_.erase(std::remove_if(targets_.begin(), targets_.end(),
                                  [target](const Target& t) { return t.handle == target; }),
                   targets_.end());
    if (state_.active_target == target) {
      state_.active_target = targets_.empty() ? 0 : targets_.front().handle;
      if (state_.is_dragging) cancel_drag();
    }
  }

  void update_target(std::uint32_t target, const GizmoTargetDesc& desc) override {
    for (Target& t : targets_) {
      if (t.handle == target) {
        t.desc = desc;
        return;
      }
    }
  }

  void set_active_target(std::uint32_t target) override {
    if (target != state_.active_target) {
      // 切目标等于放弃当前交互，否则会把位移记到别的物体上。
      if (state_.is_dragging) cancel_drag();
      state_.active_target = target;
      state_.hovered_axis = kNoAxis;
    }
  }
  std::uint32_t active_target() const override { return state_.active_target; }

  // ---- 拾取（R4：单点，像素阈值）----

  std::uint8_t pick_axis(std::uint32_t target, float px, float py,
                         const ICamera& camera) const override {
    const Target* t = find(target);
    if (t == nullptr) return kNoAxis;

    const Vec3 center{t->desc.center[0], t->desc.center[1], t->desc.center[2]};

    // 命中阈值按屏幕像素给：轴的可点击宽度 = 半宽像素，与相机无关。
    float best_dist2 = kPickRadiusPx * kPickRadiusPx;
    std::uint8_t best = kNoAxis;

    if (t->desc.mode == GizmoMode::Rotate) {
      // 旋转：点在环上（到中心的屏幕距离 ≈ 环半径）即命中。
      for (const GizmoAxis& a : t->desc.axes) {
        float cx = 0.0f;
        float cy = 0.0f;
        float depth = 0.0f;
        if (!camera.world_to_screen(center, &cx, &cy, &depth)) continue;
        const float dx = px - cx;
        const float dy = py - cy;
        const float r = std::sqrt(dx * dx + dy * dy);
        const float ring_r = ring_radius_px(t->desc, camera, center, a);
        const float d = std::fabs(r - ring_r);
        if (d < kPickRadiusPx && d < best_dist2) {
          best_dist2 = d;
          best = a.id;
        }
      }
      return best;
    }

    // 平移/缩放：点到轴线段（屏幕上）的距离
    for (const GizmoAxis& a : t->desc.axes) {
      Vec2 p0;
      Vec2 p1;
      if (!axis_screen_segment(t->desc, camera, center, a, &p0, &p1)) continue;
      const float d2 = point_segment_dist2(px, py, p0, p1);
      if (d2 < best_dist2) {
        best_dist2 = d2;
        best = a.id;
      }
    }
    return best;
  }

  // ---- 输入（唯一入口）----

  bool on_input(const device::InputEvent& e, const ICamera& camera) override {
    const Target* t = find(state_.active_target);
    if (t == nullptr) return false;

    switch (e.kind) {
      case device::InputEventKind::PointerMove:
      case device::InputEventKind::PointerDown:
      case device::InputEventKind::PointerUp:
      case device::InputEventKind::PointerCancel:
        break;
      case device::InputEventKind::Focus:
        // 失焦：放弃拖拽（否则会拿着一个永远等不到 up 的状态）
        if (state_.is_dragging) {
          cancel_drag();
          return true;
        }
        return false;
      default:
        return false;
    }

    if (e.kind == device::InputEventKind::PointerDown) {
      const std::uint8_t axis = pick_axis(state_.active_target, e.position.x, e.position.y, camera);
      if (axis == kNoAxis) return false;  // 没点中轴 → 不消费，玩法可以处理
      state_.dragging_axis = axis;
      state_.is_dragging = true;
      state_.drag_total[0] = 0.0f;
      state_.drag_total[1] = 0.0f;
      state_.drag_total[2] = 0.0f;
      last_px_ = e.position.x;
      last_py_ = e.position.y;
      emit(GizmoEvent::Kind::DragBegin, e, axis, Vec3{0.0f, 0.0f, 0.0f});
      return true;
    }

    if (e.kind == device::InputEventKind::PointerMove) {
      if (state_.is_dragging) {
        const GizmoAxis* axis_data = find_axis(*t, state_.dragging_axis);
        if (axis_data == nullptr) return true;
        const Vec3 delta = compute_delta(*t, *axis_data, e.position.x, e.position.y, camera);
        state_.drag_total[0] += delta.x;
        state_.drag_total[1] += delta.y;
        state_.drag_total[2] += delta.z;
        last_px_ = e.position.x;
        last_py_ = e.position.y;
        emit(GizmoEvent::Kind::Drag, e, state_.dragging_axis, delta);
        return true;
      }
      // 未拖拽：更新悬停
      const std::uint8_t axis = pick_axis(state_.active_target, e.position.x, e.position.y, camera);
      if (axis != state_.hovered_axis) {
        if (state_.hovered_axis != kNoAxis) {
          emit(GizmoEvent::Kind::HoverEnd, e, state_.hovered_axis, Vec3{0.0f, 0.0f, 0.0f});
        }
        state_.hovered_axis = axis;
        if (axis != kNoAxis) {
          emit(GizmoEvent::Kind::HoverBegin, e, axis, Vec3{0.0f, 0.0f, 0.0f});
        }
        return axis != kNoAxis;
      }
      return axis != kNoAxis;
    }

    if (e.kind == device::InputEventKind::PointerUp) {
      if (!state_.is_dragging) return false;
      emit(GizmoEvent::Kind::DragEnd, e, state_.dragging_axis, Vec3{0.0f, 0.0f, 0.0f});
      reset_drag();
      return true;
    }

    // PointerCancel
    if (state_.is_dragging) {
      cancel_drag();
      return true;
    }
    return false;
  }

  bool dragging() const override { return state_.is_dragging; }

  void cancel_drag() override {
    if (!state_.is_dragging) return;
    GizmoEvent ev;
    ev.kind = GizmoEvent::Kind::DragCancel;
    ev.target = state_.active_target;
    ev.axis = state_.dragging_axis;
    ev.timestamp_us = 0;
    ++state_.event_counter;
    dispatch(ev);
    reset_drag();
  }

  // ---- 订阅 ----

  void subscribe(IGizmoListener* l) override {
    if (l == nullptr) return;
    if (std::find(listeners_.begin(), listeners_.end(), l) == listeners_.end()) {
      listeners_.push_back(l);
    }
  }

  void unsubscribe(IGizmoListener* l) override {
    listeners_.erase(std::remove(listeners_.begin(), listeners_.end(), l), listeners_.end());
  }

  // ---- 渲染（只 push 覆盖层图元，不提交）----

  Status build_draw(const ICamera& camera, IOverlay2D& overlay) const override {
    const Target* t = find(state_.active_target);
    if (t == nullptr) return Status::Ok_();

    const Vec3 center{t->desc.center[0], t->desc.center[1], t->desc.center[2]};
    float cx = 0.0f;
    float cy = 0.0f;
    float depth = 0.0f;
    if (!camera.world_to_screen(center, &cx, &cy, &depth)) return Status::Ok_();  // 在相机后

    // 中心小圆点：让"目标在哪"一眼可见，也是拖拽的视觉锚点
    RingDesc dot;
    dot.center = {cx, cy};
    dot.radius_px = 4.0f;
    dot.width_px = 2.0f;
    dot.segments = 16;
    dot.color = Color4{1.0f, 1.0f, 1.0f, 0.9f};
    overlay.push_ring(dot);

    for (const GizmoAxis& a : t->desc.axes) {
      // 当前正在拖的轴 / 悬停的轴：加亮。这是唯一的"视觉状态"分支，
      // 且它只影响颜色，不影响任何逻辑。
      Color4 color = a.color;
      if (state_.is_dragging && a.id == state_.dragging_axis) {
        color = Color4{1.0f, 1.0f, 0.30f, 1.0f};
      } else if (!state_.is_dragging && a.id == state_.hovered_axis) {
        color = Color4{1.0f, 0.85f, 0.35f, 1.0f};
      }

      if (t->desc.mode == GizmoMode::Rotate) {
        RingDesc ring;
        ring.center = {cx, cy};
        ring.radius_px = ring_radius_px(t->desc, camera, center, a);
        ring.width_px = 2.5f;
        ring.segments = 48;
        ring.color = color;
        overlay.push_ring(ring);
        continue;
      }

      Vec2 p0;
      Vec2 p1;
      if (!axis_screen_segment(t->desc, camera, center, a, &p0, &p1)) continue;

      LineDesc line;
      line.from = p0;
      line.to = p1;
      line.width_px = 2.5f;
      line.color = color;
      overlay.push_line(line);

      // 轴尖的箭头（平移/缩放）：用屏幕空间小三角，尺寸恒定像素
      const float dx = p1.x - p0.x;
      const float dy = p1.y - p0.y;
      const float len = std::sqrt(dx * dx + dy * dy);
      if (len > 1e-3f) {
        const float ux = dx / len;
        const float uy = dy / len;
        const float arrow = 9.0f;
        const float half = 4.5f;
        TriDesc tri;
        tri.a = p1;
        tri.b = {p1.x - ux * arrow + (-uy) * half, p1.y - uy * arrow + ux * half};
        tri.c = {p1.x - ux * arrow - (-uy) * half, p1.y - uy * arrow - ux * half};
        tri.color = color;
        overlay.push_tri(tri);
      }

      // 缩放模式额外画一个方块把手，与平移区分开
      if (t->desc.mode == GizmoMode::Scale) {
        RectDesc box;
        box.min = {p1.x - 3.5f, p1.y - 3.5f};
        box.max = {p1.x + 3.5f, p1.y + 3.5f};
        box.color = color;
        box.border_px = 0.0f;
        overlay.push_rect(box);
      }
    }
    return Status::Ok_();
  }

  // ---- 命令流 ----

  GizmoSnapshot snapshot() const override {
    GizmoSnapshot s;
    s.active_target = state_.active_target;
    s.hovered_axis = state_.hovered_axis;
    s.dragging_axis = state_.dragging_axis;
    s.is_dragging = state_.is_dragging;
    s.drag_total[0] = state_.drag_total[0];
    s.drag_total[1] = state_.drag_total[1];
    s.drag_total[2] = state_.drag_total[2];
    s.event_counter = state_.event_counter;
    return s;
  }

  Status restore(const GizmoSnapshot& s) override {
    state_.active_target = s.active_target;
    state_.hovered_axis = s.hovered_axis;
    state_.dragging_axis = s.dragging_axis;
    state_.is_dragging = s.is_dragging;
    state_.drag_total[0] = s.drag_total[0];
    state_.drag_total[1] = s.drag_total[1];
    state_.drag_total[2] = s.drag_total[2];
    state_.event_counter = s.event_counter;
    return Status::Ok_();
  }

  float axis_world_length(std::uint32_t target, std::uint8_t axis,
                          const ICamera& camera) const override {
    const Target* t = find(target);
    if (t == nullptr) return 0.0f;
    const GizmoAxis* a = find_axis(*t, axis);
    if (a == nullptr) return 0.0f;
    const Vec3 center{t->desc.center[0], t->desc.center[1], t->desc.center[2]};
    // R3：屏幕恒定尺寸的唯一来源是相机的 world_size_per_pixel。
    // gizmo 自己**不推导**距离→大小，只调相机。
    const float wpp = camera.world_size_per_pixel(center);
    return t->desc.screen_radius_px * wpp * a->extent;
  }

 private:
  static constexpr float kPickRadiusPx = 8.0f;

  const Target* find(std::uint32_t handle) const {
    for (const Target& t : targets_) {
      if (t.handle == handle) return &t;
    }
    return nullptr;
  }

  static const GizmoAxis* find_axis(const Target& t, std::uint8_t id) {
    for (const GizmoAxis& a : t.desc.axes) {
      if (a.id == id) return &a;
    }
    return nullptr;
  }

  static float ring_radius_px(const GizmoTargetDesc& desc, const ICamera& camera,
                              const Vec3& center, const GizmoAxis& a) {
    (void)camera;
    (void)center;
    return desc.screen_radius_px * a.ring_radius_scale;
  }

  // 轴在屏幕上的两端点。世界端 = 中心 + 方向 * axis_world_length。
  static bool axis_screen_segment(const GizmoTargetDesc& desc, const ICamera& camera,
                                  const Vec3& center, const GizmoAxis& a, Vec2* p0, Vec2* p1) {
    float sx0 = 0.0f;
    float sy0 = 0.0f;
    float d0 = 0.0f;
    if (!camera.world_to_screen(center, &sx0, &sy0, &d0)) return false;

    const float wpp = camera.world_size_per_pixel(center);
    const float world_len = desc.screen_radius_px * wpp * a.extent;
    const Vec3 dir = normalize_dir(a.direction);
    const Vec3 tip = center + dir * world_len;

    float sx1 = 0.0f;
    float sy1 = 0.0f;
    float d1 = 0.0f;
    if (!camera.world_to_screen(tip, &sx1, &sy1, &d1)) return false;

    *p0 = {sx0, sy0};
    *p1 = {sx1, sy1};
    return true;
  }

  static Vec3 normalize_dir(const float d[3]) {
    const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (len < 1e-6f) return Vec3{1.0f, 0.0f, 0.0f};
    return Vec3{d[0] / len, d[1] / len, d[2] / len};
  }

  // 把屏幕移动换算成沿轴的世界增量。三种模式共用这套投影：
  // 都把鼠标投到"过中心、法线含轴"的平面上，再取沿轴分量。
  Vec3 compute_delta(const Target& t, const GizmoAxis& a, float px, float py,
                     const ICamera& camera) const {
    const Vec3 center{t.desc.center[0], t.desc.center[1], t.desc.center[2]};
    const Vec3 axis = normalize_dir(a.direction);

    // 拖拽平面：法线 = 从中心指向相机的方向（屏幕平面法线）。
    // 这样鼠标每一点移动都能投射到平面上，不会因为轴与视线近乎平行而失控。
    const Vec3 cam_pos = camera.pose().position;
    Vec3 plane_n = cam_pos - center;
    const float ln = std::sqrt(dot(plane_n, plane_n));
    if (ln < 1e-6f) {
      plane_n = Vec3{0.0f, 0.0f, 1.0f};
    } else {
      plane_n = plane_n * (1.0f / ln);
    }

    Vec3 cur;
    if (!project_to_plane(camera, center, plane_n, px, py, &cur)) return Vec3{0.0f, 0.0f, 0.0f};
    Vec3 prev;
    if (!project_to_plane(camera, center, plane_n, last_px_, last_py_, &prev)) {
      return Vec3{0.0f, 0.0f, 0.0f};
    }

    const Vec3 diff = cur - prev;
    switch (t.desc.mode) {
      case GizmoMode::Translate: {
        // 沿轴分量
        const float along = dot(diff, axis);
        return axis * along;
      }
      case GizmoMode::Rotate: {
        // 绕轴的角度：用平面内两个位置向量相对轴的夹角
        const Vec3 r0 = prev - center;
        const Vec3 r1 = cur - center;
        const Vec3 n = axis;
        const float angle = std::atan2(dot(cross(r0, r1), n), dot(r0, r1));
        return axis * angle;
      }
      case GizmoMode::Scale: {
        const Vec3 r0 = prev - center;
        const Vec3 r1 = cur - center;
        const float l0 = std::sqrt(dot(r0, r0));
        const float l1 = std::sqrt(dot(r1, r1));
        float ratio = 0.0f;
        if (l0 > 1e-6f) ratio = (l1 - l0) / l0;
        return axis * ratio;
      }
    }
    return Vec3{0.0f, 0.0f, 0.0f};
  }

  void emit(GizmoEvent::Kind kind, const device::InputEvent& e, std::uint8_t axis, Vec3 delta) {
    GizmoEvent ev;
    ev.kind = kind;
    ev.target = state_.active_target;
    ev.axis = axis;
    ev.delta[0] = delta.x;
    ev.delta[1] = delta.y;
    ev.delta[2] = delta.z;
    ev.total[0] = state_.drag_total[0];
    ev.total[1] = state_.drag_total[1];
    ev.total[2] = state_.drag_total[2];
    // 时间戳只从事件流抄，绝不读时钟（回放一致性）
    ev.timestamp_us = e.timestamp_us;
    ++state_.event_counter;
    dispatch(ev);
  }

  void dispatch(const GizmoEvent& ev) {
    // 拷一份订阅者列表再遍历：listener 在自己的回调里退订是合法用法，
    // 直接用引用遍历会迭代到已失效的迭代器。
    const std::vector<IGizmoListener*> copy = listeners_;
    for (IGizmoListener* l : copy) {
      if (l != nullptr) l->on_gizmo(ev);
    }
  }

  void reset_drag() {
    state_.is_dragging = false;
    state_.dragging_axis = kNoAxis;
    state_.drag_total[0] = 0.0f;
    state_.drag_total[1] = 0.0f;
    state_.drag_total[2] = 0.0f;
  }

  static float point_segment_dist2(float px, float py, Vec2 a, Vec2 b) {
    const float vx = b.x - a.x;
    const float vy = b.y - a.y;
    const float wx = px - a.x;
    const float wy = py - a.y;
    const float vv = vx * vx + vy * vy;
    float t = 0.0f;
    if (vv > 1e-6f) t = (wx * vx + wy * vy) / vv;
    t = std::max(0.0f, std::min(1.0f, t));
    const float dx = wx - vx * t;
    const float dy = wy - vy * t;
    return dx * dx + dy * dy;
  }

  std::vector<Target> targets_;
  std::vector<IGizmoListener*> listeners_;
  State state_{};
  std::uint32_t next_handle_ = 1;
  float last_px_ = 0.0f;
  float last_py_ = 0.0f;
};

}  // namespace

std::unique_ptr<IGizmoSystem> create_gizmo_system() { return std::make_unique<GizmoSystem>(); }

}  // namespace magicblocks::primitives
