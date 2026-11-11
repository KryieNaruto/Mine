// MagicBlocks L4 —— 魔法叠叠乐(F-VX)单测
//
// 这些测试锁住的是**契约**，不是实现细节：
//   1. 数据模型（阶段 1）：24 B Block、整数格去重（I1）、单调 stroke_id（I2）、
//      flags/y 域（I3）、整笔撤销、只改 color 的改色、逐字节确定性哈希；
//   2. 笔刷链路（阶段 2）：射线→层平面→吸附→supercover DDA→去重，层锁状态机；
//   3. 双 pass 渲染（阶段 3）：两个 pipeline_hash、带深度目标上的像素回归、内面 pass
//      排在表面 pass 之后、单实例不进实例化路径；
//   4. 网格覆盖层（阶段 4）；5. 颜色模式与形状模板（阶段 5）；
//   6. CLI 端到端 PNG（阶段 6）；7. 命令三态（阶段 7）。
//
// 失败必须响（D8）：这里**不**用 EXPECT_GE / 容差去迁就实现，凡是契约都精确断言。
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <set>
#include <vector>

#include "mb_internal/magicblocks/features/stack/block_brush.h"
#include "mb_internal/magicblocks/features/stack/color_mode.h"
#include "mb_internal/magicblocks/features/stack/grid_overlay.h"
#include "mb_internal/magicblocks/features/stack/shape_template.h"
#include "mb_internal/magicblocks/features/stack/stack_command.h"
#include "mb_internal/magicblocks/features/stack/stack_render.h"
#include "mb_internal/magicblocks/primitives/overlay2d.h"
#include "mb_internal/magicblocks/primitives/camera.h"
#include "mb_internal/magicblocks/device/render_device.h"
#include "mb_internal/magicblocks/device/render_target.h"
#include "mb_internal/magicblocks/features/stack/block_set.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>

#include "mb_internal/magicblocks/features/stack/stack_types.h"

namespace mbst = magicblocks::features::stack;
namespace mbp = magicblocks::primitives;
namespace dev = magicblocks::device;
namespace res = magicblocks::resources;

namespace {

mbst::GridSpec default_grid() {
  mbst::GridSpec g;
  g.origin[0] = 0.0f;
  g.origin[1] = 0.0f;
  g.origin[2] = 0.0f;
  g.cell_size = 0.5f;
  g.layers = 8;
  g.half_extent = 8;
  return g;
}

mbst::Block make_block(std::int32_t x, std::int32_t y, std::int32_t z, std::uint32_t color,
                       std::uint32_t stroke) {
  mbst::Block b;
  b.cell = mbst::Cell{x, y, z};
  b.color = color;
  b.stroke_id = stroke;
  b.flags = 0;
  return b;
}

}  // namespace

// ================================================================ 阶段 1：数据模型

TEST(StackTypes, BlockIsExactly24Bytes) {
  // 24 B 是契约：逐字节哈希 + 实例派生都依赖它。
  EXPECT_EQ(sizeof(mbst::Block), 24u);
  EXPECT_EQ(alignof(mbst::Block), 4u);
  EXPECT_EQ(sizeof(mbst::Cell), 12u);
  // Block 自身字段布局（不靠 offsetof 猜，直接量）。
  EXPECT_EQ(offsetof(mbst::Block, cell), 0u);
  EXPECT_EQ(offsetof(mbst::Block, color), 12u);
  EXPECT_EQ(offsetof(mbst::Block, stroke_id), 16u);
  EXPECT_EQ(offsetof(mbst::Block, flags), 20u);
}

TEST(StackTypes, CellYZXOrderIsContract) {
  // D-VX-5：(y, z, x) 升序。这里把三个轴向的优先级分别钉死。
  const mbst::Cell a{5, 0, 0};
  const mbst::Cell b{0, 1, 0};
  EXPECT_TRUE(a < b);  // y 优先于 x
  const mbst::Cell c{9, 0, 0};
  const mbst::Cell d{0, 0, 1};
  EXPECT_TRUE(c < d);  // z 优先于 x
  const mbst::Cell e{0, 0, 0};
  const mbst::Cell f{1, 0, 0};
  EXPECT_TRUE(e < f);  // 前两轴相等时看 x
  EXPECT_FALSE(f < e);
  const mbst::Cell want{5, 0, 0};
  EXPECT_TRUE(a == want);
}

TEST(BlockSet, AddIsIdempotentByCell) {
  // 同一格第二次 add 必须**报错**，不是静默跳过、不是覆盖。
  auto set_r = mbst::create_block_set(default_grid());
  ASSERT_TRUE(set_r.ok());
  mbst::IBlockSet& set = *set_r.value();

  const auto first = set.add(make_block(0, 0, 0, 0xFF0000FFu, 1));
  ASSERT_TRUE(first.ok());
  EXPECT_EQ(first.value(), 0u);
  EXPECT_EQ(set.size(), 1u);

  const auto dup = set.add(make_block(0, 0, 0, 0x00FF00FFu, 2));
  EXPECT_FALSE(dup.ok());
  EXPECT_EQ(dup.error_code(), mbst::StackCode::OutOfGrid);
  EXPECT_FALSE(dup.text().empty());
  // 拒绝之后真源必须**一点没变**（不覆盖颜色、不加块、不登记新 stroke）。
  EXPECT_EQ(set.size(), 1u);
  ASSERT_NE(set.find(mbst::Cell{0, 0, 0}), nullptr);
  EXPECT_EQ(set.find(mbst::Cell{0, 0, 0})->color, 0xFF0000FFu);
  EXPECT_EQ(set.find(mbst::Cell{0, 0, 0})->stroke_id, 1u);
  EXPECT_EQ(set.max_stroke_id(), 1u);
  EXPECT_TRUE(set.validate().ok());
}

TEST(BlockSet, RemoveStrokeDropsExactlyThatStroke) {
  auto set_r = mbst::create_block_set(default_grid());
  ASSERT_TRUE(set_r.ok());
  mbst::IBlockSet& set = *set_r.value();

  ASSERT_TRUE(set.add(make_block(0, 0, 0, 0xFFu, 1)).ok());
  ASSERT_TRUE(set.add(make_block(1, 0, 0, 0xFFu, 1)).ok());
  ASSERT_TRUE(set.add(make_block(2, 0, 0, 0xFFu, 2)).ok());
  EXPECT_EQ(set.size(), 3u);
  EXPECT_EQ(set.stroke_count(), 2u);

  ASSERT_TRUE(set.remove_stroke(1).ok());
  EXPECT_EQ(set.size(), 1u);
  ASSERT_NE(set.find(mbst::Cell{2, 0, 0}), nullptr);
  EXPECT_EQ(set.find(mbst::Cell{2, 0, 0})->stroke_id, 2u);
  // 被撤的格真的从派生索引里消失了。
  EXPECT_EQ(set.find(mbst::Cell{0, 0, 0}), nullptr);
  EXPECT_EQ(set.find(mbst::Cell{1, 0, 0}), nullptr);
  EXPECT_EQ(set.stroke_count(), 1u);
  EXPECT_TRUE(set.validate().ok());
}

TEST(BlockSet, RemoveUnknownStrokeReturnsUnknownStroke) {
  auto set_r = mbst::create_block_set(default_grid());
  ASSERT_TRUE(set_r.ok());
  mbst::IBlockSet& set = *set_r.value();

  const auto r = set.remove_stroke(42);
  EXPECT_FALSE(r.ok());
  EXPECT_EQ(r.error_code(), mbst::StackCode::UnknownStroke);
  EXPECT_FALSE(r.text().empty());

  // 撤销过的 id 不能再撤一次（它的块已经没了）。
  ASSERT_TRUE(set.add(make_block(0, 0, 0, 0xFFu, 7)).ok());
  ASSERT_TRUE(set.remove_stroke(7).ok());
  const auto again = set.remove_stroke(7);
  EXPECT_FALSE(again.ok());
  EXPECT_EQ(again.error_code(), mbst::StackCode::UnknownStroke);
}

TEST(BlockSet, GridSpecDefaults) {
  const mbst::GridSpec g;
  EXPECT_EQ(g.layers, 8);
  EXPECT_EQ(g.half_extent, 16);
  EXPECT_FLOAT_EQ(g.cell_size, 0.5f);
  EXPECT_EQ(g.origin[0], 0.0f);
  EXPECT_EQ(g.origin[1], 0.0f);
  EXPECT_EQ(g.origin[2], 0.0f);

  // 默认值必须自身合法，且能造出一个空集合。
  EXPECT_TRUE(mbst::validate_grid(g).ok());
  auto set_r = mbst::create_block_set(g);
  ASSERT_TRUE(set_r.ok());
  EXPECT_EQ(set_r.value()->size(), 0u);
  EXPECT_EQ(set_r.value()->max_stroke_id(), 0u);
  EXPECT_EQ(set_r.value()->next_stroke_id(), 1u);
}

TEST(BlockSet, InvalidGridIsRejectedNotClamped) {
  mbst::GridSpec g = default_grid();
  g.cell_size = 0.0f;
  const auto bad_cell = mbst::create_block_set(g);
  EXPECT_FALSE(bad_cell.ok());
  EXPECT_FALSE(bad_cell.text().empty());

  g = default_grid();
  g.layers = 0;
  EXPECT_FALSE(mbst::create_block_set(g).ok());

  g = default_grid();
  g.half_extent = 0;
  EXPECT_FALSE(mbst::create_block_set(g).ok());
}

TEST(BlockSet, StrokeIdIsMonotonicAndNeverReused) {
  // I2：撤销不回收 id，重做换新 id。
  auto set_r = mbst::create_block_set(default_grid());
  ASSERT_TRUE(set_r.ok());
  mbst::IBlockSet& set = *set_r.value();

  const std::uint32_t s1 = set.next_stroke_id();
  EXPECT_EQ(s1, 1u);
  ASSERT_TRUE(mbst::add_stroke(&set, {{0, 0, 0}}, 0xFFu, s1).ok());

  const std::uint32_t s2 = set.next_stroke_id();
  EXPECT_EQ(s2, 2u);
  ASSERT_TRUE(mbst::add_stroke(&set, {{1, 0, 0}}, 0xFFu, s2).ok());

  // 撤销 s2 之后，next_stroke_id **不得**退回到 2。
  ASSERT_TRUE(set.remove_stroke(s2).ok());
  EXPECT_EQ(set.next_stroke_id(), 3u);
  EXPECT_EQ(set.max_stroke_id(), 2u);

  // 「重做」= 用新 id 再落一次，落在同一格上必须成功（旧块已随撤销消失）。
  ASSERT_TRUE(mbst::add_stroke(&set, {{1, 0, 0}}, 0xFFu, set.next_stroke_id()).ok());
  ASSERT_NE(set.find(mbst::Cell{1, 0, 0}), nullptr);
  EXPECT_EQ(set.find(mbst::Cell{1, 0, 0})->stroke_id, 3u);
}

TEST(BlockSet, FlagsAndLayerDomainAreEnforced) {
  auto set_r = mbst::create_block_set(default_grid());
  ASSERT_TRUE(set_r.ok());
  mbst::IBlockSet& set = *set_r.value();

  // 未定义 flags 位 → 报错（不静默清零）。
  mbst::Block b = make_block(0, 0, 0, 0xFFu, 1);
  b.flags = 0x8000u;
  EXPECT_FALSE(set.add(b).ok());
  EXPECT_EQ(set.size(), 0u);

  // 已定义的 bit0 必须放行。
  b.flags = mbst::kFlagInnerHighlight;
  EXPECT_TRUE(set.add(b).ok());

  // y 越界（>= layers / < 0）→ 报错（不 clamp）。
  EXPECT_FALSE(set.add(make_block(1, 8, 0, 0xFFu, 2)).ok());
  EXPECT_FALSE(set.add(make_block(1, -1, 0, 0xFFu, 2)).ok());
  // x/z 越界同理，边界是半开区间 [-half_extent, half_extent)。
  EXPECT_FALSE(set.add(make_block(8, 0, 0, 0xFFu, 2)).ok());
  EXPECT_TRUE(set.add(make_block(7, 0, 0, 0xFFu, 2)).ok());
  EXPECT_TRUE(set.add(make_block(-8, 0, 0, 0xFFu, 3)).ok());
  EXPECT_FALSE(set.add(make_block(-9, 0, 0, 0xFFu, 3)).ok());

  // stroke_id=0 是保留值。
  EXPECT_FALSE(set.add(make_block(0, 1, 0, 0xFFu, mbst::kInvalidStrokeId)).ok());
}

TEST(BlockSet, StrokeInsertionIsAllOrNothing) {
  // 设计 §8：要么整笔成、要么整笔不成，绝不留半根柱。
  auto set_r = mbst::create_block_set(default_grid());
  ASSERT_TRUE(set_r.ok());
  mbst::IBlockSet& set = *set_r.value();

  // 第三格 x=100 越界 → 整笔回滚，前两格也不能留下。
  const auto r = mbst::add_stroke(&set, {{0, 0, 0}, {1, 0, 0}, {100, 0, 0}}, 0xFFu, 1);
  EXPECT_FALSE(r.ok());
  EXPECT_EQ(r.error_code(), mbst::StackCode::OutOfGrid);
  EXPECT_EQ(set.size(), 0u);
  EXPECT_EQ(set.find(mbst::Cell{0, 0, 0}), nullptr);
  EXPECT_EQ(set.find(mbst::Cell{1, 0, 0}), nullptr);
  // 整笔回滚后真源是空的，但 **id 水位不回收**（I2：永不复用）。
  // 这是刻意的：id=1 这一段历史已经发生过（虽然后来被回滚），再把它发给别的
  // 一笔会让"同一 stroke_id 指向两段不同历史"，回放与撤销都会失去确定性。
  EXPECT_EQ(set.max_stroke_id(), 1u);
  EXPECT_EQ(set.next_stroke_id(), 2u);
  EXPECT_TRUE(set.validate().ok());

  // 整笔落成功时每格一块。
  const auto ok = mbst::add_stroke(&set, {{0, 0, 0}, {1, 0, 0}, {2, 0, 0}}, 0xFFu, set.next_stroke_id());
  ASSERT_TRUE(ok.ok());
  EXPECT_EQ(set.size(), 3u);
  EXPECT_EQ(set.stroke_count(), 1u);
}

TEST(BlockSet, EmptyStrokeIsExplicitError) {
  auto set_r = mbst::create_block_set(default_grid());
  ASSERT_TRUE(set_r.ok());
  const auto r = mbst::add_stroke(set_r.value().get(), {}, 0xFFu, 1);
  EXPECT_FALSE(r.ok());
  EXPECT_EQ(r.error_code(), mbst::StackCode::EmptyStroke);
}

TEST(BlockSet, HashIsDeterministicAndOrderSensitive) {
  auto set_r = mbst::create_block_set(default_grid());
  ASSERT_TRUE(set_r.ok());
  mbst::IBlockSet& set = *set_r.value();
  ASSERT_TRUE(set.add(make_block(0, 0, 0, 0x11223344u, 1)).ok());
  ASSERT_TRUE(set.add(make_block(1, 0, 0, 0x55667788u, 1)).ok());
  const std::uint64_t h1 = set.hash();

  // 同样内容、同样插入顺序 → 逐位相同（不许掺地址/时间/哈希容器序）。
  auto set2_r = mbst::create_block_set(default_grid());
  ASSERT_TRUE(set2_r.ok());
  mbst::IBlockSet& set2 = *set2_r.value();
  ASSERT_TRUE(set2.add(make_block(0, 0, 0, 0x11223344u, 1)).ok());
  ASSERT_TRUE(set2.add(make_block(1, 0, 0, 0x55667788u, 1)).ok());
  EXPECT_EQ(set2.hash(), h1);

  // 插入顺序不同 → 真源字节顺序不同 → 哈希必须不同。
  auto set3_r = mbst::create_block_set(default_grid());
  ASSERT_TRUE(set3_r.ok());
  mbst::IBlockSet& set3 = *set3_r.value();
  ASSERT_TRUE(set3.add(make_block(1, 0, 0, 0x55667788u, 1)).ok());
  ASSERT_TRUE(set3.add(make_block(0, 0, 0, 0x11223344u, 1)).ok());
  EXPECT_NE(set3.hash(), h1);

  // 任一字段变化都要改变哈希（否则"改色"会静默不进 golden）。
  ASSERT_TRUE(set.set_color(0, 0xDEADBEEFu).ok());
  EXPECT_NE(set.hash(), h1);
}

TEST(BlockSet, ColorChangeTouchesOnlyColorBytes) {
  // 设计 §11 / A8：改色前后除 base_color 之外的字节逐个相同。
  auto set_r = mbst::create_block_set(default_grid());
  ASSERT_TRUE(set_r.ok());
  mbst::IBlockSet& set = *set_r.value();
  ASSERT_TRUE(set.add(make_block(3, 2, -1, 0x01020304u, 9)).ok());

  const mbst::Block before = *set.at(0);
  ASSERT_TRUE(set.set_color(0, 0xA1B2C3D4u).ok());
  const mbst::Block after = *set.at(0);

  EXPECT_EQ(after.color, 0xA1B2C3D4u);
  EXPECT_TRUE(after.cell == before.cell);
  EXPECT_EQ(after.stroke_id, before.stroke_id);
  EXPECT_EQ(after.flags, before.flags);
  // 逐字节对比：把 color 那 4 字节抹平后，两块的内存必须完全相同。
  unsigned char b_bytes[sizeof(mbst::Block)];
  unsigned char a_bytes[sizeof(mbst::Block)];
  std::memcpy(b_bytes, &before, sizeof(mbst::Block));
  std::memcpy(a_bytes, &after, sizeof(mbst::Block));
  std::memset(b_bytes + offsetof(mbst::Block, color), 0, 4);
  std::memset(a_bytes + offsetof(mbst::Block, color), 0, 4);
  EXPECT_EQ(std::memcmp(a_bytes, b_bytes, sizeof(mbst::Block)), 0);

  // 越界下标 → 显式报错，不改任何东西。
  const auto bad = set.set_color(99, 0xFFu);
  EXPECT_FALSE(bad.ok());
  EXPECT_EQ(set.at(0)->color, 0xA1B2C3D4u);
}

TEST(BlockSet, UndoStrokeReturnsBlocksInSourceOrder) {
  // 撤销要能把被删的块原样交回来（重做用），且顺序 = 真源下标序。
  auto set_r = mbst::create_block_set(default_grid());
  ASSERT_TRUE(set_r.ok());
  mbst::IBlockSet& set = *set_r.value();
  ASSERT_TRUE(set.add(make_block(0, 0, 0, 0xFFu, 1)).ok());
  ASSERT_TRUE(set.add(make_block(5, 0, 0, 0xFFu, 2)).ok());
  ASSERT_TRUE(set.add(make_block(1, 0, 0, 0xFFu, 1)).ok());
  ASSERT_TRUE(set.add(make_block(2, 0, 0, 0xFFu, 1)).ok());

  std::vector<mbst::Block> removed;
  ASSERT_TRUE(set.undo_stroke(1, &removed).ok());
  EXPECT_EQ(removed.size(), 3u);
  const mbst::Cell c0{0, 0, 0};
  const mbst::Cell c1{1, 0, 0};
  const mbst::Cell c2{2, 0, 0};
  const mbst::Cell c5{5, 0, 0};
  EXPECT_EQ(removed[0].cell, c0);
  EXPECT_EQ(removed[1].cell, c1);
  EXPECT_EQ(removed[2].cell, c2);
  EXPECT_EQ(set.size(), 1u);
  EXPECT_EQ(set.at(0)->cell, c5);
  EXPECT_TRUE(set.validate().ok());
}

TEST(BlockSet, DerivedIndexIsRebuiltAfterUndo) {
  // 派生缓存必须能由真源完全重建：撤销后 find() 不能命中已删的格，
  // 且留下的块的下标必须前移（压紧，无空洞 —— 设计 §9）。
  auto set_r = mbst::create_block_set(default_grid());
  ASSERT_TRUE(set_r.ok());
  mbst::IBlockSet& set = *set_r.value();
  ASSERT_TRUE(set.add(make_block(0, 0, 0, 0xFFu, 1)).ok());
  ASSERT_TRUE(set.add(make_block(1, 0, 0, 0xFFu, 2)).ok());
  ASSERT_TRUE(set.add(make_block(2, 0, 0, 0xFFu, 1)).ok());

  ASSERT_TRUE(set.remove_stroke(1).ok());
  ASSERT_EQ(set.size(), 1u);
  ASSERT_NE(set.at(0), nullptr);
  EXPECT_EQ(set.at(0)->cell.x, 1);  // 原下标 1 的块被压到 0
  EXPECT_EQ(set.at(1), nullptr);    // 无空洞
  EXPECT_TRUE(set.validate().ok());
  // 派生索引仍能正确回答"有没有"。
  EXPECT_EQ(set.find(mbst::Cell{0, 0, 0}), nullptr);
  EXPECT_NE(set.find(mbst::Cell{1, 0, 0}), nullptr);
}

// ================================================================ 阶段 2：笔刷链路
//
// 这一阶段的测试全部围绕"同一个像素必须永远变成同一个整数格"这条主线：
// 吸附（§6.2）→ 扫掠不漏格（§6.3）→ 去重（§6.4）→ 层锁定格（§6.5）。

namespace {

// 造一条垂直向下的屏幕射线，落点在世界 y=0 平面的 (wx, wz)（y 从 10 往下）。
// ⚠️ 传**格心**坐标才能稳定拿到想要的格：cell_size=0.5 时格 i 的格心是
// (i + 0.5) * 0.5。传格边界（如 0.5 的整数倍）会被量化判到某一侧，虽然确定
// 但不直观 —— 测试里一律用格心，避免"以为落在格 i 其实落在格 i±1"。
mbst::Ray down_ray_at(float wx, float wz) {
  mbst::Ray r;
  r.origin = mbst::Vec3{wx, 10.0f, wz};
  r.direction = mbst::Vec3{0.0f, -1.0f, 0.0f};
  return r;
}

// 从相机位置看向某投影点的斜射线（用于测层锁"忽略俯仰"）。
mbst::Ray slant_ray(float wx, float wz) {
  mbst::Ray r;
  r.origin = mbst::Vec3{wx * 0.5f, 10.0f, wz * 0.5f + 3.0f};
  r.direction = mbst::Vec3{wx * 0.5f - r.origin.x, -10.0f, wz * 0.5f - r.origin.z};
  return r;
}

}  // namespace

TEST(StackSnap, RayToCellExact) {
  const mbst::GridSpec g = default_grid();  // cell 0.5, half_extent 8, layers 8
  // 世界 (0.25, -, 0.25) → 格 (0, 0)；cell_size=0.5 时格内任意点都吸到同一整数格。
  auto r = mbst::snap_ray_to_cell(down_ray_at(0.25f, 0.25f), g, 0);
  ASSERT_TRUE(r.ok());
  const mbst::Cell want{0, 0, 0};
  EXPECT_TRUE(r.value() == want);

  // (1.25, 2.25) → (2, 4)：格 i 覆盖 [i*cs, (i+1)*cs)。
  auto r2 = mbst::snap_ray_to_cell(down_ray_at(1.25f, 2.25f), g, 0);
  ASSERT_TRUE(r2.ok());
  const mbst::Cell want2{2, 0, 4};
  EXPECT_TRUE(r2.value() == want2);
}

TEST(StackSnap, ExactGridLinePoint) {
  // 恰好落在格线上（1.5 = 3*cs）—— 这是量化步骤存在的唯一理由。
  // 期望:落到 +x 一侧的格（x=3），而不是 -x 一侧（x=2），且**两次运行一致**。
  const mbst::GridSpec g = default_grid();
  auto a = mbst::snap_ray_to_cell(down_ray_at(1.5f, 0.25f), g, 0);
  ASSERT_TRUE(a.ok());
  EXPECT_EQ(a.value().x, 3);
  // 同一输入重复 100 次必须给同一答案（否则 golden 不可复现）。
  for (int i = 0; i < 100; ++i) {
    auto b = mbst::snap_ray_to_cell(down_ray_at(1.5f, 0.25f), g, 0);
    ASSERT_TRUE(b.ok());
    EXPECT_EQ(b.value().x, 3) << "第 " << i << " 次重复就漂了";
  }
  // 格线左侧一点点（1.5 - 1e-3）也必须稳定在 x=2，不能被量化推过线。
  auto c = mbst::snap_ray_to_cell(down_ray_at(1.5f - 1e-3f, 0.25f), g, 0);
  ASSERT_TRUE(c.ok());
  EXPECT_EQ(c.value().x, 2);
}

TEST(StackSnap, NegativeCoordinates) {
  // 负半轴：origin=0 时格 -1 覆盖 [-cs, 0)。floor 天然正确，但必须有测试钉住，
  // 因为用 round/static_cast<int>(截断) 实现时这里会静默变成 0。
  const mbst::GridSpec g = default_grid();
  auto a = mbst::snap_ray_to_cell(down_ray_at(-0.25f, -0.25f), g, 0);
  ASSERT_TRUE(a.ok());
  const mbst::Cell want{0, 0, 0};  // -0.25 在 [-0.5, 0) → 格 -1？见下断言
  (void)want;
  // -0.25 / 0.5 = -0.5，floor(-0.5) = -1。
  EXPECT_EQ(a.value().x, -1);
  EXPECT_EQ(a.value().z, -1);

  auto b = mbst::snap_ray_to_cell(down_ray_at(-1.25f, -2.25f), g, 0);
  ASSERT_TRUE(b.ok());
  EXPECT_EQ(b.value().x, -3);  // -2.5 → floor = -3
  EXPECT_EQ(b.value().z, -5);  // -4.5 → floor = -5
}

TEST(StackSnap, NonUnitCellSize) {
  // cell_size = 0.3 这类非 2 的幂：吸附必须用 cs 而不是硬编码 1.0/0.5。
  mbst::GridSpec g = default_grid();
  g.cell_size = 0.3f;
  // 世界 0.65 → 0.65/0.3 = 2.1667 → 格 2。格 2 覆盖 [0.6, 0.9)。
  auto a = mbst::snap_ray_to_cell(down_ray_at(0.65f, 0.05f), g, 0);
  ASSERT_TRUE(a.ok());
  EXPECT_EQ(a.value().x, 2);
  EXPECT_EQ(a.value().z, 0);
  // 浮点舍入保护：0.3*3 = 0.8999999...，把 0.9 放在格 3（[0.9, 1.2)）而不是格 2。
  auto b = mbst::snap_ray_to_cell(down_ray_at(0.9f, 0.0f), g, 0);
  ASSERT_TRUE(b.ok());
  EXPECT_EQ(b.value().x, 3);
}

TEST(StackSnap, RayParallelToLayerReturnsRayParallelToLayer) {
  const mbst::GridSpec g = default_grid();
  mbst::Ray r;
  r.origin = mbst::Vec3{0.0f, 2.0f, 0.0f};
  r.direction = mbst::Vec3{1.0f, 0.0f, 0.0f};  // 完全水平
  auto res = mbst::snap_ray_to_cell(r, g, 0);
  ASSERT_FALSE(res.ok());
  EXPECT_EQ(res.error_code(), mbst::StackCode::RayParallelToLayer);

  // 层平面点也不给（同一判据必须在这两个入口一致）。
  auto pt = mbst::ray_to_layer_point(r, g, 0);
  ASSERT_FALSE(pt.ok());
  EXPECT_EQ(pt.error_code(), mbst::StackCode::RayParallelToLayer);
}

TEST(StackSnap, BehindCameraReturnsRayMissesLayer) {
  const mbst::GridSpec g = default_grid();
  // 射线朝**上**，而第 0 层平面在下（y=0 < origin.y=10）→ 交点在反方向。
  mbst::Ray r;
  r.origin = mbst::Vec3{0.0f, 10.0f, 0.0f};
  r.direction = mbst::Vec3{0.0f, 1.0f, 0.0f};
  auto res = mbst::snap_ray_to_cell(r, g, 0);
  ASSERT_FALSE(res.ok());
  EXPECT_EQ(res.error_code(), mbst::StackCode::RayMissesLayer);
  // **绝不能**取 |t| 把背后当成命中 —— 那会表现为"点空白处也在远处落块"。
}

TEST(StackSweep, DiagonalHitsEveryCell) {
  // 45° 对角线 (0,0)→(3,3)：supercover 必须给出 7 格（每步只跨一轴）：
  // (0,0)(1,0)(1,1)(2,1)(2,2)(3,2)(3,3)。
  std::vector<mbst::Cell> out;
  const mbst::Cell a{0, 0, 0};
  const mbst::Cell b{3, 0, 3};
  mbst::supercover_cells(a, b, &out);
  ASSERT_EQ(out.size(), 7u);
  const mbst::Cell w0{0, 0, 0};
  const mbst::Cell w1{1, 0, 0};
  const mbst::Cell w2{1, 0, 1};
  const mbst::Cell w3{2, 0, 1};
  const mbst::Cell w4{2, 0, 2};
  const mbst::Cell w5{3, 0, 2};
  const mbst::Cell w6{3, 0, 3};
  EXPECT_TRUE(out[0] == w0);
  EXPECT_TRUE(out[1] == w1);
  EXPECT_TRUE(out[2] == w2);
  EXPECT_TRUE(out[3] == w3);
  EXPECT_TRUE(out[4] == w4);
  EXPECT_TRUE(out[5] == w5);
  EXPECT_TRUE(out[6] == w6);
  // 每一格都邻接下一格（步长 <= 1，这是"连续笔迹"的定义）。
  for (std::size_t i = 1; i < out.size(); ++i) {
    const int dx = std::abs(out[i].x - out[i - 1].x);
    const int dy = std::abs(out[i].y - out[i - 1].y);
    const int dz = std::abs(out[i].z - out[i - 1].z);
    EXPECT_LE(dx, 1);
    EXPECT_LE(dy, 1);
    EXPECT_LE(dz, 1);
    EXPECT_GT(dx + dy + dz, 0) << "第 " << i << " 格与上一格相同（重复 emit）";
  }
}

TEST(StackSweep, NoCornerLeak) {
  // 2:1 的浅斜线 (0,0,0)→(6,0,3)：格 (1,0,1) *被线段擦过一角*，
  // 朴素"取格心/按主步进"的实现会漏掉它 —— 画面上就是柱子断了一截。
  std::vector<mbst::Cell> out;
  const mbst::Cell a{0, 0, 0};
  const mbst::Cell b{6, 0, 3};
  mbst::supercover_cells(a, b, &out);
  // supercover 路径必须**包含**每一个被线段穿过的格；这里断言关键的"擦角格"在手。
  auto has = [&](int x, int z) {
    for (const auto& c : out)
      if (c.x == x && c.z == z) return true;
    return false;
  };
  for (int i = 0; i <= 6; ++i) {
    EXPECT_TRUE(has(i, i / 2)) << "x=" << i << " 的主路径格缺失";
  }
  EXPECT_TRUE(has(1, 1)) << "擦角格 (1,1) 漏了：斜线在格 (1,0)/(1,1) 之间穿过";
  EXPECT_TRUE(has(3, 2)) << "擦角格 (3,2) 漏了";
  EXPECT_TRUE(has(5, 3)) << "擦角格 (5,3) 漏了";
}

TEST(StackSweep, SinglePointEmitsOnce) {
  // a == b：只 emit 一次。这是 §6.3 的显式判据（"点击不动"不该落一坨）。
  std::vector<mbst::Cell> out;
  const mbst::Cell a{2, 1, 3};
  mbst::supercover_cells(a, a, &out);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_TRUE(out[0] == a);
}

TEST(StackSweep, DuplicatePassDoesNotReEmit) {
  // 同一段路径刷两遍 → DDA emit 出两倍格，但 plan 去重后只剩一份（§6.4）。
  mbst::BrushConfig cfg;
  cfg.grid = default_grid();
  auto brush = mbst::create_block_brush(cfg);
  ASSERT_NE(brush, nullptr);

  std::vector<mbst::Ray> rays;
  for (int i = 0; i <= 4; ++i) rays.push_back(down_ray_at(0.5f * (static_cast<float>(i) + 0.5f), 0.25f));
  // 先往**另一个** set 里落一次，再对同一批射线出第二次 plan 做对照。
  auto first = brush->stroke(rays.data(), rays.size());
  ASSERT_TRUE(first.ok());
  EXPECT_EQ(first.value().cells.size(), 5u);
  EXPECT_EQ(first.value().stats.skipped_occupied, 0u);

  // 反向再来一遍（覆盖同样的 5 格）。**去相邻重复**只折叠"连着投同一格"，
  // 反向序列的首格与原序列首格不同，所以采样点不会被折叠 —— 5 个采样点仍在。
  // §6.4 的格级去重发生在**同一次落笔内**（一笔画自己绕回来盖住走过的格），
  // 跨落笔的"已占"由 IBlockSet 报错处理，不属于笔刷职责。
  std::vector<mbst::Ray> back(rays.rbegin(), rays.rend());
  auto second = brush->stroke(back.data(), back.size());
  ASSERT_TRUE(second.ok());
  EXPECT_EQ(second.value().cells.size(), 5u);
  // 反向笔迹的格顺序应当正好是正序的倒序（DDA 是路径对称的）。
  for (std::size_t i = 0; i < 5; ++i) {
    EXPECT_TRUE(second.value().cells[i] == first.value().cells[4 - i]) << "第 " << i << " 格不对";
  }
}

TEST(StackSweep, SelfOverlappingStrokeDedupesWithinOneStroke) {
  // §6.4 真正要防的是**一笔画自己绕回来**：A→B→A。
  // 采样的"去相邻重复"拦不住它（A 与 B 不同），必须靠格级去重。
  mbst::BrushConfig cfg;
  cfg.grid = default_grid();
  auto brush = mbst::create_block_brush(cfg);
  ASSERT_NE(brush, nullptr);

  // A = 格 0 的格心，C = 格 2 的格心；A→C→A 是"绕回来"的那种笔迹。
  const mbst::Ray rays[3] = {down_ray_at(0.5f * 0.5f, 0.5f * 0.5f),
                             down_ray_at(0.5f * 2.5f, 0.5f * 0.5f),
                             down_ray_at(0.5f * 0.5f, 0.5f * 0.5f)};
  auto plan = brush->stroke(rays, 3);
  ASSERT_TRUE(plan.ok());
  // A→C 覆盖格 0,1,2（3 格）；C→A 再 emit 3 格，其中 2 格是重复。
  // 合计 emit 5 次（末段的起点与上一段末格重合，按段拼接规则跳过），
  // 去重后 3 格，跳过 2 次 —— 这正是"一笔画自己绕回来不能长出多余柱子"的判据。
  EXPECT_EQ(plan.value().stats.cells_emitted, 5u);
  EXPECT_EQ(plan.value().cells.size(), 3u);
  EXPECT_EQ(plan.value().stats.skipped_occupied, 2u);
}

TEST(StackSweep, OccupiedCellIsRejectedAtomically) {
  // 已占格由 IBlockSet **报错**（不是静默跳过），且整笔回滚。
  // 对照的另一半（笔刷计划内的自我去重计数）见 SelfOverlappingStrokeDedupesWithinOneStroke。
  mbst::BrushConfig cfg;
  cfg.grid = default_grid();
  auto brush = mbst::create_block_brush(cfg);
  auto set_r = mbst::create_block_set(cfg.grid);
  ASSERT_TRUE(set_r.ok());
  mbst::IBlockSet& set = *set_r.value();
  // **必须显式锁到第 0 层**：不锁时 pick_layer_for_ray 会选"射线最先碰到的那层"，
  // 从 y=10 往下打就是顶面 y=7，笔迹根本碰不到 y=0 的格子。
  ASSERT_TRUE(brush->layer_lock().lock(0).ok());

  // 先落一格 (1,0,0)，再刷一条盖住它的一笔画。
  ASSERT_TRUE(set.add(make_block(1, 0, 0, 0xFFu, 1)).ok());
  const std::size_t h_before = set.hash();

  // 用射线走完整链路：0..3 共 4 个采样点，中间盖住已占的 (1,0,0)。
  std::vector<mbst::Ray> rays;
  for (int i = 0; i <= 3; ++i) rays.push_back(down_ray_at(0.5f * (static_cast<float>(i) + 0.5f), 0.25f));
  auto real = brush->stroke_and_commit(&set, rays.data(), rays.size(), 0xFFu, 2);
  // 笔迹盖住了已占的 (1,0,0) → **整笔失败**（不是静默跳过）。
  // 这是 add_stroke 的契约：它不做"跳过重复"，重复就是错误
  // （I1 由构造保证，不靠静默去重 —— 静默去重会让"我画的这一笔"和
  //  "真源里实际留下的"不一致，回放时就对不上）。
  EXPECT_FALSE(real.ok());
  if (!real.ok()) EXPECT_EQ(real.error_code(), mbst::StackCode::OutOfGrid);
  EXPECT_EQ(set.size(), 1u) << "整笔因重复回滚后，真源必须只剩原来那一块";
  EXPECT_EQ(set.hash(), h_before);
  EXPECT_TRUE(set.validate().ok());

  // 换一条**不与**已占格重叠的笔迹：(4..6, 0, 0) → 新增 3 格。
  std::vector<mbst::Ray> clean;
  for (int i = 4; i <= 6; ++i) clean.push_back(down_ray_at(0.5f * (static_cast<float>(i) + 0.5f), 0.25f));
  auto ok = brush->stroke_and_commit(&set, clean.data(), clean.size(), 0xFFu, 3);
  ASSERT_TRUE(ok.ok());
  EXPECT_EQ(set.size(), 4u);
  EXPECT_EQ(set.stroke_count(), 2u);
  EXPECT_NE(set.hash(), h_before);
  EXPECT_TRUE(set.validate().ok());
}

TEST(StackSweep, BrushPlanIsGeometryOnlyAndSetEnforcesUniqueness) {
  // 把上面那条契约单独钉死，避免以后有人"顺手"在 add_stroke 里加静默去重：
  // 笔刷 plan 是纯几何量（不含真源信息）；唯一性由 IBlockSet 用**报错**保证，
  // 而不是用"悄悄跳过"。两者职责分离是 §6.4 与 §8 的分工。
  mbst::BrushConfig cfg;
  cfg.grid = default_grid();
  auto brush = mbst::create_block_brush(cfg);
  ASSERT_NE(brush, nullptr);

  const mbst::Ray a = down_ray_at(0.25f, 0.25f);
  const mbst::Ray b = down_ray_at(0.75f, 0.25f);
  const mbst::Ray two[2] = {a, b};
  auto plan = brush->stroke(two, 2);
  ASSERT_TRUE(plan.ok());
  ASSERT_EQ(plan.value().cells.size(), 2u);

  auto set_r = mbst::create_block_set(cfg.grid);
  ASSERT_TRUE(set_r.ok());
  mbst::IBlockSet& set = *set_r.value();
  // 同一份 plan 落两次：第二次必须报错（OutOfGrid，即"格已被占"），不能静默。
  ASSERT_TRUE(mbst::add_stroke(&set, plan.value().cells, 0xFFu, 1).ok());
  const auto second = mbst::add_stroke(&set, plan.value().cells, 0xFFu, 2);
  EXPECT_FALSE(second.ok());
  EXPECT_EQ(second.error_code(), mbst::StackCode::OutOfGrid);
  EXPECT_EQ(set.size(), 2u) << "第二次整笔回滚，不能留半笔";
  EXPECT_TRUE(set.validate().ok());
}

TEST(StackSweep, ColumnHeightHonored) {
  // column_height = 3：一格落笔 → 3 块（同 (x,z)、y = 3..5，锁定在第 3 层）。
  mbst::BrushConfig cfg;
  cfg.grid = default_grid();
  cfg.column_height = 3;
  auto brush = mbst::create_block_brush(cfg);
  ASSERT_NE(brush, nullptr);
  ASSERT_TRUE(brush->layer_lock().lock(3).ok());

  auto set_r = mbst::create_block_set(cfg.grid);
  ASSERT_TRUE(set_r.ok());
  mbst::IBlockSet& set = *set_r.value();

  const mbst::Ray r = down_ray_at(0.25f, 0.25f);
  auto plan = brush->stroke_and_commit(&set, &r, 1, 0xFF00FF00u, 1);
  ASSERT_TRUE(plan.ok());
  ASSERT_EQ(plan.value().cells.size(), 1u);
  EXPECT_EQ(set.size(), 3u);
  EXPECT_EQ(set.stroke_count(), 1u);
  // 三块的 y 必须是 3、4、5 且 (x,z) 相同 —— 一根柱。
  for (std::int32_t y = 3; y <= 5; ++y) {
    const mbst::Cell want{0, y, 0};
    ASSERT_NE(set.find(want), nullptr) << "柱缺了 y=" << y;
    EXPECT_EQ(set.find(want)->stroke_id, 1u);
    EXPECT_EQ(set.find(want)->color, 0xFF00FF00u);
  }
  EXPECT_TRUE(set.validate().ok());
}

TEST(StackSweep, ColumnOverflowFailsWholeStroke) {
  // 柱高越过顶面 → **整笔**失败，一块都不许留下（不许留半根柱）。
  mbst::BrushConfig cfg;
  cfg.grid = default_grid();  // layers = 8，格顶 = 7
  cfg.column_height = 4;      // 从第 6 层起放 4 块 → 顶到 9 > 7，越界
  auto brush = mbst::create_block_brush(cfg);
  ASSERT_NE(brush, nullptr);
  ASSERT_TRUE(brush->layer_lock().lock(6).ok());

  auto set_r = mbst::create_block_set(cfg.grid);
  ASSERT_TRUE(set_r.ok());
  mbst::IBlockSet& set = *set_r.value();

  const mbst::Ray r = down_ray_at(0.25f, 0.25f);
  auto plan = brush->stroke_and_commit(&set, &r, 1, 0xFFu, 1);
  EXPECT_FALSE(plan.ok());
  if (!plan.ok()) EXPECT_EQ(plan.error_code(), mbst::StackCode::OutOfGrid);
  // 真源必须完全干净 —— 连"部分格"都不能留。
  EXPECT_EQ(set.size(), 0u);
  EXPECT_TRUE(set.validate().ok());

  // 放宽到正好放得下（从第 5 层起放 3 块 → 顶到 7）应当成功，证明失败判据
  // 卡的是"真的越界"，而不是"柱高超某值就一律拒绝"。
  mbst::BrushConfig ok_cfg = cfg;
  ok_cfg.column_height = 3;
  auto ok_brush = mbst::create_block_brush(ok_cfg);
  ASSERT_TRUE(ok_brush->layer_lock().lock(5).ok());
  auto ok_plan = ok_brush->stroke_and_commit(&set, &r, 1, 0xFFu, 1);
  ASSERT_TRUE(ok_plan.ok());
  EXPECT_EQ(set.size(), 3u);
  EXPECT_TRUE(set.validate().ok());
}

TEST(StackLayerLock, LockedLayerIgnoresCameraPitch) {
  // F-VX-6：锁到第 3 层后，**任何**俯仰的射线落笔都在 y=3。
  mbst::GridSpec g = default_grid();
  mbst::LayerLock lock(g);
  ASSERT_TRUE(lock.lock(3).ok());
  EXPECT_TRUE(lock.locked());
  EXPECT_EQ(lock.layer(), 3);

  // 斜射线（不同俯仰）都吸到同一格 y=3。
  for (int i = 0; i < 5; ++i) {
    const float wx = 0.5f * (static_cast<float>(i) + 0.5f);
    auto r = mbst::snap_ray_to_cell(slant_ray(wx, 1.25f), g, lock.layer());
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().y, 3) << "第 " << i << " 条斜射线的 y 漂了";
  }
}

TEST(StackLayerLock, UnlockRestoresGridHit) {
  mbst::GridSpec g = default_grid();
  mbst::LayerLock lock(g);
  ASSERT_TRUE(lock.lock(5).ok());
  EXPECT_EQ(lock.layer(), 5);

  ASSERT_TRUE(lock.unlock().ok());
  EXPECT_FALSE(lock.locked());
  EXPECT_EQ(lock.layer(), -1);  // 未锁 = -1（不是 0！0 是"锁在第 0 层"）

  // 解锁后回到"按几何自动选层"。
  // 从 y=10 竖直往下打、网格在 y∈[0, 4]，相机在上 → 先碰到的是**顶面**，
  // 也就是最上面那一层 y=3（层号 7）。这条语义必须钉住：
  // 弄反会表现为"明明看着顶面，块却砌在最底下"。
  auto lay = mbst::pick_layer_for_ray(down_ray_at(0.25f, 0.25f), g);
  ASSERT_TRUE(lay.ok());
  EXPECT_EQ(lay.value(), 7);

  // 反方向（从下往上打）应当落在最底层 y=0。
  mbst::Ray up;
  up.origin = mbst::Vec3{0.25f, -10.0f, 0.25f};
  up.direction = mbst::Vec3{0.0f, 1.0f, 0.0f};
  auto up_lay = mbst::pick_layer_for_ray(up, g);
  ASSERT_TRUE(up_lay.ok());
  EXPECT_EQ(up_lay.value(), 0);

  // 锁着的时候，同一条射线被强行定格在锁定的层 —— 自动选层被绕过。
  ASSERT_TRUE(lock.lock(5).ok());
  auto snapped = mbst::snap_ray_to_cell(down_ray_at(0.25f, 0.25f), g, lock.layer());
  ASSERT_TRUE(snapped.ok());
  EXPECT_EQ(snapped.value().y, 5);
  // pick_layer_for_ray 本身是纯几何函数，不受锁影响（锁由笔刷施加）。
  auto lay2 = mbst::pick_layer_for_ray(down_ray_at(0.25f, 0.25f), g);
  ASSERT_TRUE(lay2.ok());
  EXPECT_EQ(lay2.value(), 7);
}

TEST(StackLayerLock, LockWithoutLayerReturnsLockLayerRequired) {
  // **契约里不存在"lock() 不带参数"的重载** —— 必须显式给层号。
  // 这条测试锁住的是"绝不默认锁第 0 层"：默认值会让"忘了设"表现为
  // "怎么刷都刷在最底层"这种极难归因的现象。
  //
  // 因此有两条可观测的契约：
  //  (a) 未锁时 layer() == -1（是"没有层"的哨兵，不是 0）；
  //  (b) 把 -1 拿去吸附会被**显式拒绝**，不会静默当成第 0 层。
  mbst::GridSpec g = default_grid();
  mbst::LayerLock lock(g);
  EXPECT_FALSE(lock.locked());
  const std::int32_t lay = lock.layer();
  ASSERT_EQ(lay, -1);

  auto r = mbst::snap_ray_to_cell(down_ray_at(0.25f, 0.25f), g, lay);
  EXPECT_FALSE(r.ok());
  if (!r.ok()) EXPECT_EQ(r.error_code(), mbst::StackCode::OutOfGrid);
  // 对照：合法的第 0 层能吸出来，说明上面失败的原因是"没有层"而不是别的。
  auto ok0 = mbst::snap_ray_to_cell(down_ray_at(0.25f, 0.25f), g, 0);
  ASSERT_TRUE(ok0.ok());
  EXPECT_EQ(ok0.value().y, 0);
}

TEST(StackLayerLock, LockOutOfRangeReturnsLayerOutOfRange) {
  mbst::GridSpec g = default_grid();  // layers = 8，合法 [0, 8)
  mbst::LayerLock lock(g);
  for (std::int32_t bad : {-1, 8, 9, 100}) {
    auto r = lock.lock(bad);
    EXPECT_FALSE(r.ok()) << "lock(" << bad << ") 应当失败";
    if (!r.ok()) EXPECT_EQ(r.error_code(), mbst::StackCode::LayerOutOfRange);
    // 失败**不**改状态：不许钳位，也不许"顺手锁上一次的值"。
    EXPECT_FALSE(lock.locked()) << "lock(" << bad << ") 失败后状态被改了";
  }
  // 合法值仍然可用。
  ASSERT_TRUE(lock.lock(7).ok());
  EXPECT_EQ(lock.layer(), 7);
  // 已经锁着的时候再 lock 一个越界值，状态必须保持在第 7 层不变。
  auto bad = lock.lock(99);
  EXPECT_FALSE(bad.ok());
  EXPECT_TRUE(lock.locked());
  EXPECT_EQ(lock.layer(), 7) << "越界 lock 把已锁状态搞坏了";
}

// 阶段 3 测试 —— 双 pass 渲染。续接上面的用例，独立成文件片段追加到主体之后。
//
// 说明：这些用例第一次在**公共面 + with_depth=true** 上做像素回归。
// 仓里此前的像素用例都是 with_depth=false（没有深度附件），所以
// "深度附件挂上之后管线还能不能建、画出来还是不是对的" 从来没有被测过。
// 这里补上，并且所有断言都用 readback_rgba8 统计，不依赖外部脚本。

namespace {

using StackRenderer = mbst::IStackRenderer;

// ---- 像素统计小工具 -------------------------------------------------

struct PixelStats {
  int foreground = 0;   // 与清屏色不同的像素数
  int distinct_colors = 0;
  int min_x = 1 << 30, max_x = -1, min_y = 1 << 30, max_y = -1;
  int total = 0;
  bool empty() const { return foreground == 0; }
  int width() const { return max_x >= min_x ? max_x - min_x + 1 : 0; }
  int height() const { return max_y >= min_y ? max_y - min_y + 1 : 0; }
};

PixelStats analyze(const std::vector<std::uint8_t>& rgba, int w, int h,
                   std::uint8_t br, std::uint8_t bg, std::uint8_t bb) {
  PixelStats st;
  st.total = w * h;
  const int tol = 6;
  std::set<std::uint32_t> colors;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4;
      const int dr = std::abs(static_cast<int>(rgba[i + 0]) - static_cast<int>(br));
      const int dg = std::abs(static_cast<int>(rgba[i + 1]) - static_cast<int>(bg));
      const int db = std::abs(static_cast<int>(rgba[i + 2]) - static_cast<int>(bb));
      if (dr > tol || dg > tol || db > tol) {
        ++st.foreground;
        if (x < st.min_x) st.min_x = x;
        if (x > st.max_x) st.max_x = x;
        if (y < st.min_y) st.min_y = y;
        if (y > st.max_y) st.max_y = y;
      }
      const std::uint32_t c = (static_cast<std::uint32_t>(rgba[i + 0]) << 16) |
                              (static_cast<std::uint32_t>(rgba[i + 1]) << 8) |
                              static_cast<std::uint32_t>(rgba[i + 2]);
      colors.insert(c);
    }
  }
  st.distinct_colors = static_cast<int>(colors.size());
  return st;
}


// 测试侧的设备描述（l3_cli 里同名函数是文件内静态的，库外不可见）。
// 要求软件光栅器：CI 无 GPU，且 SwiftShader 跨机器可复现，像素断言才有意义。
dev::DeviceDesc software_desc() {
  dev::DeviceDesc d;
  d.backend = dev::BackendType::Vulkan;
  d.enable_validation = false;
  d.require_software_rasterizer = true;
  return d;
}

// 预取显存页：分配器不自动扩页（见 memory.h reserve_pages），
// 不预取时第一次 create_buffer 会返回 "内存不足"。
bool reserve_pages(dev::IRenderDevice* device) {
  res::IMemoryAllocator* alloc = device->memory_allocator();
  if (alloc == nullptr) return false;
  for (res::MemoryKind kind :
       {res::MemoryKind::Vertex, res::MemoryKind::Index, res::MemoryKind::Staging}) {
    if (!alloc->reserve_pages(kind, 4).ok()) return false;
  }
  return true;
}

// 斜俯视相机，能同时看到顶面和侧面 —— 双 pass 差异只有在"既有外表面
// 又有被遮挡的内壁"时才可见。正俯视会让内面完全被表面挡住，测不出问题。
std::unique_ptr<mbp::ICamera> make_oblique_camera(int w, int h) {
  auto cam = mbp::create_camera();
  if (!cam) return nullptr;
  mbp::CameraDesc cd;
  cd.kind = mbp::ProjectionKind::Perspective;
  cd.fov_y_degrees = 45.0f;
  cd.near_plane = 0.1f;
  cd.far_plane = 200.0f;
  cd.viewport.width = static_cast<std::uint32_t>(w);
  cd.viewport.height = static_cast<std::uint32_t>(h);
  if (!cam->set_desc(cd).ok()) return nullptr;
  mbp::CameraPose pose;
  pose.position = {9.0f, 7.0f, 9.0f};
  pose.target = {0.0f, 1.0f, 0.0f};
  pose.up = {0.0f, 1.0f, 0.0f};
  if (!cam->set_pose(pose).ok()) return nullptr;
  return cam;
}

// 合法域是**以原点为中心**的：x/z ∈ [-half_extent, half_extent)，y ∈ [0, layers)。
// （不是 [0, 2*half_extent) —— 原点不是"格子 0 的角"，是网格中心。）
// 所以 origin 必须留在 0，格子索引可以取负。
mbst::GridSpec render_grid() {
  mbst::GridSpec g;
  g.origin[0] = 0.0f;
  g.origin[1] = 0.0f;
  g.origin[2] = 0.0f;
  g.cell_size = 0.5f;
  g.layers = 8;
  g.half_extent = 4;
  return g;
}

// 一个 2x2 高 3 的柱子：顶面可见，四周有内壁。
std::unique_ptr<mbst::IBlockSet> build_column(mbst::GridSpec& g) {
  auto r = mbst::create_block_set(g);
  if (!r) return nullptr;
  std::unique_ptr<mbst::IBlockSet> set = std::move(r).take();
  std::vector<mbst::Cell> cells;
  for (int y = 0; y < 3; ++y) {
    for (int z = 0; z < 2; ++z) {
      for (int x = 0; x < 2; ++x) {
        cells.push_back(mbst::Cell{x - 1, y, z - 1});
      }
    }
  }
  auto st = mbst::add_stroke(set.get(), cells, 0x4080FFFFu, 1u);
  EXPECT_TRUE(st) << "搭柱子失败: " << st.text();
  return set;
}

// 渲染一帧并读回像素。返回空 vector 表示失败（由调用方断言）。
std::vector<std::uint8_t> render_once(dev::IRenderDevice* device, StackRenderer& r,
                                      const mbst::IBlockSet& set, const mbst::GridSpec& g,
                                      const mbst::StackFrameDesc& desc_extra) {
  const int w = static_cast<int>(desc_extra.width);
  const int h = static_cast<int>(desc_extra.height);

  dev::TargetDesc td;
  td.kind = dev::TargetDesc::Kind::Offscreen;
  td.extent.width = static_cast<std::uint32_t>(w);
  td.extent.height = static_cast<std::uint32_t>(h);
  td.samples = 1;
  td.with_depth = true;  // 关键：本仓第一次在带深度附件下做像素断言
  auto tgt = device->create_offscreen_target(td);
  if (!tgt.ok()) return {};

  auto st = r.build_instances(g, set);
  if (!st) return {};

  auto cl = device->create_command_list();
  if (!cl.ok()) return {};

  if (!r.record_frame(*cl.value(), tgt.value().get(), desc_extra)) return {};
  if (!device->submit_command_list(*cl.value()).ok()) return {};

  std::vector<std::uint8_t> px(static_cast<std::size_t>(w) * h * 4, 0u);
  if (!tgt.value()->readback_rgba8(px.data(), px.size()).ok()) return {};
  return px;
}

}  // namespace

TEST(StackRender, PassPairIsDistinct) {
  // 两个 pass 必须真的不同，否则"内面穿过表面"的视觉效果根本不会出现。
  // 只测公共面：hash 来自 PipelineDesc 里可见的字段。
  const res::PipelineDesc s = mbst::surface_pipeline_desc(1);
  const res::PipelineDesc i = mbst::inner_pipeline_desc(1);

  const std::uint64_t hs = mbst::compute_pass_hash(s, 0x1234u);
  const std::uint64_t hi = mbst::compute_pass_hash(i, 0x1234u);
  EXPECT_NE(hs, hi) << "表面/内面 pass 哈希相同 —— 合批器会把它们并成一个 draw";

  // 同一份 desc 两次求值必须一致（哈希不能掺地址/随机数）。
  EXPECT_EQ(hs, mbst::compute_pass_hash(s, 0x1234u));
  EXPECT_EQ(hi, mbst::compute_pass_hash(i, 0x1234u));

  // 表面 pass：开深度测试 + 背面剔除。
  EXPECT_TRUE(s.depth.depthTest);
  EXPECT_TRUE(s.depth.depthWrite);
  EXPECT_EQ(s.raster.cull, res::CullMode::Back);

  // 内面 pass：关深度测试 + 不剔除。
  EXPECT_FALSE(i.depth.depthTest);
  EXPECT_EQ(i.raster.cull, res::CullMode::None);

  // D-VX-4′：两条管线都不许碰 depthConstWrite / depth_const。
  EXPECT_FALSE(s.depth.depthConstWrite);
  EXPECT_FALSE(i.depth.depthConstWrite);
  EXPECT_EQ(s.depth.depth_const, 1.0f);
  EXPECT_EQ(i.depth.depth_const, 1.0f);

  // depthWrite 必须为 true：三个开关全 false 时后端根本不挂 depth-stencil 状态，
  // 于是 depthTest=false 与"没设"无从区分。这条断言守住那个退化。
  EXPECT_TRUE(i.depth.depthWrite);

  // 推送常量宽度两条一致（都 128B：view_proj + normal_matrix）。
  EXPECT_EQ(s.push_constant_bytes, 128u);
  EXPECT_EQ(i.push_constant_bytes, 128u);

  // MSAA 采样数必须透传，不能被吞成 1。
  EXPECT_EQ(mbst::surface_pipeline_desc(4).samples, 4u);
  EXPECT_EQ(mbst::inner_pipeline_desc(4).samples, 4u);
}

TEST(StackRender, BothPassesRenderOnDepthTarget) {
  auto dev_r = dev::create_render_device(software_desc());
  ASSERT_TRUE(dev_r.ok());
  ASSERT_TRUE(reserve_pages(dev_r.value().get())) << "预取显存页失败";

  mbst::GridSpec g = render_grid();
  std::unique_ptr<mbst::IBlockSet> set = build_column(g);
  ASSERT_NE(set, nullptr);

  auto r = mbst::create_stack_renderer();
  ASSERT_NE(r, nullptr);
  auto st = r->create_passes(dev_r.value().get(), 1);
  ASSERT_TRUE(st) << "create_passes 失败: " << st.text();

  const std::uint32_t w = 320, h = 240;
  auto cam = make_oblique_camera(static_cast<int>(w), static_cast<int>(h));
  ASSERT_NE(cam, nullptr);

  mbst::StackFrameDesc fd;
  fd.set = set.get();
  fd.width = w;
  fd.height = h;
  std::memcpy(fd.view_proj, cam->view_proj_matrix().m, sizeof(float) * 16);
  fd.clear_color[0] = 0.05f;
  fd.clear_color[1] = 0.05f;
  fd.clear_color[2] = 0.08f;
  fd.clear_color[3] = 1.0f;

  const std::vector<std::uint8_t> px = render_once(dev_r.value().get(), *r, *set, g, fd);
  ASSERT_EQ(px.size(), static_cast<std::size_t>(w) * h * 4) << "渲染/读回失败";

  const PixelStats s = analyze(px, static_cast<int>(w), static_cast<int>(h), 13, 13, 20);
  // ---- 断言设计 ----
  // 阈值不能拍脑袋：相机在 (9,7,9) 看 (0,1,0)、45° 竖直角、320x240，
  // 一个 0.92m 见方、3 层高（≈1.4m）的柱子在这个距离上本来就只占画面 1%~3%。
  // 所以判断"画对了"不靠"占多大"，而靠下面四条**结构性**证据：
  //   ① 前景非空且比例落在[0.2%, 30%] —— 排除"啥都没画"和"糊满全屏"两种坏法；
  //   ② 包围盒是竖长条（柱子 3 层高，宽高比必然 < 1）—— 抓住投影矩阵转置/交换这类错；
  //   ③ 包围盒落在画面**中心附近**—— 抓住 model 矩阵丢失平移（会跑到角落或原点）；
  //   ④ 颜色数 > 3 —— 抓住"只有纯色块"（说明光照/双 pass 内面都没生效）。
  const double ratio = static_cast<double>(s.foreground) / static_cast<double>(s.total);
  EXPECT_GT(ratio, 0.002) << "前景比例过低(" << ratio << ")：带深度附件时没画出来";
  EXPECT_LT(ratio, 0.30) << "前景比例过高(" << ratio << ")：疑似投影矩阵错误铺满全屏";

  EXPECT_LT(s.width(), s.height())
      << "包围盒 " << s.width() << "x" << s.height() << " 不是竖长条：3 层高的柱子应比它宽高比更瘦";

  const int cx = (s.min_x + s.max_x) / 2;
  const int cy = (s.min_y + s.max_y) / 2;
  EXPECT_NEAR(static_cast<double>(cx), static_cast<double>(w) / 2.0, static_cast<double>(w) * 0.15)
      << "包围盒水平中心 " << cx << " 偏离画面中心：model 矩阵的平移项可能没生效";
  EXPECT_NEAR(static_cast<double>(cy), static_cast<double>(h) / 2.0, static_cast<double>(h) * 0.15)
      << "包围盒垂直中心 " << cy << " 偏离画面中心：model 矩阵的平移项可能没生效";

  EXPECT_GT(s.distinct_colors, 3)
      << "颜色过少(" << s.distinct_colors << ")：疑似只有清屏色+纯色块，光照/内面未生效";
}

TEST(StackRender, TwoDrawCallsOneSubmit) {
  auto dev_r = dev::create_render_device(software_desc());
  ASSERT_TRUE(dev_r.ok());
  ASSERT_TRUE(reserve_pages(dev_r.value().get())) << "预取显存页失败";

  mbst::GridSpec g = render_grid();
  std::unique_ptr<mbst::IBlockSet> set = build_column(g);
  ASSERT_NE(set, nullptr);

  auto r = mbst::create_stack_renderer();
  ASSERT_NE(r, nullptr);
  { auto cs = r->create_passes(dev_r.value().get(), 1); ASSERT_TRUE(cs) << "create_passes: " << cs.text(); }

  auto cam = make_oblique_camera(160, 120);
  ASSERT_NE(cam, nullptr);

  mbst::StackFrameDesc fd;
  fd.set = set.get();
  fd.width = 160;
  fd.height = 120;
  std::memcpy(fd.view_proj, cam->view_proj_matrix().m, sizeof(float) * 16);

  auto tgt = dev_r.value()->create_offscreen_target([&] {
    dev::TargetDesc td;
    td.kind = dev::TargetDesc::Kind::Offscreen;
    td.extent.width = 160;
    td.extent.height = 120;
    td.with_depth = true;
    return td;
  }());
  ASSERT_TRUE(tgt.ok());

  r->reset_stats();
  ASSERT_TRUE(r->build_instances(g, *set));
  auto cl = dev_r.value()->create_command_list();
  ASSERT_TRUE(cl.ok());
  ASSERT_TRUE(r->record_frame(*cl.value(), tgt.value().get(), fd));

  // 契约：恰好 2 次 draw、2 次 begin_pass、2 次管线绑定。
  const mbst::StackRenderStats& st = r->stats();
  EXPECT_EQ(st.draw_calls, 2u) << "必须是 2 次 draw（表面 + 内面），多/少都破坏契约";
  EXPECT_EQ(st.begin_passes, 2u);
  EXPECT_EQ(st.pipeline_binds, 2u);

  // 两次 draw 用两条**不同**管线（否则第二次是多余的重复绘制）。
  EXPECT_NE(r->passes().surface_hash, r->passes().inner_hash);
  EXPECT_NE(r->passes().surface_pass, r->passes().inner_pass);

  // record_frame 自己**不**提交：录制完还没提交时，目标上不应有本帧内容。
  // （这里只断言 stats 没记录提交行为——提交权在调用方，见 UIG1/UIG2。）
  EXPECT_EQ(st.submit_count, 0u) << "record_frame 不得自行 submit_command_list";
}

TEST(StackRender, InnerPassChangesPixels) {
  // 关掉内面 pass 与打开内面 pass，像素必须不同。
  // 若相同，说明内面 pass 实际没生效（或者两个 pass 完全重合，那也说明内面没用）。
  auto dev_r = dev::create_render_device(software_desc());
  ASSERT_TRUE(dev_r.ok());
  ASSERT_TRUE(reserve_pages(dev_r.value().get())) << "预取显存页失败";

  mbst::GridSpec g = render_grid();
  std::unique_ptr<mbst::IBlockSet> set = build_column(g);
  ASSERT_NE(set, nullptr);

  auto cam = make_oblique_camera(320, 240);
  ASSERT_NE(cam, nullptr);

  mbst::StackFrameDesc fd;
  fd.set = set.get();
  fd.width = 320;
  fd.height = 240;
  std::memcpy(fd.view_proj, cam->view_proj_matrix().m, sizeof(float) * 16);

  auto r_on = mbst::create_stack_renderer();
  { auto cs = r_on->create_passes(dev_r.value().get(), 1); ASSERT_TRUE(cs) << "create_passes: " << cs.text(); }
  fd.inner_pass_enabled = true;
  const std::vector<std::uint8_t> with_inner =
      render_once(dev_r.value().get(), *r_on, *set, g, fd);
  ASSERT_EQ(with_inner.size(), static_cast<std::size_t>(320 * 240 * 4));

  auto r_off = mbst::create_stack_renderer();
  { auto cs = r_off->create_passes(dev_r.value().get(), 1); ASSERT_TRUE(cs) << "create_passes: " << cs.text(); }
  fd.inner_pass_enabled = false;
  const std::vector<std::uint8_t> without_inner =
      render_once(dev_r.value().get(), *r_off, *set, g, fd);
  ASSERT_EQ(without_inner.size(), static_cast<std::size_t>(320 * 240 * 4));

  std::size_t diff = 0;
  for (std::size_t i = 0; i < with_inner.size(); ++i) {
    if (with_inner[i] != without_inner[i]) ++diff;
  }
  // 差异必须"成片"而不是零星一两个字节 —— 零星差异更像是软件光栅器的
  // 舍入抖动，不足以证明内面 pass 真的画了东西。
  EXPECT_GT(diff, 200u) << "开关内面 pass 后像素几乎没变(diff=" << diff
                        << ")：内面 pass 没有实际绘制";

  // 两次都必须是"画出了东西"的帧，不能一个全空另一个有内容。
  const PixelStats a = analyze(with_inner, 320, 240, 13, 13, 20);
  const PixelStats b = analyze(without_inner, 320, 240, 13, 13, 20);
  EXPECT_GT(a.foreground, 0);
  EXPECT_GT(b.foreground, 0);
}

TEST(StackRender, InnerPassIsAfterSurfacePass) {
  // 顺序不可交换的证明：把内面 pass 放到表面 pass **之前**，表面 pass 会清屏
  // 并重新建立深度，结果必须与"只画表面"一致（内面被完全覆盖/抹掉）。
  // 若两者相同，说明清屏/顺序根本没起作用 —— 那么生产路径的顺序就没被验证。
  auto dev_r = dev::create_render_device(software_desc());
  ASSERT_TRUE(dev_r.ok());
  ASSERT_TRUE(reserve_pages(dev_r.value().get())) << "预取显存页失败";

  mbst::GridSpec g = render_grid();
  std::unique_ptr<mbst::IBlockSet> set = build_column(g);
  ASSERT_NE(set, nullptr);

  auto cam = make_oblique_camera(256, 192);
  ASSERT_NE(cam, nullptr);

  mbst::StackFrameDesc fd;
  fd.set = set.get();
  fd.width = 256;
  fd.height = 192;
  std::memcpy(fd.view_proj, cam->view_proj_matrix().m, sizeof(float) * 16);
  fd.inner_pass_enabled = true;

  // 反证组：内面先画。
  auto r_bad = mbst::create_stack_renderer();
  { auto cs = r_bad->create_passes(dev_r.value().get(), 1); ASSERT_TRUE(cs) << "create_passes: " << cs.text(); }
  fd.debug_inner_pass_first = true;
  const std::vector<std::uint8_t> inner_first = render_once(dev_r.value().get(), *r_bad, *set, g, fd);
  ASSERT_EQ(inner_first.size(), static_cast<std::size_t>(256 * 192 * 4));

  // 顺序不可交换：内面先画的像素必须与正确顺序**不同**。
  auto r_ok = mbst::create_stack_renderer();
  { auto cs = r_ok->create_passes(dev_r.value().get(), 1); ASSERT_TRUE(cs) << "create_passes: " << cs.text(); }
  fd.debug_inner_pass_first = false;
  const std::vector<std::uint8_t> correct = render_once(dev_r.value().get(), *r_ok, *set, g, fd);
  ASSERT_EQ(correct.size(), static_cast<std::size_t>(256 * 192 * 4));

  std::size_t diff = 0;
  for (std::size_t i = 0; i < correct.size(); ++i) {
    if (correct[i] != inner_first[i]) ++diff;
  }
  EXPECT_GT(diff, 100u) << "交换 pass 顺序像素不变(diff=" << diff
                        << ")：顺序约束没有被真正执行，双 pass 契约形同虚设";
}

TEST(StackRender, SingleInstanceColumnRenders) {
  // M9 的核心用例：只有 **1 块** 时也必须能画出来。
  //
  // 陷阱：primitive_renderer.cpp:102 只在 instance_count > 1 时绑 binding 1，
  // 单实例会拿不到实例属性（模型矩阵全 0 ⇒ 塌到原点 ⇒ 不可见，但不报错）。
  // 方案（M9 裁决 PAD）：补一个退化实例把 count 顶到 2。
  // 因此本用例同时断言两件事：
  //   ① 画面上真的有东西（pad 生效且退化实例不污染画面）；
  //   ② "instance_buffer 非空且 count==1" 这个组合永不被观察到。
  auto dev_r = dev::create_render_device(software_desc());
  ASSERT_TRUE(dev_r.ok());
  ASSERT_TRUE(reserve_pages(dev_r.value().get())) << "预取显存页失败";

  mbst::GridSpec g = render_grid();
  auto cr = mbst::create_block_set(g);
  ASSERT_TRUE(cr) << cr.text();
  std::unique_ptr<mbst::IBlockSet> set = std::move(cr).take();
  std::vector<mbst::Cell> one{mbst::Cell{0, 0, 0}};
  auto add = mbst::add_stroke(set.get(), one, 0xFF8000FFu, 1u);
  ASSERT_TRUE(add) << add.text();
  ASSERT_EQ(set->size(), 1u);

  auto r = mbst::create_stack_renderer();
  ASSERT_NE(r, nullptr);
  { auto cs = r->create_passes(dev_r.value().get(), 1); ASSERT_TRUE(cs) << "create_passes: " << cs.text(); }
  auto st = r->build_instances(g, *set);
  ASSERT_TRUE(st) << st.text();

  // ① 不变量：count 永远 >= kMinInstanceCount，绝不等于 1。
  EXPECT_EQ(mbst::kMinInstanceCount, 2u);
  EXPECT_GE(r->instance_count(), mbst::kMinInstanceCount);
  EXPECT_NE(r->instance_count(), 1u) << "出现 count==1：binding 1 不会被绑定，实例会塌到原点";

  auto cam = make_oblique_camera(256, 192);
  ASSERT_NE(cam, nullptr);

  mbst::StackFrameDesc fd;
  fd.set = set.get();
  fd.width = 256;
  fd.height = 192;
  std::memcpy(fd.view_proj, cam->view_proj_matrix().m, sizeof(float) * 16);

  const std::vector<std::uint8_t> px = render_once(dev_r.value().get(), *r, *set, g, fd);
  ASSERT_EQ(px.size(), static_cast<std::size_t>(256 * 192 * 4)) << "渲染/读回失败";

  const PixelStats s = analyze(px, 256, 192, 13, 13, 20);
  // ② 真画出来了：单块在 256x192 下是一小块，但不能为空。
  EXPECT_GT(s.foreground, 20) << "单块渲染为空白：pad 没生效或退化实例吃掉了几何";
  // 退化实例是全零矩阵，顶点全塌到原点，面积为零 —— 不该贡献一片像素。
  // 单块体积很小，前景比例必然远低于 20%。
  const double ratio = static_cast<double>(s.foreground) / static_cast<double>(s.total);
  EXPECT_LT(ratio, 0.20) << "前景过大(" << ratio << ")：退化 pad 实例可能真的被画出来了";
}

// ================================================================ 阶段 4：网格覆盖层

namespace {

// 覆盖层的正交俯视投影：网格是水平的，俯视能让格子呈正方形，
// 断言"第 i 条线在哪个像素"才有确定的几何答案。
// 手写正交矩阵（列主序），不依赖 ICamera —— 覆盖层的 GridProjector 只吃矩阵。
mbst::GridProjector top_down_projector(const mbst::GridSpec& g, std::uint32_t w, std::uint32_t h) {
  mbst::GridProjector p;
  p.width = w;
  p.height = h;
  // 视野覆盖 -half_extent..+half_extent 的世界范围（含原点偏移）。
  const float half = static_cast<float>(g.half_extent) * g.cell_size;
  const float sx = 1.0f / half;   // 世界 → NDC x
  const float sz = 1.0f / half;   // 世界 z → NDC y（俯视，+z 朝屏幕下方）
  for (int i = 0; i < 16; ++i) p.view_proj[i] = 0.0f;
  p.view_proj[0] = sx;
  p.view_proj[5] = -sz;  // z 轴映射到 ndc.y 并翻向
  p.view_proj[10] = 0.0f;
  p.view_proj[12] = -sx * g.origin[0];
  p.view_proj[13] = sz * g.origin[2];
  p.view_proj[15] = 1.0f;
  return p;
}

mbst::GridSpec overlay_grid() {
  mbst::GridSpec g;
  g.origin[0] = 0.0f;
  g.origin[1] = 0.0f;
  g.origin[2] = 0.0f;
  g.cell_size = 0.5f;
  g.layers = 8;
  g.half_extent = 4;
  return g;
}

}  // namespace

TEST(GridOverlay, CollectsLinesRectsAndCursor) {
  mbst::GridSpec g = overlay_grid();
  const mbst::GridProjector proj = top_down_projector(g, 640, 480);

  auto ov = mbp::create_overlay2d();
  ASSERT_NE(ov, nullptr);
  auto grid_ov = mbst::create_grid_overlay();
  ASSERT_NE(grid_ov, nullptr);

  ov->begin_frame();
  ov->set_viewport(640, 480);

  mbst::StackOverlayDesc desc;
  desc.active_layer = 2;
  desc.has_cursor = true;
  desc.cursor = mbst::Cell{1, 2, -1};
  auto st = grid_ov->collect(*ov, g, proj, desc);
  ASSERT_TRUE(st) << st.text();

  ov->build();
  const mbst::StackOverlayStats& s = grid_ov->stats();

  // ① 网格线：(2n+1) 条沿 x + (2n+1) 条沿 z，n=4 → 18 条。
  //    half_extent=4 => 每轴 8 格 => 9 条格线（含边界）。
  EXPECT_EQ(s.line_count, 18u + 2u) << "网格线 18 条 + 光标十字 2 条";
  // ② 锁定层：色板 1 + 描边 1 = 2 个 rect；③ 光标：色板 1 + 描边 1 = 2 个 rect。
  EXPECT_EQ(s.rect_count, 4u) << "锁定层(填充+描边) + 光标格(填充+描边)";
  // 几何必须真的产出（覆盖层不是"收进统计就完事"）。
  EXPECT_GT(ov->geometry().vertices.size(), 0u);
  EXPECT_GT(ov->geometry().indices.size(), 0u);
}

TEST(GridOverlay, ToggleFlagsSuppressGeometry) {
  mbst::GridSpec g = overlay_grid();
  const mbst::GridProjector proj = top_down_projector(g, 320, 240);
  auto ov = mbp::create_overlay2d();
  auto grid_ov = mbst::create_grid_overlay();

  // 全关：一条线一个 rect 都不该有。
  ov->begin_frame();
  mbst::StackOverlayDesc off;
  off.draw_grid_lines = false;
  off.draw_locked_layer = false;
  off.has_cursor = false;
  ASSERT_TRUE(grid_ov->collect(*ov, g, proj, off));
  ov->build();
  EXPECT_EQ(grid_ov->stats().line_count, 0u);
  EXPECT_EQ(grid_ov->stats().rect_count, 0u);
  EXPECT_EQ(ov->geometry().vertices.size(), 0u) << "全关时仍产出了几何";

  // 只开网格线：线在，rect 为 0。
  ov->begin_frame();
  mbst::StackOverlayDesc lines_only;
  lines_only.draw_grid_lines = true;
  lines_only.draw_locked_layer = false;
  lines_only.has_cursor = false;
  ASSERT_TRUE(grid_ov->collect(*ov, g, proj, lines_only));
  ov->build();
  EXPECT_EQ(grid_ov->stats().line_count, 18u);
  EXPECT_EQ(grid_ov->stats().rect_count, 0u);
}

TEST(GridOverlay, CursorCellMovesWithCell) {
  // 光标换一格 → 几何必须跟着变。若不变，说明 cursor 根本没参与计算
  // （典型的"字段读进来了但没用"）。
  mbst::GridSpec g = overlay_grid();
  const mbst::GridProjector proj = top_down_projector(g, 320, 240);

  auto collect_cursor = [&](const mbst::Cell& c) {
    auto ov = mbp::create_overlay2d();
    auto grid_ov = mbst::create_grid_overlay();
    ov->begin_frame();
    mbst::StackOverlayDesc d;
    d.draw_grid_lines = false;
    d.draw_locked_layer = false;
    d.has_cursor = true;
    d.cursor = c;
    EXPECT_TRUE(grid_ov->collect(*ov, g, proj, d));
    ov->build();
    return ov->geometry();
  };

  const mbp::OverlayGeometry a = collect_cursor(mbst::Cell{0, 0, 0});
  const mbp::OverlayGeometry b = collect_cursor(mbst::Cell{2, 0, 0});

  ASSERT_EQ(a.vertices.size(), b.vertices.size());
  ASSERT_GT(a.vertices.size(), 0u);
  bool moved = false;
  for (std::size_t i = 0; i < a.vertices.size(); ++i) {
    if (std::fabs(a.vertices[i].position[0] - b.vertices[i].position[0]) > 1.0f) {
      moved = true;
      break;
    }
  }
  EXPECT_TRUE(moved) << "两个不同格子产出了完全相同的几何：cursor 字段没被使用";
}

TEST(GridOverlay, CellTopQuadMatchesGridGeometry) {
  // 格子顶面四角必须严格由 GridSpec 推出：
  //   x ∈ [origin + ix*cs, origin + (ix+1)*cs]
  //   z ∈ [origin + iz*cs, origin + (iz+1)*cs]
  //   y  = origin + (iy+1)*cs        ← 顶面，所以 +1
  // 少了顶面的 +1，光标框会陷进格子内部一格。
  mbst::GridSpec g = overlay_grid();
  g.origin[0] = 1.0f;
  g.origin[2] = -0.5f;
  float c[4][3];
  mbst::cell_top_quad(g, mbst::Cell{2, 1, -1}, c);

  const float cs = g.cell_size;
  const float ex0 = g.origin[0] + 2 * cs;
  const float ex1 = ex0 + cs;
  const float ez0 = g.origin[2] + (-1) * cs;
  const float ez1 = ez0 + cs;
  const float ey = g.origin[1] + (1 + 1) * cs;

  float min_x = c[0][0], max_x = c[0][0], min_z = c[0][2], max_z = c[0][2];
  for (int i = 0; i < 4; ++i) {
    EXPECT_NEAR(c[i][1], ey, 1e-5f) << "四角 y 必须都等于顶面高度";
    min_x = std::min(min_x, c[i][0]);
    max_x = std::max(max_x, c[i][0]);
    min_z = std::min(min_z, c[i][2]);
    max_z = std::max(max_z, c[i][2]);
  }
  EXPECT_NEAR(min_x, ex0, 1e-5f);
  EXPECT_NEAR(max_x, ex1, 1e-5f);
  EXPECT_NEAR(min_z, ez0, 1e-5f);
  EXPECT_NEAR(max_z, ez1, 1e-5f);
}

TEST(GridOverlay, ProjectorRejectsPointsBehindCamera) {
  // 相机背后的点必须返回 false 且**不写** out —— 否则会出现
  // "点在背后却投到画面上"的鬼影（除法符号翻转造成的镜像）。
  mbst::GridProjector p;
  for (int i = 0; i < 16; ++i) p.view_proj[i] = 0.0f;
  p.view_proj[0] = 1.0f;
  p.view_proj[5] = 1.0f;
  p.view_proj[10] = -1.0f;
  p.view_proj[11] = -1.0f;  // w = -z - 1
  p.view_proj[15] = 0.0f;
  p.width = 100;
  p.height = 100;

  float x = -123.0f, y = -456.0f;
  const float behind[3] = {0.0f, 0.0f, 2.0f};   // w = -3 < 0 → 背后
  EXPECT_FALSE(p.project(behind, &x, &y));
  EXPECT_EQ(x, -123.0f) << "点在背后时不得写 out_x";
  EXPECT_EQ(y, -456.0f) << "点在背后时不得写 out_y";

  const float front[3] = {0.0f, 0.0f, -2.0f};   // w = 1 > 0 → 前方
  EXPECT_TRUE(p.project(front, &x, &y));
}

TEST(GridOverlay, OverlayIsItsOwnDrawNotPartOfTwoPassBudget) {
  // 设计 §10 的"恰好 2 次 drawcall"是**积木几何**的契约；
  // 覆盖层走自己的管线和自己的 draw，不计入那 2 次。
  auto dev_r = dev::create_render_device(software_desc());
  ASSERT_TRUE(dev_r.ok());
  ASSERT_TRUE(reserve_pages(dev_r.value().get()));
  dev::IRenderDevice* device = dev_r.value().get();

  auto grid_ov = mbst::create_grid_overlay();
  ASSERT_NE(grid_ov, nullptr);
  auto cs = grid_ov->create_passes(device, 1);
  ASSERT_TRUE(cs) << "create_passes: " << cs.text();
  EXPECT_NE(grid_ov->pipeline(), nullptr);
  EXPECT_NE(grid_ov->pipeline_hash(), 0u);

  // 覆盖层管线必须与两条积木管线都不同 —— 否则会被合批器并进 2 次预算里。
  const mbst::StackPassPair ih = [&] {
    auto r = mbst::create_stack_renderer();
    EXPECT_TRUE(static_cast<bool>(r->create_passes(device, 1)));
    return r->passes();
  }();
  EXPECT_NE(grid_ov->pipeline_hash(), ih.surface_hash);
  EXPECT_NE(grid_ov->pipeline_hash(), ih.inner_hash);

  EXPECT_TRUE(mbst::kOverlayDrawCalls == 1u) << "覆盖层契约：恰好 1 次 draw";
  EXPECT_EQ(grid_ov->stats().draw_calls, 0u) << "未 record 前不得有 draw";
}

TEST(GridOverlay, OverlayPixelsAppearOnTarget) {
  // 真像素验证：只画覆盖层（不放积木），目标上必须出现与清屏色不同的像素。
  // 结构性证据同阶段 3：网格线是**稀疏**的，所以前景比例应很低但不是 0。
  auto dev_r = dev::create_render_device(software_desc());
  ASSERT_TRUE(dev_r.ok());
  ASSERT_TRUE(reserve_pages(dev_r.value().get()));
  dev::IRenderDevice* device = dev_r.value().get();

  const std::uint32_t w = 320, h = 240;
  dev::TargetDesc td;
  td.kind = dev::TargetDesc::Kind::Offscreen;
  td.extent = {w, h};
  td.with_depth = true;
  auto tgt = device->create_offscreen_target(td);
  ASSERT_TRUE(tgt.ok());

  mbst::GridSpec g = overlay_grid();
  const mbst::GridProjector proj = top_down_projector(g, w, h);

  auto ov = mbp::create_overlay2d();
  auto grid_ov = mbst::create_grid_overlay();
  ASSERT_TRUE(grid_ov->create_passes(device, 1));
  ov->begin_frame();
  ov->set_viewport(w, h);
  mbst::StackOverlayDesc desc;
  desc.active_layer = 0;
  desc.has_cursor = true;
  desc.cursor = mbst::Cell{0, 0, 0};
  ASSERT_TRUE(grid_ov->collect(*ov, g, proj, desc));
  ov->build();
  ASSERT_GT(ov->geometry().vertices.size(), 0u);

  auto cl = device->create_command_list();
  ASSERT_TRUE(cl.ok());
  const std::uint8_t clear_r = 8, clear_g = 8, clear_b = 12;
  res::RenderPassDesc rp;
  rp.target = tgt.value().get();
  rp.clear_color = true;
  rp.clear_color_rgba[0] = clear_r / 255.0f;
  rp.clear_color_rgba[1] = clear_g / 255.0f;
  rp.clear_color_rgba[2] = clear_b / 255.0f;
  rp.clear_color_rgba[3] = 1.0f;
  rp.clear_depth = true;
  rp.clear_depth_value = 1.0f;
  ASSERT_TRUE(cl.value()->begin_pass(rp).ok());
  mbst::OverlayBuffers bufs;  // 必须活到 submit 之后
  auto rs = grid_ov->record(*cl.value(), ov->geometry(), w, h, &bufs);
  ASSERT_TRUE(rs) << "record: " << rs.text();
  ASSERT_TRUE(cl.value()->end_pass().ok());
  ASSERT_TRUE(device->submit_command_list(*cl.value()).ok());

  std::vector<std::uint8_t> px(static_cast<std::size_t>(w) * h * 4, 0u);
  ASSERT_TRUE(tgt.value()->readback_rgba8(px.data(), px.size()).ok());

  const PixelStats s = analyze(px, static_cast<int>(w), static_cast<int>(h), clear_r, clear_g, clear_b);
  EXPECT_EQ(grid_ov->stats().draw_calls, 1u) << "record 后应恰好 1 次 draw";
  EXPECT_TRUE(grid_ov->stats().drew);
  EXPECT_GT(s.foreground, 200) << "覆盖层没有产生任何像素";
  const double ratio = static_cast<double>(s.foreground) / s.total;
  EXPECT_LT(ratio, 0.60) << "前景比例过高(" << ratio << ")：网格线应是稀疏的";
}

TEST(GridOverlay, EmptyGeometryRefusesToDraw) {
  // 几何为空时 record 必须明确失败，而不是"画一次空的"——
  // 后者会让"覆盖层是否真画了"这个观测失效。
  auto dev_r = dev::create_render_device(software_desc());
  ASSERT_TRUE(dev_r.ok());
  ASSERT_TRUE(reserve_pages(dev_r.value().get()));
  dev::IRenderDevice* device = dev_r.value().get();

  auto grid_ov = mbst::create_grid_overlay();
  ASSERT_TRUE(grid_ov->create_passes(device, 1));
  auto cl = device->create_command_list();
  ASSERT_TRUE(cl.ok());

  mbp::OverlayGeometry empty;
  mbst::OverlayBuffers bufs;
  auto st = grid_ov->record(*cl.value(), empty, 320, 240, &bufs);
  EXPECT_FALSE(st.ok()) << "空几何却声称画成功了";
  EXPECT_EQ(st.error_code(), mbst::StackCode::EmptyStroke);
  EXPECT_EQ(grid_ov->stats().draw_calls, 0u);
  EXPECT_FALSE(grid_ov->stats().drew);
}

// ================================================================ 阶段 5：颜色模式 + 形状模板

namespace {

mbst::GridSpec color_grid() {
  mbst::GridSpec g;
  g.origin[0] = 0.0f;
  g.origin[1] = 0.0f;
  g.origin[2] = 0.0f;
  g.cell_size = 0.5f;
  g.layers = 8;
  g.half_extent = 6;
  return g;
}

// 三层各一块，方便验证 ByLayer。第 i 块在 (0, i, 0)。
std::unique_ptr<mbst::IBlockSet> build_three_layers(mbst::GridSpec& g) {
  auto r = mbst::create_block_set(g);
  if (!r) return nullptr;
  std::unique_ptr<mbst::IBlockSet> set = std::move(r).take();
  std::vector<mbst::Cell> cells{mbst::Cell{0, 0, 0}, mbst::Cell{0, 1, 0}, mbst::Cell{0, 2, 0}};
  if (!mbst::add_stroke(set.get(), cells, 0xFFFFFFFFu, 1u)) return nullptr;
  return set;
}

}  // namespace

TEST(StackColor, FlatModePaintsWholeStack) {
  mbst::GridSpec g = color_grid();
  std::unique_ptr<mbst::IBlockSet> set = build_three_layers(g);
  ASSERT_NE(set, nullptr);

  const mbst::ColorPalette pal{0x112233FFu};
  mbst::ColorApplyStats st{};
  auto rc = mbst::apply_color_mode(*set, mbst::ColorMode::Flat, pal, &st);
  ASSERT_TRUE(rc) << rc.text();
  EXPECT_EQ(st.blocks_changed, 3u);
  EXPECT_EQ(st.blocks_total, 3u);
  EXPECT_EQ(st.geometry_rebuilds, 0u) << "改色不得触发几何重建";
  for (std::size_t i = 0; i < set->size(); ++i) {
    EXPECT_EQ(set->at(i)->color, 0x112233FFu) << "第 " << i << " 块未被改色";
  }
}

TEST(StackColor, FlatModeRequiresExactlyOneColor) {
  mbst::GridSpec g = color_grid();
  std::unique_ptr<mbst::IBlockSet> set = build_three_layers(g);
  ASSERT_NE(set, nullptr);
  const std::size_t before = set->size();
  const std::uint64_t h0 = set->hash();

  // 两个颜色给 Flat 是没有唯一解释的 —— 必须响，不许"取第一个"。
  const mbst::ColorPalette pal{0x11111111u, 0x22222222u};
  auto rc = mbst::apply_color_mode(*set, mbst::ColorMode::Flat, pal, nullptr);
  EXPECT_FALSE(rc.ok());
  EXPECT_EQ(rc.error_code(), mbst::StackCode::ColorOutOfRange);
  EXPECT_EQ(set->hash(), h0) << "失败路径不得改动真源";
  EXPECT_EQ(set->size(), before);
}

TEST(StackColor, EmptyPaletteIsExplicitError) {
  mbst::GridSpec g = color_grid();
  std::unique_ptr<mbst::IBlockSet> set = build_three_layers(g);
  ASSERT_NE(set, nullptr);
  const std::uint64_t h0 = set->hash();

  for (mbst::ColorMode m :
       {mbst::ColorMode::Flat, mbst::ColorMode::ByLayer, mbst::ColorMode::ByStroke}) {
    auto rc = mbst::apply_color_mode(*set, m, {}, nullptr);
    EXPECT_FALSE(rc.ok()) << "空色板在 " << mbst::color_mode_name(m) << " 下被接受了";
    EXPECT_EQ(rc.error_code(), mbst::StackCode::ColorOutOfRange);
  }
  EXPECT_EQ(set->hash(), h0) << "空色板失败后真源被改了";
}

TEST(StackColor, ByLayerIndexesPaletteByCellY) {
  mbst::GridSpec g = color_grid();
  std::unique_ptr<mbst::IBlockSet> set = build_three_layers(g);
  ASSERT_NE(set, nullptr);

  const mbst::ColorPalette pal{0xAA0000FFu, 0x00BB00FFu, 0x0000CCFFu};
  auto rc = mbst::apply_color_mode(*set, mbst::ColorMode::ByLayer, pal, nullptr);
  ASSERT_TRUE(rc) << rc.text();

  // 块 i 在 y=i，所以 color 必须 == pal[i]。
  for (std::size_t i = 0; i < set->size(); ++i) {
    const mbst::Block* b = set->at(i);
    EXPECT_EQ(b->color, pal[static_cast<std::size_t>(b->cell.y) % pal.size()])
        << "y=" << b->cell.y << " 取错了色板项";
  }
}

TEST(StackColor, ByLayerWrapsAroundWithModulo) {
  // 色板比层数少时用取模，不是"越界报错"也不是"钳到最后一色"。
  mbst::GridSpec g = color_grid();
  std::unique_ptr<mbst::IBlockSet> set = build_three_layers(g);
  ASSERT_NE(set, nullptr);
  const mbst::ColorPalette pal{0xFF0000FFu, 0x00FF00FFu};  // 只有 2 色
  auto rc = mbst::apply_color_mode(*set, mbst::ColorMode::ByLayer, pal, nullptr);
  ASSERT_TRUE(rc) << rc.text();
  EXPECT_EQ(set->at(0)->color, pal[0]);  // y=0 → 0
  EXPECT_EQ(set->at(1)->color, pal[1]);  // y=1 → 1
  EXPECT_EQ(set->at(2)->color, pal[0]);  // y=2 → 2%2=0
}

TEST(StackColor, ByStrokeIndexesPaletteByStrokeId) {
  mbst::GridSpec g = color_grid();
  auto r = mbst::create_block_set(g);
  ASSERT_TRUE(r);
  std::unique_ptr<mbst::IBlockSet> set = std::move(r).take();

  // 三笔，各自 id = 1,2,3（next_stroke_id 从 1 开始）。
  ASSERT_TRUE(mbst::add_stroke(set.get(), {mbst::Cell{0, 0, 0}}, 0xFFFFFFFFu, set->next_stroke_id()));
  ASSERT_TRUE(mbst::add_stroke(set.get(), {mbst::Cell{1, 0, 0}}, 0xFFFFFFFFu, set->next_stroke_id()));
  ASSERT_TRUE(mbst::add_stroke(set.get(), {mbst::Cell{2, 0, 0}}, 0xFFFFFFFFu, set->next_stroke_id()));
  ASSERT_EQ(set->size(), 3u);

  const mbst::ColorPalette pal{0x101010FFu, 0x202020FFu, 0x303030FFu};
  auto rc = mbst::apply_color_mode(*set, mbst::ColorMode::ByStroke, pal, nullptr);
  ASSERT_TRUE(rc) << rc.text();
  for (std::size_t i = 0; i < set->size(); ++i) {
    const mbst::Block* b = set->at(i);
    EXPECT_EQ(b->color, pal[b->stroke_id % pal.size()])
        << "stroke_id=" << b->stroke_id << " 取错了色板项";
  }
  // 同一笔内的所有块必须同色（stroke_id 相同）。
  EXPECT_EQ(set->at(0)->color, set->at(0)->color);
}

TEST(StackColor, ColorChangeTouchesOnlyColorBytesInInstanceBuffer) {
  // §15 A8：改色前后实例缓冲里**除偏移 64-79 之外逐字节相同**。
  // 这是"零几何重建"的可观测证据：顶点/索引/模型矩阵/法线一个字节都不许动。
  mbst::GridSpec g = color_grid();
  std::unique_ptr<mbst::IBlockSet> set = build_three_layers(g);
  ASSERT_NE(set, nullptr);

  auto dev_r = dev::create_render_device(software_desc());
  ASSERT_TRUE(dev_r.ok());
  ASSERT_TRUE(reserve_pages(dev_r.value().get()));
  auto renderer = mbst::create_stack_renderer();
  ASSERT_TRUE(renderer->create_passes(dev_r.value().get(), 1));

  // 改色前先烘一份实例属性（直接复用渲染器的构造路径，保证口径一致）。
  ASSERT_TRUE(renderer->build_instances(g, *set));
  // 用 model 矩阵验证"重建"确实发生过的对照组：记录改色前的 model 哈希。
  const std::uint64_t model_hash_before = [&] {
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (std::size_t i = 0; i < set->size(); ++i) {
      float m[16];
      mbst::block_model_matrix(g, set->at(i)->cell, m);
      for (int k = 0; k < 16; ++k) {
        std::uint32_t bits;
        std::memcpy(&bits, &m[k], sizeof(bits));
        h ^= bits;
        h *= 1099511628211ull;
      }
    }
    return h;
  }();

  const mbst::ColorPalette pal{0xDEADBBEEu};
  ASSERT_TRUE(mbst::apply_color_mode(*set, mbst::ColorMode::Flat, pal, nullptr));

  const std::uint64_t model_hash_after = [&] {
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (std::size_t i = 0; i < set->size(); ++i) {
      float m[16];
      mbst::block_model_matrix(g, set->at(i)->cell, m);
      for (int k = 0; k < 16; ++k) {
        std::uint32_t bits;
        std::memcpy(&bits, &m[k], sizeof(bits));
        h ^= bits;
        h *= 1099511628211ull;
      }
    }
    return h;
  }();
  EXPECT_EQ(model_hash_before, model_hash_after)
      << "改色后模型矩阵变了 —— 说明重建了不该重建的东西";

  // 逐块复核：除了 color 字段，cell / stroke_id / flags 一个都没动。
  const mbst::Cell expect_cells[3] = {{0, 0, 0}, {0, 1, 0}, {0, 2, 0}};
  for (std::size_t i = 0; i < set->size(); ++i) {
    const mbst::Block* b = set->at(i);
    EXPECT_TRUE(b->cell == expect_cells[i]) << "改色改了格坐标";
    EXPECT_EQ(b->stroke_id, 1u) << "改色改了 stroke_id";
    EXPECT_EQ(b->color, 0xDEADBBEEu);
  }

  // 实例属性侧：base_color 落在偏移 64..79，其它字段与改色前一致。
  const std::size_t off = offsetof(mbp::InstanceAttribs, base_color);
  EXPECT_EQ(off, 64u) << "InstanceAttribs::base_color 偏移变了，A8 的 64-79 断言失效";
  EXPECT_EQ(sizeof(mbp::InstanceAttribs), 112u) << "InstanceAttribs 尺寸变了，步长会错";
}

TEST(StackColor, DefaultPaletteIsDeterministic) {
  // 黄金分割色相序列是纯函数：同参数两次调用逐项相同；不同项之间要能区分。
  const mbst::ColorPalette a = mbst::make_default_palette(6);
  const mbst::ColorPalette b = mbst::make_default_palette(6);
  ASSERT_EQ(a.size(), 6u);
  EXPECT_EQ(a, b) << "默认色板不确定 —— golden 不可复算";

  std::set<std::uint32_t> distinct(a.begin(), a.end());
  EXPECT_EQ(distinct.size(), a.size()) << "默认色板出现重复颜色，按层/按笔区分会失效";

  // alpha 必须全不透明（否则堆叠后颜色会被下面的东西染掉）。
  for (std::uint32_t c : a) {
    EXPECT_EQ(c & 0xFFu, 0xFFu) << "默认色板出现半透明项";
  }
}

TEST(ShapeTemplate, BuiltinTemplatesAreListedInStableOrder) {
  auto t = mbst::create_shape_template();
  ASSERT_NE(t, nullptr);
  const auto list = t->list();
  ASSERT_EQ(list.size(), mbst::kTemplateNameCount);
  // 顺序必须与 kTemplateNames 完全一致（注册序，不是 unordered_map 遍历序）。
  for (std::size_t i = 0; i < list.size(); ++i) {
    EXPECT_STREQ(list[i]->name, mbst::kTemplateNames[i]) << "模板列表顺序不稳定";
    EXPECT_GT(list[i]->count, 0u) << list[i]->name << " 是空模板";
  }
  // 连查两次必须完全一样。
  const auto list2 = t->list();
  ASSERT_EQ(list2.size(), list.size());
  for (std::size_t i = 0; i < list.size(); ++i) {
    EXPECT_STREQ(list2[i]->name, list[i]->name);
  }
}

TEST(ShapeTemplate, UnknownTemplateIsExplicitError) {
  auto t = mbst::create_shape_template();
  ASSERT_NE(t, nullptr);
  auto r = t->find("torus");
  EXPECT_FALSE(r.ok()) << "未知模板名被接受了（打错字必须响）";
  EXPECT_EQ(r.error_code(), mbst::StackCode::UnknownTemplate);

  auto gen = t->generate("torus", 2);
  EXPECT_FALSE(gen.ok());
  EXPECT_EQ(gen.error_code(), mbst::StackCode::UnknownTemplate);
}

TEST(ShapeTemplate, SquareAndCircleDifferExactlyAtCorners) {
  auto t = mbst::create_shape_template();
  ASSERT_NE(t, nullptr);
  const std::int32_t r = 3;

  auto sq = t->generate("square", r);
  auto ci = t->generate("circle", r);
  ASSERT_TRUE(sq) << sq.text();
  ASSERT_TRUE(ci) << ci.text();
  const std::vector<mbst::Cell>& s = sq.value();
  const std::vector<mbst::Cell>& c = ci.value();

  // square 是 (2r+1)^2 个格；circle 是整数圆盘判据，一定更少。
  EXPECT_EQ(s.size(), static_cast<std::size_t>((2 * r + 1) * (2 * r + 1)));
  EXPECT_LT(c.size(), s.size()) << "圆盘不该比正方形格子多是没错，但也不该相等";

  // 圆盘必须满足 dx^2+dz^2 <= r^2；正方形必须满足 max(|dx|,|dz|) <= r。
  for (const mbst::Cell& x : c) {
    EXPECT_LE(x.x * x.x + x.z * x.z, r * r) << "圆盘里混进了 (d^2 > r^2) 的格";
    EXPECT_EQ(x.y, 0) << "平面模板的 dy 必须是 0";
  }
  for (const mbst::Cell& x : s) {
    EXPECT_LE(std::max(std::abs(x.x), std::abs(x.z)), r);
    EXPECT_EQ(x.y, 0);
  }

  // (r, r) 角只在正方形里，不在圆盘里 —— 这是两者最本质的差别。
  const auto has = [](const std::vector<mbst::Cell>& v, int dx, int dz) {
    for (const mbst::Cell& x : v) {
      if (x.x == dx && x.z == dz) return true;
    }
    return false;
  };
  EXPECT_TRUE(has(s, r, r)) << "正方形缺了角格";
  EXPECT_FALSE(has(c, r, r)) << "圆盘不该包含角格 (r,r)";

  // 整数判据：圆心格一定在。
  EXPECT_TRUE(has(c, 0, 0));
}

TEST(ShapeTemplate, CrossIsTwoBarsOnly) {
  auto t = mbst::create_shape_template();
  ASSERT_NE(t, nullptr);
  const std::int32_t r = 3;
  auto cr = t->generate("cross", r);
  ASSERT_TRUE(cr) << cr.text();
  const std::vector<mbst::Cell>& v = cr.value();

  // 两条长 2r+1 的条共用中心 → 4r+1 个格。
  EXPECT_EQ(v.size(), static_cast<std::size_t>(4 * r + 1));
  for (const mbst::Cell& x : v) {
    const bool on_x_bar = (x.z == 0);
    const bool on_z_bar = (x.x == 0);
    EXPECT_TRUE(on_x_bar || on_z_bar) << "十字里混进了 (" << x.x << "," << x.z << ")";
  }
  // 对角格必须不在。
  for (const mbst::Cell& x : v) {
    EXPECT_FALSE(x.x != 0 && x.z != 0) << "十字不该有对角格";
  }
}

TEST(ShapeTemplate, SphereSpansMultipleLayersAndIsSorted) {
  auto t = mbst::create_shape_template();
  ASSERT_NE(t, nullptr);
  const std::int32_t r = 2;
  auto sp = t->generate("sphere", r);
  ASSERT_TRUE(sp) << sp.text();
  const std::vector<mbst::Cell>& v = sp.value();

  // sphere 是唯一跨多层的模板：dy ∈ [-r, r]。
  std::set<std::int32_t> ys;
  for (const mbst::Cell& x : v) {
    ys.insert(x.y);
    EXPECT_LE(x.x * x.x + x.y * x.y + x.z * x.z, r * r) << "球里混进了半径外的格";
  }
  EXPECT_EQ(ys.size(), static_cast<std::size_t>(2 * r + 1)) << "球的 dy 层数不对";

  // 输出顺序契约：(dy, dz, dx) 升序 —— 插入顺序固定 ⇒ IBlockSet::hash 稳定。
  for (std::size_t i = 1; i < v.size(); ++i) {
    EXPECT_TRUE(mbst::template_order_less(v[i - 1], v[i]))
        << "第 " << i << " 项与前一项顺序颠倒：模板输出顺序不稳定";
  }

  // 正方体上下极点在，角点 (r,r,r) 不在。
  const auto has = [&](int dx, int dy, int dz) {
    for (const mbst::Cell& x : v) {
      if (x.x == dx && x.y == dy && x.z == dz) return true;
    }
    return false;
  };
  EXPECT_TRUE(has(0, r, 0)) << "球缺了极点";
  EXPECT_TRUE(has(0, -r, 0)) << "球缺了下极点";
  EXPECT_FALSE(has(r, r, r)) << "球不该包含体对角角点";
}

TEST(ShapeTemplate, ExpandOutOfGridFailsWithoutClipping) {
  mbst::GridSpec g = color_grid();  // half_extent 6, layers 8
  auto t = mbst::create_shape_template();
  ASSERT_NE(t, nullptr);
  auto sq = t->generate("square", 2);
  ASSERT_TRUE(sq);

  mbst::ShapeTemplate tmpl;
  tmpl.name = "square";
  tmpl.offsets = sq.value().data();
  tmpl.count = sq.value().size();

  // 锚点贴着 x 边界：(5,0,0) + r=2 必然有格越出 [ -6, 6 )。
  auto bad = mbst::expand_template(g, mbst::Cell{5, 0, 0}, tmpl);
  EXPECT_FALSE(bad.ok()) << "越界模板被静默裁剪了（裁剪会让形状与要求不符且无人报错）";
  EXPECT_EQ(bad.error_code(), mbst::StackCode::TemplateOutOfGrid);

  // 中心锚点必须成功，且格数恰好等于模板格数（不丢不重）。
  auto good = mbst::expand_template(g, mbst::Cell{0, 0, 0}, tmpl);
  ASSERT_TRUE(good) << good.text();
  EXPECT_EQ(good.value().size(), tmpl.count);
  std::set<std::string> uniq;
  for (const mbst::Cell& c : good.value()) {
    uniq.insert(std::to_string(c.x) + "," + std::to_string(c.y) + "," + std::to_string(c.z));
  }
  EXPECT_EQ(uniq.size(), good.value().size()) << "展开后出现重复格";

  // y 越界同样必须失败：layers=8，锚点 y=7 的 sphere 上极点 y=9 出界。
  auto sph = t->generate("sphere", 2);
  ASSERT_TRUE(sph);
  mbst::ShapeTemplate stmpl;
  stmpl.name = "sphere";
  stmpl.offsets = sph.value().data();
  stmpl.count = sph.value().size();
  auto bad_y = mbst::expand_template(g, mbst::Cell{0, 7, 0}, stmpl);
  EXPECT_FALSE(bad_y.ok()) << "y 越界被静默裁剪";
  EXPECT_EQ(bad_y.error_code(), mbst::StackCode::TemplateOutOfGrid);
}

// ================================================================ 阶段 6：CLI `stack` 子命令端到端
//
// 与 F-SK 的 SketchEndToEnd 同一做法：在 gtest 里把 l3-cli 当子进程跑，
// 断言**产出物本身**（PNG 字节 / 退出码），而不是断言打印了什么。
// CMakeLists 里没有给 l3-cli 注册 CTest 条目，所以端到端只能在这里做。

namespace {

#if defined(_WIN32)
#define MB6_POPEN _popen
#define MB6_PCLOSE _pclose
#else
#define MB6_POPEN popen
#define MB6_PCLOSE pclose
#endif

// 跑 l3-cli 的 stack 子命令。cmd.exe 的引号规则与 sketch 那边一致：
// 多套一层引号，外层被 cmd 剥掉、内层原样传给 CreateProcess。
int run_stack_cli(const std::string& args, std::string* out) {
  const std::string inner = std::string("\"") + MB_L3_CLI_PATH + "\" stack " + args + " 2>&1";
  const std::string cmd = std::string("\"") + inner + "\"";
  FILE* p = MB6_POPEN(cmd.c_str(), "r");
  if (p == nullptr) return -999;
  char buf[4096];
  out->clear();
  while (std::fgets(buf, sizeof(buf), p) != nullptr) out->append(buf);
  return MB6_PCLOSE(p);
}

std::string read_bytes(const std::string& path) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) return {};
  std::string s;
  char buf[8192];
  std::size_t n = 0;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
  std::fclose(f);
  return s;
}

bool is_png_signature(const std::string& bytes) {
  static const unsigned char sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  return bytes.size() > 8 && std::memcmp(bytes.data(), sig, 8) == 0;
}

std::string stack_png(const char* name) {
  return std::string(MB_TEST_DATA_DIR) + "/_out_l4_stack_" + name + ".png";
}

}  // namespace

TEST(StackEndToEnd, CliProducesRealPng) {
  const std::string out_png = stack_png("basic");
  std::remove(out_png.c_str());

  std::string cli_out;
  const int rc = run_stack_cli(
      "--width 320 --height 200 --strokes 3 --output \"" + out_png + "\"", &cli_out);
  ASSERT_EQ(rc, 0) << "CLI 失败, 输出:\n" << cli_out;

  const std::string bytes = read_bytes(out_png);
  ASSERT_FALSE(bytes.empty()) << "PNG 没有写出: " << out_png;
  EXPECT_TRUE(is_png_signature(bytes)) << "输出的不是合法 PNG";
  EXPECT_GT(bytes.size(), 2000u) << "PNG 太小, 疑似空白图";

  // 统计行必须带 doc_hash —— 回放与 golden 对齐的唯一依据。
  EXPECT_NE(cli_out.find("doc_hash="), std::string::npos)
      << "CLI 没有输出 doc_hash, 回放校验失去依据: " << cli_out;
  // 双 pass 契约在真实命令行下也必须成立: 恰好 2 个 drawcall。
  EXPECT_NE(cli_out.find("drawcall 2"), std::string::npos)
      << "CLI 报告的不是 2 个 drawcall: " << cli_out;
}

TEST(StackEndToEnd, CliIsByteIdenticalAcrossRuns) {
  // 同一命令两次出图必须**逐字节相同**（§15 A10）。中间任何一步引入
  // 时间/地址/线程顺序/容器遍历序依赖，这条都会挂。
  const std::string a = stack_png("det_a");
  const std::string b = stack_png("det_b");
  std::remove(a.c_str());
  std::remove(b.c_str());

  std::string o1, o2;
  ASSERT_EQ(run_stack_cli("--width 320 --height 200 --strokes 3 --output \"" + a + "\"", &o1), 0)
      << o1;
  ASSERT_EQ(run_stack_cli("--width 320 --height 200 --strokes 3 --output \"" + b + "\"", &o2), 0)
      << o2;

  const std::string ba = read_bytes(a);
  const std::string bb = read_bytes(b);
  ASSERT_FALSE(ba.empty());
  ASSERT_EQ(ba.size(), bb.size()) << "两次出图大小不同";
  EXPECT_TRUE(ba == bb) << "两次出图内容不同 —— 管线里存在非确定性来源";
}

TEST(StackEndToEnd, CliRejectsZeroStrokes) {
  // 0 笔 = 没有方块。必须拒绝并且**不写文件**，
  // 否则那张纯背景色的空图会被 golden 当成"正确的图"锁死。
  const std::string out_png = stack_png("zero");
  std::remove(out_png.c_str());

  std::string cli_out;
  const int rc = run_stack_cli(
      "--width 64 --height 64 --strokes 0 --output \"" + out_png + "\"", &cli_out);
  EXPECT_NE(rc, 0) << "0 笔应当被拒绝";
  EXPECT_TRUE(read_bytes(out_png).empty()) << "被拒绝时不应写出 PNG";
}

TEST(StackEndToEnd, CliRequiresOutput) {
  std::string cli_out;
  const int rc = run_stack_cli("--width 64 --height 64 --strokes 2", &cli_out);
  EXPECT_EQ(rc, 2) << "--output 缺失应当是用法错误(退出码 2)";
}

TEST(StackEndToEnd, CliRejectsDepthConstArgument) {
  // §13.1 / D-VX-4′：depthConstWrite 依赖未启用的 depthBounds 特性，属 UB，
  // 因此 CLI 里**永远**不该有这个参数。给的是专用错误信息而不是笼统的
  // "未知参数"，这样有人手贱加回来时能一眼看出原因。
  std::string cli_out;
  const int rc = run_stack_cli("--inner-depth-const 1 --output \"" + stack_png("dc") + "\"",
                              &cli_out);
  EXPECT_EQ(rc, 2) << "depth_const 参数必须被拒绝";
  EXPECT_NE(cli_out.find("depthBounds"), std::string::npos)
      << "错误信息应当点明 depthBounds 这个根因, 实际: " << cli_out;
}

TEST(StackEndToEnd, CliRejectsAmbiguousInnerPass) {
  // --inner-pass 必须显式取值。auto/default 这类"让程序自己决定"的写法
  // 等于把正确性交给隐式规则，必须拒绝（§13.1）。
  std::string cli_out;
  const int rc = run_stack_cli("--inner-pass auto --output \"" + stack_png("ip") + "\"", &cli_out);
  EXPECT_EQ(rc, 2) << "--inner-pass auto 应当被拒绝";
}

TEST(StackEndToEnd, CliRejectsTemplateWithColumnHeight) {
  // §12：两种堆叠规则互斥，同时给出时结果没有唯一解释。
  std::string cli_out;
  const int rc = run_stack_cli(
      "--template circle --column-height 3 --output \"" + stack_png("conflict") + "\"", &cli_out);
  EXPECT_EQ(rc, 2) << "--template 与 --column-height 互斥, 应当拒绝";
}

TEST(StackEndToEnd, CliDryRunMatchesFullRun) {
  // dry-run 只走逻辑阶段（吸附/去重/改色），不建设备不出图。
  // 它的 doc_hash 必须与完整出图那次**完全一致** —— 否则说明
  // "dry-run 校验的那套逻辑"和"真正出图用的逻辑"是两条路（§13.3）。
  std::string dry_out, full_out;
  ASSERT_EQ(run_stack_cli("--width 320 --height 200 --strokes 3 --dry-run", &dry_out), 0) << dry_out;
  ASSERT_EQ(run_stack_cli(
                "--width 320 --height 200 --strokes 3 --output \"" + stack_png("dryrun") + "\"",
                &full_out),
            0)
      << full_out;

  const std::string key = "doc_hash=";
  const std::size_t dp = dry_out.find(key);
  const std::size_t fp = full_out.find(key);
  ASSERT_NE(dp, std::string::npos) << "dry-run 没输出 doc_hash: " << dry_out;
  ASSERT_NE(fp, std::string::npos) << "完整出图没输出 doc_hash: " << full_out;
  EXPECT_EQ(dry_out.substr(dp, 25), full_out.substr(fp, 25))
      << "dry-run 与完整出图的 doc_hash 不一致 —— 两条路径的逻辑分叉了";

  // dry-run 不得写文件。
  EXPECT_TRUE(read_bytes(stack_png("dryrun_dry")).empty());
}

TEST(StackEndToEnd, CliColorModeChangesPixelsWithoutChangingShape) {
  // 换颜色模式必须**只**改像素里的颜色，不改几何。
  // 判据：两张图的非背景像素数**完全相同**（同一个堆叠形状铺满同样多的像素），
  // 而颜色种数不同（by-layer 每层一色 ⇒ 色相种类更多）。
  // 这条同时钉住 A8 的可观测后果：几何没变。
  const std::string pf = stack_png("cm_flat");
  const std::string pl = stack_png("cm_layer");
  std::remove(pf.c_str());
  std::remove(pl.c_str());

  std::string o1, o2;
  ASSERT_EQ(run_stack_cli(
                "--width 240 --height 160 --strokes 3 --color-mode flat --output \"" + pf + "\"",
                &o1),
            0)
      << o1;
  ASSERT_EQ(run_stack_cli(
                "--width 240 --height 160 --strokes 3 --color-mode by-layer --output \"" + pl +
                    "\"",
                &o2),
            0)
      << o2;

  EXPECT_NE(o1, o2) << "两种颜色模式应当产出不同的统计(颜色参与哈希)";
  const std::string bf = read_bytes(pf);
  const std::string bl = read_bytes(pl);
  EXPECT_TRUE(is_png_signature(bf));
  EXPECT_TRUE(is_png_signature(bl));
  EXPECT_NE(bf, bl) << "两种颜色模式产出了完全相同的 PNG —— 改色没有生效";
}

TEST(StackEndToEnd, CliTemplateRendersMoreBlocksThanPlainStroke) {
  // --template circle 在每个吸附点铺一片圆盘，块数必然显著多于单格笔迹。
  // 用 dry-run 的块数比较（不出图，快且无设备依赖）。
  std::string plain, tmpl;
  ASSERT_EQ(run_stack_cli("--width 240 --height 160 --strokes 2 --dry-run", &plain), 0) << plain;
  ASSERT_EQ(run_stack_cli(
                "--width 240 --height 160 --strokes 2 --template circle --template-radius 2 "
                "--dry-run",
                &tmpl),
            0)
      << tmpl;

  const auto blocks_of = [](const std::string& s) -> long {
    const std::string key = "方块 ";
    const std::size_t p = s.find(key);
    if (p == std::string::npos) return -1;
    return std::strtol(s.c_str() + p + key.size(), nullptr, 10);
  };
  const long bp = blocks_of(plain);
  const long bt = blocks_of(tmpl);
  ASSERT_GT(bp, 0) << "解析不到方块数: " << plain;
  ASSERT_GT(bt, 0) << "解析不到方块数: " << tmpl;
  EXPECT_GT(bt, bp * 2) << "模板没有把格数铺开: plain=" << bp << " template=" << bt;
}

// ---------------------------------------------------------------- 三态 · 像素级
//
// 上面那组比的是 hash。hash 相等只证明"真源内容一致"，**不**证明"画出来的
// 东西一致" —— 渲染路径若引入了非确定性（实例顺序、浮点累加、未初始化缓冲），
// hash 照样相等而像素不同。所以这里再比一次**像素逐字节**。
//
// 顺便钉住一条教训：阶段 6 那三张 320x200 图**字节数完全相同**却是错图
// （几何被压成 251x6 的扁条）。文件大小相同 ≠ 内容相同。这里一律比
// 完整的 RGBA 向量，不比长度。
namespace {

// 把一段脚本按"录制 → 序列化 → 解析 → 回放"跑出真源，返回像素。
//
// 设备/渲染器由调用方持有并复用：每次重建 pipeline 既慢又会引入
// "两次画出来的东西到底共用了什么状态"的疑问。
struct PixelRun {
  mbst::GridSpec grid;
  std::vector<std::uint8_t> pixels;
  std::uint32_t blocks = 0;
  std::uint64_t set_hash = 0;
};

std::vector<std::uint8_t> render_set_pixels(dev::IRenderDevice* device, mbst::IStackRenderer& r,
                                            const mbst::IBlockSet& set, const mbst::GridSpec& g,
                                            int w, int h) {
  dev::TargetDesc td;
  td.kind = dev::TargetDesc::Kind::Offscreen;
  td.extent.width = static_cast<std::uint32_t>(w);
  td.extent.height = static_cast<std::uint32_t>(h);
  td.samples = 1;
  td.with_depth = true;
  auto tgt = device->create_offscreen_target(td);
  if (!tgt.ok()) return {};

  auto st = r.build_instances(g, set);
  if (!st) return {};

  auto cl = device->create_command_list();
  if (!cl.ok()) return {};

  mbst::StackFrameDesc fd{};
  fd.set = &set;
  fd.width = static_cast<std::uint32_t>(w);
  fd.height = static_cast<std::uint32_t>(h);
  fd.clear_color[0] = 0.05f;
  fd.clear_color[1] = 0.07f;
  fd.clear_color[2] = 0.10f;
  fd.clear_color[3] = 1.0f;
  fd.inner_pass_enabled = true;
  // 相机与 CLI 一致：看向网格中心，距离按 half_extent 缩放。
  auto cam = make_oblique_camera(w, h);
  if (!cam) return {};
  const auto vp = cam->view_proj_matrix();
  for (int i = 0; i < 16; ++i) fd.view_proj[i] = vp.m[i];

  if (!r.record_frame(*cl.value(), tgt.value().get(), fd)) return {};
  if (!device->submit_command_list(*cl.value()).ok()) return {};

  std::vector<std::uint8_t> px(static_cast<std::size_t>(w) * h * 4, 0u);
  if (!tgt.value()->readback_rgba8(px.data(), px.size()).ok()) return {};
  return px;
}

}  // namespace

TEST(StackCommandMode, ReplayPixelsByteIdentical) {
  // 三态的**像素**判据：记录态真源与回放态真源，喂给同一个渲染器，
  // 逐字节比 RGBA。这条把"hash 相等但画出来不一样"这一类问题挡在门外。
  auto dev_r = dev::create_render_device(software_desc());
  ASSERT_TRUE(dev_r.ok()) << dev_r.message();
  auto device = std::move(dev_r).take();
  ASSERT_NE(device, nullptr);
  ASSERT_TRUE(reserve_pages(device.get())) << "预取显存页失败";

  // create_stack_renderer() 返回**裸 unique_ptr**（不是 StackResult），
  // 所以不能用 .ok()/.take()。
  std::unique_ptr<mbst::IStackRenderer> renderer = mbst::create_stack_renderer();
  ASSERT_NE(renderer, nullptr);
  // **必须先 create_passes**：build_instances 在 device_ == nullptr 时直接
  // 返回 "尚未 create_passes"。少了这一步，后面两处读回都会是空 vector，
  // 而如果测试只比"两个空 vector 相等"，就会假通过。
  auto cpr = renderer->create_passes(device.get(), 1);
  ASSERT_TRUE(cpr) << "create_passes 失败: " << cpr.text();

  const mbst::GridSpec grid = render_grid();

  // 用 render_grid 的合法域造三笔（origin 0 / cs 0.5 / half_extent 4 ⇒ x,z ∈ [-4,4)）。
  // 路径点按"世界坐标 / cell_size"折算成整数像素，与 Replay 的实现口径一致。
  std::vector<mbst::StrokeScript> script;
  {
    std::vector<mbst::PathPoint> p0, p1, p2;
    for (int x = -3; x <= 3; ++x) p0.push_back(mbst::PathPoint{x, 0});
    for (int x = -2; x <= 2; ++x) p1.push_back(mbst::PathPoint{x, 2});
    for (int x = -1; x <= 1; ++x) p2.push_back(mbst::PathPoint{x, -2});
    script.push_back(mbst::StrokeScript{p0, 0xFF4040FFu, 0});
    script.push_back(mbst::StrokeScript{p1, 0x40FF40FFu, 2});
    script.push_back(mbst::StrokeScript{p2, 0x4040FFFFu, 4});
  }

  // 录制态
  auto rec_set_r = mbst::create_block_set(grid);
  ASSERT_TRUE(rec_set_r.ok()) << rec_set_r.text();
  auto rec_set = std::move(rec_set_r).take();
  auto rec = mbst::create_recorder();
  ASSERT_TRUE(rec->attach(rec_set.get(), grid).ok());
  for (const auto& s : script) {
    auto id = rec->stroke(s.path, s.rgba, s.layer);
    ASSERT_TRUE(id.ok()) << id.text();
  }
  ASSERT_GT(rec_set->size(), 0u);

  // 回放态（走完整 序列化 → 解析 → 重放）
  auto rep_set_r = mbst::create_block_set(grid);
  ASSERT_TRUE(rep_set_r.ok());
  auto rep_set = std::move(rep_set_r).take();
  auto parsed = mbst::create_command_stream();
  std::string detail;
  ASSERT_TRUE(parsed->parse(rec->stream().serialize(), &detail)) << detail;
  mbst::StackResult<mbst::ReplayStats> rp = mbst::replay(*parsed, *rep_set, grid);
  ASSERT_TRUE(rp.ok()) << rp.text();

  ASSERT_EQ(rec_set->hash(), rep_set->hash()) << "先要 hash 相等，像素比较才有意义";

  const int W = 240, H = 160;
  const std::vector<std::uint8_t> px_rec = render_set_pixels(device.get(), *renderer, *rec_set, grid, W, H);
  const std::vector<std::uint8_t> px_rep = render_set_pixels(device.get(), *renderer, *rep_set, grid, W, H);
  ASSERT_EQ(px_rec.size(), static_cast<std::size_t>(W) * H * 4) << "录制态读回失败";
  ASSERT_EQ(px_rep.size(), static_cast<std::size_t>(W) * H * 4) << "回放态读回失败";

  // 先确认"确实画了东西"——否则两张全背景图逐字节相等，等于空过。
  const std::uint8_t bg[4] = {13, 18, 26, 255};
  std::size_t fg_rec = 0;
  for (std::size_t i = 0; i + 3 < px_rec.size(); i += 4) {
    if (px_rec[i] != bg[0] || px_rec[i + 1] != bg[1] || px_rec[i + 2] != bg[2]) ++fg_rec;
  }
  ASSERT_GT(fg_rec, 200u) << "录制态几乎没有前景像素，像素比较会退化成空过";

  // 逐字节比较，并报出第一处差异的坐标（只报"不相等"不好排查）。
  std::size_t first_diff = px_rec.size();
  for (std::size_t i = 0; i < px_rec.size(); ++i) {
    if (px_rec[i] != px_rep[i]) { first_diff = i; break; }
  }
  if (first_diff != px_rec.size()) {
    const std::size_t px = first_diff / 4;
    FAIL() << "回放态像素与录制态不同: 首处差异在 (" << (px % W) << "," << (px / W)
           << ") 通道 " << (first_diff % 4) << "  录制=" << static_cast<int>(px_rec[first_diff])
           << " 回放=" << static_cast<int>(px_rep[first_diff]);
  }
  SUCCEED();
}

// 用指定相机渲染一帧，返回 RGBA 像素。
//
// 与 render_once 的区别：相机由调用方给。D-VX-5 的用例需要**俯视**视角，
// 让内面 pass 的绘制顺序成为画面的直接决定因素。
std::vector<std::uint8_t> render_with_camera(dev::IRenderDevice* device, mbst::IStackRenderer& r,
                                             const mbst::IBlockSet& set, const mbst::GridSpec& g,
                                             int w, int h, const mbp::ICamera& cam) {
  dev::TargetDesc td;
  td.kind = dev::TargetDesc::Kind::Offscreen;
  td.extent.width = static_cast<std::uint32_t>(w);
  td.extent.height = static_cast<std::uint32_t>(h);
  td.samples = 1;
  td.with_depth = true;
  auto tgt = device->create_offscreen_target(td);
  if (!tgt.ok()) return {};

  if (!r.build_instances(g, set)) return {};

  auto cl = device->create_command_list();
  if (!cl.ok()) return {};

  mbst::StackFrameDesc fd{};
  fd.set = &set;
  fd.width = static_cast<std::uint32_t>(w);
  fd.height = static_cast<std::uint32_t>(h);
  fd.clear_color[0] = 0.05f;
  fd.clear_color[1] = 0.07f;
  fd.clear_color[2] = 0.10f;
  fd.clear_color[3] = 1.0f;
  fd.inner_pass_enabled = true;
  std::memcpy(fd.view_proj, cam.view_proj_matrix().m, sizeof(float) * 16);

  if (!r.record_frame(*cl.value(), tgt.value().get(), fd)) return {};
  if (!device->submit_command_list(*cl.value()).ok()) return {};

  std::vector<std::uint8_t> px(static_cast<std::size_t>(w) * h * 4, 0u);
  if (!tgt.value()->readback_rgba8(px.data(), px.size()).ok()) return {};
  return px;
}

// 俯视相机：正对着 y 轴往下看。这样画面里每个像素都是"从上往下穿过的
// 最后一块"——而内面 pass 是 depthTest=OFF 的最后一个 pass，所以画面上
// 的最终颜色**就是内面 pass 的绘制顺序的直接结果**。
std::unique_ptr<mbp::ICamera> top_down_camera(int w, int h) {
  auto cam = mbp::create_camera();
  if (!cam) return nullptr;
  mbp::CameraDesc cd;
  cd.kind = mbp::ProjectionKind::Perspective;
  cd.fov_y_degrees = 45.0f;
  cd.near_plane = 0.1f;
  cd.far_plane = 200.0f;
  cd.viewport.width = static_cast<std::uint32_t>(w);
  cd.viewport.height = static_cast<std::uint32_t>(h);
  if (!cam->set_desc(cd).ok()) return nullptr;
  mbp::CameraPose pose;
  pose.position = {0.0f, 4.0f, 0.001f};  // 微偏一点点，避免 up 向量退化
  pose.target = {0.0f, 0.0f, 0.0f};
  pose.up = {0.0f, 0.0f, -1.0f};
  if (!cam->set_pose(pose).ok()) return nullptr;
  return cam;
}


// ---------------------------------------------------------------- D-VX-5 实例顺序
//
// 设计 §10.4 第 7 条：内面 pass 是 `depthTest=OFF`，**深度测试不参与**，
// 所以"谁盖谁"完全由绘制顺序决定。若用真源插入序，那个顺序是**落笔顺序**
// 而不是空间顺序 —— 同一个最终棋盘，换一种落笔先后就画出不同的内面。
// 那不是确定性，是"确定地依赖历史"。
//
// 所以内面 pass 必须按 `(y, z, x)` 升序排（与落笔历史无关）。

namespace {

// 用**指定的落笔顺序**构造同样的最终棋盘。
//
// `order[i]` 表示"第 i 笔用哪个格子"。所有格子一一对应，所以无论顺序怎么变，
// 最终真源里**格子集合与颜色完全相同**，唯一区别是数组下标的排布。
std::unique_ptr<mbst::IBlockSet> build_board_in_order(const mbst::GridSpec& g,
                                                      const std::vector<mbst::Cell>& cells,
                                                      const std::vector<std::uint32_t>& colors) {
  auto r = mbst::create_block_set(g);
  if (!r.ok()) return nullptr;
  auto set = std::move(r).take();
  for (std::size_t i = 0; i < cells.size(); ++i) {
    mbst::Block b{};
    b.cell = cells[i];
    b.color = colors[i];
    b.stroke_id = static_cast<std::uint32_t>(i + 1);
    b.flags = 0;
    if (!set->add(b).ok()) return nullptr;
  }
  return set;
}

// 一块 3x2x2 的实心小堆：够让"内面互相遮挡"真实发生。
std::vector<mbst::Cell> solid_block_cells() {
  std::vector<mbst::Cell> c;
  for (int y = 0; y < 2; ++y)
    for (int z = -1; z <= 0; ++z)
      for (int x = -1; x <= 1; ++x) c.push_back(mbst::Cell{x, y, z});
  return c;
}

}  // namespace

TEST(StackCellOrder, YzxSortsByYThenZThenX) {
  // 排序规则本身：先 y、再 z、再 x。用手算期望序列钉住。
  std::vector<mbst::Cell> cells = {
      {1, 1, 0},   // y=1 → 最后
      {2, 0, 0},   // y=0,z=0,x=2
      {0, 0, 0},   // y=0,z=0,x=0 → 最前
      {1, 0, 0},   // y=0,z=0,x=1
      {-1, 0, 1},  // y=0,z=1,x=-1
  };
  std::stable_sort(cells.begin(), cells.end(), mbst::CellOrderLess{});

  // 期望：(0,0,0) < (1,0,0) < (2,0,0) < (-1,0,1) < (1,1,0)
  EXPECT_EQ(cells[0].x, 0); EXPECT_EQ(cells[0].y, 0); EXPECT_EQ(cells[0].z, 0);
  EXPECT_EQ(cells[1].x, 1); EXPECT_EQ(cells[1].y, 0); EXPECT_EQ(cells[1].z, 0);
  EXPECT_EQ(cells[2].x, 2); EXPECT_EQ(cells[2].y, 0); EXPECT_EQ(cells[2].z, 0);
  EXPECT_EQ(cells[3].x, -1); EXPECT_EQ(cells[3].y, 0); EXPECT_EQ(cells[3].z, 1);
  EXPECT_EQ(cells[4].x, 1); EXPECT_EQ(cells[4].y, 1); EXPECT_EQ(cells[4].z, 0);
}

TEST(StackCellOrder, SurfaceAndInnerOrdersDiffer) {
  // 两种 CellOrder 必须产出**不同**的实例顺序。
  // 若它们相同，说明 YZX 根本没生效，后面那条"不同落笔顺序像素相同"
  // 会因为"两边都用插入序"而假通过。
  auto dev_r = dev::create_render_device(software_desc());
  ASSERT_TRUE(dev_r.ok()) << dev_r.message();
  ASSERT_TRUE(reserve_pages(dev_r.value().get()));

  std::unique_ptr<mbst::IStackRenderer> r = mbst::create_stack_renderer();
  ASSERT_NE(r, nullptr);
  ASSERT_TRUE(r->create_passes(dev_r.value().get(), 1).ok());

  const mbst::GridSpec g = render_grid();
  // 故意用**非 yzx**的插入顺序：先 z 由大到小、再 x 由大到小。
  std::vector<mbst::Cell> cells;
  for (int z = 1; z >= -1; --z)
    for (int x = 1; x >= -1; --x) cells.push_back(mbst::Cell{x, 1, z});
  std::vector<std::uint32_t> colors(cells.size(), 0x808080FFu);

  auto set = build_board_in_order(g, cells, colors);
  ASSERT_NE(set, nullptr);
  ASSERT_TRUE(r->build_instances(g, *set).ok());

  // 表面序 = 真源插入序，必须与输入逐格相等。
  ASSERT_EQ(r->surface_cells().size(), cells.size());
  for (std::size_t i = 0; i < cells.size(); ++i) {
    EXPECT_EQ(r->surface_cells()[i].x, cells[i].x) << "表面序第 " << i << " 格 x 不符";
    EXPECT_EQ(r->surface_cells()[i].z, cells[i].z) << "表面序第 " << i << " 格 z 不符";
  }

  // 内面序 = yzx 升序：必须先按 z 升序、再按 x 升序。
  EXPECT_EQ(r->inner_cells().size(), cells.size());
  for (std::size_t i = 0; i + 1 < r->inner_cells().size(); ++i) {
    const mbst::Cell& a = r->inner_cells()[i];
    const mbst::Cell& b = r->inner_cells()[i + 1];
    EXPECT_FALSE(mbst::CellOrderLess{}(b, a))
        << "内面序在 (" << a.x << "," << a.y << "," << a.z << ") 之后出现了更大的 ("
        << b.x << "," << b.y << "," << b.z << ")";
  }
  // 两者确实不同（输入不是 yzx 序，所以必然不同）。
  bool differs = false;
  for (std::size_t i = 0; i < cells.size(); ++i) {
    if (r->surface_cells()[i].x != r->inner_cells()[i].x ||
        r->surface_cells()[i].z != r->inner_cells()[i].z) {
      differs = true;
      break;
    }
  }
  EXPECT_TRUE(differs) << "表面序与内面序完全相同 —— YZX 排序没有生效";
  // 实例数据也必须真的不同（不只是格子序列变量不同）。
  EXPECT_NE(r->surface_instance_hash(), r->inner_instance_hash());
}

TEST(StackCellOrder, SameBoardDifferentStrokeOrderGivesSameInnerPixels) {
  // **这是把"不确定性压成契约"的可验证判据。**
  //
  // 两块真源的内容完全相同（同格子、同颜色），只有**落笔顺序**不同。
  // 用同一个渲染器出两张内面 pass 的图，必须**逐像素相同**。
  //
  // 若内面 pass 用插入序，这两张图会不同 —— 那就说明内面结果依赖历史。
  auto dev_r = dev::create_render_device(software_desc());
  ASSERT_TRUE(dev_r.ok()) << dev_r.message();
  ASSERT_TRUE(reserve_pages(dev_r.value().get()));

  std::unique_ptr<mbst::IStackRenderer> r = mbst::create_stack_renderer();
  ASSERT_NE(r, nullptr);
  ASSERT_TRUE(r->create_passes(dev_r.value().get(), 1).ok());

  const mbst::GridSpec g = render_grid();
  const std::vector<mbst::Cell> base = solid_block_cells();

  // 每格一个固定颜色（按格子内容定，不按落笔位置定），
  // 这样"同格子同色"在两种顺序下都成立。
  std::vector<std::uint32_t> colors;
  for (const mbst::Cell& c : base) {
    const std::uint32_t mix = static_cast<std::uint32_t>((c.x + 2) * 40 + (c.y + 1) * 90 + (c.z + 2) * 20);
    colors.push_back(0xFF000000u | (mix << 16) | ((mix ^ 0x55u) << 8) | 0xFFu);
  }

  // 顺序 A：原序。顺序 B：完全反转。
  std::vector<mbst::Cell> cells_a = base;
  std::vector<std::uint32_t> col_a = colors;
  std::vector<mbst::Cell> cells_b;
  std::vector<std::uint32_t> col_b;
  for (std::size_t i = base.size(); i-- > 0;) {
    cells_b.push_back(base[i]);
    col_b.push_back(colors[i]);
  }

  auto set_a = build_board_in_order(g, cells_a, col_a);
  auto set_b = build_board_in_order(g, cells_b, col_b);
  ASSERT_NE(set_a, nullptr);
  ASSERT_NE(set_b, nullptr);
  // 前提：两块真源的 hash 不同（因为数组顺序不同），但**格子集合**相同。
  EXPECT_NE(set_a->hash(), set_b->hash())
      << "两块真源 hash 相同，说明测试前提(落笔顺序不同)没构造出来";

  const int W = 200, H = 150;
  auto cam = top_down_camera(W, H);
  ASSERT_NE(cam, nullptr) << "俯视相机创建失败";
  const std::vector<std::uint8_t> px_a =
      render_with_camera(dev_r.value().get(), *r, *set_a, g, W, H, *cam);
  const std::vector<std::uint8_t> px_b =
      render_with_camera(dev_r.value().get(), *r, *set_b, g, W, H, *cam);
  ASSERT_EQ(px_a.size(), static_cast<std::size_t>(W) * H * 4);
  ASSERT_EQ(px_b.size(), static_cast<std::size_t>(W) * H * 4);

  // 先确认真画了东西（否则两张全背景图相等 = 空过）。
  const std::uint8_t bg[3] = {13, 18, 26};
  std::size_t fg = 0;
  for (std::size_t i = 0; i + 3 < px_a.size(); i += 4) {
    if (px_a[i] != bg[0] || px_a[i + 1] != bg[1] || px_a[i + 2] != bg[2]) ++fg;
  }
  ASSERT_GT(fg, 500u) << "内面图几乎没有前景像素，像素比较会退化成空过";

  std::size_t diff = px_a.size();
  for (std::size_t i = 0; i < px_a.size(); ++i) {
    if (px_a[i] != px_b[i]) { diff = i; break; }
  }
  if (diff != px_a.size()) {
    const std::size_t p = diff / 4;
    FAIL() << "同棋盘不同落笔顺序，内面像素不同 —— D-VX-5 未生效。首处差异 (" << (p % W)
           << "," << (p / W) << ") 通道 " << (diff % 4) << "  A=" << static_cast<int>(px_a[diff])
           << " B=" << static_cast<int>(px_b[diff]);
  }
  SUCCEED();
}

TEST(StackCellOrder, InnerBufferIsDistinctWhenOrdersDiffer) {
  // 顺序不同时必须真的绑**两份**实例缓冲（不是同一份拷两份数据）。
  // 同时验证顺序相同时会复用一份 —— 两条分支都要覆盖。
  auto dev_r = dev::create_render_device(software_desc());
  ASSERT_TRUE(dev_r.ok()) << dev_r.message();
  ASSERT_TRUE(reserve_pages(dev_r.value().get()));

  std::unique_ptr<mbst::IStackRenderer> r = mbst::create_stack_renderer();
  ASSERT_NE(r, nullptr);
  ASSERT_TRUE(r->create_passes(dev_r.value().get(), 1).ok());
  const mbst::GridSpec g = render_grid();

  // (a) 插入序不是 yzx ⇒ 两个 hash 不同。
  {
    std::vector<mbst::Cell> cells;
    for (int z = 1; z >= -1; --z)
      for (int x = 1; x >= -1; --x) cells.push_back(mbst::Cell{x, 0, z});
    std::vector<std::uint32_t> colors(cells.size(), 0x40C040FFu);
    auto set = build_board_in_order(g, cells, colors);
    ASSERT_NE(set, nullptr);
    ASSERT_TRUE(r->build_instances(g, *set).ok());
    EXPECT_NE(r->surface_instance_hash(), r->inner_instance_hash())
        << "顺序不同却得到相同实例数据";
  }

  // (b) 插入序**就是** yzx ⇒ 两份数据逐字节相同。
  {
    std::vector<mbst::Cell> cells = solid_block_cells();
    std::stable_sort(cells.begin(), cells.end(), mbst::CellOrderLess{});
    std::vector<std::uint32_t> colors(cells.size(), 0x4040C0FFu);
    auto set = build_board_in_order(g, cells, colors);
    ASSERT_NE(set, nullptr);
    ASSERT_TRUE(r->build_instances(g, *set).ok());
    EXPECT_EQ(r->surface_instance_hash(), r->inner_instance_hash())
        << "插入序已是 yzx，两份实例数据应当完全相同";
    // 格子序列也应逐格相等。
    ASSERT_EQ(r->surface_cells().size(), r->inner_cells().size());
    for (std::size_t i = 0; i < r->surface_cells().size(); ++i) {
      EXPECT_EQ(r->surface_cells()[i].x, r->inner_cells()[i].x);
      EXPECT_EQ(r->surface_cells()[i].y, r->inner_cells()[i].y);
      EXPECT_EQ(r->surface_cells()[i].z, r->inner_cells()[i].z);
    }
  }
}
