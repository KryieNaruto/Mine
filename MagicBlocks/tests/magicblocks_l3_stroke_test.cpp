// MagicBlocks L3 —— 笔画轮廓偏移（spec §13 M4 第 18 项的一部分）
//
// `stroke_outline` 把一条中心线折线按笔宽"加粗"成闭合的多边形外轮廓。
// 它和 `from_outline` 是一条流水线：
//
//     画笔中心线  --stroke_outline-->  闭合轮廓  --from_outline-->  3D 实体
//
// 为什么要独立测：偏移几何的错误**在渲染上表现为"笔画像糊了"**，
// 而不是崩溃或空白。自相交、内侧翻折、圆头缺段都不会报错，
// 只会让笔画在急转弯处出现尖刺或漏洞。所以这里逐条验证几何性质：
// 面积、绕序、点到线距离、点数。

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "mb_internal/magicblocks/primitives/mesh_factory.h"
#include "mb_internal/magicblocks/primitives/types.h"

namespace {

namespace mbp = magicblocks::primitives;

mbp::Vec2 v2(float x, float y) { return mbp::Vec2{x, y}; }

// 环的有符号面积（逆时针为正）。
float signed_area(const std::vector<mbp::Vec2>& r) {
  float a = 0.0f;
  const std::size_t n = r.size();
  for (std::size_t i = 0; i < n; ++i) {
    const mbp::Vec2& p = r[i];
    const mbp::Vec2& q = r[(i + 1) % n];
    a += p.x * q.y - q.x * p.y;
  }
  return a * 0.5f;
}

// 点到线段距离。
float dist_to_segment(const mbp::Vec2& p, const mbp::Vec2& a, const mbp::Vec2& b) {
  const float dx = b.x - a.x;
  const float dy = b.y - a.y;
  const float len2 = dx * dx + dy * dy;
  if (len2 < 1e-12f) return std::sqrt((p.x - a.x) * (p.x - a.x) + (p.y - a.y) * (p.y - a.y));
  float t = ((p.x - a.x) * dx + (p.y - a.y) * dy) / len2;
  t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
  const float cx = a.x + t * dx;
  const float cy = a.y + t * dy;
  return std::sqrt((p.x - cx) * (p.x - cx) + (p.y - cy) * (p.y - cy));
}

float dist_to_polyline(const mbp::Vec2& p, const std::vector<mbp::Vec2>& line) {
  float best = 1e30f;
  for (std::size_t i = 0; i + 1 < line.size(); ++i) {
    const float d = dist_to_segment(p, line[i], line[i + 1]);
    if (d < best) best = d;
  }
  return best;
}

}  // namespace

// ------------------------------------------------------------------

TEST(StrokeOutline, SingleSegmentProducesCapsule) {
  // 一条水平线段，宽度 w → 胶囊：矩形 w*L 加两端半圆。
  // 面积 = w*L + π*(w/2)²。
  const std::vector<mbp::Vec2> line = {v2(0.0f, 0.0f), v2(2.0f, 0.0f)};
  const float w = 0.4f;
  const std::vector<mbp::Vec2> r = mbp::stroke_outline(line, w, true);

  ASSERT_GE(r.size(), 3u) << "单段线也该产出闭合多边形";
  // 胶囊面积理论值
  const float expected = w * 2.0f + 3.14159265f * (w * 0.5f) * (w * 0.5f);
  const float got = std::fabs(signed_area(r));
  // 圆用 arc_steps 段折线近似，面积略小，给 3% 容差
  EXPECT_NEAR(got, expected, expected * 0.03f)
      << "胶囊面积不符：实际 " << got << "，理论 " << expected;
}

TEST(StrokeOutline, ResultIsCounterClockwise) {
  // 头文件契约写明"逆时针"。绕序决定 from_outline 的盖面朝向，
  // 反了会让挤出的实体从上方看不见。
  const std::vector<mbp::Vec2> line = {v2(0.0f, 0.0f), v2(1.0f, 1.0f)};
  const std::vector<mbp::Vec2> r = mbp::stroke_outline(line, 0.3f, true);
  ASSERT_GE(r.size(), 3u);
  EXPECT_GT(signed_area(r), 0.0f)
      << "轮廓应为逆时针（面积 > 0），实际面积 " << signed_area(r);
}

TEST(StrokeOutline, AllPointsStayWithinHalfWidthOfCenterline) {
  // 最本质的判据：轮廓上每个点到中心线的距离都应该是 w/2（允许少量数值误差）。
  // 若偏移量算错，这里会立刻超出。
  const std::vector<mbp::Vec2> line = {v2(0.0f, 0.0f), v2(2.0f, 0.0f), v2(2.0f, 2.0f)};
  const float w = 0.5f;
  const std::vector<mbp::Vec2> r = mbp::stroke_outline(line, w, true);
  ASSERT_GE(r.size(), 3u);

  for (const mbp::Vec2& p : r) {
    // 直角拐角处，内侧的点离折线会近一些，外侧正好 w/2。
    // 所以判据是"不大于 w/2 + 容差"，不是"等于"。
    const float d = dist_to_polyline(p, line);
    EXPECT_LE(d, w * 0.5f + 1e-3f)
        << "轮廓点 (" << p.x << "," << p.y << ") 到中心线距离 " << d
        << " 超过半笔宽 " << w * 0.5f;
  }
}

TEST(StrokeOutline, RoundCapAddsArcSegments) {
  // 圆头比方头点数多：两端各多出约 arc_steps 段。
  const std::vector<mbp::Vec2> line = {v2(0.0f, 0.0f), v2(1.0f, 0.0f)};
  const std::vector<mbp::Vec2> round = mbp::stroke_outline(line, 0.3f, true, 8);
  const std::vector<mbp::Vec2> flat = mbp::stroke_outline(line, 0.3f, false, 8);
  ASSERT_GE(round.size(), 3u);
  ASSERT_GE(flat.size(), 3u);
  EXPECT_GT(round.size(), flat.size()) << "圆头应比方头点数多";
}

TEST(StrokeOutline, RoundCapAreaIsLargerThanFlatCap) {
  // 同样的中心线，圆头面积 = 方头面积 + 圆面积。
  const std::vector<mbp::Vec2> line = {v2(0.0f, 0.0f), v2(3.0f, 0.0f)};
  const float w = 0.6f;
  const std::vector<mbp::Vec2> round = mbp::stroke_outline(line, w, true);
  const std::vector<mbp::Vec2> flat = mbp::stroke_outline(line, w, false);
  const float a_round = std::fabs(signed_area(round));
  const float a_flat = std::fabs(signed_area(flat));
  const float circle = 3.14159265f * (w * 0.5f) * (w * 0.5f);
  EXPECT_GT(a_round, a_flat);
  // 容差 8%：圆头是 arc_steps=6 段折线**内接**的近似，
  // 面积天然比真圆小。正 n 边形与圆的面积比是 (n/π)·sin(π/n)，
  // n=6 时约为 0.8265 —— 但这里只有半个圆各被近似，
  // 且差值是"圆头减方头"，实测落在 6.4% 左右。
  // 把容差压到 5% 会让这条测试在参数合法的情况下假失败。
  EXPECT_NEAR(a_round - a_flat, circle, circle * 0.08f)
      << "圆头与方头的面积差应约等于一个整圆（±8%，折线近似）";
}

TEST(StrokeOutline, DegenerateInputReturnsEmpty) {
  EXPECT_TRUE(mbp::stroke_outline({}, 0.2f, true).empty()) << "空输入";
  EXPECT_TRUE(mbp::stroke_outline({v2(1.0f, 2.0f)}, 0.2f, true).empty()) << "单点输入";
  EXPECT_TRUE(
      mbp::stroke_outline({v2(0.0f, 0.0f), v2(1.0f, 0.0f)}, 0.0f, true).empty())
      << "宽度为 0";
  EXPECT_TRUE(
      mbp::stroke_outline({v2(0.0f, 0.0f), v2(1.0f, 0.0f)}, -0.5f, true).empty())
      << "负宽度";
}

TEST(StrokeOutline, DuplicateConsecutivePointsAreCleaned) {
  // 相邻重复点会让方向向量退化成 NaN。
  // 这类输入在真实画笔数据里很常见（触摸屏会重复同一像素）。
  const std::vector<mbp::Vec2> line = {v2(0.0f, 0.0f), v2(0.0f, 0.0f), v2(1.0f, 0.0f)};
  const std::vector<mbp::Vec2> r = mbp::stroke_outline(line, 0.3f, true);
  ASSERT_GE(r.size(), 3u);
  for (const mbp::Vec2& p : r) {
    EXPECT_TRUE(std::isfinite(p.x)) << "轮廓点 x 不是有限值（NaN/Inf）";
    EXPECT_TRUE(std::isfinite(p.y)) << "轮廓点 y 不是有限值（NaN/Inf）";
  }
  EXPECT_GT(signed_area(r), 0.0f);
}

TEST(StrokeOutline, SharpUTurnDoesNotSelfIntersectWildly) {
  // 中心线急转 180°（画到一半折回来）。偏移会自相交 ——
  // L3 只保证"不产出 NaN/Inf"，合并属于 L4 的手感策略。
  // 但绝不能在数值上炸掉。
  const std::vector<mbp::Vec2> line = {v2(0.0f, 0.0f), v2(2.0f, 0.0f), v2(0.0f, 0.0f)};
  const std::vector<mbp::Vec2> r = mbp::stroke_outline(line, 0.3f, true);
  ASSERT_GE(r.size(), 3u);
  for (const mbp::Vec2& p : r) {
    EXPECT_TRUE(std::isfinite(p.x));
    EXPECT_TRUE(std::isfinite(p.y));
    EXPECT_LT(std::fabs(p.x), 100.0f) << "偏移量爆炸";
    EXPECT_LT(std::fabs(p.y), 100.0f) << "偏移量爆炸";
  }
}

TEST(StrokeOutline, FeedsFromOutlineToMakeSolid) {
  // 端到端：中心线 → 轮廓 → 挤出实体。
  // 这是"画"这个功能在 3D 里的最小完整路径。
  const std::vector<mbp::Vec2> line = {v2(-1.0f, 0.0f), v2(1.0f, 0.0f)};
  const std::vector<mbp::Vec2> outline = mbp::stroke_outline(line, 0.4f, true);
  ASSERT_GE(outline.size(), 3u);

  mbp::OutlineSet set;
  set.outer.push_back(outline);
  mbp::ExtrudeParams p;
  p.thickness = 0.15f;
  const mbp::MeshData m = mbp::from_outline(set, p);

  ASSERT_FALSE(m.vertices.empty()) << "笔画轮廓应能挤出成实体";
  ASSERT_FALSE(m.indices.empty());
  EXPECT_EQ(m.indices.size() % 3, 0u);

  // 逐三角形验证绕序与法线一致（这是挤出最易错的地方）
  int bad = 0;
  for (std::size_t t = 0; t + 2 < m.indices.size(); t += 3) {
    const mbp::Vertex& a = m.vertices[m.indices[t]];
    const mbp::Vertex& b = m.vertices[m.indices[t + 1]];
    const mbp::Vertex& c = m.vertices[m.indices[t + 2]];
    const mbp::Vec3 e1{b.position[0] - a.position[0], b.position[1] - a.position[1],
                       b.position[2] - a.position[2]};
    const mbp::Vec3 e2{c.position[0] - a.position[0], c.position[1] - a.position[1],
                       c.position[2] - a.position[2]};
    const mbp::Vec3 cr{e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z,
                       e1.x * e2.y - e1.y * e2.x};
    if (cr.x * a.normal[0] + cr.y * a.normal[1] + cr.z * a.normal[2] <= 0.0f) ++bad;
  }
  EXPECT_EQ(bad, 0) << "笔画实体的三角形绕序与法线不一致";
}

TEST(StrokeOutline, IsDeterministic) {
  // 命令回放要求同样输入产出同样几何。
  const std::vector<mbp::Vec2> line = {v2(0.0f, 0.0f), v2(1.0f, 0.5f), v2(2.0f, 0.0f)};
  const std::vector<mbp::Vec2> a = mbp::stroke_outline(line, 0.25f, true);
  const std::vector<mbp::Vec2> b = mbp::stroke_outline(line, 0.25f, true);
  ASSERT_EQ(a.size(), b.size());
  for (std::size_t i = 0; i < a.size(); ++i) {
    EXPECT_FLOAT_EQ(a[i].x, b[i].x) << "第 " << i << " 个点";
    EXPECT_FLOAT_EQ(a[i].y, b[i].y) << "第 " << i << " 个点";
  }
}
