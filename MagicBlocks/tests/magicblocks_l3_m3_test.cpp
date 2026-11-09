// MagicBlocks L3 —— M3 测试：IOverlay2D / IPicker / IGizmoSystem
//
// 这三个模块的共同验收点（spec §14 M3）：
//   - gizmo 在任何相机距离下**像素尺寸恒定**（世界尺寸 ∝ world_size_per_pixel）
//   - 拖动事件序列可录制/回放（事件是值类型、时间戳只来自事件流）
//   - gizmo 的状态可快照/恢复（命令流依赖这条）
//
// 还有一条同样重要但容易漏的：**gizmo 绝不改世界**。
// 下面用"订阅者收到事件后自己去改"来验证解耦方向是对的。

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "magicblocks/device/events.h"
#include "magicblocks/primitives/camera.h"
#include "magicblocks/primitives/gizmo.h"
#include "magicblocks/primitives/overlay2d.h"
#include "magicblocks/primitives/picker.h"
#include "magicblocks/primitives/types.h"

namespace mbp = magicblocks::primitives;
namespace dev = magicblocks::device;

namespace {

// ---------------------------------------------------------------- 工具

mbp::CameraPose pose_at(float x, float y, float z) {
  mbp::CameraPose p;
  p.position = {x, y, z};
  p.target = {0.0f, 0.0f, 0.0f};
  p.up = {0.0f, 1.0f, 0.0f};
  return p;
}

std::unique_ptr<mbp::ICamera> make_camera(float px, float py, float pz, int w = 640, int h = 400) {
  auto cam = mbp::create_camera();
  mbp::CameraDesc d;
  d.kind = mbp::ProjectionKind::Perspective;
  d.fov_y_degrees = 50.0f;
  d.near_plane = 0.1f;
  d.far_plane = 500.0f;
  d.viewport.width = static_cast<mbp::u32>(w);
  d.viewport.height = static_cast<mbp::u32>(h);
  EXPECT_TRUE(cam->set_desc(d).ok());
  EXPECT_TRUE(cam->set_pose(pose_at(px, py, pz)).ok());
  return cam;
}

dev::InputEvent ev(dev::InputEventKind kind, float x, float y, std::int64_t ts) {
  dev::InputEvent e;
  e.kind = kind;
  e.position = {x, y};
  e.timestamp_us = ts;
  return e;
}

// 记录所有 gizmo 事件的订阅者。
struct EventRecorder final : public mbp::IGizmoListener {
  std::vector<mbp::GizmoEvent> events;
  void on_gizmo(const mbp::GizmoEvent& e) override { events.push_back(e); }
  std::size_t count_of(mbp::GizmoEvent::Kind k) const {
    std::size_t n = 0;
    for (const auto& e : events) {
      if (e.kind == k) ++n;
    }
    return n;
  }
};

// 默认三轴目标（由测试自己声明 —— gizmo 内部没有"必须有 XYZ 三轴"的假设）
mbp::GizmoTargetDesc three_axis_desc(mbp::GizmoMode mode, float center_x, float center_y,
                                     float center_z, float radius_px = 80.0f) {
  mbp::GizmoTargetDesc d;
  d.mode = mode;
  d.center[0] = center_x;
  d.center[1] = center_y;
  d.center[2] = center_z;
  d.screen_radius_px = radius_px;

  mbp::GizmoAxis ax;
  ax.id = 0;
  ax.direction[0] = 1.0f;
  ax.direction[1] = 0.0f;
  ax.direction[2] = 0.0f;
  ax.color = {1.0f, 0.2f, 0.2f, 1.0f};
  d.axes.push_back(ax);

  mbp::GizmoAxis ay;
  ay.id = 1;
  ay.direction[0] = 0.0f;
  ay.direction[1] = 1.0f;
  ay.direction[2] = 0.0f;
  ay.color = {0.2f, 1.0f, 0.2f, 1.0f};
  d.axes.push_back(ay);

  mbp::GizmoAxis az;
  az.id = 2;
  az.direction[0] = 0.0f;
  az.direction[1] = 0.0f;
  az.direction[2] = 1.0f;
  az.color = {0.2f, 0.4f, 1.0f, 1.0f};
  d.axes.push_back(az);
  return d;
}

}  // namespace

// ================================================================ IOverlay2D

TEST(Overlay2D, LineProducesOneQuad) {
  auto ov = mbp::create_overlay2d();
  ov->begin_frame();
  ov->push_line(mbp::LineDesc{{10.0f, 10.0f}, {110.0f, 10.0f}, 2.0f, {1, 0, 0, 1}});
  ov->build();
  EXPECT_EQ(ov->stats().line_count, 1u);
  EXPECT_EQ(ov->geometry().vertices.size(), 4u);
  EXPECT_EQ(ov->geometry().indices.size(), 6u);
}

TEST(Overlay2D, LineWidthIsInPixelsNotWorldUnits) {
  // 线宽加倍 → 四边形应变宽，但顶点数不变（宽度只是法线偏移量）
  auto a = mbp::create_overlay2d();
  a->begin_frame();
  a->push_line(mbp::LineDesc{{0.0f, 0.0f}, {100.0f, 0.0f}, 2.0f, {1, 1, 1, 1}});
  a->build();

  auto b = mbp::create_overlay2d();
  b->begin_frame();
  b->push_line(mbp::LineDesc{{0.0f, 0.0f}, {100.0f, 0.0f}, 20.0f, {1, 1, 1, 1}});
  b->build();

  ASSERT_EQ(a->geometry().vertices.size(), 4u);
  ASSERT_EQ(b->geometry().vertices.size(), 4u);
  // 水平线：宽度体现在 y 方向的偏移
  const float ay0 = a->geometry().vertices[0].position[1];
  const float ay1 = a->geometry().vertices[1].position[1];
  const float by0 = b->geometry().vertices[0].position[1];
  const float by1 = b->geometry().vertices[1].position[1];
  EXPECT_NEAR(std::fabs(ay0 - ay1), 2.0f, 1e-4f);
  EXPECT_NEAR(std::fabs(by0 - by1), 20.0f, 1e-4f);
}

TEST(Overlay2D, DashSplitsIntoMultipleSegments) {
  auto ov = mbp::create_overlay2d();
  ov->begin_frame();
  mbp::DashDesc d;
  d.from = {0.0f, 0.0f};
  d.to = {100.0f, 0.0f};
  d.dash_px = 10.0f;
  d.gap_px = 10.0f;
  d.color = {1, 1, 1, 1};
  ov->push_dash(d);
  ov->build();
  // 100px 长、10 实 10 空 → 5 段
  EXPECT_EQ(ov->stats().line_count, 5u);
  EXPECT_EQ(ov->geometry().vertices.size(), 20u);
}

TEST(Overlay2D, FilledRectIsOneQuadBorderedRectIsFourLines) {
  auto ov = mbp::create_overlay2d();
  ov->begin_frame();
  mbp::RectDesc filled;
  filled.min = {0.0f, 0.0f};
  filled.max = {10.0f, 10.0f};
  filled.border_px = 0.0f;
  ov->push_rect(filled);
  ov->build();
  EXPECT_EQ(ov->geometry().vertices.size(), 4u);

  auto ov2 = mbp::create_overlay2d();
  ov2->begin_frame();
  mbp::RectDesc hollow = filled;
  hollow.border_px = 1.0f;
  ov2->push_rect(hollow);
  ov2->build();
  EXPECT_EQ(ov2->geometry().vertices.size(), 16u);  // 4 条边 × 4 顶点
}

TEST(Overlay2D, RingSegmentsControlVertexCount) {
  auto ov = mbp::create_overlay2d();
  ov->begin_frame();
  mbp::RingDesc r;
  r.center = {100.0f, 100.0f};
  r.radius_px = 50.0f;
  r.width_px = 2.0f;
  r.segments = 12;
  ov->push_ring(r);
  ov->build();
  // 12 段，每段一个四边形
  EXPECT_EQ(ov->geometry().vertices.size(), 12u * 4u);
  EXPECT_EQ(ov->geometry().indices.size(), 12u * 6u);
}

TEST(Overlay2D, BeginFrameClearsGeometry) {
  auto ov = mbp::create_overlay2d();
  ov->begin_frame();
  ov->push_line(mbp::LineDesc{{0, 0}, {10, 0}, 1.0f, {1, 1, 1, 1}});
  ov->build();
  EXPECT_EQ(ov->geometry().vertices.size(), 4u);

  ov->begin_frame();
  ov->build();
  EXPECT_EQ(ov->geometry().vertices.size(), 0u);
  EXPECT_EQ(ov->stats().line_count, 0u);
}

TEST(Overlay2D, BuildIsDeterministic) {
  auto run = []() {
    auto ov = mbp::create_overlay2d();
    ov->begin_frame();
    ov->push_line(mbp::LineDesc{{1, 2}, {30, 40}, 3.0f, {0.5f, 0.25f, 1.0f, 1.0f}});
    ov->push_ring(mbp::RingDesc{{50, 50}, 20.0f, 2.0f, 16, {1, 1, 0, 1}});
    ov->build();
    std::uint64_t h = 1469598103934665603ull;
    for (const auto& v : ov->geometry().vertices) {
      const std::uint8_t* p = reinterpret_cast<const std::uint8_t*>(&v);
      for (std::size_t i = 0; i < sizeof(v); ++i) {
        h = (h ^ p[i]) * 1099511628211ull;
      }
    }
    return h;
  };
  EXPECT_EQ(run(), run());
}

// ================================================================ IPicker

TEST(Picker, RayBoxHitAndMiss) {
  auto p = mbp::create_picker();
  mbp::Box box = mbp::Box::of({-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f});

  mbp::Ray hit{{0.0f, 0.0f, 5.0f}, {0.0f, 0.0f, -1.0f}};
  float t = 0.0f;
  ASSERT_TRUE(p->intersect_box(hit, box, &t));
  EXPECT_NEAR(t, 4.0f, 1e-4f);  // 从 z=5 打到 z=1

  mbp::Ray miss{{10.0f, 10.0f, 5.0f}, {0.0f, 0.0f, -1.0f}};
  EXPECT_FALSE(p->intersect_box(miss, box, &t));
}

TEST(Picker, RayBoxFromInsideReturnsZero) {
  auto p = mbp::create_picker();
  mbp::Box box = mbp::Box::of({-1, -1, -1}, {1, 1, 1});
  mbp::Ray inside{{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}};
  float t = -1.0f;
  ASSERT_TRUE(p->intersect_box(inside, box, &t));
  EXPECT_EQ(t, 0.0f);
}

TEST(Picker, RayBoxParallelOutsideIsMiss) {
  // 方向与某轴平行且不在板内 → 必为 miss（slab 法的经典边界）
  auto p = mbp::create_picker();
  mbp::Box box = mbp::Box::of({-1, -1, -1}, {1, 1, 1});
  mbp::Ray r{{0.0f, 5.0f, 0.0f}, {0.0f, 0.0f, -1.0f}};  // y=5 在盒外，方向不沿 y
  float t = 0.0f;
  EXPECT_FALSE(p->intersect_box(r, box, &t));
}

TEST(Picker, TriangleHitIsDoubleSided) {
  auto p = mbp::create_picker();
  const mbp::Vec3 a{0.0f, 0.0f, 0.0f};
  const mbp::Vec3 b{1.0f, 0.0f, 0.0f};
  const mbp::Vec3 c{0.0f, 1.0f, 0.0f};
  float t = 0.0f;
  mbp::Vec3 n;

  // 从 +z 打进去
  mbp::Ray front{{0.2f, 0.2f, 1.0f}, {0.0f, 0.0f, -1.0f}};
  ASSERT_TRUE(p->intersect_triangle(front, a, b, c, &t, &n));
  EXPECT_NEAR(t, 1.0f, 1e-4f);

  // 从 -z 打进来（背面）也必须命中 —— 单面建模的网格要能选中
  mbp::Ray back{{0.2f, 0.2f, -1.0f}, {0.0f, 0.0f, 1.0f}};
  ASSERT_TRUE(p->intersect_triangle(back, a, b, c, &t, &n));
  EXPECT_NEAR(t, 1.0f, 1e-4f);
}

TEST(Picker, TriangleMissOutsideBounds) {
  auto p = mbp::create_picker();
  const mbp::Vec3 a{0.0f, 0.0f, 0.0f};
  const mbp::Vec3 b{1.0f, 0.0f, 0.0f};
  const mbp::Vec3 c{0.0f, 1.0f, 0.0f};
  mbp::Ray r{{0.9f, 0.9f, 1.0f}, {0.0f, 0.0f, -1.0f}};  // 三角形外
  float t = 0.0f;
  EXPECT_FALSE(p->intersect_triangle(r, a, b, c, &t, nullptr));
}

TEST(Picker, PlaneIntersect) {
  auto p = mbp::create_picker();
  mbp::Plane ground;
  ground.normal = {0.0f, 1.0f, 0.0f};
  ground.d = 0.0f;  // y = 0 平面
  mbp::Ray down{{0.0f, 5.0f, 0.0f}, {0.0f, -1.0f, 0.0f}};
  float t = 0.0f;
  ASSERT_TRUE(p->intersect_plane(down, ground, &t));
  EXPECT_NEAR(t, 5.0f, 1e-4f);

  mbp::Ray up{{0.0f, 5.0f, 0.0f}, {0.0f, 1.0f, 0.0f}};
  EXPECT_FALSE(p->intersect_plane(up, ground, &t));  // 背离
}

TEST(Picker, SphereIntersect) {
  auto p = mbp::create_picker();
  mbp::Ray r{{0.0f, 0.0f, 5.0f}, {0.0f, 0.0f, -1.0f}};
  float t = 0.0f;
  mbp::Vec3 n;
  ASSERT_TRUE(p->intersect_sphere(r, {0.0f, 0.0f, 0.0f}, 1.0f, &t, &n));
  EXPECT_NEAR(t, 4.0f, 1e-4f);
  EXPECT_NEAR(n.z, 1.0f, 1e-4f);  // 法线朝来向

  EXPECT_FALSE(p->intersect_sphere(r, {10.0f, 0.0f, 0.0f}, 1.0f, &t, &n));
}

TEST(Picker, ScreenPickHitsCenterOfView) {
  auto cam = make_camera(0.0f, 0.0f, 5.0f);
  auto p = mbp::create_picker();
  mbp::Box box = mbp::Box::of({-1, -1, -1}, {1, 1, 1});
  float t = 0.0f;
  // 视口中心必然打到原点处的盒子
  ASSERT_TRUE(p->pick_aabb_screen(*cam, 320.0f, 200.0f, box, &t));
  EXPECT_GT(t, 0.0f);
  // 屏幕角落打不到
  EXPECT_FALSE(p->pick_aabb_screen(*cam, 5.0f, 5.0f, box, &t));
}

TEST(Picker, ScenePickReturnsNearest) {
  auto cam = make_camera(0.0f, 0.0f, 10.0f);  // 相机在 +z，看向 -z
  auto p = mbp::create_picker();
  p->begin_scene();
  // 距离 = |cam.z - box.z|。z=3..5 距离 5..7（近）；z=-3..-1 距离 11..13（远）。
  p->add_object(1, mbp::Box::of({-1, -1, -3}, {1, 1, -1}));  // 远
  p->add_object(2, mbp::Box::of({-1, -1, 3}, {1, 1, 5}));    // 近

  mbp::PickHit hit;
  ASSERT_TRUE(p->pick_scene(*cam, 320.0f, 200.0f, &hit));
  // 近的那个必须胜出 —— 而且**与登记顺序无关**（下面换个顺序再验一次）
  EXPECT_EQ(hit.entity, 2u);
  EXPECT_LT(hit.distance, 10.0f);
  EXPECT_EQ(p->stats().entities_tested, 2u);

  auto p2 = mbp::create_picker();
  p2->begin_scene();
  p2->add_object(2, mbp::Box::of({-1, -1, 3}, {1, 1, 5}));    // 近，先登记
  p2->add_object(1, mbp::Box::of({-1, -1, -3}, {1, 1, -1}));  // 远，后登记
  ASSERT_TRUE(p2->pick_scene(*cam, 320.0f, 200.0f, &hit));
  EXPECT_EQ(hit.entity, 2u);

  auto p3 = mbp::create_picker();
  p3->begin_scene();
  p3->add_object(2, mbp::Box::of({-1, -1, 3}, {1, 1, 5}));    // 近，后登记
  p3->add_object(1, mbp::Box::of({-1, -1, -3}, {1, 1, -1}));  // 远，先登记
  ASSERT_TRUE(p3->pick_scene(*cam, 320.0f, 200.0f, &hit));
  EXPECT_EQ(hit.entity, 2u);
}

TEST(Picker, ScenePickMissReturnsFalse) {
  auto cam = make_camera(0.0f, 0.0f, 10.0f);
  auto p = mbp::create_picker();
  p->begin_scene();
  p->add_object(1, mbp::Box::of({100, 100, 100}, {101, 101, 101}));
  mbp::PickHit hit;
  EXPECT_FALSE(p->pick_scene(*cam, 320.0f, 200.0f, &hit));
}

TEST(Picker, MeshPickReportsTriangleIndex) {
  auto cam = make_camera(0.0f, 0.0f, 5.0f);
  auto p = mbp::create_picker();
  // 一个朝 +z 的方块（两个三角形）
  const mbp::Vec3 verts[4] = {{-1, -1, 0}, {1, -1, 0}, {1, 1, 0}, {-1, 1, 0}};
  const std::uint32_t idx[6] = {0, 1, 2, 0, 2, 3};
  mbp::PickHit hit;
  ASSERT_TRUE(p->pick_mesh_screen(*cam, 320.0f, 200.0f, verts, 4, idx, 6, &hit));
  EXPECT_EQ(hit.triangle, 0u);
  EXPECT_NEAR(hit.distance, 5.0f, 1e-3f);
  EXPECT_TRUE(hit.valid());
}

// ================================================================ IGizmoSystem

TEST(Gizmo, ConfigureReturnsHandleAndBecomesActive) {
  auto g = mbp::create_gizmo_system();
  const std::uint32_t h = g->configure(three_axis_desc(mbp::GizmoMode::Translate, 0, 0, 0));
  EXPECT_NE(h, 0u);
  EXPECT_EQ(g->active_target(), h);
}

TEST(Gizmo, NoInputIsConsumedWithoutTarget) {
  auto g = mbp::create_gizmo_system();
  auto cam = make_camera(0, 0, 5);
  EXPECT_FALSE(g->on_input(ev(dev::InputEventKind::PointerDown, 320, 200, 0), *cam));
}

// **核心验收**：屏幕尺寸恒定。
// 断言：轴的世界长度 ∝ world_size_per_pixel，即"离得越远、世界长度越大，
// 屏幕上看起来一样长"。
TEST(Gizmo, AxisWorldLengthScalesWithCameraDistance) {
  auto g = mbp::create_gizmo_system();
  const std::uint32_t h = g->configure(three_axis_desc(mbp::GizmoMode::Translate, 0, 0, 0));

  auto near_cam = make_camera(0.0f, 0.0f, 5.0f);
  auto far_cam = make_camera(0.0f, 0.0f, 20.0f);

  const float len_near = g->axis_world_length(h, 0, *near_cam);
  const float len_far = g->axis_world_length(h, 0, *far_cam);

  EXPECT_GT(len_near, 0.0f);
  EXPECT_GT(len_far, len_near);  // 远 → 世界长度必须更大

  // 比例应当等于两相机 world_size_per_pixel 的比例
  const mbp::Vec3 origin{0.0f, 0.0f, 0.0f};
  const float wpp_near = near_cam->world_size_per_pixel(origin);
  const float wpp_far = far_cam->world_size_per_pixel(origin);
  ASSERT_GT(wpp_near, 0.0f);
  EXPECT_NEAR(len_far / len_near, wpp_far / wpp_near, 1e-3f);
}

TEST(Gizmo, AxisScreenLengthIsConstantAcrossDistances) {
  // 更直接的验证：把轴端点投回屏幕，长度应当基本不变（像素恒定）
  auto g = mbp::create_gizmo_system();
  const std::uint32_t h = g->configure(three_axis_desc(mbp::GizmoMode::Translate, 0, 0, 0));

  auto measure = [&](mbp::ICamera& cam) {
    const mbp::Vec3 center{0.0f, 0.0f, 0.0f};
    const float world_len = g->axis_world_length(h, 0, cam);  // X 轴
    float x0 = 0.0f, y0 = 0.0f, d0 = 0.0f;
    float x1 = 0.0f, y1 = 0.0f, d1 = 0.0f;
    EXPECT_TRUE(cam.world_to_screen(center, &x0, &y0, &d0));
    EXPECT_TRUE(cam.world_to_screen({world_len, 0.0f, 0.0f}, &x1, &y1, &d1));
    return std::fabs(x1 - x0);
  };

  auto c1 = make_camera(0.0f, 0.0f, 5.0f);
  auto c2 = make_camera(0.0f, 0.0f, 15.0f);
  const float px1 = measure(*c1);
  const float px2 = measure(*c2);

  EXPECT_GT(px1, 1.0f);
  // 屏幕长度应当 ≈ screen_radius_px(80)，两个距离下误差都在 2% 内
  EXPECT_NEAR(px1, 80.0f, 2.0f);
  EXPECT_NEAR(px2, 80.0f, 2.0f);
}

TEST(Gizmo, HoverEmitsBeginAndEnd) {
  auto g = mbp::create_gizmo_system();
  auto cam = make_camera(0.0f, 0.0f, 5.0f);
  const std::uint32_t h = g->configure(three_axis_desc(mbp::GizmoMode::Translate, 0, 0, 0));

  EventRecorder rec;
  g->subscribe(&rec);

  // 找一个确定命中 X 轴的屏幕点：X 轴端点在屏幕上
  const float len = g->axis_world_length(h, 0, *cam);
  float sx = 0.0f, sy = 0.0f, d = 0.0f;
  ASSERT_TRUE(cam->world_to_screen({len * 0.5f, 0.0f, 0.0f}, &sx, &sy, &d));

  EXPECT_TRUE(g->on_input(ev(dev::InputEventKind::PointerMove, sx, sy, 100), *cam));
  EXPECT_EQ(rec.count_of(mbp::GizmoEvent::Kind::HoverBegin), 1u);

  // 移到远处 → HoverEnd
  g->on_input(ev(dev::InputEventKind::PointerMove, 5.0f, 5.0f, 200), *cam);
  EXPECT_EQ(rec.count_of(mbp::GizmoEvent::Kind::HoverEnd), 1u);
  EXPECT_EQ(rec.count_of(mbp::GizmoEvent::Kind::HoverBegin), 1u);
}

TEST(Gizmo, DragEmitsBeginDragEndInOrder) {
  auto g = mbp::create_gizmo_system();
  auto cam = make_camera(0.0f, 0.0f, 5.0f);
  const std::uint32_t h = g->configure(three_axis_desc(mbp::GizmoMode::Translate, 0, 0, 0));
  (void)h;

  EventRecorder rec;
  g->subscribe(&rec);

  const float len = g->axis_world_length(g->active_target(), 0, *cam);
  float sx = 0.0f, sy = 0.0f, d = 0.0f;
  ASSERT_TRUE(cam->world_to_screen({len * 0.5f, 0.0f, 0.0f}, &sx, &sy, &d));

  ASSERT_TRUE(g->on_input(ev(dev::InputEventKind::PointerDown, sx, sy, 1000), *cam));
  EXPECT_TRUE(g->dragging());
  EXPECT_EQ(rec.count_of(mbp::GizmoEvent::Kind::DragBegin), 1u);

  g->on_input(ev(dev::InputEventKind::PointerMove, sx + 20.0f, sy, 1010), *cam);
  g->on_input(ev(dev::InputEventKind::PointerMove, sx + 40.0f, sy, 1020), *cam);
  EXPECT_EQ(rec.count_of(mbp::GizmoEvent::Kind::Drag), 2u);

  ASSERT_TRUE(g->on_input(ev(dev::InputEventKind::PointerUp, sx + 40.0f, sy, 1030), *cam));
  EXPECT_FALSE(g->dragging());
  EXPECT_EQ(rec.count_of(mbp::GizmoEvent::Kind::DragEnd), 1u);

  // 顺序必须是 Begin, Drag, Drag, End
  ASSERT_GE(rec.events.size(), 4u);
  EXPECT_EQ(rec.events[0].kind, mbp::GizmoEvent::Kind::DragBegin);
  EXPECT_EQ(rec.events[1].kind, mbp::GizmoEvent::Kind::Drag);
  EXPECT_EQ(rec.events[2].kind, mbp::GizmoEvent::Kind::Drag);
  EXPECT_EQ(rec.events[3].kind, mbp::GizmoEvent::Kind::DragEnd);
}

TEST(Gizmo, DragAccumulatesTotalAlongAxis) {
  auto g = mbp::create_gizmo_system();
  auto cam = make_camera(0.0f, 0.0f, 5.0f);
  g->configure(three_axis_desc(mbp::GizmoMode::Translate, 0, 0, 0));

  EventRecorder rec;
  g->subscribe(&rec);

  const float len = g->axis_world_length(g->active_target(), 0, *cam);
  float sx = 0.0f, sy = 0.0f, d = 0.0f;
  ASSERT_TRUE(cam->world_to_screen({len * 0.5f, 0.0f, 0.0f}, &sx, &sy, &d));

  g->on_input(ev(dev::InputEventKind::PointerDown, sx, sy, 0), *cam);
  g->on_input(ev(dev::InputEventKind::PointerMove, sx + 10.0f, sy, 10), *cam);
  g->on_input(ev(dev::InputEventKind::PointerMove, sx + 20.0f, sy, 20), *cam);

  // 最后一次 Drag 事件的 total 应当是两次 delta 之和（沿 X 轴）
  const mbp::GizmoEvent* last_drag = nullptr;
  for (const auto& e : rec.events) {
    if (e.kind == mbp::GizmoEvent::Kind::Drag) last_drag = &e;
  }
  ASSERT_NE(last_drag, nullptr);
  EXPECT_GT(last_drag->total[0], 0.0f);
  // 累计必须沿 X 轴（另外两个分量 ~0）
  EXPECT_NEAR(last_drag->total[1], 0.0f, 1e-3f);
  EXPECT_NEAR(last_drag->total[2], 0.0f, 1e-3f);

  // 快照里的 drag_total 必须与事件一致
  const mbp::GizmoSnapshot snap = g->snapshot();
  EXPECT_TRUE(snap.is_dragging);
  EXPECT_NEAR(snap.drag_total[0], last_drag->total[0], 1e-4f);
}

TEST(Gizmo, CancelEmitsDragCancelAndReturnsToIdle) {
  auto g = mbp::create_gizmo_system();
  auto cam = make_camera(0.0f, 0.0f, 5.0f);
  g->configure(three_axis_desc(mbp::GizmoMode::Translate, 0, 0, 0));

  EventRecorder rec;
  g->subscribe(&rec);

  const float len = g->axis_world_length(g->active_target(), 0, *cam);
  float sx = 0.0f, sy = 0.0f, d = 0.0f;
  ASSERT_TRUE(cam->world_to_screen({len * 0.5f, 0.0f, 0.0f}, &sx, &sy, &d));

  g->on_input(ev(dev::InputEventKind::PointerDown, sx, sy, 0), *cam);
  ASSERT_TRUE(g->dragging());

  g->cancel_drag();
  EXPECT_FALSE(g->dragging());
  EXPECT_EQ(rec.count_of(mbp::GizmoEvent::Kind::DragCancel), 1u);

  // 状态机回到 Idle：hovered/dragging 轴都清零
  const mbp::GizmoSnapshot snap = g->snapshot();
  EXPECT_EQ(snap.dragging_axis, mbp::IGizmoSystem::kNoAxis);
  EXPECT_FALSE(snap.is_dragging);
}

TEST(Gizmo, FocusLossCancelsDrag) {
  auto g = mbp::create_gizmo_system();
  auto cam = make_camera(0.0f, 0.0f, 5.0f);
  g->configure(three_axis_desc(mbp::GizmoMode::Translate, 0, 0, 0));

  const float len = g->axis_world_length(g->active_target(), 0, *cam);
  float sx = 0.0f, sy = 0.0f, d = 0.0f;
  ASSERT_TRUE(cam->world_to_screen({len * 0.5f, 0.0f, 0.0f}, &sx, &sy, &d));

  g->on_input(ev(dev::InputEventKind::PointerDown, sx, sy, 0), *cam);
  ASSERT_TRUE(g->dragging());

  EXPECT_TRUE(g->on_input(ev(dev::InputEventKind::Focus, 0, 0, 0), *cam));
  EXPECT_FALSE(g->dragging());
}

// **解耦验证**：gizmo 自己不改世界。它只发事件，改世界是订阅者的事。
TEST(Gizmo, GizmoNeverMutatesWorldOnlyEmits) {
  auto g = mbp::create_gizmo_system();
  auto cam = make_camera(0.0f, 0.0f, 5.0f);
  g->configure(three_axis_desc(mbp::GizmoMode::Translate, 0, 0, 0));

  // 一个"玩法"订阅者：收到 Drag 就记录自己想改多少。
  struct Player final : public mbp::IGizmoListener {
    float received_x = 0.0f;
    void on_gizmo(const mbp::GizmoEvent& e) override {
      if (e.kind == mbp::GizmoEvent::Kind::Drag) received_x += e.delta[0];
    }
  } player;
  g->subscribe(&player);

  const float len = g->axis_world_length(g->active_target(), 0, *cam);
  float sx = 0.0f, sy = 0.0f, d = 0.0f;
  ASSERT_TRUE(cam->world_to_screen({len * 0.5f, 0.0f, 0.0f}, &sx, &sy, &d));

  g->on_input(ev(dev::InputEventKind::PointerDown, sx, sy, 0), *cam);
  g->on_input(ev(dev::InputEventKind::PointerMove, sx + 25.0f, sy, 10), *cam);
  g->on_input(ev(dev::InputEventKind::PointerUp, sx + 25.0f, sy, 20), *cam);

  // 玩法收到了事件 —— 说明解耦方向正确（事件流出，不出流入世界）
  EXPECT_NE(player.received_x, 0.0f);
}

TEST(Gizmo, SnapshotRestoreRoundTrip) {
  auto g = mbp::create_gizmo_system();
  auto cam = make_camera(0.0f, 0.0f, 5.0f);
  g->configure(three_axis_desc(mbp::GizmoMode::Translate, 0, 0, 0));

  const float len = g->axis_world_length(g->active_target(), 0, *cam);
  float sx = 0.0f, sy = 0.0f, d = 0.0f;
  ASSERT_TRUE(cam->world_to_screen({len * 0.5f, 0.0f, 0.0f}, &sx, &sy, &d));

  g->on_input(ev(dev::InputEventKind::PointerDown, sx, sy, 0), *cam);
  g->on_input(ev(dev::InputEventKind::PointerMove, sx + 15.0f, sy, 10), *cam);

  const mbp::GizmoSnapshot before = g->snapshot();
  ASSERT_TRUE(before.is_dragging);

  // 恢复到一个干净状态，再恢复回来
  auto g2 = mbp::create_gizmo_system();
  g2->configure(three_axis_desc(mbp::GizmoMode::Translate, 0, 0, 0));
  ASSERT_TRUE(g2->restore(before).ok());
  const mbp::GizmoSnapshot after = g2->snapshot();

  EXPECT_TRUE(before == after);
}

TEST(Gizmo, EventTimestampsComeFromStreamNotClock) {
  auto g = mbp::create_gizmo_system();
  auto cam = make_camera(0.0f, 0.0f, 5.0f);
  g->configure(three_axis_desc(mbp::GizmoMode::Translate, 0, 0, 0));

  EventRecorder rec;
  g->subscribe(&rec);

  const float len = g->axis_world_length(g->active_target(), 0, *cam);
  float sx = 0.0f, sy = 0.0f, d = 0.0f;
  ASSERT_TRUE(cam->world_to_screen({len * 0.5f, 0.0f, 0.0f}, &sx, &sy, &d));

  const std::int64_t kTs = 123456789;
  g->on_input(ev(dev::InputEventKind::PointerDown, sx, sy, kTs), *cam);
  ASSERT_FALSE(rec.events.empty());
  EXPECT_EQ(rec.events.back().timestamp_us, kTs);
}

TEST(Gizmo, RecordedEventStreamReplaysIdentically) {
  // 回放一致性：同一串输入两次喂给两个独立 gizmo，事件序列必须完全相同。
  auto replay = []() {
    auto g = mbp::create_gizmo_system();
    auto cam = make_camera(0.0f, 0.0f, 5.0f);
    g->configure(three_axis_desc(mbp::GizmoMode::Translate, 0, 0, 0));
    EventRecorder rec;
    g->subscribe(&rec);

    const float len = g->axis_world_length(g->active_target(), 0, *cam);
    float sx = 0.0f, sy = 0.0f, d = 0.0f;
    EXPECT_TRUE(cam->world_to_screen({len * 0.5f, 0.0f, 0.0f}, &sx, &sy, &d));

    // 录制
    dev::EventStream stream;
    stream.append(ev(dev::InputEventKind::PointerMove, sx, sy, 100));
    stream.append(ev(dev::InputEventKind::PointerDown, sx, sy, 200));
    stream.append(ev(dev::InputEventKind::PointerMove, sx + 10.0f, sy, 300));
    stream.append(ev(dev::InputEventKind::PointerMove, sx + 20.0f, sy, 400));
    stream.append(ev(dev::InputEventKind::PointerUp, sx + 20.0f, sy, 500));

    // 回放
    stream.rewind();
    dev::InputEvent e;
    while (stream.next(&e)) {
      g->on_input(e, *cam);
    }

    std::vector<std::pair<int, std::int64_t>> sig;
    for (const auto& x : rec.events) {
      sig.emplace_back(static_cast<int>(x.kind), x.timestamp_us);
    }
    return sig;
  };

  EXPECT_EQ(replay(), replay());
}

TEST(Gizmo, BuildDrawPushesGeometry) {
  auto g = mbp::create_gizmo_system();
  auto cam = make_camera(0.0f, 0.0f, 5.0f);
  g->configure(three_axis_desc(mbp::GizmoMode::Translate, 0, 0, 0));

  auto ov = mbp::create_overlay2d();
  ov->begin_frame();
  ASSERT_TRUE(g->build_draw(*cam, *ov).ok());
  ov->build();

  // 中心点圆环 + 3 根轴（线 + 箭头三角）。
  //
  // 注意不能断言"恰好 3 个三角"：相机正对 -z 时，Z 轴的箭头从中心沿视线
  // **垂直**出去，在屏幕上长度≈0，箭头三角退化成一点 —— 于是只产出 2 个。
  // 这是正确的几何行为（该轴的屏幕投影确实为零），不是 bug。
  // 所以这里断言"至少 2 个、且线恰好 3 根"，再单独用斜视角验证 3 个三角。
  EXPECT_GT(ov->stats().ring_count, 0u);
  EXPECT_EQ(ov->stats().line_count, 3u);
  EXPECT_GE(ov->stats().tri_count, 2u);

  // 斜视角（相机不在轴上）：三根轴在屏幕上都有长度 → 三个箭头都在
  auto cam2 = make_camera(3.0f, 3.0f, 5.0f);
  auto ov2 = mbp::create_overlay2d();
  ov2->begin_frame();
  ASSERT_TRUE(g->build_draw(*cam2, *ov2).ok());
  ov2->build();
  EXPECT_EQ(ov2->stats().line_count, 3u);
  EXPECT_EQ(ov2->stats().tri_count, 3u);
}

TEST(Gizmo, RotateModeDrawsRingsNotLines) {
  auto g = mbp::create_gizmo_system();
  auto cam = make_camera(0.0f, 0.0f, 5.0f);
  g->configure(three_axis_desc(mbp::GizmoMode::Rotate, 0, 0, 0));

  auto ov = mbp::create_overlay2d();
  ov->begin_frame();
  ASSERT_TRUE(g->build_draw(*cam, *ov).ok());
  ov->build();

  // 中心点 1 个环 + 3 个轴环；没有线
  EXPECT_EQ(ov->stats().ring_count, 4u);
  EXPECT_EQ(ov->stats().line_count, 0u);
}

TEST(Gizmo, UnsubscribeStopsEvents) {
  auto g = mbp::create_gizmo_system();
  auto cam = make_camera(0.0f, 0.0f, 5.0f);
  g->configure(three_axis_desc(mbp::GizmoMode::Translate, 0, 0, 0));

  EventRecorder rec;
  g->subscribe(&rec);
  g->unsubscribe(&rec);

  const float len = g->axis_world_length(g->active_target(), 0, *cam);
  float sx = 0.0f, sy = 0.0f, d = 0.0f;
  ASSERT_TRUE(cam->world_to_screen({len * 0.5f, 0.0f, 0.0f}, &sx, &sy, &d));
  g->on_input(ev(dev::InputEventKind::PointerDown, sx, sy, 0), *cam);

  EXPECT_TRUE(rec.events.empty());
}

TEST(Gizmo, PickAxisReturnsNoAxisWhenFar) {
  auto g = mbp::create_gizmo_system();
  auto cam = make_camera(0.0f, 0.0f, 5.0f);
  const std::uint32_t h = g->configure(three_axis_desc(mbp::GizmoMode::Translate, 0, 0, 0));
  EXPECT_EQ(g->pick_axis(h, 5.0f, 5.0f, *cam), mbp::IGizmoSystem::kNoAxis);
}

TEST(Gizmo, PickAxisHitsXAxis) {
  auto g = mbp::create_gizmo_system();
  auto cam = make_camera(0.0f, 0.0f, 5.0f);
  const std::uint32_t h = g->configure(three_axis_desc(mbp::GizmoMode::Translate, 0, 0, 0));

  const float len = g->axis_world_length(h, 0, *cam);
  float sx = 0.0f, sy = 0.0f, d = 0.0f;
  ASSERT_TRUE(cam->world_to_screen({len * 0.5f, 0.0f, 0.0f}, &sx, &sy, &d));

  EXPECT_EQ(g->pick_axis(h, sx, sy, *cam), 0u);  // 轴 id = 0（X）
}
