// MagicBlocks L4 —— 魔法涂鸦：画布 + 输入链路实现
//
// 见头文件顶部：本文件**零投影数学**，屏幕↔世界的正反变换只调 L3 的
// `ICamera::screen_to_ray` 与 `primitives::intersect_ray_plane`。
#include "magicblocks/features/sketch/sketch_canvas.h"

#include <cmath>

namespace magicblocks::features::sketch {
namespace {

pm::Mat4 to_mat4(const float m[16]) {
  pm::Mat4 out;
  for (int i = 0; i < 16; ++i) out.m[i] = m[i];
  return out;
}

}  // namespace

Canvas::Canvas(const CanvasParams& params) : params_(params) {}

pm::Mat4 Canvas::transform() const { return to_mat4(params_.transform); }

pm::Vec3 Canvas::normal() const {
  // 画布法线 = 变换矩阵的第三列（列主序 m[8..10]），归一化。
  // 注意不能只取 m[8..10] 就用：画布可能带非均匀缩放。
  const pm::Mat4 t = transform();
  return pm::normalize(pm::Vec3{t.m[8], t.m[9], t.m[10]});
}

pm::Vec3 Canvas::center() const {
  const pm::Mat4 t = transform();
  return pm::Vec3{t.m[12], t.m[13], t.m[14]};
}

pm::Status Canvas::set_params(const CanvasParams& params) {
  if (!(params.width > 0.0f) || !(params.height > 0.0f)) {
    return pm::Status::Invalid("画布宽/高必须为正");
  }
  if (!(params.extrude_thickness >= 0.0f)) {
    return pm::Status::Invalid("画布挤出厚度不得为负");
  }
  if (!(params.min_spacing >= 0.0f)) {
    return pm::Status::Invalid("画布最小采样间距不得为负");
  }
  const pm::Mat4 t = to_mat4(params.transform);
  bool ok = false;
  const pm::Mat4 inv = t.inverse(&ok);
  if (!ok) return pm::Status::Invalid("画布变换矩阵奇异(不可逆)");
  // 法线退化（第三列为零向量）同样拒绝：那样的"画布"没有平面可求交。
  if (pm::length(pm::Vec3{t.m[8], t.m[9], t.m[10]}) <= 1e-9f) {
    return pm::Status::Invalid("画布变换的第三列为零:画布没有法线");
  }
  (void)inv;
  params_ = params;
  return pm::Status::Ok_();
}

CanvasHit Canvas::intersect_screen(const pm::ICamera& camera, float px, float py) const {
  CanvasHit hit;

  // ① 屏幕像素 → 世界射线。**唯一**的屏幕→世界入口（D-SK-5 / D-L3-4）。
  const pm::Ray ray = camera.screen_to_ray(px, py);

  // ② 射线 ∩ 画布平面。平面方程 n·p + d = 0，n 归一化后 d = -n·center。
  const pm::Vec3 n = normal();
  const pm::Plane plane{n, -pm::dot(n, center())};
  float t = 0.0f;
  // t_min = 0：交点在相机**背后**的射线不算命中（透视相机下 screen_to_ray
  // 的起点是相机位置，t<0 就是背后的方向）。
  if (!pm::intersect_ray_plane(ray, plane, &t, 0.0f)) {
    // 平行或点在背后 —— 两者都返回未命中，由调用方计数后**显式**处理，
    // 不在这里编一个"最近的画布点"糊过去（那就是静默降级）。
    return hit;
  }

  hit.world = ray.origin + ray.direction * t;

  // ③ 世界 → 画布局部坐标。求逆在这里做（画布参数是场景常量，
  //    每帧不重复求逆；求逆用的是 L3 Mat4 的**唯一**实现，不另写一份）。
  bool ok = false;
  const pm::Mat4 inv = transform().inverse(&ok);
  if (!ok) return hit;  // set_params 已经拒绝奇异矩阵，这里只是兜底
  const pm::Vec3 local = inv.transform_point(hit.world);
  hit.local = pm::Vec2{local.x, local.y};
  hit.hit = true;
  return hit;
}

pm::Vec3 Canvas::local_to_world(const pm::Vec2& uv) const {
  return transform().transform_point(pm::Vec3{uv.x, uv.y, 0.0f});
}

pm::Vec2 Canvas::clamp_local(const pm::Vec2& uv) const {
  if (!params_.clamp_to_canvas) return uv;
  const float hw = params_.width * 0.5f;
  const float hh = params_.height * 0.5f;
  pm::Vec2 out = uv;
  if (out.x < -hw) out.x = -hw;
  if (out.x > hw) out.x = hw;
  if (out.y < -hh) out.y = -hh;
  if (out.y > hh) out.y = hh;
  return out;
}

bool Canvas::inside(const pm::Vec2& uv) const {
  const float hw = params_.width * 0.5f;
  const float hh = params_.height * 0.5f;
  return uv.x >= -hw && uv.x <= hw && uv.y >= -hh && uv.y <= hh;
}

void Canvas::append_sample(const pm::Vec2& local, float px, float py) {
  if (params_.min_spacing > 0.0f && !active_points_.empty()) {
    const pm::Vec2 d = local - active_points_.back();
    if (pm::length(d) < params_.min_spacing) {
      ++stats_.samples_dropped_spacing;
      return;
    }
  }
  if (params_.max_points_per_stroke != 0 &&
      active_points_.size() >= params_.max_points_per_stroke) {
    ++stats_.samples_dropped_limit;
    return;
  }
  active_points_.push_back(local);
  active_screen_.push_back(pm::Vec2{px, py});
  ++stats_.samples_accepted;
}

void Canvas::cancel_current() {
  active_ = false;
  active_points_.clear();
  active_screen_.clear();
  active_closed = false;
}

void Canvas::reset_active() {
  active_ = false;
  active_pointer_ = 0;
  active_points_.clear();
  active_screen_.clear();
  active_closed = false;
}

bool Canvas::sample_screen(const pm::ICamera& camera, float px, float py) {
  const CanvasHit h = intersect_screen(camera, px, py);
  if (!h.hit) {
    ++stats_.samples_missed_plane;
    return false;
  }
  const bool was_inside = inside(h.local);
  if (params_.clamp_to_canvas) {
    if (!was_inside) ++stats_.samples_clamped;
    append_sample(clamp_local(h.local), px, py);
    return true;
  }
  if (!was_inside) {
    // 不夹取时，画布外的点**不采样**：它不属于这张画布。
    // 记进统计，由调用方根据"这一笔有没有采到点"显式处理。
    ++stats_.samples_off_canvas_rejected;
    return false;
  }
  append_sample(h.local, px, py);
  return true;
}

Canvas::StrokeOutcome Canvas::feed(const pm::ICamera& camera, const pm::StrokeEvent& event,
                                  Stroke stroke_template) {
  StrokeOutcome out;

  switch (event.kind) {
    case pm::StrokeEventKind::Begin: {
      // 上一笔没收到 End 就来了新的 Begin：把已采样的部分**提交**，
      // 而不是直接丢掉（丢几何会让用户看到"刚画的东西凭空消失"）。
      StrokeOutcome pending;
      if (active_) {
        const pm::StrokeEvent end_ev{pm::StrokeEventKind::End, active_pointer_, event.x, event.y,
                                     event.timestamp_us};
        pending = feed(camera, end_ev, std::move(stroke_template));
      }
      reset_active();
      active_ = true;
      active_pointer_ = event.pointer_id;
      sample_screen(camera, event.x, event.y);
      // Begin 的返回值只携带"隐式提交的上一笔"；本笔尚未提交。
      return (pending.kind == StrokeOutcome::Kind::Committed) ? pending : StrokeOutcome{};
    }

    case pm::StrokeEventKind::Move: {
      if (!active_ || event.pointer_id != active_pointer_) {
        // Idle 收到 Move 是非法转移：忽略并计数（设计 §7.1）。
        ++stats_.illegal_transitions;
        return out;
      }
      sample_screen(camera, event.x, event.y);
      return out;
    }

    case pm::StrokeEventKind::End: {
      if (!active_ || event.pointer_id != active_pointer_) {
        ++stats_.illegal_transitions;
        return out;
      }
      // End 事件本身可能带一个最终位置，也要收进来（否则末段少一截）。
      sample_screen(camera, event.x, event.y);
      out.stroke = std::move(stroke_template);
      out.stroke.points = active_points_;
      out.stroke.closed = active_closed;
      out.stroke.max_points = params_.max_points_per_stroke;
      out.kind = active_points_.empty() ? StrokeOutcome::Kind::Empty
                                        : StrokeOutcome::Kind::Committed;
      reset_active();
      return out;
    }

    case pm::StrokeEventKind::Cancel: {
      if (!active_ || event.pointer_id != active_pointer_) {
        ++stats_.illegal_transitions;
        return out;
      }
      // Cancel：整笔丢弃，一个采样点都不提交（设计 §7.1）。
      cancel_current();
      out.kind = StrokeOutcome::Kind::Cancelled;
      return out;
    }
  }
  ++stats_.illegal_transitions;
  return out;
}

// ---------------------------------------------------------------- 事件映射

bool map_input(const device::InputEvent& in, pm::StrokeEvent* out) {
  pm::StrokeEvent ev;
  ev.pointer_id = in.pointer_id;
  ev.x = in.position.x;
  ev.y = in.position.y;
  ev.timestamp_us = in.timestamp_us;  // D6：时间戳原样带过来，不读时钟

  switch (in.kind) {
    case device::InputEventKind::PointerDown:
      // 只认主键/主触点。右键/中指在 UI 里是别的语义（旋转相机等），
      // 当成画笔画会出现"拖动相机顺带画了一条线"。
      if (in.button != 0) return false;
      ev.kind = pm::StrokeEventKind::Begin;
      break;
    case device::InputEventKind::PointerMove:
      ev.kind = pm::StrokeEventKind::Move;
      break;
    case device::InputEventKind::PointerUp:
      if (in.button != 0) return false;
      ev.kind = pm::StrokeEventKind::End;
      break;
    case device::InputEventKind::PointerCancel:
      ev.kind = pm::StrokeEventKind::Cancel;
      break;
    default:
      return false;  // KeyDown / Resize / Focus / Pause ... 不是绘制事件
  }
  *out = ev;
  return true;
}

InputMapStats map_input_stream(const device::InputEvent* events, std::size_t count,
                               std::vector<pm::StrokeEvent>* out) {
  InputMapStats st;
  if (events == nullptr) return st;
  for (std::size_t i = 0; i < count; ++i) {
    const device::InputEvent& in = events[i];
    ++st.total;
    pm::StrokeEvent ev;
    if (!map_input(in, &ev)) {
      ++st.ignored;
      const bool is_pointer = in.kind == device::InputEventKind::PointerDown ||
                              in.kind == device::InputEventKind::PointerUp ||
                              in.kind == device::InputEventKind::PointerMove ||
                              in.kind == device::InputEventKind::PointerCancel;
      if (is_pointer) ++st.wrong_button;
      continue;
    }
    ++st.mapped;
    out->push_back(ev);
  }
  return st;
}

// ---------------------------------------------------------------- 与 L3 IPainter 的接线

PainterFeedResult feed_painter_and_canvas(pm::IPainter& painter, Canvas& canvas,
                                          const pm::ICamera& camera,
                                          const pm::StrokeEvent* events, std::size_t count,
                                          Stroke stroke_template) {
  PainterFeedResult res;
  const std::uint32_t before = painter.stats().segments_appended;
  for (std::size_t i = 0; i < count; ++i) {
    // 顺序很重要：先喂画笔（L3 增量几何），再喂画布采样。
    // 两者都只看事件本身，互不依赖，但固定顺序让 stats 可复现。
    painter.feed(events[i]);
    ++res.events_drawn;
    Canvas::StrokeOutcome oc = canvas.feed(camera, events[i], stroke_template);
    if (oc.kind == Canvas::StrokeOutcome::Kind::Committed) {
      res.committed.push_back(std::move(oc.stroke));
    }
  }
  res.painter_segments_appended = painter.stats().segments_appended - before;
  return res;
}

}  // namespace magicblocks::features::sketch
