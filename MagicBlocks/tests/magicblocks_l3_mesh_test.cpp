// MagicBlocks L3 —— 网格与图元的单元测试
//
// 几何测试的重点不是"跑通"，而是**拓扑正确性**：
//   顶点数/索引数是否符合公式、绕序是否一致（背面剔除会因此失效）、
//   AABB 是否与顶点一致。这些都是"看起来能跑但画面是错的"的典型来源。
#include <gtest/gtest.h>

#include <cmath>
#include <set>
#include <vector>

#include "mb_internal/magicblocks/primitives/mesh.h"
#include "mb_internal/magicblocks/primitives/mesh_factory.h"

namespace mbp = magicblocks::primitives;

TEST(Mesh, VertexLayoutHasFourContiguousLocations) {
  const auto layout = mbp::mesh_vertex_layout();
  ASSERT_EQ(layout.bindings.size(), 1u);
  EXPECT_EQ(layout.bindings[0].binding, 0u);
  EXPECT_EQ(layout.bindings[0].stride, sizeof(mbp::Vertex));
  EXPECT_FALSE(layout.bindings[0].instanced);

  ASSERT_EQ(layout.attributes.size(), 4u);
  // location 必须从 0 连续，不能跳号（L1 的硬规定）
  for (std::size_t i = 0; i < layout.attributes.size(); ++i) {
    EXPECT_EQ(layout.attributes[i].location, i) << "location 出现跳号";
    EXPECT_EQ(layout.attributes[i].binding, 0u);
  }
}

TEST(Mesh, CubeTopologyIsClosed) {
  auto f = mbp::create_mesh_factory();
  ASSERT_NE(f, nullptr);
  const mbp::MeshData cube = f->cube();

  // 6 面 × 4 顶点 = 24（每面独立顶点，硬边）
  EXPECT_EQ(cube.vertices.size(), 24u);
  // 6 面 × 2 三角形 × 3 = 36
  EXPECT_EQ(cube.indices.size(), 36u);

  // 所有索引都在范围内
  for (std::uint32_t idx : cube.indices) {
    EXPECT_LT(idx, cube.vertices.size());
  }
}

TEST(Mesh, CubeBoundsAreUnit) {
  auto f = mbp::create_mesh_factory();
  const mbp::MeshData cube = f->cube();

  const float eps = 1e-6f;
  EXPECT_NEAR(cube.bounds.min.x, -0.5f, eps);
  EXPECT_NEAR(cube.bounds.min.y, -0.5f, eps);
  EXPECT_NEAR(cube.bounds.min.z, -0.5f, eps);
  EXPECT_NEAR(cube.bounds.max.x, 0.5f, eps);
  EXPECT_NEAR(cube.bounds.max.y, 0.5f, eps);
  EXPECT_NEAR(cube.bounds.max.z, 0.5f, eps);
}

// 绕序检查：每个三角形的几何法线（叉积）必须与顶点法线同向。
// 绕序反了的话，背面剔除会让这个面消失 —— 而且只在特定视角可见，
// 属于最难 debug 的一类问题，所以必须机器验证。
TEST(Mesh, CubeTriangleWindingMatchesNormals) {
  auto f = mbp::create_mesh_factory();
  const mbp::MeshData cube = f->cube();

  ASSERT_EQ(cube.indices.size() % 3, 0u);
  for (std::size_t t = 0; t < cube.indices.size(); t += 3) {
    const mbp::Vertex& a = cube.vertices[cube.indices[t + 0]];
    const mbp::Vertex& b = cube.vertices[cube.indices[t + 1]];
    const mbp::Vertex& c = cube.vertices[cube.indices[t + 2]];

    // 边向量
    const float e1[3] = {b.position[0] - a.position[0], b.position[1] - a.position[1],
                         b.position[2] - a.position[2]};
    const float e2[3] = {c.position[0] - a.position[0], c.position[1] - a.position[1],
                         c.position[2] - a.position[2]};
    // 几何法线 = e1 × e2
    const float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                        e1[0] * e2[1] - e1[1] * e2[0]};

    const float dot = n[0] * a.normal[0] + n[1] * a.normal[1] + n[2] * a.normal[2];
    EXPECT_GT(dot, 0.0f) << "三角形 #" << (t / 3) << " 绕序与法线相反（背面剔除后会消失）";
  }
}

TEST(Mesh, BoxFacesMaskSelectsSubset) {
  auto f = mbp::create_mesh_factory();

  // 只要 +Y 一个面 → 4 顶点 / 6 索引
  const mbp::MeshData top = f->box_faces(static_cast<std::uint8_t>(mbp::BoxFace::kPosY));
  EXPECT_EQ(top.vertices.size(), 4u);
  EXPECT_EQ(top.indices.size(), 6u);

  // 空掩码 → 空网格
  const mbp::MeshData none = f->box_faces(0);
  EXPECT_TRUE(none.empty());

  // 全掩码等价于 cube()
  const mbp::MeshData all = f->box_faces(0xFF);
  EXPECT_EQ(all.vertices.size(), 24u);
}

TEST(Mesh, QuadIsFourVertsSixIndices) {
  auto f = mbp::create_mesh_factory();
  const mbp::MeshData q = f->quad();

  EXPECT_EQ(q.vertices.size(), 4u);
  EXPECT_EQ(q.indices.size(), 6u);
  // 法线应指向 +Z，uv 覆盖 0..1
  for (const auto& v : q.vertices) {
    EXPECT_NEAR(v.normal[2], 1.0f, 1e-6f);
    EXPECT_GE(v.uv[0], 0.0f);
    EXPECT_LE(v.uv[0], 1.0f);
    EXPECT_GE(v.uv[1], 0.0f);
    EXPECT_LE(v.uv[1], 1.0f);
  }
}

TEST(Mesh, GridVertexAndIndexCounts) {
  auto f = mbp::create_mesh_factory();

  const mbp::MeshData g4 = f->grid(4);
  // (n+1)² 顶点，n²×6 索引
  EXPECT_EQ(g4.vertices.size(), 25u);
  EXPECT_EQ(g4.indices.size(), 4u * 4u * 6u);
  // 法线全部朝 +Y
  for (const auto& v : g4.vertices) {
    EXPECT_NEAR(v.normal[1], 1.0f, 1e-6f);
  }

  // divisions=0 应返回空而不是崩溃
  EXPECT_TRUE(f->grid(0).empty());
}

// 一万个积木共享同一个 Cube 网格 —— 这是 §4.1 drawcall 预算成立的前提。
TEST(Mesh, CubeDataIsReusableAcrossInstances) {
  auto f = mbp::create_mesh_factory();
  const mbp::MeshData a = f->cube();
  const mbp::MeshData b = f->cube();

  // 纯函数：两次生成必须字节级一致（否则 MeshId 共享会出问题）
  ASSERT_EQ(a.vertices.size(), b.vertices.size());
  ASSERT_EQ(a.indices.size(), b.indices.size());
  EXPECT_EQ(a.indices, b.indices);
  for (std::size_t i = 0; i < a.vertices.size(); ++i) {
    EXPECT_EQ(a.vertices[i].position[0], b.vertices[i].position[0]);
    EXPECT_EQ(a.vertices[i].normal[1], b.vertices[i].normal[1]);
  }
}

// ============================================================ M4 旋转体图元
//
// 这一组测试的共同套路：**所有图元都必须过同一道绕序检查**。
// 绕序错的几何在背面剔除下会整块消失，而且只在某些视角可见 ——
// 靠肉眼看渲染结果基本抓不住，必须机器验证每个三角形。
namespace {

// 通用绕序检查：每个三角形的几何法线（叉积）必须与顶点法线同向。
// 返回第一个违规三角形编号，无违规则返回 -1。
int find_bad_winding(const mbp::MeshData& m) {
  for (std::size_t t = 0; t + 2 < m.indices.size(); t += 3) {
    const mbp::Vertex& a = m.vertices[m.indices[t + 0]];
    const mbp::Vertex& b = m.vertices[m.indices[t + 1]];
    const mbp::Vertex& c = m.vertices[m.indices[t + 2]];
    const float e1[3] = {b.position[0] - a.position[0], b.position[1] - a.position[1],
                         b.position[2] - a.position[2]};
    const float e2[3] = {c.position[0] - a.position[0], c.position[1] - a.position[1],
                         c.position[2] - a.position[2]};
    const float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                        e1[0] * e2[1] - e1[1] * e2[0]};
    // 用三角形三个顶点法线的均值做参考，避免退化三角形（叉积≈0）误报
    const float nx = (a.normal[0] + b.normal[0] + c.normal[0]) / 3.0f;
    const float ny = (a.normal[1] + b.normal[1] + c.normal[1]) / 3.0f;
    const float nz = (a.normal[2] + b.normal[2] + c.normal[2]) / 3.0f;
    const float dot = n[0] * nx + n[1] * ny + n[2] * nz;
    // 叉积模长 < 1e-9 的三角形面积视为 0（极点退化），跳过
    const float area2 = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (area2 < 1e-9f) continue;
    if (dot <= 0.0f) return static_cast<int>(t / 3);
  }
  return -1;
}

// 顶点法线全部是单位长度
void expect_normals_unit(const mbp::MeshData& m) {
  for (std::size_t i = 0; i < m.vertices.size(); ++i) {
    const mbp::Vertex& v = m.vertices[i];
    const float len = std::sqrt(v.normal[0] * v.normal[0] + v.normal[1] * v.normal[1] +
                                v.normal[2] * v.normal[2]);
    ASSERT_NEAR(len, 1.0f, 1e-4f) << "顶点 #" << i << " 法线不是单位长度";
  }
}

// 所有索引都必须在顶点范围内（越界索引 = 越界读，随机崩溃）
void expect_indices_in_range(const mbp::MeshData& m) {
  for (std::size_t i = 0; i < m.indices.size(); ++i) {
    ASSERT_LT(m.indices[i], m.vertices.size()) << "索引 #" << i << " 越界";
  }
}

}  // namespace

TEST(Mesh, CylinderTopologyAndWinding) {
  auto f = mbp::create_mesh_factory();
  const mbp::MeshData c = f->cylinder(0.5f, 2.0f, 16);

  ASSERT_FALSE(c.empty()) << "cylinder 应已实现";
  EXPECT_EQ(c.indices.size() % 3, 0u) << "索引数必须是 3 的倍数";
  expect_indices_in_range(c);
  expect_normals_unit(c);
  EXPECT_EQ(find_bad_winding(c), -1) << "有三角形绕序与法线相反";

  // 侧面 16 段 × 2 三角形 + 上下盖各 16 三角形
  const std::size_t expected_tris = 16 * 2 + 16 * 2;
  EXPECT_EQ(c.indices.size() / 3, expected_tris);

  // 包围盒：半径 .5、总高 2，中心在原点
  EXPECT_NEAR(c.bounds.min.y, -1.0f, 1e-5f);
  EXPECT_NEAR(c.bounds.max.y, +1.0f, 1e-5f);
  EXPECT_NEAR(c.bounds.max.x, 0.5f, 1e-5f);
  EXPECT_NEAR(c.bounds.min.x, -0.5f, 1e-5f);
}

TEST(Mesh, CylinderSideNormalsAreHorizontal) {
  auto f = mbp::create_mesh_factory();
  const mbp::MeshData c = f->cylinder(1.0f, 2.0f, 12);

  // 侧面的关键是**法线水平**（y 分量 0）。这区分了"圆柱"与"多面棱柱"：
  // 若把侧面法线写成了面法线（有非零 y 或变成离散的），圆柱看起来就有棱。
  int side_verts = 0;
  for (const mbp::Vertex& v : c.vertices) {
    // 只看法线几乎水平的顶点（排除端盖的 ±Y）
    if (std::fabs(v.normal[1]) < 0.01f) {
      ++side_verts;
      // 水平法线还应与位置径向一致：normal ≈ normalize(x, 0, z)
      const float r = std::sqrt(v.position[0] * v.position[0] + v.position[2] * v.position[2]);
      ASSERT_GT(r, 1e-4f);
      EXPECT_NEAR(v.normal[0], v.position[0] / r, 1e-4f) << "侧面法线必须是径向的";
      EXPECT_NEAR(v.normal[2], v.position[2] / r, 1e-4f);
    }
  }
  // 16 段 → (seg+1)*2 = 26 个侧面顶点（12 段 → 26）
  EXPECT_EQ(side_verts, 26) << "侧面顶点数不符，法线可能被平均掉了";
}

TEST(Mesh, ConeApexNormalsAreNotAllVertical) {
  auto f = mbp::create_mesh_factory();
  const mbp::MeshData c = f->cone(1.0f, 2.0f, 12);

  ASSERT_FALSE(c.empty());
  expect_indices_in_range(c);
  expect_normals_unit(c);
  EXPECT_EQ(find_bad_winding(c), -1) << "圆锥有三角形绕序与法线相反";

  // 圆锥侧面的坑：如果整个锥面共用一个法线，光照会完全不随角度变化。
  // 这里断言法线的水平分量**不同方向都存在**，即确实随经度旋转。
  bool has_pos_x = false, has_neg_x = false, has_pos_z = false, has_neg_z = false;
  for (const mbp::Vertex& v : c.vertices) {
    if (v.normal[1] > 0.99f || v.normal[1] < -0.99f) continue;  // 跳过端盖
    if (v.normal[0] > 0.5f) has_pos_x = true;
    if (v.normal[0] < -0.5f) has_neg_x = true;
    if (v.normal[2] > 0.5f) has_pos_z = true;
    if (v.normal[2] < -0.5f) has_neg_z = true;
  }
  EXPECT_TRUE(has_pos_x && has_neg_x && has_pos_z && has_neg_z)
      << "锥面法线没有随经度变化 —— 光照会是一块平板";

  // 圆锥法线应有恒定的倾角：n_up/n_rad = 2*radius/height = 1.0
  // (radius=1, height=2) → 倾角 45 度
  for (const mbp::Vertex& v : c.vertices) {
    if (std::fabs(v.normal[1]) > 0.99f) continue;
    const float horiz = std::sqrt(v.normal[0] * v.normal[0] + v.normal[2] * v.normal[2]);
    EXPECT_NEAR(v.normal[1] / horiz, 1.0f, 1e-4f) << "锥面法线倾角不对";
    break;
  }
}

TEST(Mesh, SphereNormalsEqualNormalizedPosition) {
  auto f = mbp::create_mesh_factory();
  const mbp::MeshData s = f->sphere(2.0f, 8, 16);

  ASSERT_FALSE(s.empty());
  expect_indices_in_range(s);
  expect_normals_unit(s);
  EXPECT_EQ(find_bad_winding(s), -1) << "球面有三角形绕序与法线相反";

  // 球体最干净的定义：法线 == 归一化位置。
  // 任何有偏差都说明法线是单独算的，迟早会在某些细分下走样。
  for (const mbp::Vertex& v : s.vertices) {
    const float r = std::sqrt(v.position[0] * v.position[0] +
                              v.position[1] * v.position[1] +
                              v.position[2] * v.position[2]);
    ASSERT_NEAR(r, 2.0f, 1e-4f) << "球面顶点不在半径 2 的球上";
    EXPECT_NEAR(v.normal[0], v.position[0] / r, 1e-5f);
    EXPECT_NEAR(v.normal[1], v.position[1] / r, 1e-5f);
    EXPECT_NEAR(v.normal[2], v.position[2] / r, 1e-5f);
  }

  // 包围盒应是 [-2,2]^3
  EXPECT_NEAR(s.bounds.min.x, -2.0f, 1e-5f);
  EXPECT_NEAR(s.bounds.max.x, +2.0f, 1e-5f);
  EXPECT_NEAR(s.bounds.min.y, -2.0f, 1e-5f);
  EXPECT_NEAR(s.bounds.max.y, +2.0f, 1e-5f);
}

TEST(Mesh, SpherePolesHaveNoDegenerateTriangles) {
  auto f = mbp::create_mesh_factory();
  const mbp::MeshData s = f->sphere(1.0f, 6, 12);

  // 极点是退化四边形：两个顶点位置重合。如果按四边形发索引，
  // 会产生一半面积为零的三角形，白白浪费顶点处理。
  int degenerate = 0;
  for (std::size_t t = 0; t + 2 < s.indices.size(); t += 3) {
    const mbp::Vertex& a = s.vertices[s.indices[t + 0]];
    const mbp::Vertex& b = s.vertices[s.indices[t + 1]];
    const mbp::Vertex& c = s.vertices[s.indices[t + 2]];
    const float e1[3] = {b.position[0] - a.position[0], b.position[1] - a.position[1],
                         b.position[2] - a.position[2]};
    const float e2[3] = {c.position[0] - a.position[0], c.position[1] - a.position[1],
                         c.position[2] - a.position[2]};
    const float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                        e1[0] * e2[1] - e1[1] * e2[0]};
    if (std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) < 1e-9f) ++degenerate;
  }
  EXPECT_EQ(degenerate, 0) << "球面含零面积三角形（极点没有按三角形发索引）";
}

TEST(Mesh, PrismHasFlatShadingNotSmooth) {
  auto f = mbp::create_mesh_factory();
  const mbp::MeshData p = f->prism(1.0f, 2.0f, 6);  // 六棱柱

  ASSERT_FALSE(p.empty());
  expect_indices_in_range(p);
  expect_normals_unit(p);

  // 棱柱与圆柱的根本差别：**平直着色**。
  // 每个侧面是一个独立平面，同一面内所有顶点法线完全相同。
  // 如果这里退化成平滑着色（相邻面法线被平均），棱柱看起来就是个圆角柱。
  int side_verts = 0;
  for (std::size_t i = 0; i + 3 < p.vertices.size(); i += 4) {
    const mbp::Vertex& v0 = p.vertices[i];
    if (std::fabs(v0.normal[1]) > 0.01f) continue;  // 跳过端盖
    ++side_verts;
    for (int k = 1; k < 4; ++k) {
      const mbp::Vertex& v = p.vertices[i + k];
      EXPECT_NEAR(v.normal[0], v0.normal[0], 1e-6f) << "同一侧面内法线不一致 → 用了平滑着色";
      EXPECT_NEAR(v.normal[2], v0.normal[2], 1e-6f);
    }
  }
  EXPECT_EQ(side_verts, 6) << "六棱柱应有 6 个侧面";

  // 六棱柱：侧面 6×2 + 端盖 6×2 = 24 三角形
  EXPECT_EQ(p.indices.size() / 3, 6u * 2 + 6u * 2);
}

TEST(Mesh, PrismWindingMatchesNormals) {
  auto f = mbp::create_mesh_factory();
  // 平直着色的棱柱绕序最容易写反（每面独立顶点，没有共享顶点可依赖）。
  // 三角形、四边形、八边形的棱柱都验一遍。
  for (std::uint32_t sides : {3u, 4u, 5u, 8u}) {
    const mbp::MeshData p = f->prism(1.0f, 1.0f, sides);
    ASSERT_FALSE(p.empty()) << sides << " 边棱柱为空";
    EXPECT_EQ(find_bad_winding(p), -1) << sides << " 边棱柱绕序错误";
  }
}

TEST(Mesh, TrapezoidDegeneratesCorrectly) {
  auto f = mbp::create_mesh_factory();

  // 上半径 == 下半径 → 就是圆柱。这是最容易写反的边界：
  // 法线倾斜量 n_up = (rb - rt) 应为 0。
  {
    const mbp::MeshData t = f->trapezoid(1.0f, 1.0f, 2.0f, 12);
    const mbp::MeshData c = f->cylinder(1.0f, 2.0f, 12);
    ASSERT_FALSE(t.empty());
    EXPECT_EQ(t.vertices.size(), c.vertices.size());
    EXPECT_EQ(find_bad_winding(t), -1);
    // 侧面法线必须水平
    for (const mbp::Vertex& v : t.vertices) {
      if (std::fabs(v.normal[1]) > 0.01f) {
        // 允许端盖的 ±Y
        EXPECT_NEAR(std::fabs(v.normal[1]), 1.0f, 1e-5f);
      }
    }
  }

  // 上半径 == 0 → 圆锥（退化成一点）。顶盖不应生成。
  {
    const mbp::MeshData t = f->trapezoid(1.0f, 0.0f, 2.0f, 12);
    ASSERT_FALSE(t.empty());
    EXPECT_EQ(find_bad_winding(t), -1);
    // 只能有一个端盖（底面），所以顶点里只有一组 ±Y 纯竖直法线
    int bottom_cap = 0;
    for (const mbp::Vertex& v : t.vertices) {
      if (v.normal[1] < -0.99f) ++bottom_cap;
    }
    EXPECT_GT(bottom_cap, 0) << "圆锥应有底盖";
    EXPECT_TRUE(t.bounds.max.x <= 1.0f + 1e-5f);
  }

  // 倒锥（上大下小）：法线应朝下倾斜
  {
    const mbp::MeshData t = f->trapezoid(0.5f, 1.5f, 2.0f, 12);
    ASSERT_FALSE(t.empty());
    EXPECT_EQ(find_bad_winding(t), -1) << "倒锥绕序错误";
    for (const mbp::Vertex& v : t.vertices) {
      if (std::fabs(v.normal[1]) > 0.99f) continue;  // 跳过端盖
      EXPECT_LT(v.normal[1], 0.0f) << "上大下小时外法线应朝下倾斜";
      break;
    }
  }
}

TEST(Mesh, TrapezoidNormalsTiltWithSlope) {
  auto f = mbp::create_mesh_factory();

  // 上细下粗（印章把手的形状）：法线应**朝上**倾斜。
  const float rb = 1.0f, rt = 0.5f, h = 2.0f;
  const mbp::MeshData t = f->trapezoid(rb, rt, h, 24);
  ASSERT_FALSE(t.empty());

  // 解析解：L = sqrt(h² + (rb-rt)²)，n_up = (rb-rt)/L，n_rad = h/L
  const float L = std::sqrt(h * h + (rb - rt) * (rb - rt));
  const float expect_up = (rb - rt) / L;
  const float expect_rad = h / L;

  for (const mbp::Vertex& v : t.vertices) {
    if (std::fabs(v.normal[1]) > 0.99f) continue;  // 跳过端盖
    EXPECT_NEAR(v.normal[1], expect_up, 1e-4f) << "法线竖直分量与斜边角度不符";
    const float horiz = std::sqrt(v.normal[0] * v.normal[0] + v.normal[2] * v.normal[2]);
    EXPECT_NEAR(horiz, expect_rad, 1e-4f);
    break;
  }

  // 几何包围盒：底半径大，所以 x 极值由底面决定
  EXPECT_NEAR(t.bounds.min.y, -h / 2.0f, 1e-5f);
  EXPECT_NEAR(t.bounds.max.y, +h / 2.0f, 1e-5f);
  EXPECT_NEAR(t.bounds.max.x, rb, 1e-5f);
}

TEST(Mesh, InvalidParametersReturnEmpty) {
  auto f = mbp::create_mesh_factory();

  // 非法参数必须返回空，而不是 clamp 到最小值 ——
  // 静默修正会让调用方以为自己的参数生效了。
  EXPECT_TRUE(f->cylinder(0.0f, 1.0f, 8).empty()) << "半径 0";
  EXPECT_TRUE(f->cylinder(1.0f, 0.0f, 8).empty()) << "高度 0";
  EXPECT_TRUE(f->cylinder(1.0f, 1.0f, 2).empty()) << "分段 < 3";
  EXPECT_TRUE(f->cylinder(-1.0f, 1.0f, 8).empty()) << "负半径";

  EXPECT_TRUE(f->cone(0.0f, 1.0f, 8).empty());
  EXPECT_TRUE(f->cone(1.0f, 1.0f, 2).empty());

  EXPECT_TRUE(f->sphere(0.0f, 8, 16).empty());
  EXPECT_TRUE(f->sphere(1.0f, 1, 16).empty()) << "rings < 2 构不成球";
  EXPECT_TRUE(f->sphere(1.0f, 8, 2).empty()) << "segments < 3";

  EXPECT_TRUE(f->prism(1.0f, 1.0f, 2).empty()) << "边数 < 3";

  EXPECT_TRUE(f->trapezoid(0.0f, 1.0f, 1.0f, 8).empty()) << "底半径必须 > 0";
  EXPECT_TRUE(f->trapezoid(1.0f, 1.0f, 0.0f, 8).empty()) << "高度必须 > 0";
}

TEST(Mesh, UnitHelpersMatchExplicitParameters) {
  auto f = mbp::create_mesh_factory();

  // 单位尺寸便利函数必须与显式参数完全等价，否则调用方会得到
  // 两种"单位"，共享 MeshId 的前提就不成立了。
  const mbp::MeshData cu = f->cylinder_unit(16);
  const mbp::MeshData ce = f->cylinder(0.5f, 1.0f, 16);
  ASSERT_EQ(cu.vertices.size(), ce.vertices.size());
  EXPECT_EQ(cu.indices.size(), ce.indices.size());

  const mbp::MeshData su = f->sphere_unit(8, 12);
  const mbp::MeshData se = f->sphere(0.5f, 8, 12);
  ASSERT_EQ(su.vertices.size(), se.vertices.size());

  const mbp::MeshData pu = f->prism_unit(6);
  const mbp::MeshData pe = f->prism(0.5f, 1.0f, 6);
  ASSERT_EQ(pu.vertices.size(), pe.vertices.size());

  // 单位圆柱的包围盒必须是 [-.5,.5] x [-.5,.5] x [-.5,.5] 的"外接盒"
  EXPECT_NEAR(cu.bounds.max.y, 0.5f, 1e-5f);
  EXPECT_NEAR(cu.bounds.min.y, -0.5f, 1e-5f);
  EXPECT_NEAR(cu.bounds.max.x, 0.5f, 1e-5f);
}
