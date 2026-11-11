// MagicBlocks L3 —— 双表示（R-SK-12 / A13）在 L3 层的直接用例。
//
// 设计 §6.3.1(c) 把 `PathStyle` / `RegionOp` / `OutlineSet.source_style` 定为
// **L3 primitives** 的能力。本文件只验证 L3 本体：
//   - Ribbon 走笔宽 offset，Region 不走；
//   - Region 产出闭合多边形，且首尾不重复；
//   - 未实现的分支（Difference/Intersect/Xor、未闭合 Region）**显式报错**，
//     且**不退回**另一种解读（D8）。
//
// L4（F-SK）侧的"source_style 与输入一致"由 magicblocks_l4_sketch_test.cpp 验证 ——
// 那一条断言的是 L4 输出，不是 L3 契约。
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "mb_internal/magicblocks/primitives/mesh_factory.h"
#include "mb_internal/magicblocks/primitives/types.h"

namespace {

namespace mbp = magicblocks::primitives;

// 有符号面积（鞋带），>0 = 逆时针。
float signed_area(const std::vector<mbp::Vec2>& ring) {
  float a = 0.0f;
  for (std::size_t i = 0; i < ring.size(); ++i) {
    const mbp::Vec2& p = ring[i];
    const mbp::Vec2& q = ring[(i + 1) % ring.size()];
    a += p.x * q.y - q.x * p.y;
  }
  return 0.5f * a;
}

// 闭合方框（首尾**重复**存了起点，与真实采样一致）。
std::vector<mbp::Vec2> closed_square(float half) {
  return {{-half, -half}, {half, -half}, {half, half}, {-half, half}, {-half, -half}};
}

float ring_max_abs(const std::vector<mbp::Vec2>& ring) {
  float m = 0.0f;
  for (const auto& p : ring) m = std::max(m, std::max(std::fabs(p.x), std::fabs(p.y)));
  return m;
}

}  // namespace

// ---------------------------------------------------------------- 能力表

TEST(RegionOpSupport, OnlyUnionIsSupported) {
  EXPECT_TRUE(mbp::supports_region_op(mbp::RegionOp::Union));
  EXPECT_FALSE(mbp::supports_region_op(mbp::RegionOp::Difference));
  EXPECT_FALSE(mbp::supports_region_op(mbp::RegionOp::Intersect));
  EXPECT_FALSE(mbp::supports_region_op(mbp::RegionOp::Xor));
}

// ---------------------------------------------------------------- Ribbon vs Region

TEST(PathStyle, RibbonOffsetsByWidthRegionDoesNot) {
  const auto pts = closed_square(0.4f);
  const float width = 0.1f;

  const mbp::OutlineResult ribbon =
      mbp::make_outline(pts, mbp::PathStyle::Ribbon, width, /*round_cap=*/true);
  const mbp::OutlineResult region =
      mbp::make_outline(pts, mbp::PathStyle::Region, width, /*round_cap=*/true);

  ASSERT_TRUE(ribbon.ok()) << ribbon.error;
  ASSERT_TRUE(region.ok()) << region.error;

  ASSERT_EQ(ribbon.outline.outer.size(), 1u);
  ASSERT_EQ(region.outline.outer.size(), 1u);

  // Region 恰好是方框本身：外接半径 0.4，面积 ~0.64（4 点方框）。
  EXPECT_NEAR(ring_max_abs(region.outline.outer[0]), 0.4f, 1e-5f);
  EXPECT_NEAR(std::fabs(signed_area(region.outline.outer[0])), 0.64f, 1e-5f);

  // Ribbon 因笔宽外扩：外接半径 ~0.45，且点数远多于 4（圆头端帽 + 逐顶点法线）。
  EXPECT_NEAR(ring_max_abs(ribbon.outline.outer[0]), 0.4f + width * 0.5f, 1e-4f);
  EXPECT_GT(ribbon.outline.outer[0].size(), 4u);
  // Ribbon 是**空心框**：包围盒更大，但围起来的面积只是四根 0.1 宽的条。
  // ⚠️ 不要用"面积比 Region 大"来断言 —— 4*0.8*0.1 ≈ 0.32 < 0.64，会反着来。
  // （这正是 Region 存在意义的一半：它填的是内部，Ribbon 只画边框。）
  EXPECT_LT(std::fabs(signed_area(ribbon.outline.outer[0])),
            std::fabs(signed_area(region.outline.outer[0])))
      << "Ribbon 只是空心边框，包围面积必须小于 Region 的填充面积";
}

TEST(PathStyle, RegionOutputIsClosedAndDeduplicated) {
  auto pts = closed_square(0.3f);  // 5 个点，首尾相同
  const mbp::OutlineResult r = mbp::make_outline(pts, mbp::PathStyle::Region, 0.05f, true);
  ASSERT_TRUE(r.ok()) << r.error;
  ASSERT_EQ(r.outline.outer.size(), 1u);
  const auto& ring = r.outline.outer[0];

  // 首尾**不**重复存同一个点（from_outline 隐式闭合）。
  const float dx = ring.front().x - ring.back().x;
  const float dy = ring.front().y - ring.back().y;
  EXPECT_GT(std::sqrt(dx * dx + dy * dy), 1e-6f) << "Region 环首尾重复,会让 tess 看到零长度边";
  EXPECT_EQ(ring.size(), 4u);

  // 逆时针（from_outline 靠它判断外法线方向）。
  EXPECT_GT(signed_area(ring), 0.0f);
}

TEST(PathStyle, SourceStyleAndSourceHashAreRecorded) {
  const auto pts = closed_square(0.3f);
  const mbp::OutlineResult a = mbp::make_outline(pts, mbp::PathStyle::Region, 0.05f, true);
  const mbp::OutlineResult b = mbp::make_outline(pts, mbp::PathStyle::Ribbon, 0.05f, true);
  ASSERT_TRUE(a.ok());
  ASSERT_TRUE(b.ok());

  // A13 第 3 条在 L3 的形态：来源标注与请求的解读一致。
  EXPECT_EQ(a.outline.source_style, mbp::PathStyle::Region);
  EXPECT_EQ(b.outline.source_style, mbp::PathStyle::Ribbon);

  // source_hash 必须区分"同一组点的两种解读"与"不同输入"。
  EXPECT_NE(a.outline.source_hash, b.outline.source_hash);
  EXPECT_NE(a.outline.source_hash, 0u);
  EXPECT_NE(a.outline.source_hash,
            mbp::make_outline(closed_square(0.31f), mbp::PathStyle::Region, 0.05f, true)
                .outline.source_hash);
}

TEST(PathStyle, SameInputGivesSameSourceHash) {
  const auto pts = closed_square(0.3f);
  for (int i = 0; i < 5; ++i) {
    const mbp::OutlineResult r = mbp::make_outline(pts, mbp::PathStyle::Region, 0.05f, true);
    ASSERT_TRUE(r.ok());
    if (i == 0) continue;
    EXPECT_EQ(r.outline.source_hash,
              mbp::make_outline(pts, mbp::PathStyle::Region, 0.05f, true).outline.source_hash);
  }
}

// ---------------------------------------------------------------- 失败必响

TEST(PathStyle, OpenRegionIsExplicitErrorNotAutoClosed) {
  // 不闭合的折线：**不自动连首尾**。自动闭合会把用户的 "C" 变成 "O"。
  std::vector<mbp::Vec2> open = {{-0.4f, -0.4f}, {0.4f, -0.4f}, {0.4f, 0.4f}};
  const mbp::OutlineResult r = mbp::make_outline(open, mbp::PathStyle::Region, 0.05f, true);
  EXPECT_FALSE(r.ok());
  EXPECT_STRNE(r.error, "");
  EXPECT_TRUE(r.outline.empty()) << "失败时不得产出任何轮廓";
  // 特别重要：**不能**退回 Ribbon。
  EXPECT_EQ(r.outline.source_style, mbp::PathStyle::Region);
}

TEST(PathStyle, TooFewPointsIsExplicitError) {
  std::vector<mbp::Vec2> two = {{0.0f, 0.0f}, {1.0f, 0.0f}};
  // Region 至少要 3 个点才能围出一块面。
  EXPECT_FALSE(mbp::make_outline(two, mbp::PathStyle::Region, 0.05f, true).ok());
  // Ribbon 两点就是一段线段 —— **这是合法的**（胶囊条带），不是退化。
  EXPECT_TRUE(mbp::make_outline(two, mbp::PathStyle::Ribbon, 0.05f, true).ok());
  // 单点两向都不成立：没有方向的折线不成几何。
  std::vector<mbp::Vec2> one = {{0.0f, 0.0f}};
  EXPECT_FALSE(mbp::make_outline(one, mbp::PathStyle::Ribbon, 0.05f, true).ok());
  EXPECT_FALSE(mbp::make_outline(one, mbp::PathStyle::Region, 0.05f, true).ok());
  std::vector<mbp::Vec2> none;
  EXPECT_FALSE(mbp::make_outline(none, mbp::PathStyle::Ribbon, 0.05f, true).ok());
}

TEST(PathStyle, RibbonRejectsNonPositiveWidth) {
  std::vector<mbp::Vec2> line = {{0.0f, 0.0f}, {1.0f, 0.0f}};
  EXPECT_FALSE(mbp::make_outline(line, mbp::PathStyle::Ribbon, 0.0f, true).ok());
  EXPECT_FALSE(mbp::make_outline(line, mbp::PathStyle::Ribbon, -0.1f, true).ok());
}

// ---------------------------------------------------------------- 挤出

TEST(PathStyle, RegionExtrudesToWatertightSolid) {
  const mbp::OutlineResult r =
      mbp::make_outline(closed_square(0.3f), mbp::PathStyle::Region, 0.05f, true);
  ASSERT_TRUE(r.ok()) << r.error;

  mbp::ExtrudeParams ep;
  ep.thickness = 0.1f;
  const mbp::MeshData m = mbp::from_outline(r.outline, ep);
  ASSERT_FALSE(m.vertices.empty());
  ASSERT_FALSE(m.indices.empty());
  // 方框区域的外接盒就是 0.6 x 0.6（没有被笔宽撑大）。
  EXPECT_NEAR(m.bounds.max.x - m.bounds.min.x, 0.6f, 1e-4f);
  EXPECT_NEAR(m.bounds.max.y - m.bounds.min.y, 0.6f, 1e-4f);
}
