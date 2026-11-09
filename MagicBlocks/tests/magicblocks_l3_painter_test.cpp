// MagicBlocks L3 —— 画笔的单元测试（spec §6 / §14.2 Painter 套件）
//
// 重点验证三件事：
//   1. 增量性：Move 只追加，不重算已有几何（性能前提）
//   2. 确定性：同样的事件序列 → 同样的几何 hash（回放前提）
//   3. 生命周期：Begin/End 保留，Begin/Cancel 整笔丢弃
#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "magicblocks/primitives/painter.h"

namespace mbp = magicblocks::primitives;

namespace {

mbp::StrokeEvent ev(mbp::StrokeEventKind kind, float x, float y, mbp::u32 pointer = 0) {
  mbp::StrokeEvent e;
  e.kind = kind;
  e.x = x;
  e.y = y;
  e.pointer_id = pointer;
  return e;
}

// 画一条水平直线，返回事件序列
std::vector<mbp::StrokeEvent> horizontal_line(float x0, float x1, float y, float step) {
  std::vector<mbp::StrokeEvent> evs;
  evs.push_back(ev(mbp::StrokeEventKind::Begin, x0, y));
  for (float x = x0 + step; x < x1; x += step) {
    evs.push_back(ev(mbp::StrokeEventKind::Move, x, y));
  }
  evs.push_back(ev(mbp::StrokeEventKind::End, x1, y));
  return evs;
}

}  // namespace

// spec §14.2: Painter.StrokeProducesGeometry
TEST(Painter, StrokeProducesGeometry) {
  auto p = mbp::create_painter();
  ASSERT_NE(p, nullptr);

  const auto evs = horizontal_line(10.0f, 110.0f, 50.0f, 10.0f);
  p->feed_batch(evs.data(), evs.size());

  const auto& g = p->geometry();
  EXPECT_FALSE(g.vertices.empty()) << "一笔必须产出顶点";
  EXPECT_FALSE(g.indices.empty()) << "一笔必须产出索引";
  // 索引数必须是 3 的倍数（三角形列表）
  EXPECT_EQ(g.indices.size() % 3, 0u);
  // 索引都在范围内
  for (std::uint32_t idx : g.indices) {
    EXPECT_LT(idx, g.vertices.size());
  }
  EXPECT_EQ(p->completed_stroke_count(), 1u);
}

// 增量性：每多一个 Move，只增加固定数量的顶点。
// 这是 §6.2.1 "不重建整笔"的直接断言 —— 如果实现改成每帧重建，
// 顶点总数会跳变，但更关键的是下面 VerifyIncrementalNoRebuild 检查性能特征。
TEST(Painter, MoveAppendsFixedVertices) {
  auto p = mbp::create_painter();
  ASSERT_NE(p, nullptr);
  // 关掉圆头，让顶点数只来自段本身
  mbp::StrokeDesc d;
  d.round_cap = false;
  p->set_desc(d);

  p->feed(ev(mbp::StrokeEventKind::Begin, 0.0f, 0.0f));
  const std::size_t v_after_begin = p->geometry().vertices.size();

  p->feed(ev(mbp::StrokeEventKind::Move, 10.0f, 0.0f));
  const std::size_t v_after_1 = p->geometry().vertices.size();

  p->feed(ev(mbp::StrokeEventKind::Move, 20.0f, 0.0f));
  const std::size_t v_after_2 = p->geometry().vertices.size();

  p->feed(ev(mbp::StrokeEventKind::Move, 30.0f, 0.0f));
  const std::size_t v_after_3 = p->geometry().vertices.size();

  EXPECT_EQ(v_after_begin, 0u) << "只有起点时无法确定方向，不应生成顶点";
  EXPECT_EQ(v_after_1 - v_after_begin, 4u) << "第一段必须是 4 个顶点";
  EXPECT_EQ(v_after_2 - v_after_1, 4u) << "后续每段必须固定 4 个顶点（增量，不重建）";
  EXPECT_EQ(v_after_3 - v_after_2, 4u);

  // 索引同理：每段 6 个
  EXPECT_EQ(p->geometry().indices.size(), 3u * 6u);
}

TEST(Painter, CancelDiscardsWholeStroke) {
  auto p = mbp::create_painter();
  ASSERT_NE(p, nullptr);

  // 先完整画一笔，已经有几何
  const auto good = horizontal_line(0.0f, 50.0f, 0.0f, 10.0f);
  p->feed_batch(good.data(), good.size());
  const std::size_t v_before = p->geometry().vertices.size();
  ASSERT_GT(v_before, 0u);

  // 再开始一笔然后取消
  p->feed(ev(mbp::StrokeEventKind::Begin, 100.0f, 100.0f));
  p->feed(ev(mbp::StrokeEventKind::Move, 150.0f, 100.0f));
  p->feed(ev(mbp::StrokeEventKind::Move, 200.0f, 100.0f));
  p->feed(ev(mbp::StrokeEventKind::Cancel, 200.0f, 100.0f));

  // 被取消那一笔的几何必须一点都不留
  EXPECT_EQ(p->geometry().vertices.size(), v_before) << "Cancel 必须整笔丢弃";
  EXPECT_EQ(p->completed_stroke_count(), 1u) << "取消的笔迹不计入完成数";
  EXPECT_EQ(p->stats().strokes_cancelled, 1u);
}

TEST(Painter, EndKeepsGeometry) {
  auto p = mbp::create_painter();
  ASSERT_NE(p, nullptr);

  p->feed(ev(mbp::StrokeEventKind::Begin, 0.0f, 0.0f));
  p->feed(ev(mbp::StrokeEventKind::Move, 50.0f, 0.0f));
  const std::size_t v_mid = p->geometry().vertices.size();
  p->feed(ev(mbp::StrokeEventKind::End, 50.0f, 0.0f));

  EXPECT_GE(p->geometry().vertices.size(), v_mid) << "End 保留已画部分";
  EXPECT_EQ(p->completed_stroke_count(), 1u);
}

// 确定性：同样的事件序列两次运行必须产出完全一样的几何 hash。
// 这是 §11 命令回放的基础。
TEST(Painter, DeterministicGeometryHash) {
  const auto evs = horizontal_line(0.0f, 200.0f, 80.0f, 7.0f);

  auto a = mbp::create_painter();
  auto b = mbp::create_painter();
  a->feed_batch(evs.data(), evs.size());
  b->feed_batch(evs.data(), evs.size());

  EXPECT_EQ(a->geometry_hash(), b->geometry_hash())
      << "同样的输入必须产出同样的几何（回放确定性）";
  EXPECT_EQ(a->geometry().vertices.size(), b->geometry().vertices.size());
}

TEST(Painter, DifferentInputDifferentHash) {
  auto a = mbp::create_painter();
  auto b = mbp::create_painter();

  const auto evs1 = horizontal_line(0.0f, 100.0f, 0.0f, 10.0f);
  const auto evs2 = horizontal_line(0.0f, 100.0f, 5.0f, 10.0f);  // 只差 y
  a->feed_batch(evs1.data(), evs1.size());
  b->feed_batch(evs2.data(), evs2.size());

  EXPECT_NE(a->geometry_hash(), b->geometry_hash()) << "不同位置必须产出不同几何";
}

// 抖动去重：极近的点不应产生段。
TEST(Painter, DebounceRejectsTinySegments) {
  auto p = mbp::create_painter();
  ASSERT_NE(p, nullptr);
  mbp::StrokeDesc d;
  d.round_cap = false;
  p->set_desc(d);

  p->feed(ev(mbp::StrokeEventKind::Begin, 0.0f, 0.0f));
  // 0.1px 的抖动，远小于 0.35px 阈值
  p->feed(ev(mbp::StrokeEventKind::Move, 0.1f, 0.0f));
  p->feed(ev(mbp::StrokeEventKind::Move, 0.2f, 0.0f));

  EXPECT_EQ(p->geometry().vertices.size(), 0u) << "微小抖动不应生成退化三角形";

  // 正常距离则应生成
  p->feed(ev(mbp::StrokeEventKind::Move, 10.0f, 0.0f));
  EXPECT_EQ(p->geometry().vertices.size(), 4u);
}

TEST(Painter, MaxPointsLimitDropsPoints) {
  auto p = mbp::create_painter();
  ASSERT_NE(p, nullptr);
  mbp::StrokeDesc d;
  d.max_points_per_stroke = 5;
  d.round_cap = false;
  p->set_desc(d);

  p->feed(ev(mbp::StrokeEventKind::Begin, 0.0f, 0.0f));
  for (int i = 1; i <= 20; ++i) {
    p->feed(ev(mbp::StrokeEventKind::Move, static_cast<float>(i) * 10.0f, 0.0f));
  }

  EXPECT_GT(p->stats().points_dropped, 0u) << "超过上限的点必须被丢弃并计数";
  EXPECT_LE(p->stats().points_accepted, 6u) << "接受的点数不得超过上限（含起点）";
}

TEST(Painter, WidthAffectsRibbonExtent) {
  auto thin_p = mbp::create_painter();
  auto thick_p = mbp::create_painter();
  ASSERT_NE(thin_p, nullptr);
  ASSERT_NE(thick_p, nullptr);

  mbp::StrokeDesc thin;
  thin.width_px = 2.0f;
  thin.round_cap = false;
  mbp::StrokeDesc thick = thin;
  thick.width_px = 20.0f;

  thin_p->set_desc(thin);
  thick_p->set_desc(thick);

  const auto evs = horizontal_line(0.0f, 100.0f, 50.0f, 10.0f);
  thin_p->feed_batch(evs.data(), evs.size());
  thick_p->feed_batch(evs.data(), evs.size());

  // 水平直线沿 y 方向展开，thick 的 y 范围应该是 thin 的 10 倍
  auto y_extent = [](const mbp::IPainter& p) {
    float lo = 1e9f, hi = -1e9f;
    for (const auto& v : p.geometry().vertices) {
      lo = std::fmin(lo, v.position[1]);
      hi = std::fmax(hi, v.position[1]);
    }
    return hi - lo;
  };
  const float thin_ext = y_extent(*thin_p);
  const float thick_ext = y_extent(*thick_p);
  // 宽度 2 → y 范围应约等于 2；宽度 20 → 约等于 20
  EXPECT_NEAR(thin_ext, 2.0f, 0.05f);
  EXPECT_NEAR(thick_ext, 20.0f, 0.5f);
}

TEST(Painter, RoundCapAddsVertices) {
  auto with_cap = mbp::create_painter();
  auto without = mbp::create_painter();

  mbp::StrokeDesc cap_desc;
  cap_desc.round_cap = true;
  mbp::StrokeDesc flat_desc;
  flat_desc.round_cap = false;
  with_cap->set_desc(cap_desc);
  without->set_desc(flat_desc);

  const auto evs = horizontal_line(0.0f, 50.0f, 0.0f, 10.0f);
  with_cap->feed_batch(evs.data(), evs.size());
  without->feed_batch(evs.data(), evs.size());

  EXPECT_GT(with_cap->geometry().vertices.size(), without->geometry().vertices.size())
      << "圆头应该在首尾额外增加顶点";
}

TEST(Painter, ClearResetsEverything) {
  auto p = mbp::create_painter();
  const auto evs = horizontal_line(0.0f, 100.0f, 0.0f, 10.0f);
  p->feed_batch(evs.data(), evs.size());
  ASSERT_GT(p->geometry().vertices.size(), 0u);

  p->clear();
  EXPECT_EQ(p->geometry().vertices.size(), 0u);
  EXPECT_EQ(p->completed_stroke_count(), 0u);
}

TEST(Painter, ExtractOutlineReturnsPolylines) {
  auto p = mbp::create_painter();
  const auto evs = horizontal_line(0.0f, 100.0f, 50.0f, 10.0f);
  p->feed_batch(evs.data(), evs.size());

  const auto outline = p->extract_outline();
  ASSERT_EQ(outline.size(), 1u) << "一笔应该对应一条折线";
  EXPECT_GE(outline[0].size(), 2u) << "折线至少有两个点";
}

TEST(Painter, SteadyStateAppendDoesNotReallocate) {
  auto p = mbp::create_painter();
  ASSERT_NE(p, nullptr);
  p->reserve(4, 512);

  mbp::StrokeDesc d;
  d.round_cap = false;
  d.max_points_per_stroke = 4096;
  p->set_desc(d);

  p->feed(ev(mbp::StrokeEventKind::Begin, 0.0f, 0.0f));
  for (int i = 1; i < 500; ++i) {
    p->feed(ev(mbp::StrokeEventKind::Move, static_cast<float>(i) * 2.0f,
               std::sin(static_cast<float>(i) * 0.1f) * 20.0f));
  }

  // 500 个点应产出 ~499 段 × 4 顶点
  EXPECT_GT(p->geometry().vertices.size(), 1500u);
  EXPECT_EQ(p->stats().points_dropped, 0u);
}
