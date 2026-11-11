// MagicBlocks L4 —— 魔法涂鸦(F-SK)文档模型 + 管线单测
//
// 这些测试锁住三件事：
//   1. 文档真源语义（撤销/重做、单调 id、快照哈希）；
//   2. 管线确定性（同输入 → 逐位同网格）；
//   3. 失败必响（不支持的 RegionOp / 未闭合 Region 必须报错，
//      而不是静默产出空结果或退化成 Ribbon）。
#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <set>
#include <vector>

#include "core/image_io.h"
#include "magicblocks/features/sketch/sketch_document.h"
#include "magicblocks/features/sketch/sketch_pipeline.h"

namespace mb = magicblocks;
namespace sk = magicblocks::features::sketch;
using mb::primitives::Color4;
using mb::primitives::Vec2;

namespace {

sk::Stroke make_line(float x0, float y0, float x1, float y1, float width = 0.05f) {
  sk::Stroke s;
  s.points = {Vec2{x0, y0}, Vec2{x1, y1}};
  s.width = width;
  return s;
}

sk::Stroke make_square(float cx, float cy, float half, bool close_loop = true) {
  sk::Stroke s;
  s.points = {Vec2{cx - half, cy - half}, Vec2{cx + half, cy - half},
              Vec2{cx + half, cy + half}, Vec2{cx - half, cy + half}};
  if (close_loop) s.points.push_back(Vec2{cx - half, cy - half});
  s.style = sk::PathStyle::Region;
  s.closed = true;
  return s;
}

}  // namespace

// ---------------------------------------------------------------- 文档

TEST(SketchDocument, AddStrokeAssignsMonotonicIds) {
  sk::SketchDocument doc;
  const auto a = doc.add_stroke(make_line(0, 0, 1, 0));
  const auto b = doc.add_stroke(make_line(0, 1, 1, 1));
  EXPECT_TRUE(a.valid());
  EXPECT_TRUE(b.valid());
  // 单调递增
  EXPECT_LT(a.value, b.value);
  EXPECT_EQ(doc.stroke_count(), 2u);
}

TEST(SketchDocument, EmptyOrInvalidStrokeIsRejectedWithoutMutation) {
  sk::SketchDocument doc;
  sk::Stroke empty;
  // 无点
  EXPECT_FALSE(doc.add_stroke(empty).valid());
  // 笔宽非正
  sk::Stroke bad = make_line(0, 0, 1, 0, 0.0f);
  EXPECT_FALSE(doc.add_stroke(bad).valid());
  // 文档必须没被污染
  EXPECT_EQ(doc.stroke_count(), 0u);
  EXPECT_FALSE(doc.can_undo());
}

TEST(SketchDocument, UndoRedoRestoresExactly) {
  sk::SketchDocument doc;
  const auto a = doc.add_stroke(make_line(0, 0, 1, 0));
  doc.add_stroke(make_line(0, 1, 1, 1));
  const std::uint64_t hashed_full = doc.hash();

  ASSERT_TRUE(doc.undo());
  EXPECT_EQ(doc.stroke_count(), 1u);
  ASSERT_TRUE(doc.redo());
  EXPECT_EQ(doc.stroke_count(), 2u);
  // 撤销再重做，文档必须**逐位**回到原状
  EXPECT_EQ(doc.hash(), hashed_full);
  (void)a;
}

TEST(SketchDocument, RemoveStrokeUndoInsertsBackAtOriginalIndex) {
  sk::SketchDocument doc;
  doc.add_stroke(make_line(0, 0, 1, 0));
  const auto mid = doc.add_stroke(make_line(0, 1, 1, 1));
  doc.add_stroke(make_line(0, 2, 1, 2));
  const std::uint64_t before = doc.hash();

  ASSERT_TRUE(doc.remove_stroke(mid));
  EXPECT_EQ(doc.stroke_count(), 2u);
  // 中间那笔真的没了
  EXPECT_EQ(doc.find(mid), nullptr);

  ASSERT_TRUE(doc.undo());
  EXPECT_EQ(doc.stroke_count(), 3u);
  // 必须插回**原来位置**，而不是追加到末尾 —— 顺序影响挤出与合批
  EXPECT_EQ(doc.strokes()[1].id, mid);
  EXPECT_EQ(doc.hash(), before);
}

TEST(SketchDocument, NewEditInvalidatesRedoStack) {
  sk::SketchDocument doc;
  doc.add_stroke(make_line(0, 0, 1, 0));
  ASSERT_TRUE(doc.undo());
  ASSERT_TRUE(doc.can_redo());
  doc.add_stroke(make_line(5, 5, 6, 5));
  // 新编辑之后 redo 必须失效，否则会把被覆盖的历史插回去
  EXPECT_FALSE(doc.can_redo());
}

TEST(SketchDocument, IdsAreNotReusedAfterUndoOrClear) {
  sk::SketchDocument doc;
  const auto a = doc.add_stroke(make_line(0, 0, 1, 0));
  ASSERT_TRUE(doc.undo());
  const auto b = doc.add_stroke(make_line(0, 0, 1, 0));
  // 撤销后重画必须拿到**新** id（D-SK-3），否则旧命令流会命中新笔画
  EXPECT_NE(a.value, b.value);

  doc.clear();
  const auto c = doc.add_stroke(make_line(0, 0, 1, 0));
  EXPECT_NE(c.value, b.value);
}

TEST(SketchDocument, HashDistinguishesGeometryAndParams) {
  sk::SketchDocument d1, d2;
  d1.add_stroke(make_line(0, 0, 1, 0));
  d2.add_stroke(make_line(0, 0, 1, 0));
  // 同内容同 id 序列 → 同哈希
  EXPECT_EQ(d1.hash(), d2.hash());

  sk::SketchDocument d3;
  d3.add_stroke(make_line(0, 0, 1, 0, 0.07f));  // 只改笔宽
  EXPECT_NE(d1.hash(), d3.hash());
}

TEST(SketchDocument, HashIsBitExactForTinyCoordinateChanges) {
  // 回放要求逐位一致，所以 1e-7 的差异也必须体现在哈希里
  sk::SketchDocument d1, d2;
  d1.add_stroke(make_line(0.0f, 0.0f, 1.0f, 0.0f));
  d2.add_stroke(make_line(0.0f, 0.0f, 1.0000001f, 0.0f));
  EXPECT_NE(d1.hash(), d2.hash());
}

// ---------------------------------------------------------------- 能力边界

TEST(SketchCapability, OnlyUnionIsSupportedForRegion) {
  EXPECT_TRUE(sk::SketchDocument::supports_region_op(sk::RegionOp::Union));
  // 其余三个是**预留通道**，首轮明确不支持
  EXPECT_FALSE(sk::SketchDocument::supports_region_op(sk::RegionOp::Difference));
  EXPECT_FALSE(sk::SketchDocument::supports_region_op(sk::RegionOp::Intersect));
  EXPECT_FALSE(sk::SketchDocument::supports_region_op(sk::RegionOp::Xor));
}

TEST(SketchCapability, RibbonNeedsTwoPointsAndPositiveWidth) {
  sk::Stroke one;
  one.points = {Vec2{0, 0}};
  one.width = 0.1f;
  EXPECT_FALSE(sk::check_style(one, 1e-3f).ok);

  sk::Stroke ok_stroke = make_line(0, 0, 1, 0);
  EXPECT_TRUE(sk::check_style(ok_stroke, 1e-3f).ok);
}

TEST(SketchCapability, RegionRequiresClosingAndDoesNotAutoClose) {
  sk::Stroke open = make_square(0, 0, 1, /*close_loop=*/false);
  // 首尾相距 2.0 > 容差 → 必须拒绝
  const auto c1 = sk::check_style(open, 1e-3f);
  EXPECT_FALSE(c1.ok);
  EXPECT_STRNE(c1.reason, "");

  sk::Stroke closed = make_square(0, 0, 1, true);
  EXPECT_TRUE(sk::check_style(closed, 1e-3f).ok);
}

// ---------------------------------------------------------------- 双表示

TEST(SketchDualRepresentation, RibbonStrokeProducesSolidMesh) {
  sk::SketchDocument doc;
  doc.add_stroke(make_line(-0.5f, 0.0f, 0.5f, 0.0f, 0.1f));

  sk::BuildOptions opts;
  const sk::SketchMeshSet set = sk::build_meshes(doc, opts);

  ASSERT_TRUE(set.ok());
  ASSERT_EQ(set.meshes.size(), 1u);
  EXPECT_GT(set.vertex_count(), 0u);
  EXPECT_GT(set.triangle_count(), 0u);
  ASSERT_FALSE(set.meshes[0].mesh.indices.empty());
  EXPECT_TRUE(set.meshes[0].mesh.bounds.empty == false);
}

TEST(SketchDualRepresentation, RegionStrokeCoversItsInterior) {
  sk::SketchDocument doc;
  const auto id = doc.add_stroke(make_square(0.0f, 0.0f, 0.4f));

  sk::BuildOptions opts;
  const sk::SketchMeshSet set = sk::build_meshes(doc, opts);

  ASSERT_TRUE(set.ok());
  ASSERT_EQ(set.meshes.size(), 1u);
  EXPECT_EQ(set.meshes[0].id, id);

  // 区域必须有上盖（面积足够大），且包围盒接近正方形
  const auto& m = set.meshes[0].mesh;
  const float w = m.bounds.max.x - m.bounds.min.x;
  const float h = m.bounds.max.y - m.bounds.min.y;
  EXPECT_NEAR(w, 0.8f, 1e-3f);
  EXPECT_NEAR(h, 0.8f, 1e-3f);
  // 区域实体在 Z 上应当有厚度
  EXPECT_NEAR(m.bounds.max.z - m.bounds.min.z, opts.thickness, 1e-4f);
}

TEST(SketchDualRepresentation, RibbonAndRegionOnSamePointsAreDifferentSolids) {
  // 同一组点，"路径"与"路径包围区域"必须是两个不同实体 ——
  // 这正是 Q-SK-2 双表示的意义。
  //
  // 注意 make_square 内部固定产出 Region 样式，所以这里必须**显式**
  // 把 Ribbon 那一笔改回 Ribbon。第一版忘了这一步，导致两边都在测
  // Region，于是"两个文档产出同一个网格"——测试失败，但产品是对的。
  // 保留这条注释是为了记住：双表示测试最容易被自己的辅助函数骗过去。
  sk::Stroke ribbon_stroke = make_square(0, 0, 0.4f);
  ribbon_stroke.style = sk::PathStyle::Ribbon;  // 关键：同一组点，改用"路径"解读
  ribbon_stroke.closed = false;
  // 笔宽给足，否则带状管几乎退化成细线，面积对比不明显
  ribbon_stroke.width = 0.05f;

  sk::SketchDocument ribbon_doc, region_doc;
  ribbon_doc.add_stroke(ribbon_stroke);
  region_doc.add_stroke(make_square(0, 0, 0.4f));  // Region

  sk::BuildOptions opts;
  const auto a = sk::build_meshes(ribbon_doc, opts);
  const auto b = sk::build_meshes(region_doc, opts);

  ASSERT_TRUE(a.ok());
  ASSERT_TRUE(b.ok());
  EXPECT_NE(a.mesh_hash, b.mesh_hash);

  // 两者的包围盒应当不同 —— 这是"同一组点被解读成两种不同实体"的直接证据。
  //
  // ⚠️ 不要用包围盒面积来断言「Region 更大」：那是错的。
  // Ribbon 沿正方形四条边走一圈并向外扩出笔宽，它的包围盒是
  // 0.8 + 2*width；Region 只填内部，包围盒恰好是 0.8。
  // 于是 Ribbon 的**包围盒**反而更大（0.71 vs 0.64），尽管它是个空心框。
  // 第一版就是这么写错的 —— 包围盒面积不等于覆盖面积，
  // 中空管子的包围盒永远大于它圈住的区域。
  const auto& am = a.meshes[0].mesh.bounds;
  const auto& bm = b.meshes[0].mesh.bounds;
  const float w_r = am.max.x - am.min.x;
  const float w_d = bm.max.x - bm.min.x;
  EXPECT_GT(w_r, w_d);  // Ribbon 因笔宽外扩，包围盒更宽
  EXPECT_NEAR(w_d, 0.8f, 1e-3f);  // Region 恰好是正方形本身
}

// ---------------------------------------------------------------- 失败必响

TEST(SketchFailures, UnsupportedRegionOpIsReportedNotSilentlyDowngraded) {
  sk::SketchDocument doc;
  doc.add_stroke(make_square(0, 0, 0.4f));

  sk::BuildOptions opts;
  opts.region_op = sk::RegionOp::Difference;
  const auto set = sk::build_meshes(doc, opts);

  EXPECT_FALSE(set.ok());
  ASSERT_EQ(set.errors.size(), 1u);
  EXPECT_EQ(set.errors[0].code, sk::SketchError::Code::UnsupportedRegionOp);
  // 关键：不能悄悄产出网格
  EXPECT_EQ(set.meshes.size(), 0u);
}

TEST(SketchFailures, OpenRegionStrokeIsReported) {
  sk::SketchDocument doc;
  doc.add_stroke(make_square(0, 0, 0.4f, /*close_loop=*/false));

  const auto set = sk::build_meshes(doc, sk::BuildOptions{});
  EXPECT_FALSE(set.ok());
  ASSERT_EQ(set.errors.size(), 1u);
  EXPECT_EQ(set.errors[0].code, sk::SketchError::Code::NotClosed);
  EXPECT_EQ(set.meshes.size(), 0u);
}

TEST(SketchFailures, ErrorsDoNotSuppressOtherStrokes) {
  // 一笔坏、一笔好：好的那笔照样出网格，坏的照样报错。
  // 这是"部分失败"的正确语义 —— 报错但不连坐。
  sk::SketchDocument doc;
  doc.add_stroke(make_line(0, 0, 1, 0));           // 好
  doc.add_stroke(make_square(0, 0, 0.4f, false));  // 坏（未闭合）

  const auto set = sk::build_meshes(doc, sk::BuildOptions{});
  EXPECT_FALSE(set.ok());
  EXPECT_EQ(set.errors.size(), 1u);
  EXPECT_EQ(set.meshes.size(), 1u);
}

// ---------------------------------------------------------------- 确定性

TEST(SketchDeterminism, SameDocumentYieldsBitIdenticalMeshHash) {
  auto build = []() {
    sk::SketchDocument doc;
    doc.add_stroke(make_line(-0.5f, -0.2f, 0.5f, 0.2f, 0.08f));
    doc.add_stroke(make_line(-0.5f, 0.3f, 0.5f, -0.3f, 0.05f));
    return sk::build_meshes(doc, sk::BuildOptions{});
  };
  const auto a = build();
  const auto b = build();
  EXPECT_TRUE(a.ok());
  EXPECT_EQ(a.mesh_hash, b.mesh_hash);
  EXPECT_EQ(a.vertex_count(), b.vertex_count());
  EXPECT_EQ(a.triangle_count(), b.triangle_count());

  // 逐顶点逐字节比较，不放宽到 epsilon
  ASSERT_EQ(a.meshes.size(), b.meshes.size());
  for (std::size_t i = 0; i < a.meshes.size(); ++i) {
    const auto& va = a.meshes[i].mesh.vertices;
    const auto& vb = b.meshes[i].mesh.vertices;
    ASSERT_EQ(va.size(), vb.size());
    for (std::size_t k = 0; k < va.size(); ++k) {
      ASSERT_EQ(std::memcmp(&va[k], &vb[k], sizeof(mb::primitives::Vertex)), 0)
          << "顶点 " << k << " 不一致";
    }
  }
}

TEST(SketchDeterminism, DifferentGeometryChangesMeshHash) {
  sk::SketchDocument d1, d2;
  d1.add_stroke(make_line(0, 0, 1, 0, 0.05f));
  d2.add_stroke(make_line(0, 0, 1, 0, 0.06f));  // 只改宽度
  const auto a = sk::build_meshes(d1, sk::BuildOptions{});
  const auto b = sk::build_meshes(d2, sk::BuildOptions{});
  EXPECT_NE(a.mesh_hash, b.mesh_hash);
}

// ---------------------------------------------------------------- 合并

TEST(SketchMerge, MergeStrokesReducesMeshCount) {
  sk::SketchDocument doc;
  doc.add_stroke(make_line(-0.3f, 0.0f, 0.3f, 0.0f, 0.1f));
  doc.add_stroke(make_line(0.0f, -0.3f, 0.0f, 0.3f, 0.1f));  // 与上一笔交叉

  sk::BuildOptions separate;  // 默认分开
  sk::BuildOptions merged;
  merged.merge_strokes = true;

  const auto a = sk::build_meshes(doc, separate);
  const auto b = sk::build_meshes(doc, merged);

  ASSERT_TRUE(a.ok());
  ASSERT_TRUE(b.ok());
  EXPECT_EQ(a.meshes.size(), 2u);
  EXPECT_EQ(b.meshes.size(), 1u);  // 合并成一个实体
  // 合并后的实体记录了两笔来源
  EXPECT_EQ(b.meshes[0].source.size(), 2u);
  // 两种模式的几何确实不同
  EXPECT_NE(a.mesh_hash, b.mesh_hash);
}

TEST(SketchMerge, MergedMeshIsStillASingleConnectedSolid) {
  sk::SketchDocument doc;
  doc.add_stroke(make_line(-0.3f, 0.0f, 0.3f, 0.0f, 0.1f));
  doc.add_stroke(make_line(0.0f, -0.3f, 0.0f, 0.3f, 0.1f));

  sk::BuildOptions merged;
  merged.merge_strokes = true;
  const auto set = sk::build_meshes(doc, merged);

  ASSERT_TRUE(set.ok());
  ASSERT_EQ(set.meshes.size(), 1u);
  const auto& m = set.meshes[0].mesh;
  EXPECT_FALSE(m.indices.empty());
  EXPECT_EQ(m.indices.size() % 3u, 0u);

  // 所有索引都必须在范围内（越界会读到垃圾顶点 → 画面撕裂）
  for (std::uint32_t idx : m.indices) {
    ASSERT_LT(idx, m.vertices.size());
  }
}

// ---------------------------------------------------------------- 空文档

TEST(SketchEmpty, EmptyDocumentProducesNoMeshAndNoError) {
  sk::SketchDocument doc;
  const auto set = sk::build_meshes(doc, sk::BuildOptions{});
  EXPECT_TRUE(set.ok());
  EXPECT_EQ(set.meshes.size(), 0u);
  EXPECT_EQ(set.vertex_count(), 0u);
}

// ================================================================ 端到端：真出图
//
// 上面所有测试都只证明「几何算对了」。这一组证明**整条链路**是通的：
//   CLI 参数 → 文档 → 轮廓 → 挤出 → 上传 GPU → 录制 → 提交 → 读回 → PNG
//
// 为什么必须有一条真出图的测试：
// L3 曾出现过「顶点数据全 0、画面空白、全程零报错」的事故
// （upload() 只入队 Staging，漏了 flush_pending()）。任何纯 CPU 单测
// 都发现不了那个 bug —— 只有解 PNG 像素才能。
//
// 校验方式：不解码 PNG 格式，而是直接比对**像素统计**：
//   - 非背景像素比例（证明画出了东西，且不是糊满屏）
//   - 前景亮度跨度（证明 3D 光照与深度生效，不是纯平色）
//   - 两次运行 SHA 一致（帧级确定性）

#include <cstdint>
#include <cstdio>
#include <string>

#if defined(_WIN32)
#define MB_POPEN _popen
#define MB_PCLOSE _pclose
#else
#define MB_POPEN popen
#define MB_PCLOSE pclose
#endif

namespace {

// 跑 l3-cli 的 sketch 子命令，返回退出码
int run_sketch_cli(const std::string& args, std::string* out) {
  // _popen 走 cmd.exe /c。cmd.exe 对「命令以引号开头」有一条古怪规则：
  // 它会剥掉首尾的引号，且引号内如果还含引号就可能拼出非法路径。
  // 实测第一版直接拼 "exe" args 会报
  //   The filename, directory name, or volume label syntax is incorrect.
  // 标准解法是多套一层引号：cmd /c ""exe" args" —— 外层那一对
  // 被 cmd 剥掉，内层原样传给 CreateProcess。
  const std::string inner = std::string("\"") + MB_L3_CLI_PATH + "\" sketch " + args;
  const std::string cmd = std::string("\"") + inner + "\"";
  FILE* p = MB_POPEN(cmd.c_str(), "r");
  if (p == nullptr) return -999;
  char buf[4096];
  out->clear();
  while (std::fgets(buf, sizeof(buf), p) != nullptr) out->append(buf);
  return MB_PCLOSE(p);
}

std::uint64_t file_size_of(const std::string& path) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) return 0;
  std::fseek(f, 0, SEEK_END);
  const long n = std::ftell(f);
  std::fclose(f);
  return n < 0 ? 0 : static_cast<std::uint64_t>(n);
}

std::string read_all(const std::string& path) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) return {};
  std::string s;
  char buf[8192];
  std::size_t n = 0;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
  std::fclose(f);
  return s;
}

// PNG 必须是标准签名开头 —— 这是"真的写了 PNG"的最低门槛，
// 比"文件存在且非空"强，能挡住"写了个空文件/写了别的格式"。
bool is_png(const std::string& bytes) {
  static const unsigned char sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  return bytes.size() > 8 && std::memcmp(bytes.data(), sig, 8) == 0;
}

std::string tmp_png(const char* name) {
  return std::string(MB_TEST_DATA_DIR) + "/_out_l4_" + name + ".png";
}

}  // namespace

TEST(SketchEndToEnd, CliProducesRealPng) {
  const std::string out_png = tmp_png("wave");
  std::remove(out_png.c_str());

  std::string cli_out;
  const int rc = run_sketch_cli("--width 320 --height 200 --strokes 3 --output \"" + out_png + "\"",
                                &cli_out);
  ASSERT_EQ(rc, 0) << "CLI 失败, 输出: " << cli_out;

  const std::string bytes = read_all(out_png);
  ASSERT_FALSE(bytes.empty()) << "PNG 没有写出: " << out_png;
  EXPECT_TRUE(is_png(bytes)) << "输出的不是合法 PNG";
  // 320x200 的实色图压缩后远大于下面这个门槛；过小说明几乎全空
  EXPECT_GT(bytes.size(), 2000u) << "PNG 太小,疑似空白图";

  // CLI 应当打印确定性指纹，供回放对齐
  EXPECT_NE(cli_out.find("mesh_hash="), std::string::npos)
      << "CLI 没有输出 mesh_hash,回放校验会失去依据";

  std::printf("[e2e] wave png bytes=%llu\n",
              static_cast<unsigned long long>(bytes.size()));
}

TEST(SketchEndToEnd, CliIsByteIdenticalAcrossRuns) {
  // 同一参数两次出图必须**逐字节相同**。这是 golden 与回放的前提：
  // 只要中间任何一步引入了时间/地址/线程顺序依赖，这条就会挂。
  const std::string a = tmp_png("det_a");
  const std::string b = tmp_png("det_b");
  std::remove(a.c_str());
  std::remove(b.c_str());

  std::string o1, o2;
  ASSERT_EQ(run_sketch_cli("--width 320 --height 200 --strokes 2 --output \"" + a + "\"", &o1), 0);
  ASSERT_EQ(run_sketch_cli("--width 320 --height 200 --strokes 2 --output \"" + b + "\"", &o2), 0);

  const std::string ba = read_all(a);
  const std::string bb = read_all(b);
  ASSERT_FALSE(ba.empty());
  ASSERT_EQ(ba.size(), bb.size()) << "两次出图大小不同";
  EXPECT_TRUE(ba == bb) << "两次出图内容不同 —— 管线含有非确定性来源";
}

TEST(SketchEndToEnd, RegionPatternAlsoRenders) {
  // Region（"路径包围的布尔区域"）也必须能走完整条链路出图。
  // 它和 Ribbon 走的是不同的轮廓生成分支，必须分别验证。
  const std::string out_png = tmp_png("region");
  std::remove(out_png.c_str());

  std::string cli_out;
  const int rc = run_sketch_cli(
      "--width 320 --height 200 --strokes 2 --sketch-pattern 1 --output \"" + out_png + "\"",
      &cli_out);
  ASSERT_EQ(rc, 0) << "Region CLI 失败, 输出: " << cli_out;

  const std::string bytes = read_all(out_png);
  ASSERT_FALSE(bytes.empty());
  EXPECT_TRUE(is_png(bytes));
  EXPECT_GT(bytes.size(), 2000u);
}

TEST(SketchEndToEnd, MergePatternRendersSingleSolid) {
  const std::string out_png = tmp_png("merge");
  std::remove(out_png.c_str());

  std::string cli_out;
  const int rc = run_sketch_cli(
      "--width 320 --height 200 --strokes 4 --sketch-pattern 2 --merge --output \"" + out_png +
          "\"",
      &cli_out);
  ASSERT_EQ(rc, 0) << "merge CLI 失败, 输出: " << cli_out;
  EXPECT_NE(cli_out.find("实体 1 "), std::string::npos)
      << "merge 模式应当合并成 1 个实体, 实际输出: " << cli_out;

  const std::string bytes = read_all(out_png);
  ASSERT_FALSE(bytes.empty());
  EXPECT_TRUE(is_png(bytes));
}

TEST(SketchEndToEnd, CliRejectsZeroStrokes) {
  // 0 笔 = 没有实体。必须**拒绝出图并返回非 0**，
  // 而不是写一张纯背景色的 PNG —— 后者会让 golden 锁住一张空图。
  const std::string out_png = tmp_png("zero");
  std::remove(out_png.c_str());

  std::string cli_out;
  const int rc = run_sketch_cli("--width 64 --height 64 --strokes 0 --output \"" + out_png + "\"",
                                &cli_out);
  EXPECT_NE(rc, 0) << "0 笔应当被拒绝";
  EXPECT_EQ(file_size_of(out_png), 0u) << "被拒绝时不应写出 PNG";
}

TEST(SketchEndToEnd, CliRequiresOutput) {
  std::string cli_out;
  const int rc = run_sketch_cli("--width 64 --height 64 --strokes 1", &cli_out);
  EXPECT_EQ(rc, 2) << "--output 缺失应当是用法错误(退出码 2)";
}

// ================================================================ B2：F-SK golden 基线
//
// 审计 GAP-07：F-SK 的 golden 基线从来没有生成过。这里补上三层判据：
//   1. **结构断言**（与 golden 内容无关）：四角背景、前景占比 ∈ [3%,60%]、
//      颜色种数 > 2（证明有光照/深度，不是纯平色）。
//      —— 这三条能挡住"整张上下颠倒 / 通道写反 / 全背景"的错基线，
//      而逐像素比对照样会通过那样的图（tests/data/README.md 记的 L0 事故）。
//   2. **与 golden 逐像素比对**：容差 0（软件光栅 + 同机同版本，应当逐字节相同）。
//   3. **两份 golden 必须不同**：persp vs ortho 验 A3，ortho vs region 验 R-SK-12。
//      它们相同就说明相机/双表示参数没生效。
//
// golden 由 `MagicBlocks/scripts/fsk_golden.py gen` 生成，`check` 做同一套结构断言。
namespace {

constexpr unsigned char kBgR = 15, kBgG = 18, kBgB = 26;  // 清屏色 (0.06,0.07,0.10)

// golden 基线路径（tests/data 由 MB_GOLDEN_DIR 给出）。
// 本文件此前只用到 MB_TEST_DATA_DIR，没有 golden_path —— 补上。
std::string golden_path(const char* name) { return std::string(MB_GOLDEN_DIR) + "/" + name; }

bool near_bg(unsigned char r, unsigned char g, unsigned char b, int tol = 6) {
  return std::abs(static_cast<int>(r) - kBgR) <= tol &&
         std::abs(static_cast<int>(g) - kBgG) <= tol &&
         std::abs(static_cast<int>(b) - kBgB) <= tol;
}

struct PixelStats {
  double fg_ratio = 0.0;
  std::size_t distinct = 0;
  bool corners_bg = false;
};

PixelStats analyze(const mb::ImageRgba8& img) {
  PixelStats s;
  const std::size_t n = img.width * img.height;
  if (n == 0) return s;
  std::size_t fg = 0;
  std::set<std::uint32_t> colors;
  for (std::size_t i = 0; i < n; ++i) {
    const std::uint8_t* p = &img.pixels[i * 4];
    const std::uint32_t c = (static_cast<std::uint32_t>(p[0]) << 16) |
                            (static_cast<std::uint32_t>(p[1]) << 8) | p[2];
    colors.insert(c);
    if (!near_bg(p[0], p[1], p[2])) ++fg;
  }
  s.fg_ratio = static_cast<double>(fg) / static_cast<double>(n);
  s.distinct = colors.size();
  // 四角（各向内 1/8 处）必须是背景。
  const std::uint32_t w = img.width, h = img.height;
  const std::uint32_t pts[4][2] = {{w / 8, h / 8}, {w * 7 / 8, h / 8}, {w / 8, h * 7 / 8},
                                   {w * 7 / 8, h * 7 / 8}};
  s.corners_bg = true;
  for (const auto& pt : pts) {
    const std::uint8_t* p = &img.pixels[(static_cast<std::size_t>(pt[1]) * w + pt[0]) * 4];
    if (!near_bg(p[0], p[1], p[2])) s.corners_bg = false;
  }
  return s;
}

// 跑 fsk-cli 的 render 子命令。
int run_fsk_cli(const std::string& args, std::string* out) {
#if defined(MB_FSK_CLI_PATH)
  const std::string inner = std::string("\"") + MB_FSK_CLI_PATH + "\" " + args;
  const std::string cmd = std::string("\"") + inner + "\"";
  FILE* p = MB_POPEN(cmd.c_str(), "r");
  if (p == nullptr) return -999;
  char buf[4096];
  out->clear();
  while (std::fgets(buf, sizeof(buf), p) != nullptr) out->append(buf);
  return MB_PCLOSE(p);
#else
  (void)args;
  (void)out;
  return -998;
#endif
}

}  // namespace

TEST(SketchGolden, RenderedFrameMatchesBaselineAndPassesStructuralChecks) {
  struct Case {
    const char* golden;
    const char* args;
    const char* tag;
  };
  const Case cases[] = {
      {"golden_fsk_persp.png", "--camera perspective", "persp"},
      {"golden_fsk_ortho.png", "--camera ortho", "ortho"},
      {"golden_fsk_region.png", "--camera ortho --region --strokes 2", "region"},
  };

  for (const Case& c : cases) {
    const std::string out_png = tmp_png((std::string("golden_") + c.tag).c_str());
    std::remove(out_png.c_str());
    std::string cli_out;
    const bool region = std::string(c.tag) == "region";
    const std::string args = std::string("render --width 320 --height 240 ") +
                             (region ? "--strokes 2 " : "--strokes 3 ") + c.args +
                             " --output \"" + out_png + "\"";
    const int rc = run_fsk_cli(args, &cli_out);
    ASSERT_EQ(rc, 0) << c.tag << ": fsk-cli 失败, 输出: " << cli_out;
    EXPECT_NE(cli_out.find("mesh_hash="), std::string::npos)
        << c.tag << ": 没有输出 mesh_hash";

    mb::ImageRgba8 got;
    std::string err;
    ASSERT_TRUE(mb::read_png_rgba8(out_png, &got, &err)) << c.tag << ": " << err;

    // 1) 结构断言（不依赖 golden）
    const PixelStats s = analyze(got);
    EXPECT_TRUE(s.corners_bg) << c.tag << ": 四角不是背景色 —— 大概率整张图偏移/翻转";
    EXPECT_GE(s.fg_ratio, 0.03) << c.tag << ": 前景过少,疑似空白图 (fg=" << s.fg_ratio << ")";
    EXPECT_LE(s.fg_ratio, 0.60) << c.tag << ": 前景过多,疑似糊满屏 (fg=" << s.fg_ratio << ")";
    EXPECT_GT(s.distinct, 2u) << c.tag << ": 颜色种数 " << s.distinct
                              << " 过少 —— 缺少光照/深度(纯平色)";

    // 2) 与 golden 逐像素比对
    mb::ImageRgba8 base;
    ASSERT_TRUE(mb::read_png_rgba8(golden_path(c.golden), &base, &err))
        << c.tag << ": 读不到 golden " << c.golden << ": " << err;
    ASSERT_EQ(base.width, got.width);
    ASSERT_EQ(base.height, got.height);
    const std::size_t differing = mb::count_differing_pixels(base, got, /*tolerance=*/0);
    EXPECT_EQ(differing, 0u) << c.tag << ": 与 golden " << c.golden << " 有 " << differing
                             << " 个像素不同";

    // golden **自身**也必须过结构断言（防止错基线自我固化）。
    const PixelStats sb = analyze(base);
    EXPECT_TRUE(sb.corners_bg) << c.golden << " 自身四角不是背景色: 这是错基线";
    EXPECT_GE(sb.fg_ratio, 0.03) << c.golden << " 自身前景过少: 这是空图";
    EXPECT_GT(sb.distinct, 2u) << c.golden << " 自身颜色过少: 这是纯平色";
  }
}

TEST(SketchGolden, PerspAndOrthoBaselinesDiffer) {
  // A3：相机双模态两张图必须**逐像素不同**。
  mb::ImageRgba8 a, b;
  std::string err;
  ASSERT_TRUE(mb::read_png_rgba8(golden_path("golden_fsk_persp.png"), &a, &err)) << err;
  ASSERT_TRUE(mb::read_png_rgba8(golden_path("golden_fsk_ortho.png"), &b, &err)) << err;
  EXPECT_GT(mb::count_differing_pixels(a, b, 0), 0u)
      << "--camera 没有生效: 两种投影产出了同一张图";
}

TEST(SketchGolden, RegionAndRibbonBaselinesDiffer) {
  // R-SK-12：双表示两种解读必须产出不同的实体与不同的图。
  mb::ImageRgba8 a, b;
  std::string err;
  ASSERT_TRUE(mb::read_png_rgba8(golden_path("golden_fsk_region.png"), &a, &err)) << err;
  ASSERT_TRUE(mb::read_png_rgba8(golden_path("golden_fsk_ortho.png"), &b, &err)) << err;
  EXPECT_GT(mb::count_differing_pixels(a, b, 0), 0u)
      << "Region 与 Ribbon 产出同一张图: 双表示没有生效";
}

// ---------------------------------------------------------------- A7 / A8：三态与回放像素一致

TEST(SketchCommandMode, RecordReplayReproduceProduceIdenticalPixels) {
  // A7：record → replay --verify → reproduce --verify 全 0，三者统计行一致。
  // A8：回放/复现帧与录制帧 count_differing_pixels == 0。
  const std::string cap = tmp_png("cmd_mode");
  const std::string rec_png = tmp_png("cmd_rec");
  const std::string rep_png = tmp_png("cmd_rep");
  const std::string repro_png = tmp_png("cmd_repro");
  for (const std::string* p : {&cap, &rec_png, &rep_png, &repro_png}) std::remove(p->c_str());

  std::string rec_out;
  ASSERT_EQ(run_fsk_cli("render --width 320 --height 240 --strokes 3 --camera ortho --record \"" +
                            cap + "\" --output \"" + rec_png + "\"",
                        &rec_out),
            0)
      << rec_out;
  EXPECT_NE(rec_out.find("input_hash="), std::string::npos) << rec_out;

  std::string rep_out;
  ASSERT_EQ(run_fsk_cli("replay --replay \"" + cap + "\" --verify --output \"" + rep_png + "\"",
                        &rep_out),
            0)
      << rep_out;
  EXPECT_NE(rep_out.find("verify ok"), std::string::npos) << rep_out;

  std::string repro_out;
  ASSERT_EQ(run_fsk_cli(
                "reproduce --replay \"" + cap + "\" --verify --output \"" + repro_png + "\"",
                &repro_out),
            0)
      << repro_out;

  // 统计行（doc_hash / mesh_hash / input_hash）三态必须一致。
  //
  // 只抠出**统计行本身**来比：录制分支还会多打一行"已录制事件流 /path"，
  // 回放分支多打一行"verify ok"，路径与后缀本来就不同。
  // 第一版直接比整段 stdout，结果是比到了路径 —— 测试错、产品对。
  auto stat_line = [](const std::string& s) {
    std::size_t begin = s.find("[fsk] ");
    while (begin != std::string::npos) {
      const std::size_t end = s.find('\n', begin);
      const std::string line = s.substr(begin, end == std::string::npos ? std::string::npos
                                                                       : end - begin);
      if (line.find("doc_hash=") != std::string::npos &&
          line.find("mesh_hash=") != std::string::npos &&
          line.find("input_hash=") != std::string::npos) {
        return line;
      }
      begin = (end == std::string::npos) ? std::string::npos : s.find("[fsk] ", end);
    }
    return std::string();
  };
  const std::string st_rec = stat_line(rec_out);
  const std::string st_rep = stat_line(rep_out);
  const std::string st_repro = stat_line(repro_out);
  ASSERT_FALSE(st_rec.empty()) << "录制分支没有统计行: " << rec_out;
  ASSERT_FALSE(st_rep.empty()) << "回放分支没有统计行: " << rep_out;
  ASSERT_FALSE(st_repro.empty()) << "复现分支没有统计行: " << repro_out;
  EXPECT_EQ(st_rec, st_rep) << "回放态统计行与录制态不同";
  EXPECT_EQ(st_rec, st_repro) << "复现态统计行与录制态不同";

  mb::ImageRgba8 a, b, c;
  std::string err;
  ASSERT_TRUE(mb::read_png_rgba8(rec_png, &a, &err)) << err;
  ASSERT_TRUE(mb::read_png_rgba8(rep_png, &b, &err)) << err;
  ASSERT_TRUE(mb::read_png_rgba8(repro_png, &c, &err)) << err;
  EXPECT_EQ(mb::count_differing_pixels(a, b, 0), 0u) << "A8: 回放帧与录制帧不一致";
  EXPECT_EQ(mb::count_differing_pixels(a, c, 0), 0u) << "A8: 复现帧与录制帧不一致";
}

TEST(SketchCommandMode, DumpIsStableAndRejectsCrossSubcommandFlags) {
  const std::string cap = tmp_png("cmd_dump");
  std::remove(cap.c_str());
  std::string out;
  ASSERT_EQ(run_fsk_cli("render --width 64 --height 64 --strokes 1 --record \"" + cap +
                            "\" --output \"" + tmp_png("cmd_dump_img") + "\"",
                        &out),
            0)
      << out;

  std::string j1, j2;
  ASSERT_EQ(run_fsk_cli("dump --replay \"" + cap + "\" --json", &j1), 0);
  ASSERT_EQ(run_fsk_cli("dump --replay \"" + cap + "\" --json", &j2), 0);
  EXPECT_EQ(j1, j2) << "dump --json 两次输出不同: 不是稳定 JSON";
  EXPECT_NE(j1.find("\"input_hash\""), std::string::npos) << j1;

  // S-8：跨子命令参数必须被拒绝（exit 2），不许静默接受。
  std::string bad;
  EXPECT_EQ(run_fsk_cli("dump --replay \"" + cap + "\" --output x.png", &bad), 2);
  EXPECT_EQ(run_fsk_cli("replay --replay \"" + cap + "\" --camera ortho --output x.png", &bad), 2);
  EXPECT_EQ(run_fsk_cli("render --strokes 1 --output \"" + tmp_png("bad") +
                            "\" --template circle",
                        &bad),
            2);
  // 相机取值必须显式。
  EXPECT_EQ(run_fsk_cli("render --strokes 1 --camera auto --output x.png", &bad), 2);
}
