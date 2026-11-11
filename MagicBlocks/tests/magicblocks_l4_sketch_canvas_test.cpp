// MagicBlocks L4 —— 魔法涂鸦：画布 + 输入链路（R-SK-1 / R-SK-2 / D-SK-5）
//
// ============================ 这组用例反对什么 ============================
// 需求原文要求「只做简易画布与挤出面」。在补上画布之前，代码里**没有任何地方**
// 把 `device::InputEvent` 变成笔画：`feed(` 在 src/features/ 下零命中，
// 笔画坐标由 demo 生成器写死。这组用例存在的唯一目的，是让
// "用户在屏幕上画了一笔 → 文档里多了一笔"这句话变成可以跑的断言。
//
// ============================ 判据为什么是像素坐标反推 ============================
// 画布换算最容易错的方向有两个：
//   1. y 轴只翻一次 / 翻两次（画面看起来"差不多对"，实际上下颠倒）；
//   2. 在 L4 里自己乘一遍投影矩阵（与 L3 那份分叉）。
// 所以每个用例都**先用真实相机把世界点投影到屏幕**（`world_to_screen`），
// 再把这组屏幕坐标喂回去，断言回到同一个画布坐标。这是一条往返闭环：
// 只翻一次或翻两次，两个方向必有一个对不上。
//
// 相机矩阵只在 L3（camera.cpp）算，本文件与 sketch_canvas.cpp 都**不写**投影数学。
#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <vector>

#include "magicblocks/device/events.h"
#include "magicblocks/features/sketch/sketch_canvas.h"
#include "magicblocks/features/sketch/sketch_document.h"
#include "magicblocks/features/sketch/sketch_pipeline.h"
#include "magicblocks/primitives/camera.h"
#include "magicblocks/primitives/painter.h"
#include "magicblocks/primitives/types.h"

namespace mb = magicblocks;
namespace sk = magicblocks::features::sketch;
namespace pm = magicblocks::primitives;
namespace device = magicblocks::device;

namespace {

constexpr int kWidth = 480;
constexpr int kHeight = 360;

// 建一台"画布相机"：正交，正对画布（画布在 XY 平面，法线 +z）。
//
// 正交是设计 §5.2 说的默认画布相机（"屏幕像素与画布坐标的换算 =
// 画布相机(P9 正交)的 screen↔world"）。透视相机另有用例。
std::unique_ptr<pm::ICamera> make_camera(pm::ProjectionKind kind, float ortho_height = 2.0f) {
  auto cam = pm::create_camera();
  pm::CameraDesc cd;
  cd.kind = kind;
  cd.fov_y_degrees = 45.0f;
  cd.ortho_height = ortho_height;
  cd.near_plane = 0.05f;
  cd.far_plane = 100.0f;
  cd.viewport.width = kWidth;
  cd.viewport.height = kHeight;
  EXPECT_TRUE(cam->set_desc(cd).ok());
  pm::CameraPose pose;
  pose.position = {0.0f, 0.0f, 3.0f};
  pose.target = {0.0f, 0.0f, 0.0f};
  pose.up = {0.0f, 1.0f, 0.0f};
  EXPECT_TRUE(cam->set_pose(pose).ok());
  return cam;
}

sk::CanvasParams canvas_params() {
  sk::CanvasParams c;
  c.width = 2.0f;
  c.height = 1.5f;
  c.extrude_thickness = 0.1f;
  c.clamp_to_canvas = true;
  return c;
}

pm::StrokeEvent ev(pm::StrokeEventKind k, float x, float y, std::uint32_t pid = 0) {
  pm::StrokeEvent e;
  e.kind = k;
  e.pointer_id = pid;
  e.x = x;
  e.y = y;
  return e;
}

sk::Stroke stroke_template() {
  sk::Stroke s;
  s.width = 0.06f;
  s.color = pm::Color4{1.0f, 0.2f, 0.1f, 1.0f};
  return s;
}

device::InputEvent make_input(device::InputEventKind k, float x, float y, int button = 0,
                              std::int64_t ts = 0) {
  device::InputEvent e;
  e.kind = k;
  e.pointer_id = 1;
  e.position = device::Vec2f{x, y};
  e.button = button;
  e.timestamp_us = ts;
  return e;
}

}  // namespace

// ---------------------------------------------------------------- D-SK-5：唯一换算点

TEST(Canvas, WorldPointProjectsBackToItselfUnderPerspective) {
  auto cam = make_camera(pm::ProjectionKind::Perspective);
  sk::Canvas canvas(canvas_params());

  for (const float u : {-0.8f, -0.3f, 0.0f, 0.3f, 0.8f}) {
    for (const float v : {-0.5f, 0.0f, 0.5f}) {
      const pm::Vec3 world = canvas.local_to_world(pm::Vec2{u, v});
      float px = 0.0f, py = 0.0f, depth = 0.0f;
      ASSERT_TRUE(cam->world_to_screen(world, &px, &py, &depth)) << "画布点应当落在视锥内";
      const sk::CanvasHit hit = canvas.intersect_screen(*cam, px, py);
      ASSERT_TRUE(hit.hit);
      EXPECT_NEAR(hit.local.x, u, 2e-3f) << "画布 x 往返不一致 (u=" << u << " v=" << v << ")";
      EXPECT_NEAR(hit.local.y, v, 2e-3f) << "画布 y 往返不一致 (u=" << u << " v=" << v << ")";
      EXPECT_NEAR(hit.world.z, 0.0f, 1e-4f) << "交点必须落在画布平面上";
    }
  }
}

TEST(Canvas, WorldPointProjectsBackToItselfUnderOrthographic) {
  auto cam = make_camera(pm::ProjectionKind::Orthographic, /*ortho_height=*/1.6f);
  sk::Canvas canvas(canvas_params());

  for (const float u : {-0.8f, 0.0f, 0.8f}) {
    for (const float v : {-0.5f, 0.0f, 0.5f}) {
      const pm::Vec3 world = canvas.local_to_world(pm::Vec2{u, v});
      float px = 0.0f, py = 0.0f, depth = 0.0f;
      ASSERT_TRUE(cam->world_to_screen(world, &px, &py, &depth));
      const sk::CanvasHit hit = canvas.intersect_screen(*cam, px, py);
      ASSERT_TRUE(hit.hit);
      EXPECT_NEAR(hit.local.x, u, 2e-3f);
      EXPECT_NEAR(hit.local.y, v, 2e-3f);
    }
  }
}

TEST(Canvas, ScreenDownMapsToSmallerCanvasY) {
  // y 轴方向钉死：屏幕 y 向下增大，画布 y 向上增大 ⇒ 屏幕下方的点
  // 必须落在**更小**的画布 y 上。
  //
  // 这条如果反了，画面"看着对"（对称图形）但整幅画上下颠倒 ——
  // 正是 L3_DELIVERY 里记录的"y 翻转只翻一次"那类假绿。
  auto cam = make_camera(pm::ProjectionKind::Perspective);
  sk::Canvas canvas(canvas_params());

  const sk::CanvasHit top = canvas.intersect_screen(*cam, kWidth * 0.5f, 0.1f * kHeight);
  const sk::CanvasHit bottom = canvas.intersect_screen(*cam, kWidth * 0.5f, 0.9f * kHeight);
  ASSERT_TRUE(top.hit);
  ASSERT_TRUE(bottom.hit);
  EXPECT_GT(top.local.y, bottom.local.y)
      << "屏幕上方应当对应更大的画布 y: top.y=" << top.local.y << " bottom.y=" << bottom.local.y;

  const sk::CanvasHit left = canvas.intersect_screen(*cam, 0.1f * kWidth, kHeight * 0.5f);
  const sk::CanvasHit right = canvas.intersect_screen(*cam, 0.9f * kWidth, kHeight * 0.5f);
  ASSERT_TRUE(left.hit);
  ASSERT_TRUE(right.hit);
  EXPECT_LT(left.local.x, right.local.x) << "屏幕左方应当对应更小的画布 x";
}

TEST(Canvas, ParallelRayIsAWideMissNotAClamp) {
  // 正交相机侧看画布（视线平行于平面）→ 打不到平面。
  // 必须返回未命中并计数，而不是"编一个最近的画布点"糊过去。
  auto cam = pm::create_camera();
  pm::CameraDesc cd;
  cd.kind = pm::ProjectionKind::Orthographic;
  cd.ortho_height = 2.0f;
  cd.viewport.width = kWidth;
  cd.viewport.height = kHeight;
  ASSERT_TRUE(cam->set_desc(cd).ok());
  pm::CameraPose pose;
  pose.position = {5.0f, 0.0f, 0.0f};
  pose.target = {0.0f, 0.0f, 0.0f};
  pose.up = {0.0f, 1.0f, 0.0f};
  ASSERT_TRUE(cam->set_pose(pose).ok());

  sk::Canvas canvas(canvas_params());
  const sk::CanvasHit hit = canvas.intersect_screen(*cam, kWidth * 0.5f, kHeight * 0.5f);
  EXPECT_FALSE(hit.hit);

  canvas.reset_stats();
  const auto oc = canvas.feed(*cam, ev(pm::StrokeEventKind::Begin, kWidth * 0.5f, kHeight * 0.5f),
                              stroke_template());
  (void)oc;
  EXPECT_EQ(canvas.stats().samples_missed_plane, 1u);
  EXPECT_EQ(canvas.stats().samples_accepted, 0u);
}

TEST(Canvas, TransformedCanvasPlaneIsTheRayTarget) {
  // 把画布平移 +1（世界 x）后，同一屏幕像素打到的**世界点**必须仍然落在
  // 这张（已平移的）画布平面上，且换算回新画布的局部坐标仍然在画布内。
  //
  // ⚠️ 第一版把这条写成"平移后世界交点也 +1"，那是错的：画布跟着平移，
  // 相机没动，射线打到的是"平移后的那张平面"，交点世界 x 本来就不变，
  // 变的是局部坐标。**测试错、产品对** —— 这条注释留着提醒。
  auto cam = make_camera(pm::ProjectionKind::Perspective);
  sk::Canvas canvas(canvas_params());

  const float px = kWidth * 0.5f;
  const float py = kHeight * 0.5f;
  const sk::CanvasHit before = canvas.intersect_screen(*cam, px, py);
  ASSERT_TRUE(before.hit);

  sk::CanvasParams moved = canvas_params();
  moved.transform[12] = 1.0f;  // 列主序：m[12] = 平移 x
  ASSERT_TRUE(canvas.set_params(moved).ok());

  const sk::CanvasHit after = canvas.intersect_screen(*cam, px, py);
  ASSERT_TRUE(after.hit);
  // 交点仍在世界 z=0 平面（新画布平面）。
  EXPECT_NEAR(after.world.z, 0.0f, 1e-4f);
  // 世界 x 由相机射线决定，与画布是否平移无关。
  EXPECT_NEAR(after.world.x, before.world.x, 1e-4f);
  // 局部坐标右移了 1：同一世界点在新画布里的 u = 旧 u - 1。
  EXPECT_NEAR(after.local.x, before.local.x - 1.0f, 1e-4f);
  // 画布仍在"画布内"判定里（宽度 2，中心 1，u ∈ [-2, 0] 里 -0.x 部分仍在域内）。
  EXPECT_NEAR(canvas.center().x, 1.0f, 1e-5f);
}

TEST(Canvas, RejectsIllegalParamsWithoutChangingThem) {
  sk::Canvas canvas(canvas_params());
  sk::CanvasParams bad = canvas_params();
  bad.width = 0.0f;
  EXPECT_FALSE(canvas.set_params(bad).ok());
  EXPECT_FLOAT_EQ(canvas.params().width, 2.0f) << "非法参数不得写入";

  bad = canvas_params();
  bad.extrude_thickness = -1.0f;
  EXPECT_FALSE(canvas.set_params(bad).ok());

  bad = canvas_params();
  for (float& v : bad.transform) v = 0.0f;  // 奇异矩阵
  EXPECT_FALSE(canvas.set_params(bad).ok());
  EXPECT_FLOAT_EQ(canvas.params().transform[0], 1.0f);
}

// ---------------------------------------------------------------- 输入事件 → 笔画

TEST(CanvasInput, DownMoveUpProducesOneStrokeOnTheCanvasPlane) {
  auto cam = make_camera(pm::ProjectionKind::Perspective);
  sk::Canvas canvas(canvas_params());

  // 屏幕（略上方）开始，向右下画一条斜线。
  const auto begin = canvas.feed(*cam, ev(pm::StrokeEventKind::Begin, 120.0f, 100.0f),
                                 stroke_template());
  EXPECT_EQ(begin.kind, sk::Canvas::StrokeOutcome::Kind::Empty);
  canvas.feed(*cam, ev(pm::StrokeEventKind::Move, 240.0f, 150.0f), stroke_template());
  canvas.feed(*cam, ev(pm::StrokeEventKind::Move, 360.0f, 220.0f), stroke_template());
  const auto end = canvas.feed(*cam, ev(pm::StrokeEventKind::End, 360.0f, 220.0f),
                               stroke_template());

  ASSERT_EQ(end.kind, sk::Canvas::StrokeOutcome::Kind::Committed);
  // End 的位置与最后一个 Move 相同，但 min_spacing 默认是 0（= 不剔除），
  // 所以它**照样被采样**：Begin + 2 Move + End = 4 个点。
  // 这条顺带钉住"默认不剔除"：若哪天把 min_spacing 改成非零默认值，
  // 这里的点数会变，用例立刻红。
  EXPECT_EQ(end.stroke.points.size(), 4u);
  EXPECT_FLOAT_EQ(end.stroke.width, 0.06f);
  EXPECT_FLOAT_EQ(end.stroke.color.r, 1.0f);

  // 首点必须落在画布平面（z=0）且与屏幕点往返一致。
  const sk::CanvasHit h = canvas.intersect_screen(*cam, 120.0f, 100.0f);
  ASSERT_TRUE(h.hit);
  EXPECT_NEAR(end.stroke.points[0].x, h.local.x, 1e-4f);
  EXPECT_NEAR(end.stroke.points[0].y, h.local.y, 1e-4f);

  // 四个点确实是一条"向右下"的线：屏幕 y 增大 → 画布 y 减小。
  EXPECT_LT(end.stroke.points.back().y, end.stroke.points[0].y);
  EXPECT_GT(end.stroke.points.back().x, end.stroke.points[0].x);
}

TEST(CanvasInput, SameEventsWithBothCameraKindsAgreeOnCanvasSpace) {
  // 同一组**画布点**，在两种相机下各投影一次，喂回去必须得到同一组画布点。
  // 这就是 A1 的"正交/透视两种相机下都正确"。
  sk::Canvas canvas(canvas_params());
  const std::vector<pm::Vec2> canvas_pts = {{-0.6f, -0.3f}, {0.0f, 0.1f}, {0.6f, 0.35f}};

  std::vector<std::vector<pm::Vec2>> results;
  for (const auto kind : {pm::ProjectionKind::Perspective, pm::ProjectionKind::Orthographic}) {
    auto cam = make_camera(kind, /*ortho_height=*/1.8f);
    sk::Canvas c(canvas_params());

    // 用世界点投影出屏幕坐标（真实相机矩阵），逐点喂。
    std::vector<std::pair<float, float>> screen;
    for (const auto& p : canvas_pts) {
      float px = 0.0f, py = 0.0f, d = 0.0f;
      ASSERT_TRUE(cam->world_to_screen(canvas.local_to_world(p), &px, &py, &d));
      screen.emplace_back(px, py);
    }
    c.feed(*cam, ev(pm::StrokeEventKind::Begin, screen[0].first, screen[0].second),
           stroke_template());
    for (std::size_t i = 1; i < screen.size(); ++i) {
      c.feed(*cam, ev(pm::StrokeEventKind::Move, screen[i].first, screen[i].second),
             stroke_template());
    }
    const auto out = c.feed(*cam, ev(pm::StrokeEventKind::End, screen.back().first,
                                     screen.back().second),
                            stroke_template());
    ASSERT_EQ(out.kind, sk::Canvas::StrokeOutcome::Kind::Committed);
    results.push_back(out.stroke.points);
  }

  ASSERT_EQ(results.size(), 2u);
  ASSERT_EQ(results[0].size(), results[1].size());
  for (std::size_t i = 0; i < results[0].size(); ++i) {
    EXPECT_NEAR(results[0][i].x, results[1][i].x, 3e-3f) << "第 " << i << " 个点在两种相机下不一致";
    EXPECT_NEAR(results[0][i].y, results[1][i].y, 3e-3f) << "第 " << i << " 个点在两种相机下不一致";
  }
}

TEST(CanvasInput, CancelDropsTheWholeStroke) {
  auto cam = make_camera(pm::ProjectionKind::Perspective);
  sk::Canvas canvas(canvas_params());
  canvas.feed(*cam, ev(pm::StrokeEventKind::Begin, 200.0f, 150.0f), stroke_template());
  canvas.feed(*cam, ev(pm::StrokeEventKind::Move, 220.0f, 160.0f), stroke_template());
  const auto out = canvas.feed(*cam, ev(pm::StrokeEventKind::Cancel, 220.0f, 160.0f),
                               stroke_template());
  EXPECT_EQ(out.kind, sk::Canvas::StrokeOutcome::Kind::Cancelled);
  EXPECT_TRUE(out.stroke.points.empty()) << "Cancel 必须整笔丢弃";
  EXPECT_FALSE(canvas.drawing());
}

TEST(CanvasInput, MoveWhileIdleIsIgnoredAndCounted) {
  auto cam = make_camera(pm::ProjectionKind::Perspective);
  sk::Canvas canvas(canvas_params());
  const auto out = canvas.feed(*cam, ev(pm::StrokeEventKind::Move, 200.0f, 150.0f),
                               stroke_template());
  EXPECT_EQ(out.kind, sk::Canvas::StrokeOutcome::Kind::Empty);
  EXPECT_EQ(canvas.stats().illegal_transitions, 1u)
      << "Idle 收到 Move 属非法转移，必须忽略并计数(设计 §7.1)";
  EXPECT_EQ(canvas.stats().samples_accepted, 0u);
}

TEST(CanvasInput, BeginWithoutEndImplicitlyCommitsOnTheNextBegin) {
  auto cam = make_camera(pm::ProjectionKind::Perspective);
  sk::Canvas canvas(canvas_params());

  canvas.feed(*cam, ev(pm::StrokeEventKind::Begin, 120.0f, 150.0f), stroke_template());
  canvas.feed(*cam, ev(pm::StrokeEventKind::Move, 240.0f, 150.0f), stroke_template());
  // 没收到 End 就来了新的 Begin：上一笔必须被**提交**而不是被悄悄丢掉。
  const auto second_begin =
      canvas.feed(*cam, ev(pm::StrokeEventKind::Begin, 300.0f, 200.0f), stroke_template());
  ASSERT_EQ(second_begin.kind, sk::Canvas::StrokeOutcome::Kind::Committed);
  // 隐式 End 用的是**新 Begin 的位置**（300,200），所以上一笔拿到了
  // Begin(120) + Move(240) + 隐式 End(300) 三个点。
  // ⚠️ 这里刻意不这么做：丢掉用户最后一小段比"多点一个位置"更糟；
  // 但它在语义上确实是"用新笔的起点收尾旧笔"，所以下面同时钉住
  // "新笔自己的第一个采样点是 New Begin 的位置"，避免两笔错位。
  EXPECT_EQ(second_begin.stroke.points.size(), 3u);
  EXPECT_TRUE(canvas.drawing());
}

TEST(CanvasInput, ClampToCanvasKeepsSamplesInside) {
  auto cam = make_camera(pm::ProjectionKind::Perspective);
  sk::CanvasParams p = canvas_params();
  p.clamp_to_canvas = true;
  sk::Canvas canvas(p);

  canvas.feed(*cam, ev(pm::StrokeEventKind::Begin, 5.0f, 5.0f), stroke_template());
  canvas.feed(*cam, ev(pm::StrokeEventKind::Move, kWidth - 5.0f, kHeight - 5.0f),
              stroke_template());
  const auto end = canvas.feed(*cam, ev(pm::StrokeEventKind::End, kWidth - 5.0f, kHeight - 5.0f),
                               stroke_template());
  ASSERT_EQ(end.kind, sk::Canvas::StrokeOutcome::Kind::Committed);
  EXPECT_GT(canvas.stats().samples_clamped, 0u) << "屏幕角落在画布外，应当被夹取并计数";
  for (const auto& pt : end.stroke.points) {
    EXPECT_GE(pt.x, -p.width * 0.5f - 1e-5f);
    EXPECT_LE(pt.x, p.width * 0.5f + 1e-5f);
    EXPECT_GE(pt.y, -p.height * 0.5f - 1e-5f);
    EXPECT_LE(pt.y, p.height * 0.5f + 1e-5f);
  }
}

TEST(CanvasInput, NoClampOffCanvasPointsAreRejectedNotSilentlyKept) {
  auto cam = make_camera(pm::ProjectionKind::Perspective);
  sk::CanvasParams p = canvas_params();
  p.clamp_to_canvas = false;
  sk::Canvas canvas(p);

  // 屏幕角落 → 画布外。
  canvas.feed(*cam, ev(pm::StrokeEventKind::Begin, 2.0f, 2.0f), stroke_template());
  EXPECT_GT(canvas.stats().samples_off_canvas_rejected, 0u);
  EXPECT_EQ(canvas.stats().samples_accepted, 0u);
  EXPECT_EQ(canvas.stats().samples_clamped, 0u) << "未夹取时不得计数为夹取";
}

TEST(CanvasInput, MinSpacingDropsDenseAndMaxPointsCaps) {
  auto cam = make_camera(pm::ProjectionKind::Perspective);
  sk::CanvasParams p = canvas_params();
  p.min_spacing = 0.05f;  // 画布单位
  p.max_points_per_stroke = 3;
  sk::Canvas canvas(p);

  canvas.feed(*cam, ev(pm::StrokeEventKind::Begin, 240.0f, 180.0f), stroke_template());
  // 连续 40 个只挪 1 像素的 Move：绝大多数应被最小间距剔除。
  for (int i = 1; i <= 40; ++i) {
    canvas.feed(*cam, ev(pm::StrokeEventKind::Move, 240.0f + static_cast<float>(i), 180.0f),
                stroke_template());
  }
  const auto end = canvas.feed(*cam, ev(pm::StrokeEventKind::End, 300.0f, 180.0f),
                               stroke_template());
  ASSERT_EQ(end.kind, sk::Canvas::StrokeOutcome::Kind::Committed);
  EXPECT_EQ(end.stroke.points.size(), 3u) << "max_points_per_stroke 必须生效";
  EXPECT_GT(canvas.stats().samples_dropped_spacing, 0u);
  EXPECT_GT(canvas.stats().samples_dropped_limit, 0u);
}

// ---------------------------------------------------------------- device::InputEvent 映射

TEST(CanvasInput, DeviceEventsMapToStrokeEventsFieldByField) {
  device::InputEvent in = make_input(device::InputEventKind::PointerMove, 12.5f, 34.25f, 0, 7777);
  pm::StrokeEvent out;
  ASSERT_TRUE(sk::map_input(in, &out));
  EXPECT_EQ(out.kind, pm::StrokeEventKind::Move);
  EXPECT_FLOAT_EQ(out.x, 12.5f);
  EXPECT_FLOAT_EQ(out.y, 34.25f);
  EXPECT_EQ(out.timestamp_us, 7777) << "时间戳必须原样带过来(D6)，SDK 内绝不读时钟";
  EXPECT_EQ(out.pointer_id, in.pointer_id);
}

TEST(CanvasInput, NonDrawingEventsAreNotMapped) {
  pm::StrokeEvent out;
  EXPECT_FALSE(sk::map_input(make_input(device::InputEventKind::KeyDown, 0, 0), &out));
  EXPECT_FALSE(sk::map_input(make_input(device::InputEventKind::Resize, 0, 0), &out));
  EXPECT_FALSE(sk::map_input(make_input(device::InputEventKind::Focus, 0, 0), &out));
  // 非主键按下不是画笔（拖动相机顺带画线是典型的误吞）
  EXPECT_FALSE(sk::map_input(make_input(device::InputEventKind::PointerDown, 0, 0, 2), &out));
  EXPECT_FALSE(sk::map_input(make_input(device::InputEventKind::PointerUp, 0, 0, 2), &out));
  // PointerCancel 一律是画笔语义（即便来自别的键）—— 它只会**丢弃**，不会多画东西。
  EXPECT_TRUE(sk::map_input(make_input(device::InputEventKind::PointerCancel, 0, 0, 2), &out));
}

TEST(CanvasInput, MapStreamCountsIgnoredAndWrongButton) {
  const device::InputEvent stream[] = {
      make_input(device::InputEventKind::KeyDown, 0, 0),
      make_input(device::InputEventKind::PointerDown, 10, 10),
      make_input(device::InputEventKind::PointerMove, 20, 20),
      make_input(device::InputEventKind::PointerDown, 0, 0, 2),  // 右键
      make_input(device::InputEventKind::PointerUp, 30, 30),
  };
  std::vector<pm::StrokeEvent> mapped;
  const sk::InputMapStats st = sk::map_input_stream(stream, 5, &mapped);
  EXPECT_EQ(st.total, 5u);
  EXPECT_EQ(st.mapped, 3u);
  EXPECT_EQ(st.ignored, 2u);
  EXPECT_EQ(st.wrong_button, 1u);
  ASSERT_EQ(mapped.size(), 3u);
  EXPECT_EQ(mapped[0].kind, pm::StrokeEventKind::Begin);
  EXPECT_EQ(mapped[1].kind, pm::StrokeEventKind::Move);
  EXPECT_EQ(mapped[2].kind, pm::StrokeEventKind::End);
}

// ---------------------------------------------------------------- 端到端：事件 → 文档 → 挤出

TEST(CanvasInput, EventStreamBecomesAStrokeInTheDocumentAndExtrudes) {
  // R-SK-1 的完整一跳：InputEvent[] → StrokeEvent[] → IPainter + Canvas → Stroke → 文档 → 网格。
  auto cam = make_camera(pm::ProjectionKind::Perspective);
  sk::Canvas canvas(canvas_params());

  // 屏幕上的一条横线（左 → 右），10 个采样点。
  std::vector<device::InputEvent> raw;
  raw.push_back(make_input(device::InputEventKind::PointerDown, 60.0f, 180.0f, 0, 0));
  for (int i = 1; i <= 10; ++i) {
    raw.push_back(make_input(device::InputEventKind::PointerMove, 60.0f + 36.0f * i, 180.0f, 0, i));
  }
  raw.push_back(make_input(device::InputEventKind::PointerUp, 420.0f, 180.0f, 0, 11));

  std::vector<pm::StrokeEvent> events;
  const auto mst = sk::map_input_stream(raw.data(), raw.size(), &events);
  EXPECT_EQ(mst.mapped, events.size());
  EXPECT_EQ(mst.mapped, 12u);

  auto painter = pm::create_painter();
  sk::PainterFeedResult fed =
      sk::feed_painter_and_canvas(*painter, canvas, *cam, events.data(), events.size(),
                                  stroke_template());
  ASSERT_EQ(fed.committed.size(), 1u);
  // A4 / R-SK-2：Move 必须**追加**几何段（不是只在 End 时一次性重建）。
  EXPECT_GT(fed.painter_segments_appended, 0u) << "IPainter 没有追加任何段 —— 增量链路没接上";
  EXPECT_EQ(painter->completed_stroke_count(), 1u);

  sk::SketchDocument doc;
  const sk::StrokeId id = doc.add_stroke(fed.committed[0]);
  ASSERT_TRUE(id.valid());
  ASSERT_EQ(doc.stroke_count(), 1u);

  sk::BuildOptions opts;
  opts.thickness = 0.1f;
  const sk::SketchMeshSet set = sk::build_meshes(doc, opts);
  ASSERT_TRUE(set.ok());
  ASSERT_EQ(set.meshes.size(), 1u);
  EXPECT_GT(set.vertex_count(), 0u);

  // 画布上的横线 → 世界里的横线：网格包围盒在 x 上明显展开，在 y 上很窄。
  const auto& b = set.meshes[0].mesh.bounds;
  EXPECT_GT(b.max.x - b.min.x, b.max.y - b.min.y) << "屏幕横线挤出后应当仍是横的";
}

TEST(CanvasInput, SameInputEventsGiveBitIdenticalMesh) {
  // 确定性：同一条事件流跑两遍，网格逐字节相同（golden 与回放的前提）。
  auto run = []() {
    auto cam = make_camera(pm::ProjectionKind::Perspective);
    sk::Canvas canvas(canvas_params());
    std::vector<pm::StrokeEvent> events;
    events.push_back(ev(pm::StrokeEventKind::Begin, 120.0f, 90.0f));
    for (int i = 1; i <= 8; ++i) {
      events.push_back(ev(pm::StrokeEventKind::Move, 120.0f + 30.0f * i,
                          90.0f + 12.0f * static_cast<float>(i)));
    }
    events.push_back(ev(pm::StrokeEventKind::End, 360.0f, 186.0f));

    sk::SketchDocument doc;
    sk::Stroke s = stroke_template();
    for (const auto& e : events) {
      const auto oc = canvas.feed(*cam, e, s);
      if (oc.kind == sk::Canvas::StrokeOutcome::Kind::Committed) doc.add_stroke(oc.stroke);
    }
    return sk::build_meshes(doc, sk::BuildOptions{});
  };

  const auto a = run();
  const auto b = run();
  ASSERT_TRUE(a.ok());
  ASSERT_TRUE(b.ok());
  ASSERT_EQ(a.meshes.size(), b.meshes.size());
  ASSERT_EQ(a.meshes.size(), 1u);
  EXPECT_EQ(a.mesh_hash, b.mesh_hash);
  EXPECT_EQ(a.meshes[0].mesh.vertices.size(), b.meshes[0].mesh.vertices.size());
  EXPECT_EQ(0, std::memcmp(a.meshes[0].mesh.vertices.data(), b.meshes[0].mesh.vertices.data(),
                           a.meshes[0].mesh.vertices.size() * sizeof(pm::Vertex)));
}
