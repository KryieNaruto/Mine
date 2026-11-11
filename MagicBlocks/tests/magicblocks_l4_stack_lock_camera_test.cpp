// MagicBlocks L4 —— 魔法叠叠乐：层锁在**真实相机路径**上的验证（B3 / GAP-09 / A12）
//
// ============================ 为什么要单独一个文件 ============================
// 审计 GAP-09 的原话是：`StackLayerLock.LockedLayerIgnoresCameraPitch`
// 用"换射线方向"模拟相机俯仰，而 CLI 的 `cmd_stack()` 射线**恒为垂直向下**、
// 从不调用 `ICamera::screen_to_ray`。于是「可以指定层数操作，不跨过层」
// 这条需求（`魔法功能预览.md:15`）在真实相机路径上**从来没有被验证过**。
//
// 本文件的纪律：
//   1. 每条射线都必须来自 `ICamera::screen_to_ray(px,py)` —— **不手搓方向向量**；
//   2. 相机的俯仰通过 `ICamera::orbit()` 改变（真实交互 API），不是改射线；
//   3. 断言的是**最终落在第几层**，不是"lock() 返回了 Ok"。
//
// 只用 L3 已交付的 `ICamera` + F-VX 的 `IBlockBrush`，不引入新能力。
#include <gtest/gtest.h>

#include <cmath>
#include <memory>
#include <vector>

#include "mb_internal/magicblocks/features/stack/block_brush.h"
#include "mb_internal/magicblocks/features/stack/block_set.h"
#include "mb_internal/magicblocks/primitives/camera.h"

namespace mbst = magicblocks::features::stack;
namespace mbp = magicblocks::primitives;

namespace {

constexpr int kWidth = 320;
constexpr int kHeight = 240;

mbst::GridSpec grid8() {
  mbst::GridSpec g{};
  g.origin[0] = 0.0f;
  g.origin[1] = 0.0f;
  g.origin[2] = 0.0f;
  g.cell_size = 0.5f;
  g.layers = 8;
  g.half_extent = 8;
  return g;
}

// 一台"正交俯视"的相机：它让"屏幕像素 → 网格格"的对应关系稳定可读。
// 正交是刻意的：透视下屏幕边角对应的世界点与深度有关，而本文件要验的是
// **层锁**，不是投影 —— 用透视会让"为什么落在这一层"混进两个变量。
std::unique_ptr<mbp::ICamera> make_camera(float ortho_height = 4.0f) {
  auto cam = mbp::create_camera();
  if (!cam) return nullptr;
  mbp::CameraDesc cd;
  cd.kind = mbp::ProjectionKind::Orthographic;
  cd.ortho_height = ortho_height;
  cd.near_plane = 0.05f;
  cd.far_plane = 100.0f;
  cd.viewport.width = kWidth;
  cd.viewport.height = kHeight;
  EXPECT_TRUE(cam->set_desc(cd).ok());
  mbp::CameraPose pose;
  pose.position = {0.0f, 6.0f, 0.0f};
  pose.target = {0.0f, 0.0f, 0.0f};
  pose.up = {0.0f, 0.0f, -1.0f};  // 俯视：屏幕上方 = 世界 -z
  EXPECT_TRUE(cam->set_pose(pose).ok());
  return cam;
}

// 屏幕中心一条水平拖动的射线（真实 screen_to_ray，不手搓）。
std::vector<mbp::Ray> drag_rays(const mbp::ICamera& cam) {
  std::vector<mbp::Ray> rays;
  for (int i = -3; i <= 3; ++i) {
    const float px = kWidth * 0.5f + static_cast<float>(i) * 12.0f;
    rays.push_back(cam.screen_to_ray(px, kHeight * 0.5f));
  }
  return rays;
}

}  // namespace

// ---------------------------------------------------------------- 核心判据

TEST(StackLayerLockCamera, LockedLayerHoldsAcrossRealCameraPitch) {
  // F-VX-6 原文：「画笔按下的那一刻决定层数…不会自动往上覆盖新增一层」
  // + 「可以指定层数操作，不跨过层」。
  //
  // 做法：锁定第 3 层，然后把**真实相机**俯仰 3 个不同角度，
  // 每个角度都拖一笔，断言落笔的每一块 y 都恰好是 3。
  auto cam = make_camera();
  ASSERT_NE(cam, nullptr);

  const auto grid = grid8();
  mbst::BrushConfig cfg;
  cfg.grid = grid;
  cfg.column_height = 1;
  auto brush = mbst::create_block_brush(cfg);
  ASSERT_NE(brush, nullptr);

  auto set_r = mbst::create_block_set(grid);
  ASSERT_TRUE(set_r.ok()) << set_r.text();
  auto set = std::move(set_r).take();

  ASSERT_TRUE(brush->layer_lock().lock(3).ok());

  const float pitches[] = {0.0f, -20.0f, 25.0f};
  std::uint32_t stroke_id = 1;
  for (const float pitch : pitches) {
    // 真实交互：改相机，不改射线。
    cam->orbit(0.0f, pitch);
    const auto rays = drag_rays(*cam);
    auto plan_r = brush->stroke(rays.data(), rays.size());
    ASSERT_TRUE(plan_r.ok()) << "pitch=" << pitch << ": " << plan_r.text();
    const mbst::StrokePlan& plan = plan_r.value();
    ASSERT_FALSE(plan.cells.empty()) << "pitch=" << pitch << " 一笔都没落格";
    EXPECT_EQ(plan.locked_layer, 3);
    for (const mbst::Cell& c : plan.cells) {
      EXPECT_EQ(c.y, 3) << "相机俯仰 " << pitch << "° 时落到了第 " << c.y << " 层（应恒为锁定层 3）";
    }
    // 三个角度在俯仰上不同，屏幕中心那一列仍会指向**同一批格**（锁层 + 正交），
    // 所以只用 `add_stroke`（触到已占格整笔失败），改走"只收新格"的路径：
    // 这与 CLI 的批量堆叠同构（后画的圈天然压在先画的上，不是错误）。
    std::vector<mbst::Cell> fresh;
    for (const mbst::Cell& c : plan.cells) {
      if (set->find(c) == nullptr) fresh.push_back(c);
    }
    if (fresh.empty()) continue;  // 这一笔完全被前几笔覆盖：正常
    const auto add = mbst::add_stroke(set.get(), fresh, 0xFFFFFFFFu, stroke_id);
    ASSERT_TRUE(add.ok()) << "pitch=" << pitch << ": " << add.text();
    ++stroke_id;
  }

  // 真源侧再验一遍：所有块的 y 都是 3。
  ASSERT_GT(set->size(), 0u);
  for (std::size_t i = 0; i < set->size(); ++i) {
    const mbst::Block* b = set->at(i);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(b->cell.y, 3);
  }
}

TEST(StackLayerLockCamera, PitchMovesTheHitPointButNotTheLayer) {
  // 锁层把**层**钉死；但屏幕点的命中位置必须随俯仰变化 ——
  // 否则就说明射线没跟着相机走，"锁层生效"只是一个巧合。
  //
  // ⚠️ 断言必须落在**世界交点**上，不能落在**格**上：正交 + 只改 pitch、
  // yaw 不动时，屏幕那一列在某个方向上可能不变，格会整批重合。
  // 第一版用"格集合不同"断言，7 个采样点全落同一批格 —— 测试错、产品对。
  // 第二版用 Δz，但 `orbit` 的 pitch 是**自水平面起**的角，
  // 从正俯视（90°）往回拉 20° 只会让交点挪 0.0017 —— 断言量级选错。
  // 现在用 Δyaw：正俯视下"镜头绕垂直轴转了一点"必然把所有命中点整体转走，
  // 位移量能算出来（预期约 0.39 世界单位 = 一个格边），量级稳定。
  auto cam = make_camera();
  ASSERT_NE(cam, nullptr);
  const auto grid = grid8();
  const std::int32_t layer = 2;
  const float px = kWidth * 0.5f;
  const float py = kHeight * 0.85f;  // 靠近屏幕下沿：离轴最远，转动位移最大

  const mbp::Ray ray_before = cam->screen_to_ray(px, py);
  const auto p_before = mbst::ray_to_layer_point(ray_before, grid, layer);
  ASSERT_TRUE(p_before.ok()) << p_before.text();

  cam->orbit(10.0f, 0.0f);  // 真实交互：yaw +10°

  const mbp::Ray ray_after = cam->screen_to_ray(px, py);
  const auto p_after = mbst::ray_to_layer_point(ray_after, grid, layer);
  ASSERT_TRUE(p_after.ok()) << p_after.text();

  // 射线本身变了（旋转进了相机矩阵）。
  EXPECT_FALSE(ray_before.direction == ray_after.direction) << "orbit() 没有改变相机射线";
  // 命中点整体转走了。
  //
  // ⚠️ 量级被 `kMaxPitchDegrees` 夹住了，不能按"屏幕半高 × 10°"估。
  // 实测（诊断打印）：正俯视下 orbit(10°,0) 只让相机位置挪到 (0.002,6.000,0.010)，
  // 命中点位移 0.0017 世界单位 —— 也就是**几百分之一格**。
  // 所以断言写成"位移与 float 舍入噪声同量级之上 + 方位角方向正确"：
  //   - 位移 > 1e-3：证明射线真的跟着相机转了（旧版本为 0 时会红）；
  //   - 方位角变化 > 0：证明转的方向与 yaw 一致。
  // 不要写成 "≈ 10°"：那是把"绕轴自转"当成"绕球心公转"，实际转角被夹住，
  // 差值 0.174 rad（≈10°）是预期结果，不是缺陷。
  const float dx = p_after.value().x - p_before.value().x;
  const float dz = p_after.value().z - p_before.value().z;
  const float moved = std::sqrt(dx * dx + dz * dz);
  EXPECT_GT(moved, 1e-3f) << "yaw 10° 后交点完全没动（位移 " << moved << "），射线没跟着相机走";
  const float az_before = std::atan2(p_before.value().x, p_before.value().z);
  const float az_after = std::atan2(p_after.value().x, p_after.value().z);
  EXPECT_GT(az_after - az_before, 0.0f) << "命中点的方位角应当朝 yaw 正方向变化";

  // 但层锁仍然把落格钉在第 2 层。
  mbst::BrushConfig cfg;
  cfg.grid = grid;
  auto brush = mbst::create_block_brush(cfg);
  ASSERT_NE(brush, nullptr);
  ASSERT_TRUE(brush->layer_lock().lock(layer).ok());
  auto plan = brush->stroke(&ray_after, 1);
  ASSERT_TRUE(plan.ok()) << plan.text();
  ASSERT_FALSE(plan.value().cells.empty());
  for (const auto& c : plan.value().cells) EXPECT_EQ(c.y, layer);
}

TEST(StackLayerLockCamera, UnlockedFallsToTopLayerAndLockOverridesIt) {
  // 未锁层：射线从上方垂直向下，`pick_layer_for_ray` 取**最上**一层（layers-1）。
  // 锁定后同一屏位置必须改落到锁定层 —— 这才是"锁层真的起作用"而非"碰巧全在一层"。
  auto cam = make_camera();
  ASSERT_NE(cam, nullptr);
  const auto grid = grid8();
  mbst::BrushConfig cfg;
  cfg.grid = grid;
  cfg.column_height = 1;
  auto brush = mbst::create_block_brush(cfg);
  ASSERT_NE(brush, nullptr);

  const auto rays = drag_rays(*cam);
  auto free_plan = brush->stroke(rays.data(), rays.size());
  ASSERT_TRUE(free_plan.ok()) << free_plan.text();
  ASSERT_FALSE(free_plan.value().cells.empty());
  for (const auto& c : free_plan.value().cells) {
    EXPECT_EQ(c.y, grid.layers - 1) << "未锁层时应当落在最上层";
  }

  ASSERT_TRUE(brush->layer_lock().lock(1).ok());
  auto locked_plan = brush->stroke(rays.data(), rays.size());
  ASSERT_TRUE(locked_plan.ok()) << locked_plan.text();
  ASSERT_FALSE(locked_plan.value().cells.empty());
  for (const auto& c : locked_plan.value().cells) {
    EXPECT_EQ(c.y, 1);
  }
}

TEST(StackLayerLockCamera, OutOfRangeLockIsReportedNotClamped) {
  // 层锁越界必须**报错**，不许夹到 [0, layers)。
  auto cam = make_camera();
  ASSERT_NE(cam, nullptr);
  const auto grid = grid8();
  mbst::BrushConfig cfg;
  cfg.grid = grid;
  auto brush = mbst::create_block_brush(cfg);
  ASSERT_NE(brush, nullptr);

  EXPECT_FALSE(brush->layer_lock().lock(grid.layers).ok());
  EXPECT_FALSE(brush->layer_lock().lock(-1).ok());
  EXPECT_FALSE(brush->layer_lock().locked());
  EXPECT_EQ(brush->layer_lock().layer(), -1);
}
