// MagicBlocks SDK —— L3 primitives 单元测试(M1:types + camera)
//
// 测试目标(spec §14.2):先把零 GPU 依赖的数学部分打满,再谈渲染。
// 这里**不建设备**:L3 的相机/材质/哈希必须能在纯 CPU 下验证,
// 否则每改一次数学都要起 Vulkan,回归会慢到没人跑。
#include <gtest/gtest.h>

#include <cmath>
#include <set>

#include "magicblocks/primitives/camera.h"
#include "magicblocks/primitives/material.h"
#include "magicblocks/primitives/shader_library.h"
#include "magicblocks/primitives/types.h"

namespace mbp = magicblocks::primitives;
using mbp::ICamera;
using mbp::Mat4;
using mbp::Vec3;

namespace {

constexpr float kEps = 1e-4f;

bool near_eq(float a, float b, float eps = kEps) { return std::fabs(a - b) <= eps; }
bool near_eq(const Vec3& a, const Vec3& b, float eps = kEps) {
  return near_eq(a.x, b.x, eps) && near_eq(a.y, b.y, eps) && near_eq(a.z, b.z, eps);
}

// 构造一台默认透视相机:800x600,位置 (0,2,5) 看向原点。
std::unique_ptr<ICamera> make_perspective() {
  auto cam = mbp::create_camera();
  mbp::CameraDesc d;
  d.kind = mbp::ProjectionKind::Perspective;
  d.fov_y_degrees = 45.0f;
  d.near_plane = 0.05f;
  d.far_plane = 100.0f;
  d.viewport = magicblocks::device::Extent2D{800, 600};
  EXPECT_TRUE(cam->set_desc(d).ok());
  return cam;
}

std::unique_ptr<ICamera> make_orthographic() {
  auto cam = mbp::create_camera();
  mbp::CameraDesc d;
  d.kind = mbp::ProjectionKind::Orthographic;
  d.ortho_height = 4.0f;
  d.near_plane = 0.05f;
  d.far_plane = 100.0f;
  d.viewport = magicblocks::device::Extent2D{800, 600};
  EXPECT_TRUE(cam->set_desc(d).ok());
  return cam;
}

}  // namespace

// ================================================================ 摄像机

// 屏幕中心 → 射线方向应等于相机前向(像素原点左上的约定验证点)。
TEST(Camera, PerspectiveScreenCenterRayIsForward) {
  auto cam = make_perspective();
  const mbp::Ray r = cam->screen_to_ray(400.0f, 300.0f);
  EXPECT_TRUE(near_eq(r.origin, cam->pose().position));
  EXPECT_TRUE(near_eq(r.direction, cam->forward()));
}

// 像素 y 向下 + Vulkan NDC y 向下 → 屏幕上方(像素 y 小)对应 NDC y 负。
// 方向自洽要求:像素 y 越小,射线在世界里的 y 分量越**大**(往上抬)。
// 这个测试守住"D-L3-2 的像素口径"与"投影矩阵 Y 约定"两者的组合关系。
TEST(Camera, PixelYDownMapsToWorldUp) {
  auto cam = make_perspective();
  const mbp::Ray top = cam->screen_to_ray(400.0f, 0.0f);
  const mbp::Ray bottom = cam->screen_to_ray(400.0f, 600.0f);
  EXPECT_GT(top.direction.y, cam->forward().y) << "屏幕上方的射线应更朝上";
  EXPECT_LT(bottom.direction.y, cam->forward().y) << "屏幕下方的射线应更朝下";
}

// 像素 x 向右:屏幕右侧射线沿相机右轴偏移。
TEST(Camera, PixelXRightMapsToCameraRight) {
  auto cam = make_perspective();
  const mbp::Ray right = cam->screen_to_ray(800.0f, 300.0f);
  const mbp::Ray left = cam->screen_to_ray(0.0f, 300.0f);
  EXPECT_GT(mbp::dot(right.direction, cam->right()), 0.0f);
  EXPECT_LT(mbp::dot(left.direction, cam->right()), 0.0f);
}

// screen_to_ray / world_to_screen 必须互逆(spec §14.2 ScreenToRayMatchesWorldToScreen)。
TEST(Camera, ScreenToRayMatchesWorldToScreen) {
  auto cam = make_perspective();
  const float xs[] = {0.0f, 123.0f, 400.0f, 777.0f, 799.0f};
  const float ys[] = {0.0f, 45.0f, 300.0f, 512.0f, 599.0f};
  for (float px : xs) {
    for (float py : ys) {
      const mbp::Ray r = cam->screen_to_ray(px, py);
      // 在射线上取一点(原点前方 2 单位),反投影应回到同一像素。
      const Vec3 p = r.origin + r.direction * 2.0f;
      float ox = -1.0f;
      float oy = -1.0f;
      ASSERT_TRUE(cam->world_to_screen(p, &ox, &oy, nullptr));
      EXPECT_NEAR(ox, px, 0.01f) << "px=" << px << " py=" << py;
      EXPECT_NEAR(oy, py, 0.01f) << "px=" << px << " py=" << py;
    }
  }
}

// 正交相机同样要互逆。
TEST(Camera, OrthographicScreenToRayMatchesWorldToScreen) {
  auto cam = make_orthographic();
  for (float px : {0.0f, 400.0f, 799.0f}) {
    for (float py : {0.0f, 300.0f, 599.0f}) {
      const mbp::Ray r = cam->screen_to_ray(px, py);
      const Vec3 p = r.origin + r.direction * 3.0f;
      float ox = -1.0f;
      float oy = -1.0f;
      ASSERT_TRUE(cam->world_to_screen(p, &ox, &oy, nullptr));
      EXPECT_NEAR(ox, px, 0.01f);
      EXPECT_NEAR(oy, py, 0.01f);
    }
  }
}

// 正交相机:射线方向恒等于前向,且不同像素的起点平行。
TEST(Camera, OrthographicRaysAreParallel) {
  auto cam = make_orthographic();
  const mbp::Ray a = cam->screen_to_ray(100.0f, 100.0f);
  const mbp::Ray b = cam->screen_to_ray(700.0f, 500.0f);
  EXPECT_TRUE(near_eq(a.direction, b.direction, 1e-5f));
  EXPECT_TRUE(near_eq(a.direction, cam->forward(), 1e-5f));
}

// 透视:world_size_per_pixel 随距离线性增长(spec §14.2 WorldSizePerPixelIsScaleInvariant)。
// 两倍距离 → 两倍世界尺度。这是 gizmo 屏幕恒定尺寸的数学基础。
TEST(Camera, WorldSizePerPixelScalesWithDistance) {
  auto cam = make_perspective();
  const Vec3 f = cam->forward();
  const Vec3 base = cam->pose().position;
  const float d1 = cam->world_size_per_pixel(base + f * 1.0f);
  const float d2 = cam->world_size_per_pixel(base + f * 2.0f);
  const float d4 = cam->world_size_per_pixel(base + f * 4.0f);
  EXPECT_GT(d1, 0.0f);
  EXPECT_NEAR(d2 / d1, 2.0f, 1e-3f);
  EXPECT_NEAR(d4 / d1, 4.0f, 1e-3f);
}

// 正交:world_size_per_pixel 与位置无关(恒定)。
// 这才是"正交下 gizmo 尺寸天然恒定"的来源。
TEST(Camera, OrthographicWorldSizePerPixelIsConstant) {
  auto cam = make_orthographic();
  const Vec3 f = cam->forward();
  const Vec3 base = cam->pose().position;
  const float d1 = cam->world_size_per_pixel(base + f * 1.0f);
  const float d9 = cam->world_size_per_pixel(base + f * 9.0f);
  EXPECT_NEAR(d1, d9, 1e-6f);
  // 800x600 覆盖 ortho_height=4 → 每像素 4/600。
  EXPECT_NEAR(d1, 4.0f / 600.0f, 1e-6f);
}

// orbit 保持 target 与距离不变(spec §14.2 OrbitPanZoomKeepTarget 之一)。
TEST(Camera, OrbitKeepsTargetAndDistance) {
  auto cam = make_perspective();
  const Vec3 target = cam->pose().target;
  const float dist = cam->distance();
  cam->orbit(37.0f, -12.0f);
  EXPECT_TRUE(near_eq(cam->pose().target, target));
  EXPECT_NEAR(cam->distance(), dist, 1e-3f);
}

// orbit 的 pitch 必须被夹紧:否则视线与 up 共线,视图矩阵退化。
TEST(Camera, OrbitClampsPitch) {
  auto cam = make_perspective();
  cam->orbit(0.0f, 1000.0f);   // 猛拉向上
  const Vec3 f = cam->forward();
  // 夹紧目标是 89.9° → |f.y| ≈ sin(89.9°) ≈ 0.9999985。
  // 判据:必须严格小于 1(未退化),且确实接近上限(说明夹紧真的发生了)。
  EXPECT_LT(std::fabs(f.y), 1.0f) << "视线不能与 up 共线";
  EXPECT_GT(std::fabs(f.y), 0.9999f) << "应被夹到接近 90° 但不到 90°";
  // 矩阵仍然有限。
  const Mat4& view = cam->view_matrix();
  for (int i = 0; i < 16; ++i) EXPECT_TRUE(std::isfinite(view.m[i]));
}

// pan 平移 target 与 position 同量,距离不变。
TEST(Camera, PanKeepsDistance) {
  auto cam = make_perspective();
  const float dist = cam->distance();
  const Vec3 t0 = cam->pose().target;
  cam->pan(50.0f, -30.0f);
  EXPECT_NEAR(cam->distance(), dist, 1e-3f);
  // 确实动了。
  EXPECT_FALSE(near_eq(cam->pose().target, t0));
  // 垂直于视线方向移动(沿前向的分量应为 0)。
  const Vec3 delta = cam->pose().target - t0;
  EXPECT_NEAR(mbp::dot(delta, cam->forward()), 0.0f, 1e-3f);
}

// zoom 沿视线推拉:target 不变,距离变化。
TEST(Camera, ZoomChangesDistanceKeepsTarget) {
  auto cam = make_perspective();
  const Vec3 target = cam->pose().target;
  const float d0 = cam->distance();
  cam->zoom(0.1f);   // 放大 → 靠近
  EXPECT_TRUE(near_eq(cam->pose().target, target));
  EXPECT_LT(cam->distance(), d0);
  cam->zoom(-0.1f);  // 回去
  EXPECT_NEAR(cam->distance(), d0, 1e-3f);
}

// 正交 zoom 改 ortho_height,不改位置。
TEST(Camera, OrthographicZoomChangesOrthoHeight) {
  auto cam = make_orthographic();
  const Vec3 pos = cam->pose().position;
  const float h0 = cam->desc().ortho_height;
  cam->zoom(0.25f);
  EXPECT_LT(cam->desc().ortho_height, h0);
  EXPECT_TRUE(near_eq(cam->pose().position, pos));
}

// focus_on 把 target 移到球心,且让半径入画。
TEST(Camera, FocusOnFramesSphere) {
  auto cam = make_perspective();
  const Vec3 center{10.0f, -3.0f, 7.0f};
  cam->focus_on(center, 2.0f);
  EXPECT_TRUE(near_eq(cam->pose().target, center));
  // 包围球上沿视线方向最远的点应仍在视锥内(distances > radius)。
  EXPECT_GT(cam->distance(), 2.0f);
}

// 参数校验:非法 desc 必须被拒,并给出 Invalid 而非静默修正。
TEST(Camera, RejectsInvalidDesc) {
  auto cam = mbp::create_camera();
  mbp::CameraDesc d;
  d.viewport = magicblocks::device::Extent2D{0, 600};
  EXPECT_FALSE(cam->set_desc(d).ok());   // 视口宽 0

  d.viewport = magicblocks::device::Extent2D{800, 600};
  d.near_plane = 0.0f;
  EXPECT_FALSE(cam->set_desc(d).ok());   // near 非正

  d.near_plane = 10.0f;
  d.far_plane = 1.0f;
  EXPECT_FALSE(cam->set_desc(d).ok());   // far <= near

  d.far_plane = 100.0f;
  d.fov_y_degrees = 0.0f;
  EXPECT_FALSE(cam->set_desc(d).ok());   // fov 0
}

// reverse_z 需要显式设备能力支持,否则 Unsupported(spec §14.2 ReverseZRequiresCaps)。
TEST(Camera, ReverseZRequiresCaps) {
  auto cam = mbp::create_camera();
  mbp::CameraDesc d;
  d.viewport = magicblocks::device::Extent2D{800, 600};
  d.reverse_z = true;
  const auto st = cam->set_desc(d);
  EXPECT_FALSE(st.ok());
  EXPECT_EQ(st.code, magicblocks::device::StatusCode::Unsupported);
}

// 视线退化(position == target)必须被拒,否则 look_at 产出 NaN。
TEST(Camera, RejectsDegeneratePose) {
  auto cam = make_perspective();
  mbp::CameraPose p;
  p.position = Vec3{1.0f, 1.0f, 1.0f};
  p.target = Vec3{1.0f, 1.0f, 1.0f};
  p.up = Vec3{0.0f, 1.0f, 0.0f};
  EXPECT_FALSE(cam->set_pose(p).ok());
}

// 惰性求值:同一帧内反复读矩阵不得重复重算(spec §14.2 性能断言)。
TEST(Camera, MatrixRebuiltOnceUntilDirty) {
  auto cam = make_perspective();
  // make_perspective() 内部 set_desc(800x600) 会置一次脏是**正确**的
  // (参数确实变了)。所以先读一次把缓存预热,再取基线。
  (void)cam->view_proj_matrix();
  const magicblocks::device::u64 after_warm = cam->matrix_rebuild_count();
  (void)cam->view_proj_matrix();
  (void)cam->view_proj_matrix();
  (void)cam->inverse_view_proj_matrix();
  (void)cam->view_proj_matrix();
  EXPECT_EQ(cam->matrix_rebuild_count(), after_warm) << "重复读矩阵不应触发重算";

  cam->orbit(10.0f, 0.0f);
  (void)cam->view_proj_matrix();
  const magicblocks::device::u64 after_orbit = cam->matrix_rebuild_count();
  EXPECT_GT(after_orbit, after_warm) << "pose 变化后必须重算";
  (void)cam->view_proj_matrix();
  EXPECT_EQ(cam->matrix_rebuild_count(), after_orbit) << "重算后再次读取不应重复算";
}

// inverse(view_proj) 与 view_proj 相乘应为单位阵。
TEST(Camera, InverseViewProjIsCorrect) {
  auto cam = make_perspective();
  cam->orbit(23.0f, 17.0f);
  const Mat4 vp = cam->view_proj_matrix();
  const Mat4 inv = cam->inverse_view_proj_matrix();
  const Mat4 id = vp * inv;
  for (int col = 0; col < 4; ++col) {
    for (int row = 0; row < 4; ++row) {
      const float expect = (col == row) ? 1.0f : 0.0f;
      EXPECT_NEAR(id.at(col, row), expect, 1e-3f) << "col=" << col << " row=" << row;
    }
  }
}

// 快照往返:restore 后矩阵与行为一致(命令流回放基础)。
TEST(Camera, SnapshotRoundTrip) {
  auto cam = make_perspective();
  cam->orbit(31.0f, -8.0f);
  cam->zoom(0.2f);
  const mbp::CameraSnapshot snap = cam->snapshot();
  const Mat4 vp_before = cam->view_proj_matrix();

  auto other = mbp::create_camera();
  ASSERT_TRUE(other->restore(snap).ok());
  EXPECT_TRUE(snap == other->snapshot());
  const Mat4 vp_after = other->view_proj_matrix();
  for (int i = 0; i < 16; ++i) EXPECT_NEAR(vp_before.m[i], vp_after.m[i], 1e-5f);
}

// 世界点在相机后方:world_to_screen 返回 false,不写输出。
TEST(Camera, PointBehindCameraReturnsFalse) {
  auto cam = make_perspective();
  const Vec3 behind = cam->pose().position - cam->forward() * 2.0f;
  float px = -12345.0f;
  float py = -12345.0f;
  EXPECT_FALSE(cam->world_to_screen(behind, &px, &py, nullptr));
  EXPECT_EQ(px, -12345.0f);
  EXPECT_EQ(py, -12345.0f);
}

// ================================================================ 数学工具

TEST(Math, LookAtPutsTargetOnNegativeZ) {
  const Mat4 v = mbp::make_look_at(Vec3{0, 0, 5}, Vec3{0, 0, 0}, Vec3{0, 1, 0});
  // 相机看向 -z,target 应变换到 (0,0,-5)。
  const auto t = v.transform_point(Vec3{0, 0, 0});
  EXPECT_TRUE(near_eq(t, Vec3{0.0f, 0.0f, -5.0f}));
  // 相机自身在原点。
  const auto c = v.transform_point(Vec3{0, 0, 5});
  EXPECT_TRUE(near_eq(c, Vec3{0.0f, 0.0f, 0.0f}));
}

// 透视矩阵 Y 约定:Vulkan NDC 的 +y 是**向下**的,而世界 +y 向上。
// 因此投影后"世界在上方的点"必须有**更小的 NDC y**(更负)。
// 这就是画面不颠倒的判据 —— 与像素坐标(py 小 = 屏幕上方)方向一致。
TEST(Math, PerspectiveKeepsYUp) {
  const Mat4 p =
      mbp::make_perspective(45.0f * 3.14159265f / 180.0f, 4.0f / 3.0f, 0.1f, 100.0f);
  const auto up = p.transform_point(Vec3{0.0f, 1.0f, -5.0f});
  const auto down = p.transform_point(Vec3{0.0f, -1.0f, -5.0f});
  EXPECT_LT(up.y, 0.0f) << "世界 +y(上)→ Vulkan NDC -y(屏幕上方)";
  EXPECT_GT(down.y, 0.0f);
  EXPECT_LT(up.y, down.y);
}

// 透视矩阵:Vulkan 惯例 z ∈ [0,1],near 平面点 z≈0,far 平面点 z≈1。
TEST(Math, PerspectiveZRangeIsZeroToOne) {
  const Mat4 p =
      mbp::make_perspective(45.0f * 3.14159265f / 180.0f, 1.0f, 0.1f, 100.0f);
  const auto near_pt = p.transform_point(Vec3{0.0f, 0.0f, -0.1f});
  const auto far_pt = p.transform_point(Vec3{0.0f, 0.0f, -100.0f});
  EXPECT_NEAR(near_pt.z, 0.0f, 1e-3f);
  EXPECT_NEAR(far_pt.z, 1.0f, 1e-3f);
}

TEST(Math, MatrixInverseRoundTrip) {
  const Mat4 m = mbp::make_look_at(Vec3{3, 4, 5}, Vec3{0, 0, 0}, Vec3{0, 1, 0});
  bool ok = false;
  const Mat4 inv = m.inverse(&ok);
  ASSERT_TRUE(ok);
  const Mat4 id = m * inv;
  for (int col = 0; col < 4; ++col) {
    for (int row = 0; row < 4; ++row) {
      EXPECT_NEAR(id.at(col, row), (col == row) ? 1.0f : 0.0f, 1e-4f);
    }
  }
}

TEST(Math, QuaternionRotationMatchesAxisRotation) {
  // 绕 y 轴 90° 的 quaternion:(0, sin(45°), 0, cos(45°))。
  // 右手系绕 +y 转 90°:+x 应转到 -z。
  const float s = std::sin(3.14159265f * 0.25f);
  const float c = std::cos(3.14159265f * 0.25f);
  const Mat4 r = mbp::make_rotation_quat(0.0f, s, 0.0f, c);
  const auto x_axis = r.transform_direction(Vec3{1, 0, 0});
  EXPECT_TRUE(near_eq(x_axis, Vec3{0.0f, 0.0f, -1.0f}, 1e-4f));
}

TEST(Math, TrsComposesInOrder) {
  // T * R * S:先缩放再旋转再平移。
  const float t[3] = {10.0f, 0.0f, 0.0f};
  const float sc[3] = {2.0f, 2.0f, 2.0f};
  const float qs = std::sin(3.14159265f * 0.25f);
  const float qc = std::cos(3.14159265f * 0.25f);
  const float q[4] = {0.0f, qs, 0.0f, qc};   // 绕 y 轴 90°
  const Mat4 m = mbp::make_trs(t, q, sc);
  const auto p = m.transform_point(Vec3{1, 0, 0});
  // 缩放 → (2,0,0);旋转 90° → (0,0,-2);平移 → (10,0,-2)。
  EXPECT_TRUE(near_eq(p, Vec3{10.0f, 0.0f, -2.0f}, 1e-4f));
}

// ================================================================ 求交

TEST(Intersect, RayBoxHitsFromOutside) {
  mbp::Box b;
  b.min = Vec3{-1, -1, -1};
  b.max = Vec3{1, 1, 1};
  b.empty = false;
  mbp::Ray r;
  r.origin = Vec3{0, 0, 5};
  r.direction = Vec3{0, 0, -1};
  float t = 0.0f;
  ASSERT_TRUE(mbp::intersect_ray_box(r, b, &t));
  EXPECT_NEAR(t, 4.0f, 1e-4f);
}

TEST(Intersect, RayBoxMissesWhenParallelAndOutside) {
  mbp::Box b;
  b.min = Vec3{-1, -1, -1};
  b.max = Vec3{1, 1, 1};
  b.empty = false;
  mbp::Ray r;
  r.origin = Vec3{5, 5, 0};
  r.direction = Vec3{0, 0, -1};   // 平行于 z 轴,但在盒子外
  float t = 0.0f;
  EXPECT_FALSE(mbp::intersect_ray_box(r, b, &t));
}

TEST(Intersect, RayBoxFromInside) {
  mbp::Box b;
  b.min = Vec3{-1, -1, -1};
  b.max = Vec3{1, 1, 1};
  b.empty = false;
  mbp::Ray r;
  r.origin = Vec3{0, 0, 0};   // 盒内
  r.direction = Vec3{0, 0, 1};
  float t = 0.0f;
  ASSERT_TRUE(mbp::intersect_ray_box(r, b, &t));
  EXPECT_NEAR(t, 1.0f, 1e-4f);   // 从内部射向 +z 面
}

TEST(Intersect, RayTriangleIsDoubleSided) {
  const Vec3 a{0, 0, 0};
  const Vec3 b{1, 0, 0};
  const Vec3 c{0, 1, 0};
  mbp::Ray front;
  front.origin = Vec3{0.2f, 0.2f, 1.0f};
  front.direction = Vec3{0, 0, -1};
  float t = 0.0f;
  ASSERT_TRUE(mbp::intersect_ray_triangle(front, a, b, c, &t));
  EXPECT_NEAR(t, 1.0f, 1e-4f);

  // 背面也必须命中(拾取要"点到就算")。
  mbp::Ray back;
  back.origin = Vec3{0.2f, 0.2f, -1.0f};
  back.direction = Vec3{0, 0, 1};
  EXPECT_TRUE(mbp::intersect_ray_triangle(back, a, b, c, &t));
}

TEST(Intersect, RayTriangleMissesOutside) {
  const Vec3 a{0, 0, 0};
  const Vec3 b{1, 0, 0};
  const Vec3 c{0, 1, 0};
  mbp::Ray r;
  r.origin = Vec3{5, 5, 1};
  r.direction = Vec3{0, 0, -1};
  float t = 0.0f;
  EXPECT_FALSE(mbp::intersect_ray_triangle(r, a, b, c, &t));
}

TEST(Intersect, RayPlaneBasic) {
  mbp::Plane p;
  p.normal = Vec3{0, 1, 0};
  p.d = 0.0f;   // y = 0 平面
  mbp::Ray r;
  r.origin = Vec3{0, 5, 0};
  r.direction = Vec3{0, -1, 0};
  float t = 0.0f;
  ASSERT_TRUE(mbp::intersect_ray_plane(r, p, &t));
  EXPECT_NEAR(t, 5.0f, 1e-4f);
}

TEST(Intersect, DistancePointSegmentClampsEnds) {
  const Vec3 a{0, 0, 0};
  const Vec3 b{1, 0, 0};
  // 投影落在段内。
  EXPECT_NEAR(mbp::distance_point_segment(Vec3{0.5f, 1.0f, 0.0f}, a, b), 1.0f, 1e-5f);
  // 投影落在段外 → 到端点距离。
  EXPECT_NEAR(mbp::distance_point_segment(Vec3{2.0f, 0.0f, 0.0f}, a, b), 1.0f, 1e-5f);
}

// ================================================================ 材质 / Shader 联动

TEST(Material, InternDedupsSameDesc) {
  auto table = mbp::create_material_table();
  const std::size_t base = table->size();   // 槽位 0 是默认材质
  mbp::MaterialDesc d = mbp::default_material_desc();
  d.params.base_color[0] = 1.0f;
  d.params.base_color[1] = 0.0f;
  d.params.base_color[2] = 0.0f;

  const auto s1 = table->intern(d);
  const auto s2 = table->intern(d);
  ASSERT_TRUE(s1.ok());
  ASSERT_TRUE(s2.ok());
  EXPECT_EQ(s1.value(), s2.value());
  EXPECT_EQ(table->size(), base + 1);
  EXPECT_GE(table->dedup_hits(), 1u);
}

TEST(Material, DifferentParamsGetDifferentSlots) {
  auto table = mbp::create_material_table();
  mbp::MaterialDesc a = mbp::default_material_desc();
  mbp::MaterialDesc b = mbp::default_material_desc();
  a.params.roughness = 0.1f;
  b.params.roughness = 0.9f;
  const auto sa = table->intern(a);
  const auto sb = table->intern(b);
  ASSERT_TRUE(sa.ok());
  ASSERT_TRUE(sb.ok());
  EXPECT_NE(sa.value(), sb.value());
}

// 哈希必须与 -0.0 / +0.0 无关(浮点规范化)。
TEST(Material, HashTreatsMinusZeroEqual) {
  mbp::MaterialDesc a = mbp::default_material_desc();
  mbp::MaterialDesc b = mbp::default_material_desc();
  a.params.base_color[0] = 0.0f;
  b.params.base_color[0] = -0.0f;
  EXPECT_EQ(mbp::hash_material_desc(a), mbp::hash_material_desc(b));
}

// 规则 B:透明/不透明必须能靠 ShaderKey 分开(D-L3-3)。
TEST(Material, AlphaBlendIsDistinctShaderKey) {
  const mbp::ShaderKey opaque =
      mbp::make_shader_key(mbp::shader_names::kStandardUnlit, 0);
  const mbp::ShaderKey blended =
      mbp::make_shader_key(mbp::shader_names::kStandardUnlit, mbp::kShaderFlagAlphaBlend);
  EXPECT_NE(opaque, blended);
  EXPECT_NE(opaque.name_hash, 0u);
}

// 规则 A:同 ShaderKey + 同 RasterState 不同参数 → 同 pipeline_key(能合批)。
TEST(Material, SameShaderSamePipelineKey) {
  auto table = mbp::create_material_table();
  mbp::MaterialDesc a = mbp::default_material_desc();
  mbp::MaterialDesc b = mbp::default_material_desc();
  a.params.base_color[0] = 1.0f;
  a.params.roughness = 0.2f;
  b.params.base_color[2] = 1.0f;
  b.params.roughness = 0.8f;
  const auto sa = table->intern(a);
  const auto sb = table->intern(b);
  ASSERT_TRUE(sa.ok());
  ASSERT_TRUE(sb.ok());
  EXPECT_NE(sa.value(), sb.value()) << "参数不同 → 槽位不同";
  const auto* ma = table->material(sa.value());
  const auto* mb = table->material(sb.value());
  ASSERT_NE(ma, nullptr);
  ASSERT_NE(mb, nullptr);
  EXPECT_EQ(ma->pipeline_key(), mb->pipeline_key())
      << "参数不同但 shader/raster 相同 → 可合批成一次 draw";
}

// 换 shader → 换 pipeline_key(不能合批)。
TEST(Material, DifferentShaderDifferentPipelineKey) {
  auto table = mbp::create_material_table();
  mbp::MaterialDesc a = mbp::default_material_desc();
  mbp::MaterialDesc b = mbp::default_material_desc();
  a.shader = mbp::make_shader_key(mbp::shader_names::kStandardUnlit);
  b.shader = mbp::make_shader_key(mbp::shader_names::kStandardPbr);
  const auto sa = table->intern(a);
  const auto sb = table->intern(b);
  ASSERT_TRUE(sa.ok());
  ASSERT_TRUE(sb.ok());
  EXPECT_NE(table->material(sa.value())->pipeline_key(),
            table->material(sb.value())->pipeline_key());
}

// 换深度状态 → 换 pipeline_key(固定功能状态影响管线)。
TEST(Material, DifferentRasterDifferentPipelineKey) {
  auto table = mbp::create_material_table();
  mbp::MaterialDesc a = mbp::default_material_desc();
  mbp::MaterialDesc b = mbp::default_material_desc();
  a.raster.depth_test = true;
  b.raster.depth_test = false;
  const auto sa = table->intern(a);
  const auto sb = table->intern(b);
  ASSERT_TRUE(sa.ok());
  ASSERT_TRUE(sb.ok());
  EXPECT_NE(table->material(sa.value())->pipeline_key(),
            table->material(sb.value())->pipeline_key());
}

// 槽位 0 恒为默认材质;快照往返可复现。
TEST(Material, SnapshotRoundTrip) {
  auto table = mbp::create_material_table();
  ASSERT_TRUE(table->material(0) != nullptr);
  EXPECT_TRUE(table->desc_of(0)->shader == mbp::default_material_desc().shader);

  for (int i = 0; i < 5; ++i) {
    mbp::MaterialDesc d = mbp::default_material_desc();
    d.params.base_color[0] = static_cast<float>(i) / 5.0f;
    ASSERT_TRUE(table->intern(d).ok());
  }
  const auto snap = table->snapshot();

  auto other = mbp::create_material_table();
  ASSERT_TRUE(other->restore(snap).ok());
  EXPECT_EQ(other->size(), table->size());
  const auto snap2 = other->snapshot();
  ASSERT_EQ(snap.size(), snap2.size());
  for (std::size_t i = 0; i < snap.size(); ++i) EXPECT_TRUE(snap[i] == snap2[i]);
}

// 槽位不可重排:与前缀不一致的恢复必须失败。
TEST(Material, RestoreRejectsPrefixMismatch) {
  auto table = mbp::create_material_table();
  mbp::MaterialDesc d = mbp::default_material_desc();
  d.params.roughness = 0.123f;
  ASSERT_TRUE(table->intern(d).ok());
  auto snap = table->snapshot();
  snap[1].params.roughness = 0.999f;   // 篡改历史槽位
  EXPECT_FALSE(table->restore(snap).ok());
}

// ================================================================ Shader 库

namespace {

mbp::ShaderTemplateDesc make_fake_template(const char* name) {
  mbp::ShaderTemplateDesc t;
  t.name = name;
  static const std::uint8_t kFakeSpirv[16] = {1, 0, 0, 0, 0, 0, 0, 0};
  t.vertex.spirv = kFakeSpirv;
  t.vertex.size_bytes = 16;
  t.fragment = t.vertex;
  mbp::VertexInputBinding b0;
  b0.binding = 0;
  b0.stride = 12;
  t.layout.bindings.push_back(b0);
  mbp::VertexAttribute a0;
  a0.location = 0;
  a0.binding = 0;
  a0.format = mbp::VertexFormat::Float3;
  t.layout.attributes.push_back(a0);
  return t;
}

}  // namespace

TEST(ShaderLibrary, RegistersTemplatesAndRejectsDuplicates) {
  auto lib = mbp::create_shader_library(nullptr);
  const mbp::ShaderTemplateDesc t = make_fake_template(mbp::shader_names::kStandardUnlit);

  EXPECT_TRUE(lib->register_template(t).ok());
  EXPECT_TRUE(lib->has(mbp::shader_names::kStandardUnlit));
  EXPECT_EQ(lib->template_count(), 1u);
  EXPECT_FALSE(lib->register_template(t).ok()) << "重名必须拒绝";
  EXPECT_FALSE(lib->has("no_such_shader"));
}

TEST(ShaderLibrary, RejectsTemplateWithoutSpirv) {
  auto lib = mbp::create_shader_library(nullptr);
  mbp::ShaderTemplateDesc t;
  t.name = "x";
  EXPECT_FALSE(lib->register_template(t).ok());
}

TEST(ShaderLibrary, RejectsUnregisteredKey) {
  auto lib = mbp::create_shader_library(nullptr);
  const auto k = mbp::make_shader_key("never_registered");
  const auto r = lib->acquire_pipeline(k, {}, {});
  EXPECT_FALSE(r.ok());
}

// 纯软件态(无设备)不能派生可绑定管线,且**不得**把失败项留在缓存里。
TEST(ShaderLibrary, NoDeviceCannotCreatePipeline) {
  auto lib = mbp::create_shader_library(nullptr);
  ASSERT_TRUE(
      lib->register_template(make_fake_template(mbp::shader_names::kStandardUnlit)).ok());

  const auto k = mbp::make_shader_key(mbp::shader_names::kStandardUnlit);
  EXPECT_FALSE(lib->acquire_pipeline(k, {}, {}).ok());
  // 失败不缓存:pipeline 计数仍为 0。
  EXPECT_EQ(lib->pipeline_count(), 0u);
  EXPECT_EQ(lib->pipeline_create_count(), 0u);
}

// shader 名字哈希必须跨进程稳定(命令流按它复现)。
TEST(ShaderLibrary, NameHashIsStable) {
  const std::uint32_t h1 = mbp::shader_name_hash(mbp::shader_names::kStandardPbr);
  const std::uint32_t h2 = mbp::shader_name_hash("std_pbr");
  EXPECT_EQ(h1, h2);
  EXPECT_NE(h1, mbp::shader_name_hash(mbp::shader_names::kStandardUnlit));

  // FNV-1a 32 手工核算,锚定算法不被无意改变(改了会打破所有历史命令流)。
  std::uint32_t expect = 0x811c9dc5u;
  for (const char* p = "std_pbr"; *p != '\0'; ++p) {
    expect ^= static_cast<std::uint8_t>(*p);
    expect *= 0x01000193u;
  }
  EXPECT_EQ(h1, expect);
}
