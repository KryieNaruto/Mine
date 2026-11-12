// MagicBlocks —— Android 场景渲染器的**顶点布局契约**（宿主机可测）
//
// 为什么要有这份测试：
//
//   pad 上点预设模型时，屏幕上曾经是"三角面片全炸"——兔子的每个三角形三个角
//   各自飞到毫不相干的位置。根因不是模型文件、也不是加载器，而是**管线布局选错**：
//   内置立方体顶点 36 字节（pos3+nrm3+col3 全 float），模型网格顶点 48 字节
//   （pos3+pad / nrm3+pad / RGBA8+uv2+pad，见 primitives/mesh.h）。两者共用
//   一条 36 字节步长的管线，GPU 从第二个顶点起每个字段都读在错位的字节上 ——
//   而且**不报任何错**，画面上就是一堆碎片。
//
//   这类错误编译期看不出来、运行期不崩，只能靠断言钉住。所以这里把
//   SceneRenderer 的两条管线描述取出来逐字段比对：
//
//     * 模型管线必须与 primitives::mesh_vertex_layout() 完全一致
//       （stride / 每个 location 的格式与偏移）；
//     * 模型管线绝不能与立方体管线相同（相同就等于退化回那条炸掉的路径）；
//     * 两条管线除顶点输入布局外必须一致（同一份着色器/采样数/push constant），
//       否则会出现"采样数对不上 render pass → vkCmdDraw 被静默丢弃"这类
//       更难查的症状。
//
// 这两个函数原本住在 scene_renderer.cpp 的匿名 namespace 里（测试看不到）。
// 2026-11-12 把它们挪进了 scene_renderer.h 的声明 + magicblocks_android_scene
// 目标：JNI 的 .so 与这份测试链接的是**同一份实现**，不会"测的是另一份代码"。
#include <gtest/gtest.h>

#include <cstddef>
#include <string>

#include "android/scene_renderer.h"
#include "magicblocks/primitives/mesh.h"

namespace mba = magicblocks::android;
namespace mbp = magicblocks::primitives;
namespace res = magicblocks::resources;

namespace {

// 把 VertexLayout 压成一行可读文本，断言失败时一眼看出差在哪。
std::string describe(const res::VertexLayout& layout) {
  std::string s =
      "stride=" + std::to_string(layout.bindings.empty() ? 0u : layout.bindings[0].stride);
  for (const auto& a : layout.attributes) {
    s += " [" + std::to_string(a.location) + "/" + std::to_string(static_cast<int>(a.format)) +
         "/" + std::to_string(a.offset) + "]";
  }
  return s;
}

const res::VertexAttribute* find_location(const res::VertexLayout& layout, res::u32 loc) {
  for (const auto& a : layout.attributes) {
    if (a.location == loc) return &a;
  }
  return nullptr;
}

}  // namespace

// ---------------------------------------------------------------- 模型管线

// 模型管线的顶点输入布局必须**逐字节**等于 L3 网格的真实布局。
//
// 这条断言就是"三角面片全炸"的守门人：只要有人把 stride 改回 36、把颜色属性
// 改回 vec3、或者给 mesh.h 加了字段却忘了同步这里，它立刻红。
TEST(SceneRendererModelPipeline, ModelLayoutMatchesMeshVertexLayout) {
  const res::PipelineDesc desc = mba::model_pipeline_desc(1);
  const res::VertexLayout expected = mbp::mesh_vertex_layout();

  ASSERT_EQ(desc.vertexLayout.bindings.size(), expected.bindings.size())
      << "binding 数量不一致: " << describe(desc.vertexLayout) << " vs " << describe(expected);
  EXPECT_EQ(desc.vertexLayout.bindings[0].stride, expected.bindings[0].stride)
      << "stride 不一致（模型顶点是 48 字节，不是立方体的 36 字节）: "
      << describe(desc.vertexLayout) << " vs " << describe(expected);
  EXPECT_EQ(desc.vertexLayout.bindings[0].stride, sizeof(mbp::Vertex));
  EXPECT_EQ(desc.vertexLayout.bindings[0].binding, 0u);
  EXPECT_EQ(desc.vertexLayout.bindings[0].instanced, expected.bindings[0].instanced);

  // 着色器（cube.vert）只声明了 location 0/1/2。多声明的 location 3(uv) 没有
  // 对应输入，驱动会静默忽略 —— 宁可不声明。
  ASSERT_EQ(desc.vertexLayout.attributes.size(), 3u);
  for (std::size_t i = 0; i < desc.vertexLayout.attributes.size(); ++i) {
    EXPECT_EQ(desc.vertexLayout.attributes[i].location, i);
  }

  for (const auto& got : desc.vertexLayout.attributes) {
    const res::VertexAttribute* want = find_location(expected, got.location);
    ASSERT_NE(want, nullptr) << "模型/网格布局里没有 location " << got.location;
    EXPECT_EQ(got.format, want->format)
        << "location " << got.location << " 的格式不一致: " << describe(desc.vertexLayout);
    EXPECT_EQ(got.offset, want->offset)
        << "location " << got.location << " 的偏移不一致: " << describe(desc.vertexLayout);
    EXPECT_EQ(got.binding, 0u);
  }

  // 三个字段的真实偏移摊开写一遍，让"期望值"在测试里可读。
  EXPECT_EQ(offsetof(mbp::Vertex, position), 0u);
  EXPECT_EQ(offsetof(mbp::Vertex, normal), 16u);
  EXPECT_EQ(offsetof(mbp::Vertex, color), 32u);
}

// 颜色属性是 RGBA8 归一化，**不是** vec3。
// 格式错了不会崩，只会把邻居顶点的字节当颜色读（"颜色随机但形状正常"），
// 比形状错更难发现，所以单独钉一条。
TEST(SceneRendererModelPipeline, ColorAttributeIsUByte4Norm) {
  const res::PipelineDesc desc = mba::model_pipeline_desc(1);
  const res::VertexAttribute* col = find_location(desc.vertexLayout, 2);
  ASSERT_NE(col, nullptr) << "模型管线缺少 location 2（颜色）";
  EXPECT_EQ(col->format, res::VertexFormat::UByte4Norm);
  EXPECT_EQ(col->offset, offsetof(mbp::Vertex, color));
}

// ---------------------------------------------------------------- 立方体管线

TEST(SceneRendererCubePipeline, CubeLayoutIsThirtySixByteInterleaved) {
  const res::PipelineDesc desc = mba::cube_pipeline_desc(1);
  ASSERT_EQ(desc.vertexLayout.bindings.size(), 1u);
  EXPECT_EQ(desc.vertexLayout.bindings[0].stride, sizeof(mba::Vertex));
  EXPECT_EQ(sizeof(mba::Vertex), 36u) << "内置立方体顶点是 36 字节（9 个 float）";

  ASSERT_EQ(desc.vertexLayout.attributes.size(), 3u);
  EXPECT_EQ(desc.vertexLayout.attributes[0].format, res::VertexFormat::Float3);  // position
  EXPECT_EQ(desc.vertexLayout.attributes[1].format, res::VertexFormat::Float3);  // normal
  EXPECT_EQ(desc.vertexLayout.attributes[2].format, res::VertexFormat::Float3);  // color
  EXPECT_EQ(desc.vertexLayout.attributes[2].offset, offsetof(mba::Vertex, r));
}

// ---------------------------------------------------------------- 两条管线的差异

// 模型管线**必须**与立方体管线不同：vertexLayout 相等就等于退回
// 2026-11-12 修掉的那条路径（36 字节步长读 48 字节数据 → 三角形爆炸）。
TEST(SceneRendererModelPipeline, ModelDescDiffersFromCubeDescOnlyInVertexLayout) {
  const res::PipelineDesc cube = mba::cube_pipeline_desc(1);
  const res::PipelineDesc model = mba::model_pipeline_desc(1);

  bool layout_differs = cube.vertexLayout.bindings.size() != model.vertexLayout.bindings.size() ||
                        cube.vertexLayout.attributes.size() !=
                            model.vertexLayout.attributes.size();
  if (!layout_differs && !cube.vertexLayout.bindings.empty()) {
    layout_differs = cube.vertexLayout.bindings[0].stride != model.vertexLayout.bindings[0].stride;
  }
  EXPECT_TRUE(layout_differs)
      << "模型管线与立方体管线的顶点布局完全相同 —— 那就是'三角面片全炸'的回归: "
      << describe(model.vertexLayout);

  // 除布局以外其余字段必须一致：同一份着色器、同样的采样数、同样的 push constant。
  ASSERT_EQ(cube.stages.size(), model.stages.size());
  ASSERT_EQ(cube.stages.size(), 2u) << "必须恰好是 Vertex + Fragment 两个阶段";
  for (std::size_t i = 0; i < cube.stages.size(); ++i) {
    EXPECT_EQ(cube.stages[i].stage, model.stages[i].stage);
    EXPECT_EQ(cube.stages[i].spirv, model.stages[i].spirv)
        << "两条管线必须共用 cube.vert / cube.frag";
    EXPECT_EQ(cube.stages[i].size_bytes, model.stages[i].size_bytes);
  }
  EXPECT_EQ(cube.samples, model.samples);
  EXPECT_EQ(cube.push_constant_bytes, model.push_constant_bytes);
  EXPECT_EQ(cube.depth.depthTest, model.depth.depthTest);
  EXPECT_EQ(cube.depth.depthWrite, model.depth.depthWrite);
  EXPECT_EQ(cube.primitive, model.primitive);
  EXPECT_EQ(sizeof(mba::PushConstants), 128u) << "push constant 是两个 mat4";
  EXPECT_EQ(cube.push_constant_bytes, 128u);
}

// 采样数必须原样传到两条管线。
//
// 交换链真实生效 4x、管线却按 1x 建时，管线与 render pass 的逐项兼容性检查会
// 失败，vkCmdDraw 被**静默丢弃**：屏幕只剩清屏色，没有任何报错。所以管线描述
// 里的 samples 必须跟着参数走，不能在某个分支里写死。
TEST(SceneRendererModelPipeline, SamplesPropagateToBothPipelines) {
  for (const res::u32 s : {1u, 4u, 8u}) {
    EXPECT_EQ(mba::cube_pipeline_desc(s).samples, s) << "cube samples=" << s;
    EXPECT_EQ(mba::model_pipeline_desc(s).samples, s) << "model samples=" << s;
  }
}

// 布局不能为空：空 vertexLayout 在 Vulkan 里是合法的（"管线不读顶点"），
// 于是模型会缩成一个点 —— 同样不报错。
TEST(SceneRendererModelPipeline, LayoutIsNotEmpty) {
  const res::PipelineDesc desc = mba::model_pipeline_desc(1);
  EXPECT_FALSE(desc.vertexLayout.bindings.empty());
  EXPECT_FALSE(desc.vertexLayout.attributes.empty());
}
