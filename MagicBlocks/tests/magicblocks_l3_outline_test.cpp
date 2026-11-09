// MagicBlocks L3 —— 轮廓挤出（spec §13 M4 第 18 项）
//
// `from_outline` 把 2D 轮廓（带孔）挤出成闭合网格。
// 它是"画笔描边 / 二维形状实体化"的底座，也是 L4 里
// `IShapeTemplate` 唯一需要的几何后端。
//
// 这一组测试的立场：**挤出最容易在细节上错，而且错了看不出来**。
//   - 盖面绕序反了 → 从上面看不见，从下面才看得见（渲染出来像缺了一块）
//   - 侧面法线朝内 → 光照全黑
//   - 孔的绕序没被识别 → 孔被填实
//   - 退化轮廓没被挡住 → 产出零面积三角形，进 GPU 后 z-fighting
// 所以这里逐条验证拓扑与法线，而不只是"非空"。

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "magicblocks/primitives/mesh_factory.h"
#include "magicblocks/primitives/types.h"

namespace {

namespace mbp = magicblocks::primitives;

mbp::Vec2 v2(float x, float y) { return mbp::Vec2{x, y}; }

// 轴对齐正方形轮廓（逆时针）。
std::vector<mbp::Vec2> square(float half) {
  return {v2(-half, -half), v2(half, -half), v2(half, half), v2(-half, half)};
}

// 顺时针正方形 —— 用于造孔。
std::vector<mbp::Vec2> square_cw(float half) {
  return {v2(-half, -half), v2(-half, half), v2(half, half), v2(half, -half)};
}

// 统计三角形里有多少个是退化（零面积）的。
int count_degenerate(const mbp::MeshData& m) {
  int n = 0;
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
    if (cr.x * cr.x + cr.y * cr.y + cr.z * cr.z < 1e-12f) ++n;
  }
  return n;
}

// 三角形绕序与顶点法线是否一致（点积 > 0）。
// 不一致意味着"看到的正面被判成背面"，光照和剔除都会错。
int count_winding_mismatch(const mbp::MeshData& m) {
  int n = 0;
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
    const mbp::Vec3 nrm{a.normal[0], a.normal[1], a.normal[2]};
    if (cr.x * nrm.x + cr.y * nrm.y + cr.z * nrm.z <= 0.0f) ++n;
  }
  return n;
}

// 网格在 Z 方向的范围。
void z_range(const mbp::MeshData& m, float* lo, float* hi) {
  *lo = 1e30f;
  *hi = -1e30f;
  for (const mbp::Vertex& v : m.vertices) {
    if (v.position[2] < *lo) *lo = v.position[2];
    if (v.position[2] > *hi) *hi = v.position[2];
  }
}

mbp::OutlineSet one_square(float half) {
  mbp::OutlineSet s;
  s.outer.push_back(square(half));
  return s;
}

}  // namespace

// ------------------------------------------------------------------

TEST(FromOutline, SimpleSquareProducesSidesAndTwoCaps) {
  mbp::ExtrudeParams p;
  p.thickness = 0.5f;
  const mbp::MeshData m = mbp::from_outline(one_square(1.0f), p);

  ASSERT_FALSE(m.vertices.empty()) << "正方形挤出不该是空网格";
  ASSERT_FALSE(m.indices.empty());
  EXPECT_EQ(m.indices.size() % 3, 0u) << "索引必须是完整三角形";

  // 厚度确实被用上了
  float lo = 0.0f;
  float hi = 0.0f;
  z_range(m, &lo, &hi);
  EXPECT_NEAR(lo, 0.0f, 1e-5f) << "z_base 默认 0，最低点应在 0";
  EXPECT_NEAR(hi, 0.5f, 1e-5f) << "最高点应在 thickness 处";

  EXPECT_EQ(count_degenerate(m), 0) << "不该有零面积三角形";
  EXPECT_EQ(count_winding_mismatch(m), 0) << "绕序与法线必须一致";
}

TEST(FromOutline, SquareWithHoleKeepsHoleOpen) {
  // 带孔轮廓：孔的位置不能有盖面三角形覆盖。
  // 这是"孔被填实"这类错误的唯一可靠判据 —— 光看三角形数看不出来。
  mbp::OutlineSet s;
  s.outer.push_back(square(2.0f));
  s.holes.push_back(square_cw(0.8f));  // 孔，顺时针

  mbp::ExtrudeParams p;
  p.thickness = 0.3f;
  const mbp::MeshData m = mbp::from_outline(s, p);
  ASSERT_FALSE(m.indices.empty());

  // 检查每个盖面三角形（法线沿 ±Z）的质心：都不该落在孔内部。
  // 允许一定容差，因为三角形会贴着孔的边界。
  int inside_hole = 0;
  for (std::size_t t = 0; t + 2 < m.indices.size(); t += 3) {
    const mbp::Vertex& a = m.vertices[m.indices[t]];
    if (std::fabs(a.normal[2]) < 0.9f) continue;  // 只看盖面
    const mbp::Vertex& b = m.vertices[m.indices[t + 1]];
    const mbp::Vertex& c = m.vertices[m.indices[t + 2]];
    const float cx = (a.position[0] + b.position[0] + c.position[0]) / 3.0f;
    const float cy = (a.position[1] + b.position[1] + c.position[1]) / 3.0f;
    // 孔是 |x|<0.8 且 |y|<0.8 的正方形。用 0.75 做内缩，避开边界三角形。
    if (std::fabs(cx) < 0.75f && std::fabs(cy) < 0.75f) ++inside_hole;
  }
  EXPECT_EQ(inside_hole, 0) << "孔的内部不该有任何盖面三角形（孔被填实了）";
}

TEST(FromOutline, OverlappingOutlinesAreMergedNotDoubled) {
  // 两笔重叠的轮廓：非零环绕规则下，重叠区仍是实心，
  // 不会因为重复绘制而出现双层盖面（z-fighting 的根源）。
  mbp::OutlineSet s;
  s.outer.push_back(square(1.0f));
  // 第二个正方形向右偏移 0.5，与第一个重叠一半
  std::vector<mbp::Vec2> shifted;
  for (const mbp::Vec2& v : square(1.0f)) shifted.push_back(v2(v.x + 0.5f, v.y));
  s.outer.push_back(shifted);

  mbp::ExtrudeParams p;
  p.thickness = 0.2f;
  const mbp::MeshData m = mbp::from_outline(s, p);
  ASSERT_FALSE(m.indices.empty());

  // 每个 XY 位置的上盖面只应被覆盖一次。
  //
  // 判据的选择：直接数三角形个数会依赖具体三角化算法（libtess2 对
  // 这个并集形状给出 6 个三角形/盖面），换实现就变。所以这里用
  // **几何判据**：把盖面三角形按质心累加有符号面积，
  // 若重叠区被重复填充，总面积会大于并集面积。
  float cap_area = 0.0f;
  for (std::size_t t = 0; t + 2 < m.indices.size(); t += 3) {
    const mbp::Vertex& a = m.vertices[m.indices[t]];
    if (std::fabs(a.normal[2]) < 0.9f) continue;  // 只看盖面
    if (a.normal[2] < 0.0f) continue;            // 只算上盖，避免上下盖抵消
    const mbp::Vertex& b = m.vertices[m.indices[t + 1]];
    const mbp::Vertex& c = m.vertices[m.indices[t + 2]];
    const float abx = b.position[0] - a.position[0];
    const float aby = b.position[1] - a.position[1];
    const float acx = c.position[0] - a.position[0];
    const float acy = c.position[1] - a.position[1];
    cap_area += std::fabs(abx * acy - aby * acx) * 0.5f;
  }

  // 几何核算：每个正方形边长 2、Y 范围都是 [-1, 1]；
  // 第二个向右平移 0.5，所以 X 范围 [-1,1] 与 [0.5,1.5]。
  // 两者**等高**，并集正好是矩形 x∈[-1,1.5]、y∈[-1,1]，
  // 面积 = 2.5 × 2 = 5。
  //
  // （我最初算成"4 + 4 − 重叠 = 7"，把重叠区当成 0.5×2 = 1 且
  //   忘了并集是个矩形 —— 这个 5 更像是"漏填"造成的假象，
  //   但实际它就是并集的精确面积。用错期望值会逼着去"修"
  //   一个本来就正确的实现。）
  const float union_area = 5.0f;
  EXPECT_NEAR(cap_area, union_area, 0.05f)
      << "上盖面积 " << cap_area << " 与并集面积 " << union_area
      << " 不符：偏大说明重叠区被重复填充（z-fighting），偏小说明有区域漏填";
  EXPECT_EQ(count_degenerate(m), 0);
}

TEST(FromOutline, ZeroThicknessProducesSingleLayerNoSides) {
  mbp::ExtrudeParams p;
  p.thickness = 0.0f;
  const mbp::MeshData m = mbp::from_outline(one_square(1.0f), p);

  ASSERT_FALSE(m.indices.empty());
  float lo = 0.0f;
  float hi = 0.0f;
  z_range(m, &lo, &hi);
  EXPECT_NEAR(lo, hi, 1e-6f) << "thickness=0 时所有顶点应在同一个 Z 上";
  EXPECT_EQ(count_degenerate(m), 0);
  EXPECT_EQ(count_winding_mismatch(m), 0);
}

TEST(FromOutline, CapFlagsControlWhichLayersExist) {
  mbp::OutlineSet s = one_square(1.0f);

  mbp::ExtrudeParams both;
  both.thickness = 0.4f;
  both.cap_bottom = true;
  both.cap_top = true;
  const std::size_t n_both = mbp::from_outline(s, both).indices.size();

  mbp::ExtrudeParams top_only = both;
  top_only.cap_bottom = false;
  const mbp::MeshData m_top = mbp::from_outline(s, top_only);
  EXPECT_LT(m_top.indices.size(), n_both) << "关掉下盖后三角形应减少";
  EXPECT_GT(m_top.indices.size(), 0u) << "关掉下盖不该产出空网格";

  // 关掉下盖时，最低层的顶点应该只属于侧面（法线不含纯 -Z）
  float lo = 0.0f;
  float hi = 0.0f;
  z_range(m_top, &lo, &hi);
  int bottom_caps = 0;
  for (const mbp::Vertex& v : m_top.vertices) {
    if (std::fabs(v.position[2] - lo) < 1e-5f && v.normal[2] < -0.9f) ++bottom_caps;
  }
  EXPECT_EQ(bottom_caps, 0) << "cap_bottom=false 时不该有朝下的盖面顶点";
}

TEST(FromOutline, EmptyOutlineYieldsEmptyMesh) {
  mbp::OutlineSet empty;
  mbp::ExtrudeParams p;
  const mbp::MeshData m = mbp::from_outline(empty, p);
  EXPECT_TRUE(m.vertices.empty());
  EXPECT_TRUE(m.indices.empty());
}

TEST(FromOutline, DegenerateRingIsRejected) {
  // 三个点共线 —— 零面积轮廓。必须返回空网格，
  // 而不是产出一堆零面积三角形（后者会在 GPU 上造成 z-fighting
  // 且不报任何错，是典型的"看起来能跑但画面闪烁"来源）。
  mbp::OutlineSet s;
  s.outer.push_back({v2(0.0f, 0.0f), v2(0.0f, 1.0f), v2(0.0f, 2.0f)});
  mbp::ExtrudeParams p;
  const mbp::MeshData m = mbp::from_outline(s, p);
  EXPECT_TRUE(m.indices.empty()) << "共线轮廓应返回空网格";
}

TEST(FromOutline, DuplicatePointsAreCleaned) {
  // 相邻重复点会让侧面产生零长度四边形 → 零面积三角形。
  mbp::OutlineSet s;
  s.outer.push_back({v2(-1.0f, -1.0f), v2(-1.0f, -1.0f), v2(1.0f, -1.0f), v2(1.0f, 1.0f),
                     v2(-1.0f, 1.0f)});
  mbp::ExtrudeParams p;
  p.thickness = 0.2f;
  const mbp::MeshData m = mbp::from_outline(s, p);
  ASSERT_FALSE(m.indices.empty());
  EXPECT_EQ(count_degenerate(m), 0) << "重复点应在挤出前被清理掉";
}

TEST(FromOutline, NormalsAreUnitLength) {
  mbp::OutlineSet s;
  s.outer.push_back(square(1.5f));
  s.holes.push_back(square_cw(0.5f));
  mbp::ExtrudeParams p;
  p.thickness = 0.7f;
  const mbp::MeshData m = mbp::from_outline(s, p);
  ASSERT_FALSE(m.vertices.empty());

  for (const mbp::Vertex& v : m.vertices) {
    const float len = std::sqrt(v.normal[0] * v.normal[0] + v.normal[1] * v.normal[1] +
                                v.normal[2] * v.normal[2]);
    EXPECT_NEAR(len, 1.0f, 1e-3f) << "法线必须是单位长度，否则光照强度不稳定";
  }
}

TEST(FromOutline, ZBaseOffsetsGeometry) {
  mbp::ExtrudeParams p;
  p.thickness = 0.5f;
  p.z_base = 2.0f;
  const mbp::MeshData m = mbp::from_outline(one_square(1.0f), p);
  ASSERT_FALSE(m.vertices.empty());
  float lo = 0.0f;
  float hi = 0.0f;
  z_range(m, &lo, &hi);
  EXPECT_NEAR(lo, 2.0f, 1e-5f);
  EXPECT_NEAR(hi, 2.5f, 1e-5f);
}

TEST(FromOutline, ResultIsDeterministic) {
  // 同样的输入两次必须产出逐字节相同的网格。
  // 这是命令录制回放的前提 —— 不确定的几何会让回放画面偏移。
  mbp::OutlineSet s;
  s.outer.push_back(square(2.0f));
  s.outer.push_back(square(1.2f));
  s.holes.push_back(square_cw(0.4f));
  mbp::ExtrudeParams p;
  p.thickness = 0.35f;

  const mbp::MeshData a = mbp::from_outline(s, p);
  const mbp::MeshData b = mbp::from_outline(s, p);
  ASSERT_EQ(a.vertices.size(), b.vertices.size());
  ASSERT_EQ(a.indices.size(), b.indices.size());
  for (std::size_t i = 0; i < a.vertices.size(); ++i) {
    EXPECT_EQ(a.vertices[i].position[0], b.vertices[i].position[0]) << "顶点 " << i;
    EXPECT_EQ(a.vertices[i].position[1], b.vertices[i].position[1]) << "顶点 " << i;
    EXPECT_EQ(a.vertices[i].position[2], b.vertices[i].position[2]) << "顶点 " << i;
  }
  for (std::size_t i = 0; i < a.indices.size(); ++i) {
    EXPECT_EQ(a.indices[i], b.indices[i]) << "索引 " << i;
  }
}

TEST(FromOutline, IndicesAreInRange) {
  mbp::OutlineSet s;
  s.outer.push_back(square(1.0f));
  s.holes.push_back(square_cw(0.3f));
  mbp::ExtrudeParams p;
  p.thickness = 0.25f;
  const mbp::MeshData m = mbp::from_outline(s, p);
  ASSERT_FALSE(m.indices.empty());
  for (std::uint32_t idx : m.indices) {
    EXPECT_LT(idx, m.vertices.size()) << "索引越界会让 GPU 读到垃圾顶点";
  }
}
