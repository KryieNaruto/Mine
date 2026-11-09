// MagicBlocks L3 —— OBJ 加载器单元测试（spec §13 M4 第 19 项）
//
// 这里的测试全部走**内存文本**（load_obj_text），不碰磁盘。
// 理由：文件类测试会因工作目录/编码/行尾差异而随机失败，
// 而加载器真正要验证的是"文本 → 网格"这一步的语义。
// 文件 IO 只留一条真实文件用例（用临时文件）。
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "magicblocks/primitives/mesh_loader.h"

namespace mbp = magicblocks::primitives;

namespace {

// 三角形边长都是 sqrt(2) 的直角三角形，用于数值断言
const char* kTriangle = R"(v 0 0 0
v 1 0 0
v 0 1 0
f 1 2 3
)";

// 四边形（两个三角形），法线 +Z
const char* kQuad = R"(v 0 0 0
v 1 0 0
v 1 1 0
v 0 1 0
f 1 2 3 4
)";

// 立方体，8 个顶点 12 个三角形（无 vn）
const char* kCubeNoNormals = R"(v -1 -1 -1
v  1 -1 -1
v  1  1 -1
v -1  1 -1
v -1 -1  1
v  1 -1  1
v  1  1  1
v -1  1  1
f 1 2 3
f 1 3 4
f 5 6 7
f 5 7 8
f 1 5 6
f 1 6 2
f 2 6 7
f 2 7 3
f 3 7 8
f 3 8 4
f 4 8 5
f 4 5 1
)";

void expect_normals_unit(const mbp::MeshData& m) {
  for (std::size_t i = 0; i < m.vertices.size(); ++i) {
    const mbp::Vertex& v = m.vertices[i];
    const float l = std::sqrt(v.normal[0] * v.normal[0] + v.normal[1] * v.normal[1] +
                              v.normal[2] * v.normal[2]);
    ASSERT_NEAR(l, 1.0f, 1e-4f) << "顶点 #" << i << " 法线不是单位长度";
  }
}

}  // namespace

// ------------------------------------------------------------ 基础解析
TEST(ObjLoader, ParsesSingleTriangle) {
  auto ld = mbp::create_mesh_loader();
  const mbp::MeshLoadResult r = ld->load_obj_text(kTriangle);

  ASSERT_TRUE(r.ok()) << "加载失败: " << r.message;
  EXPECT_EQ(r.data.vertices.size(), 3u);
  EXPECT_EQ(r.data.indices.size(), 3u);
  EXPECT_EQ(r.stats.positions, 3u);
  EXPECT_EQ(r.stats.faces, 1u);
  EXPECT_EQ(r.stats.triangles, 1u);
}

TEST(ObjLoader, TriangulatesQuadIntoTwoTriangles) {
  auto ld = mbp::create_mesh_loader();
  const mbp::MeshLoadResult r = ld->load_obj_text(kQuad);

  ASSERT_TRUE(r.ok()) << r.message;
  EXPECT_EQ(r.stats.triangles, 2u);
  EXPECT_EQ(r.data.indices.size(), 6u);
  EXPECT_EQ(r.stats.polygons_split, 1u) << "四边形应被记录为拆分过一次";
}

TEST(ObjLoader, ComputesBounds) {
  auto ld = mbp::create_mesh_loader();
  const mbp::MeshLoadResult r = ld->load_obj_text(kQuad);

  ASSERT_TRUE(r.ok()) << r.message;
  EXPECT_NEAR(r.data.bounds.min.x, 0.0f, 1e-6f);
  EXPECT_NEAR(r.data.bounds.max.x, 1.0f, 1e-6f);
  EXPECT_NEAR(r.data.bounds.min.y, 0.0f, 1e-6f);
  EXPECT_NEAR(r.data.bounds.max.y, 1.0f, 1e-6f);
}

// ------------------------------------------------------------ 法线
TEST(ObjLoader, UsesFileNormalsWhenPresent) {
  const char* with_normals = R"(v 0 0 0
v 1 0 0
v 0 1 0
vn 0 0 7
f 1//1 2//1 3//1
)";
  auto ld = mbp::create_mesh_loader();
  const mbp::MeshLoadResult r = ld->load_obj_text(with_normals);

  ASSERT_TRUE(r.ok()) << r.message;
  EXPECT_FALSE(r.stats.normals_synthesized) << "文件里有法线就不该重算";

  // 文件里是 (0,0,7)，非单位长度 —— loader 必须归一化，
  // 否则光照强度会随美术导出的数值乱跳。
  for (const mbp::Vertex& v : r.data.vertices) {
    EXPECT_NEAR(v.normal[0], 0.0f, 1e-5f);
    EXPECT_NEAR(v.normal[1], 0.0f, 1e-5f);
    EXPECT_NEAR(v.normal[2], 1.0f, 1e-5f);
  }
}

TEST(ObjLoader, SynthesizesNormalsWhenMissing) {
  // 这是本加载器**最重要**的行为：OBJ 里没有 vn 时，
  // 若不重算就会留下零法线 → 光照项全 0 → 模型全黑，不崩不报错。
  auto ld = mbp::create_mesh_loader();
  const mbp::MeshLoadResult r = ld->load_obj_text(kCubeNoNormals);

  ASSERT_TRUE(r.ok()) << r.message;
  EXPECT_TRUE(r.stats.normals_synthesized);
  EXPECT_EQ(r.stats.normals, 0u) << "源文件确实没有 vn";
  expect_normals_unit(r.data);

  // 立方体的面法线必须是 ±X/±Y/±Z 之一。
  //
  // ⚠️ 这里曾经写错过两次，两次都只在光照上表现出来：
  //   第一次：焊接按位置做，8 个角每角 3 个面法线互相抵消成零向量，
  //           归一化失败退回 (0,1,0) → 整个盒子像从顶上打光。
  //   第二次：修掉零向量后变成"对角法线" (0.33,0.67,0.67) ——
  //           数学上是正确的平滑法线，但对**没有 s 指令**的盒子是错的：
  //           OBJ 缺省是硬边，法线不该被邻面平均。
  int axis_aligned = 0;
  for (const mbp::Vertex& v : r.data.vertices) {
    const float ax = std::fabs(v.normal[0]);
    const float ay = std::fabs(v.normal[1]);
    const float az = std::fabs(v.normal[2]);
    if (ax > 0.99f || ay > 0.99f || az > 0.99f) ++axis_aligned;
  }
  EXPECT_EQ(axis_aligned, static_cast<int>(r.data.vertices.size()))
      << "立方体(无 s 指令)的法线应全是轴向的";

  // 硬边没被平均的**真正**判据：试一个角点处的顶点，
  // 它的法线必须等于某个面法线本身，而不是几个面法线的平均值。
  // 立方体在 (±1,±1,±1) 每个角被 3 个面共用；若平均过，
  // 该位置顶点的法线会是 (±1,±1,±1)/√3 ≈ ±0.577 这类对角值，
  // 上面 axis_aligned 的断言已经能抓到这种情况。
  //
  // 这里额外确认顶点没有被压到 8 个 —— 8 个顶点意味着跨面共享。
  // 但**不能断言恰好 24**：24 只在"每面一个四边形 f 语句"时成立，
  // 而这份测试数据把每面写成两个三角形（共 12 个 f 语句），
  // 面内复用后每个三角形独立成面拓扑，正确结果是 36。
  // 曾经把 24 写死，导致加载器明明正确却报失败 ——
  // 拓扑由数据决定，测试不该替数据做决定。
  EXPECT_GT(r.data.vertices.size(), 8u)
      << "顶点数等于位置数(8)说明跨面共享了顶点，硬边会被平均成圆角";
}

TEST(ObjLoader, SmoothingGroupEnablesVertexSharing) {
  // 同样是立方体，但声明 `s 1`（平滑组）：
  // 这时**应该**共享顶点并平均法线，顶点数会降到 8。
  // 这条用例和上一条互为镜像，确保 s 指令真的被读进去了 ——
  // 如果两条都通过，说明区分是有效的；如果实现忽略了 s，
  // 必然有且只有一条失败。
  const char* smooth_cube = R"(s 1
v -1 -1 -1
v  1 -1 -1
v  1  1 -1
v -1  1 -1
v -1 -1  1
v  1 -1  1
v  1  1  1
v -1  1  1
f 1 2 3
f 1 3 4
f 5 6 7
f 5 7 8
f 1 5 6
f 1 6 2
f 2 6 7
f 2 7 3
f 3 7 8
f 3 8 4
f 4 8 5
f 4 5 1
)";
  auto ld = mbp::create_mesh_loader();
  const mbp::MeshLoadResult r = ld->load_obj_text(smooth_cube);

  ASSERT_TRUE(r.ok()) << r.message;
  expect_normals_unit(r.data);
  EXPECT_EQ(r.data.vertices.size(), 8u)
      << "s 1 下同一位置的顶点应被复用（8 个角各 1 个顶点）";

  // 平滑法线指向对角方向，不是轴向的
  int axis_aligned = 0;
  for (const mbp::Vertex& v : r.data.vertices) {
    if (std::fabs(v.normal[0]) > 0.99f || std::fabs(v.normal[1]) > 0.99f ||
        std::fabs(v.normal[2]) > 0.99f) {
      ++axis_aligned;
    }
  }
  EXPECT_EQ(axis_aligned, 0) << "平滑组的法线应是平均后的对角方向";
}

TEST(ObjLoader, SynthesizedNormalsAreSmoothOnSphereLikeSurface) {
  // 用一块"折起来的纸"验证平滑：两个共用一条边、夹角 90° 的四边形。
  // 共享边上的顶点法线应是两个面法线的平均，而不是任一个。
  //
  // 必须写 `s 1`：没有平滑组时 OBJ 缺省是硬边，
  // 顶点不该被共享（见 SynthesizesNormalsWhenMissing 的说明）。
  //
  // 几何：面 A 在 XY 平面（法线 +Z），面 B 在 XZ 平面（法线 +Y），
  // 共用边 x=0..1, y=0..0, z=0。所以共享边上顶点的法线应是 +Y+Z 方向。
  const char* folded = R"(s 1
v 0 0 0
v 1 0 0
v 1 1 0
v 0 1 0
v 1 0 1
v 0 0 1
f 1 2 3
f 1 3 4
f 1 2 5
f 1 5 6
)";
  auto ld = mbp::create_mesh_loader();
  const mbp::MeshLoadResult r = ld->load_obj_text(folded);

  ASSERT_TRUE(r.ok()) << r.message;
  expect_normals_unit(r.data);

  // 6 个位置，但因两平面夹角 90°，共享边(顶点 0,1)之外的顶点不共享。
  //
  // 共享边上的法线是两面法线的平均。这里**只验证"确实是平均值"**，
  // 不断言朝向 —— 朝向由 OBJ 的绕序决定，是美术数据的事。
  // 实测这组数据的绕序让两面法线分别是 +Z 与 -Y，
  // 平均后是 (0, -k, +k)；最初我以为该是 (0, +k, +k) 而写死了符号，
  // 测试因此假失败了一次。断言符号等于把测试绑在数据的一个偶然属性上。
  const float k = 1.0f / std::sqrt(2.0f);
  int checked = 0;
  for (const mbp::Vertex& v : r.data.vertices) {
    const bool on_shared_edge =
        std::fabs(v.position[2]) < 1e-6f && std::fabs(v.position[1]) < 1e-6f;
    if (!on_shared_edge) continue;
    ++checked;
    // 两个面的法线分别沿 Z 与 Y，都不含 X 分量
    EXPECT_NEAR(v.normal[0], 0.0f, 1e-4f) << "这两个面的法线都不含 X 分量";
    // 关键断言：两个分量都存在且等大 —— 这才是"平均"。
    // 若只等于某一个面法线（1.0 / 0.0），说明焊接失效、法线没被平均。
    EXPECT_NEAR(std::fabs(v.normal[1]), k, 1e-4f) << "应含 Y 分量（来自侧面）";
    EXPECT_NEAR(std::fabs(v.normal[2]), k, 1e-4f) << "应含 Z 分量（来自顶面）";
  }
  EXPECT_EQ(checked, 2) << "共享边应有两个顶点(0 和 1)";
}

TEST(ObjLoader, ZeroNormalsInFileFallBackToFaceNormal) {
  // 有些导出器会写 `vn 0 0 0`。直接用会让光照全 0（全黑），
  // 归一化时长度 < eps 就该退化成面法线。
  const char* bad_vn = R"(v 0 0 0
v 1 0 0
v 0 1 0
vn 0 0 0
f 1//1 2//1 3//1
)";
  auto ld = mbp::create_mesh_loader();
  const mbp::MeshLoadResult r = ld->load_obj_text(bad_vn);

  ASSERT_TRUE(r.ok()) << r.message;
  expect_normals_unit(r.data);
  // 面在 XY 平面，法线应是 ±Z
  for (const mbp::Vertex& v : r.data.vertices) {
    EXPECT_NEAR(std::fabs(v.normal[2]), 1.0f, 1e-4f) << "零法线应退化成面法线";
  }
}

// ------------------------------------------------------------ 索引形式
TEST(ObjLoader, SupportsNegativeIndices) {
  auto ld = mbp::create_mesh_loader();
  // -1/-2/-3 是相对于"当前已有顶点数"的索引
  const char* neg = R"(v 0 0 0
v 1 0 0
v 0 1 0
f -3 -2 -1
)";
  const mbp::MeshLoadResult r = ld->load_obj_text(neg);
  ASSERT_TRUE(r.ok()) << r.message;
  EXPECT_EQ(r.stats.triangles, 1u);
  EXPECT_EQ(r.data.indices.size(), 3u);
}

TEST(ObjLoader, SupportsVSlashVtSlashVnForms) {
  auto ld = mbp::create_mesh_loader();
  // 三种 f 写法都要认：v、v/vt、v//vn、v/vt/vn
  const char* forms = R"(v 0 0 0
v 1 0 0
v 0 1 0
v 0 0 1
vt 0 0
vt 1 0
vt 0 1
vn 0 0 1
f 1 2 3
f 1/1 4/2 3/3
f 1//1 2//1 4//1
f 1/1/1 2/2/1 3/3/1
)";
  const mbp::MeshLoadResult r = ld->load_obj_text(forms);
  ASSERT_TRUE(r.ok()) << r.message;
  EXPECT_EQ(r.stats.triangles, 4u);
}

TEST(ObjLoader, PreservesUvCoordinates) {
  const char* uv_obj = R"(v 0 0 0
v 1 0 0
v 0 1 0
vt 0.25 0.75
vt 0.5 0.75
vt 0.25 1.0
f 1/1 2/2 3/3
)";
  auto ld = mbp::create_mesh_loader();
  const mbp::MeshLoadResult r = ld->load_obj_text(uv_obj);
  ASSERT_TRUE(r.ok()) << r.message;

  // uv 必须原样保留：贴图坐标是美术定的，loader 不能动
  bool found_025_075 = false;
  for (const mbp::Vertex& v : r.data.vertices) {
    if (std::fabs(v.uv[0] - 0.25f) < 1e-6f && std::fabs(v.uv[1] - 0.75f) < 1e-6f) {
      found_025_075 = true;
    }
  }
  EXPECT_TRUE(found_025_075) << "uv (0.25, 0.75) 未出现在结果里";
}

// ------------------------------------------------------------ 错误处理
TEST(ObjLoader, EmptyTextIsEmptyGeometryNotParseError) {
  auto ld = mbp::create_mesh_loader();
  const mbp::MeshLoadResult r = ld->load_obj_text("");

  // 空文件是"合法但没有几何"，不是语法错误。
  // 上层据此可以走 fallback，而语法错误应该报警。
  EXPECT_FALSE(r.ok());
  EXPECT_EQ(r.error, mbp::MeshLoadError::kEmptyGeometry);
}

TEST(ObjLoader, PointsOnlyIsEmptyGeometry) {
  auto ld = mbp::create_mesh_loader();
  const mbp::MeshLoadResult r = ld->load_obj_text("v 0 0 0\nv 1 0 0\n");
  EXPECT_EQ(r.error, mbp::MeshLoadError::kEmptyGeometry);
}

TEST(ObjLoader, BadIndexIsParseErrorWithLineNumber) {
  auto ld = mbp::create_mesh_loader();
  const mbp::MeshLoadResult r = ld->load_obj_text(
      "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 99\n");

  EXPECT_EQ(r.error, mbp::MeshLoadError::kParseFailed);
  // 报错必须带行号 —— 否则上千行的 OBJ 根本没法定位
  EXPECT_NE(r.message.find("4"), std::string::npos) << "message 应含行号: " << r.message;
}

TEST(ObjLoader, ZeroIndexIsRejected) {
  auto ld = mbp::create_mesh_loader();
  // OBJ 索引从 1 开始，0 是非法值（不是"第一个顶点"）
  const mbp::MeshLoadResult r =
      ld->load_obj_text("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 0 1 2\n");
  EXPECT_EQ(r.error, mbp::MeshLoadError::kParseFailed);
}

TEST(ObjLoader, NonNumericCoordinateIsParseError) {
  auto ld = mbp::create_mesh_loader();
  const mbp::MeshLoadResult r = ld->load_obj_text("v abc 0 0\n");
  EXPECT_EQ(r.error, mbp::MeshLoadError::kParseFailed);
}

TEST(ObjLoader, ConcavePolygonIsRejectedNotSilentlyMisTriangulated) {
  // 一个凹四边形（"箭头"形状）。扇式三角化会把三角形铺到凹口外。
  // 与其产出一个形状错误但能显示的网格，不如明确报错。
  const char* concave = R"(v 0 0 0
v 2 0 0
v 1 0.5 0
v 2 1 0
v 0 1 0
f 1 2 3 4 5
)";
  auto ld = mbp::create_mesh_loader();
  const mbp::MeshLoadResult r = ld->load_obj_text(concave);

  EXPECT_EQ(r.error, mbp::MeshLoadError::kConcaveFace)
      << "凹多边形必须被拒绝，实际 error=" << mbp::to_string(r.error);
}

TEST(ObjLoader, DegenerateFaceIsSkippedNotFatal) {
  // 共线的三个点构成零面积面。真实 OBJ 里这种碎面很常见，
  // 报错会让本来可用的文件加载不了，所以应该静默跳过。
  const char* iffy = R"(v 0 0 0
v 1 0 0
v 2 0 0
v 0 1 0
f 1 2 3
f 1 2 4
)";
  auto ld = mbp::create_mesh_loader();
  const mbp::MeshLoadResult r = ld->load_obj_text(iffy);

  ASSERT_TRUE(r.ok()) << r.message;
  EXPECT_EQ(r.stats.triangles, 1u) << "退化面应被跳过，只有 1 个三角形存活";
}

TEST(ObjLoader, MissingFileReturnsFileNotFound) {
  auto ld = mbp::create_mesh_loader();
  const mbp::MeshLoadResult r = ld->load_obj_file(" definitely/not/here.obj");
  EXPECT_EQ(r.error, mbp::MeshLoadError::kFileNotFound);
}

TEST(ObjLoader, LoadsFromActualFile) {
  // 唯一一条真实文件用例：确认 ifstream 路径也是通的
  const char* path = "./_obj_loader_test.obj";
  {
    std::ofstream out(path);
    ASSERT_TRUE(out) << "无法创建临时文件";
    out << kCubeNoNormals;
  }
  auto ld = mbp::create_mesh_loader();
  const mbp::MeshLoadResult r = ld->load_obj_file(path);
  std::remove(path);

  ASSERT_TRUE(r.ok()) << r.message;
  EXPECT_EQ(r.stats.triangles, 12u);
  expect_normals_unit(r.data);
}

// ------------------------------------------------------------ 其它
TEST(ObjLoader, IgnoresCommentsAndUnknownKeywords) {
  const char* noisy = R"(# 这是注释
mtllib something.mtl
o MyObject
g group1
s off
usemtl red
v 0 0 0
v 1 0 0
v 0 1 0
f 1 2 3
)";
  auto ld = mbp::create_mesh_loader();
  const mbp::MeshLoadResult r = ld->load_obj_text(noisy);
  ASSERT_TRUE(r.ok()) << r.message;
  EXPECT_EQ(r.stats.triangles, 1u);
}

TEST(ObjLoader, HandlesWindowsLineEndings) {
  // Windows 行尾的 \r 必须被正确剥离，否则最后一个数字会解析失败
  std::string crlf;
  for (const char* p = kTriangle; *p; ++p) {
    if (*p == '\n') crlf += '\r';
    crlf += *p;
  }
  auto ld = mbp::create_mesh_loader();
  const mbp::MeshLoadResult r = ld->load_obj_text(crlf);
  ASSERT_TRUE(r.ok()) << r.message;
  EXPECT_EQ(r.stats.triangles, 1u);
  EXPECT_NEAR(r.data.bounds.max.x, 1.0f, 1e-6f) << "CRLF 下数值仍应正确";
}

TEST(ObjLoader, ErrorToStringCoversAllCodes) {
  for (int i = 0; i <= static_cast<int>(mbp::MeshLoadError::kTooLarge); ++i) {
    const char* s = mbp::to_string(static_cast<mbp::MeshLoadError>(i));
    EXPECT_NE(s, nullptr);
    EXPECT_STRNE(s, "unknown") << "错误码 " << i << " 没有对应字符串";
  }
}

TEST(ObjLoader, RespectsVertexLimit) {
  mbp::MeshLoadOptions opt;
  opt.max_vertices = 2;
  auto ld = mbp::create_mesh_loader();
  const mbp::MeshLoadResult r = ld->load_obj_text(kTriangle, opt);
  EXPECT_EQ(r.error, mbp::MeshLoadError::kTooLarge);
}
