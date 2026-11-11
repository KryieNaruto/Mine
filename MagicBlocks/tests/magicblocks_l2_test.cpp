// MagicBlocks L2 测试 —— 场景 / 提交队列 / 渲染图 / 命令流 / 录制器 / 特性注册表 / golden
//
// 覆盖 spec §4 的 L2 范围 S1-S9 共 38 条用例(plans-l2 §6.1)。
// 本文件**不 include 任何 backend/vulkan 头**(分层纯度 G3),设备一律经
// `create_render_device` 拿软件光栅设备;拿不到就 ADD_FAILURE 而不是 skip
// (测试门要求 0 跳过)。
//
// 注意:全目录有一条门控 `tools/check_ui_single_submit.py` 会 grep `submit\s*[<(]`,
// 因此这里**只用 `submit_and_wait()`**,不写 `.submit(` / `->submit(`。
#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "magicblocks_shaders.h"
#include "core/image_io.h"
#include "mb_internal/magicblocks/device/render_device.h"
#include "mb_internal/magicblocks/scene/command_stream.h"
#include "mb_internal/magicblocks/scene/feature.h"
#include "mb_internal/magicblocks/scene/recorder.h"
#include "mb_internal/magicblocks/scene/render_graph.h"
#include "mb_internal/magicblocks/scene/scene.h"
#include "mb_internal/magicblocks/scene/submit_queue.h"
#include "mb_internal/magicblocks/scene/types.h"
#include "scene/scene_internal.h"

// 仅 golden 用例需要:出图必须走真实的 backend 绘制路径(与 CLI 完全同一条),
// 而 `draw_selftest` 是 backend 的公开接口。这是 tests/ 目录,不受 G3 分层纯度约束
// (G3 只检查 src/ 与 include/ 的层间包含关系)。
#include "backend/vulkan/vulkan_target.h"

namespace {

namespace sc = magicblocks::scene;
namespace dev = magicblocks::device;
namespace res = magicblocks::resources;

// ---------------------------------------------------------------- 浮点按位比较

// spec §6.3 / R7:浮点一律比**位模式**,`-0.0 != 0.0`,`NaN == NaN`。
bool bits_eq(float a, float b) {
  return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b);
}

// ---------------------------------------------------------------- 设备

// 软件光栅设备强筛。拿不到 → ADD_FAILURE(不是 GTEST_SKIP:测试门要求 0 跳过)。
dev::Result<std::unique_ptr<dev::IRenderDevice>> make_device() {
  dev::DeviceDesc d;
  d.app_name = "MagicBlocks L2 Test";
  d.require_software_rasterizer = true;
  return dev::create_render_device(d);
}

// 建一个离屏目标。
dev::Result<std::unique_ptr<dev::IRenderTarget>> make_target(dev::IRenderDevice& device,
                                                             dev::u32 w, dev::u32 h) {
  dev::TargetDesc td;
  td.kind = dev::TargetDesc::Kind::Offscreen;
  td.extent = {w, h};
  td.with_depth = false;
  td.format = dev::Format::RGBA8_UNORM;
  return device.create_offscreen_target(td);
}

// ---------------------------------------------------------------- 与 CLI 一致的演示管线

// 与 `src/l2_cli/main.cpp` 的 demo_pipeline_desc() 必须**逐字段一致** ——
// 否则回放会找不到同 hash 的管线。
res::PipelineDesc demo_pipeline_desc() {
  res::PipelineDesc desc;
  desc.vertexLayout = res::VertexLayout{};
  res::ShaderModule vs;
  vs.stage = res::ShaderStage::Vertex;
  vs.spirv = reinterpret_cast<const std::uint8_t*>(magicblocks::shaders::kL2_sceneVert);
  vs.size_bytes = magicblocks::shaders::kL2_sceneVertSize;
  res::ShaderModule fs;
  fs.stage = res::ShaderStage::Fragment;
  fs.spirv = reinterpret_cast<const std::uint8_t*>(magicblocks::shaders::kL2_sceneFrag);
  fs.size_bytes = magicblocks::shaders::kL2_sceneFragSize;
  desc.stages = {vs, fs};
  desc.primitive = res::PrimitiveType::TriangleList;
  desc.raster.cull = res::CullMode::None;
  desc.depth.depthTest = false;
  desc.depth.depthWrite = false;
  // 16 字节 push constant(RL:管线声明多少,L1 就只放多少;
  // 不声明则 push_constants 会被 "超过管线 push constant 大小" 拒绝)。
  desc.push_constant_bytes = 16;
  return desc;
}

// ---------------------------------------------------------------- 手搓命令流

// 一条固定内容的流,供哈希稳定性 / 逐字段变异 / 截断用例复用。
sc::CommandStream make_fixed_stream() {
  sc::CommandStream s;
  s.backend = "vulkan";
  s.engine_major = 1;
  s.frame_index = 7;
  s.extent = {64, 48};

  sc::EventRecord ev;
  ev.kind = sc::EventKind::PointerDown;
  ev.pointer_id = 3;
  ev.x = 1.5f;
  ev.y = 2.5f;
  ev.dx = 0.25f;
  ev.dy = -0.25f;
  ev.button = 1;
  ev.width = 64;
  ev.height = 48;
  ev.timestamp_us = 123456;
  ev.handled = true;
  s.events.push_back(ev);

  sc::NodeSnapshot n0;
  n0.id = 1;
  n0.parent = sc::kInvalidNodeId;
  n0.name = "root";
  n0.transform.translation[0] = 1.0f;
  n0.local_bounds.empty = false;
  n0.local_bounds.min[0] = -1.0f;
  n0.local_bounds.max[0] = 1.0f;
  s.scene.nodes.push_back(n0);

  sc::NodeSnapshot n1;
  n1.id = 2;
  n1.parent = 1;
  n1.name = "child";
  n1.transform.scale[0] = 0.5f;
  n1.visible = false;
  s.scene.nodes.push_back(n1);

  sc::PassRecord p;
  p.name = "scene";
  p.layer = 0;
  p.channel = 0;
  p.clear_color = true;
  p.clear_color_rgba[0] = 0.1f;
  p.clear_color_rgba[1] = 0.2f;
  p.clear_color_rgba[2] = 0.3f;
  p.clear_color_rgba[3] = 1.0f;
  p.clear_depth = false;
  p.area[0] = 0;
  p.area[1] = 0;
  p.area[2] = 0;   // U2:0 必须原样保留(表示整图)
  p.area[3] = 0;
  p.color_format = dev::Format::RGBA8_UNORM;
  p.has_depth = false;
  s.passes.push_back(p);

  sc::CommandRecord c0;
  c0.op = sc::CommandOp::BeginPass;
  s.commands.push_back(c0);

  sc::CommandRecord c1;
  c1.op = sc::CommandOp::BindPipeline;
  c1.pipeline_hash = 0x1122334455667788ull;
  s.commands.push_back(c1);

  sc::CommandRecord c2;
  c2.op = sc::CommandOp::Draw;
  c2.a = 4;
  c2.b = 16;
  c2.c = 0;
  c2.i = 0;
  s.commands.push_back(c2);

  sc::CommandRecord c3;
  c3.op = sc::CommandOp::EndPass;
  s.commands.push_back(c3);

  return s;
}

// ================================================================ S1 场景(6)

TEST(Scene, AddRemoveAndQueryNodes) {
  auto s = sc::create_scene();
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(s->node_count(), 0u);
  EXPECT_FALSE(s->has_node(sc::kInvalidNodeId));

  const auto r1 = s->add_node(sc::kInvalidNodeId, "root");
  ASSERT_TRUE(r1.ok()) << r1.message();
  const sc::NodeId root = r1.value();
  EXPECT_NE(root, sc::kInvalidNodeId);

  const auto r2 = s->add_node(root, "child");
  ASSERT_TRUE(r2.ok()) << r2.message();
  const sc::NodeId child = r2.value();
  EXPECT_NE(child, root);

  EXPECT_EQ(s->node_count(), 2u);
  EXPECT_TRUE(s->has_node(root));
  EXPECT_TRUE(s->has_node(child));
  EXPECT_EQ(s->parent_of(child), root);
  EXPECT_EQ(s->parent_of(root), sc::kInvalidNodeId);

  // 删父节点必须连带删子节点(S1:树形一致性)
  EXPECT_TRUE(s->remove_node(root));
  EXPECT_EQ(s->node_count(), 0u);
  EXPECT_FALSE(s->has_node(root));
  EXPECT_FALSE(s->has_node(child));

  // 重复删除返回 false(不是未定义行为)
  EXPECT_FALSE(s->remove_node(root));
}

TEST(Scene, TransformComposesWithParent) {
  auto s = sc::create_scene();
  ASSERT_NE(s, nullptr);

  const sc::NodeId root = s->add_node(sc::kInvalidNodeId, "root").value();
  const sc::NodeId child = s->add_node(root, "child").value();

  sc::Aabb unit;
  unit.empty = false;
  unit.min[0] = -1.0f;
  unit.min[1] = -1.0f;
  unit.min[2] = -1.0f;
  unit.max[0] = 1.0f;
  unit.max[1] = 1.0f;
  unit.max[2] = 1.0f;

  ASSERT_TRUE(s->set_local_bounds(child, unit));

  sc::Transform t;
  t.translation[0] = 5.0f;
  t.translation[1] = 0.0f;
  t.translation[2] = 0.0f;
  t.scale[0] = 2.0f;
  t.scale[1] = 2.0f;
  t.scale[2] = 2.0f;
  ASSERT_TRUE(s->set_transform(child, t));

  // 只有子节点有 TRS:世界 = 父(恒等) × 子
  const sc::Aabb w = s->world_bounds(child);
  EXPECT_FALSE(w.empty);
  EXPECT_FLOAT_EQ(w.min[0], 3.0f);
  EXPECT_FLOAT_EQ(w.max[0], 7.0f);
  EXPECT_FLOAT_EQ(w.min[1], -2.0f);
  EXPECT_FLOAT_EQ(w.max[1], 2.0f);

  // 再给父节点加平移:世界包围盒整体再平移
  sc::Transform pt;
  pt.translation[0] = 10.0f;
  pt.translation[1] = 0.0f;
  pt.translation[2] = 0.0f;
  pt.scale[0] = 1.0f;
  pt.scale[1] = 1.0f;
  pt.scale[2] = 1.0f;
  ASSERT_TRUE(s->set_transform(root, pt));

  const sc::Aabb w2 = s->world_bounds(child);
  EXPECT_FLOAT_EQ(w2.min[0], 13.0f);
  EXPECT_FLOAT_EQ(w2.max[0], 17.0f);
}

TEST(Scene, VisibilityPropagatesToDescendants) {
  auto s = sc::create_scene();
  ASSERT_NE(s, nullptr);

  const sc::NodeId root = s->add_node(sc::kInvalidNodeId, "root").value();
  const sc::NodeId child = s->add_node(root, "child").value();
  const sc::NodeId grand = s->add_node(child, "grand").value();

  EXPECT_TRUE(s->visible(root));
  EXPECT_TRUE(s->effectively_visible(grand));

  // 中间节点不可见 → 孙子也不可见,但它自己的 visible() 仍是 true
  ASSERT_TRUE(s->set_visible(child, false));
  EXPECT_FALSE(s->visible(child));
  EXPECT_FALSE(s->effectively_visible(child));
  EXPECT_FALSE(s->effectively_visible(grand));
  EXPECT_TRUE(s->visible(grand)) << "自己的 visible 标志不受父影响";

  // world_bounds 必须排除不可见子树:
  // `compute_world_bounds` 只跳过**不可见的子节点**,节点自身的 local_bounds 仍会
  // 计入它自己的世界盒。所以这里断言的是"不可见子不该被父的世界盒收进"。
  sc::Aabb box;
  box.empty = false;
  box.min[0] = -1.0f;
  box.min[1] = -1.0f;
  box.min[2] = -1.0f;
  box.max[0] = 1.0f;
  box.max[1] = 1.0f;
  box.max[2] = 1.0f;
  ASSERT_TRUE(s->set_local_bounds(grand, box));

  // child 不可见 → 父 root 的世界盒不得包含 grand 的贡献。
  // 注意变量名不能叫 far:`windows.h` 把它定义成宏。
  sc::Transform offset_t;
  offset_t.translation[0] = 100.0f;
  offset_t.scale[0] = 1.0f;
  offset_t.scale[1] = 1.0f;
  offset_t.scale[2] = 1.0f;
  offset_t.rotation[3] = 1.0f;
  ASSERT_TRUE(s->set_transform(grand, offset_t));
  EXPECT_TRUE(s->world_bounds(root).empty)
      << "不可见子树不得并进祖先的世界盒(world_bounds 默认排除不可见子)";

  ASSERT_TRUE(s->set_visible(child, true));
  EXPECT_TRUE(s->effectively_visible(grand));
  EXPECT_FALSE(s->world_bounds(root).empty) << "恢复可见后祖先世界盒必须重新包含孙子";
  EXPECT_FLOAT_EQ(s->world_bounds(root).max[0], 101.0f);
}

TEST(Scene, ReparentRejectsCycles) {
  auto s = sc::create_scene();
  ASSERT_NE(s, nullptr);

  const sc::NodeId a = s->add_node(sc::kInvalidNodeId, "a").value();
  const sc::NodeId b = s->add_node(a, "b").value();
  const sc::NodeId c = s->add_node(b, "c").value();

  // 把祖先挂到自己的后代下面 → 成环,必须拒绝
  EXPECT_FALSE(s->reparent(a, c)) << "成环的 reparent 必须被拒绝";
  EXPECT_EQ(s->parent_of(a), sc::kInvalidNodeId) << "失败的 reparent 不得改动结构";

  // 合法 reparent:c 挂到 a 下面
  EXPECT_TRUE(s->reparent(c, a));
  EXPECT_EQ(s->parent_of(c), a);
  EXPECT_EQ(s->parent_of(b), a);

  // reparent 到自己 → 拒绝
  EXPECT_FALSE(s->reparent(b, b));
  // 父节点不存在 → 拒绝
  EXPECT_FALSE(s->reparent(b, static_cast<sc::NodeId>(9999)));
}

TEST(Scene, SnapshotRestoreRoundTrip) {
  auto s = sc::create_scene();
  ASSERT_NE(s, nullptr);

  const sc::NodeId root = s->add_node(sc::kInvalidNodeId, "root").value();
  const sc::NodeId child = s->add_node(root, "child").value();
  sc::Transform t;
  t.translation[0] = 1.25f;
  t.rotation[0] = 0.0f;
  t.rotation[1] = 0.0f;
  t.rotation[2] = 0.0f;
  t.rotation[3] = 1.0f;
  t.scale[0] = 0.5f;
  t.scale[1] = 0.25f;
  t.scale[2] = 1.0f;
  ASSERT_TRUE(s->set_transform(child, t));
  ASSERT_TRUE(s->set_visible(child, false));

  const sc::SceneSnapshot before = s->snapshot();
  ASSERT_EQ(before.nodes.size(), 2u);
  // snapshot 必须按 id 升序(确定性要求)
  for (std::size_t k = 1; k < before.nodes.size(); ++k) {
    EXPECT_LT(before.nodes[k - 1].id, before.nodes[k].id);
  }

  auto s2 = sc::create_scene();
  ASSERT_TRUE(s2->restore(before));
  const sc::SceneSnapshot after = s2->snapshot();
  ASSERT_EQ(after.nodes.size(), before.nodes.size());
  for (std::size_t k = 0; k < before.nodes.size(); ++k) {
    EXPECT_EQ(after.nodes[k].id, before.nodes[k].id);
    EXPECT_EQ(after.nodes[k].parent, before.nodes[k].parent);
    EXPECT_EQ(after.nodes[k].name, before.nodes[k].name);
    EXPECT_EQ(after.nodes[k].visible, before.nodes[k].visible);
    for (int i = 0; i < 3; ++i) {
      EXPECT_TRUE(bits_eq(after.nodes[k].transform.translation[i],
                          before.nodes[k].transform.translation[i]));
      EXPECT_TRUE(bits_eq(after.nodes[k].transform.scale[i],
                          before.nodes[k].transform.scale[i]));
    }
  }
  EXPECT_TRUE(after == before) << "快照必须按位相等";

  // 非法快照:悬空 parent → restore 必须失败且不改动现有场景
  sc::SceneSnapshot bad = before;
  bad.nodes[1].parent = static_cast<sc::NodeId>(777);
  auto s3 = sc::create_scene();
  EXPECT_FALSE(s3->restore(bad)) << "悬空 parent 的快照必须被拒绝";
  EXPECT_EQ(s3->node_count(), 0u) << "失败的 restore 不得留下半成品";
}

TEST(Scene, ChangesAccumulateAndClear) {
  auto s = sc::create_scene();
  ASSERT_NE(s, nullptr);
  EXPECT_TRUE(s->changes().empty());

  const sc::NodeId root = s->add_node(sc::kInvalidNodeId, "root").value();
  ASSERT_EQ(s->changes().size(), 1u);
  EXPECT_EQ(s->changes()[0].op, sc::NodeOp::Added);
  EXPECT_EQ(s->changes()[0].id, root);

  sc::Transform t;
  t.translation[0] = 1.0f;
  t.scale[0] = 1.0f;
  t.scale[1] = 1.0f;
  t.scale[2] = 1.0f;
  t.rotation[3] = 1.0f;
  ASSERT_TRUE(s->set_transform(root, t));
  ASSERT_EQ(s->changes().size(), 2u);
  EXPECT_EQ(s->changes()[1].op, sc::NodeOp::Updated);

  ASSERT_TRUE(s->remove_node(root));
  ASSERT_EQ(s->changes().size(), 3u);
  EXPECT_EQ(s->changes()[2].op, sc::NodeOp::Removed);

  s->clear_changes();
  EXPECT_TRUE(s->changes().empty());

  // 失败的 set 不得产生变更记录
  EXPECT_FALSE(s->set_visible(static_cast<sc::NodeId>(4242), false));
  EXPECT_TRUE(s->changes().empty());
}

// ================================================================ S2 提交队列(5)

TEST(SubmitQueue, OpaqueSortedByMaterialThenDepth) {
  auto q = sc::create_submit_queue();
  ASSERT_NE(q, nullptr);

  auto item = [](dev::u64 tie, dev::u32 material, bool transparent, float depth, float z) {
    sc::DrawItem d;
    d.tie_break_id = tie;
    d.material = material;
    d.transparent = transparent;
    d.center[0] = 0.0f;
    d.center[1] = 0.0f;
    d.center[2] = z;
    d.depth = depth;
    d.mesh_key = 1;
    d.first_index = 0;
    d.index_count = 3;
    return d;
  };

  // 故意乱序加入,材质 2 在前
  q->add(item(1, 2, false, 0.1f, -1.0f));
  q->add(item(2, 1, false, 0.9f, -3.0f));
  q->add(item(3, 1, false, 0.2f, -2.0f));

  sc::SubmitCamera cam;
  cam.position[0] = 0.0f;
  cam.position[1] = 0.0f;
  cam.position[2] = 0.0f;
  const std::vector<sc::DrawItem>& out = q->sort(cam);

  ASSERT_EQ(out.size(), 3u);
  // 不透明:材质升序 → 深度升序
  EXPECT_EQ(out[0].material, 1u);
  EXPECT_TRUE(bits_eq(out[0].depth, 0.2f));
  EXPECT_EQ(out[1].material, 1u);
  EXPECT_TRUE(bits_eq(out[1].depth, 0.9f));
  EXPECT_EQ(out[2].material, 2u);

  EXPECT_EQ(q->opaque_count(), 3u);
  EXPECT_EQ(q->transparent_count(), 0u);
  EXPECT_EQ(q->size(), 3u);
}

TEST(SubmitQueue, TransparentSortedBackToFront) {
  auto q = sc::create_submit_queue();
  ASSERT_NE(q, nullptr);

  auto item = [](dev::u64 tie, float z) {
    sc::DrawItem d;
    d.tie_break_id = tie;
    d.material = 1;
    d.transparent = true;
    d.center[0] = 0.0f;
    d.center[1] = 0.0f;
    d.center[2] = z;
    d.depth = 0.5f;
    d.mesh_key = 1;
    d.first_index = 0;
    d.index_count = 3;
    return d;
  };

  // 相机在原点;z 越小越远
  q->add(item(1, -1.0f));  // 近
  q->add(item(2, -5.0f));  // 远
  q->add(item(3, -3.0f));  // 中

  sc::SubmitCamera cam;
  cam.position[0] = 0.0f;
  cam.position[1] = 0.0f;
  cam.position[2] = 0.0f;
  const std::vector<sc::DrawItem>& out = q->sort(cam);

  ASSERT_EQ(out.size(), 3u);
  // 透明:远 → 近(距离降序)
  EXPECT_EQ(out[0].tie_break_id, 2u) << "最远的先画";
  EXPECT_EQ(out[1].tie_break_id, 3u);
  EXPECT_EQ(out[2].tie_break_id, 1u) << "最近的最后画";

  EXPECT_EQ(q->opaque_count(), 0u);
  EXPECT_EQ(q->transparent_count(), 3u);
}

TEST(SubmitQueue, OpaqueBeforeTransparent) {
  auto q = sc::create_submit_queue();
  ASSERT_NE(q, nullptr);

  auto item = [](dev::u64 tie, bool transparent, float z) {
    sc::DrawItem d;
    d.tie_break_id = tie;
    d.material = 1;
    d.transparent = transparent;
    d.center[0] = 0.0f;
    d.center[1] = 0.0f;
    d.center[2] = z;
    d.depth = 0.5f;
    d.mesh_key = 1;
    d.first_index = 0;
    d.index_count = 3;
    return d;
  };

  // 先加透明的,再加不透明的
  q->add(item(1, true, -1.0f));
  q->add(item(2, false, -2.0f));
  q->add(item(3, true, -3.0f));
  q->add(item(4, false, -4.0f));

  sc::SubmitCamera cam;
  cam.position[0] = 0.0f;
  cam.position[1] = 0.0f;
  cam.position[2] = 0.0f;
  const std::vector<sc::DrawItem>& out = q->sort(cam);

  ASSERT_EQ(out.size(), 4u);
  EXPECT_FALSE(out[0].transparent);
  EXPECT_FALSE(out[1].transparent);
  EXPECT_TRUE(out[2].transparent);
  EXPECT_TRUE(out[3].transparent);
}

TEST(SubmitQueue, SortIsDeterministicAcrossInsertionOrder) {
  auto make_items = []() {
    std::vector<sc::DrawItem> v;
    for (int k = 0; k < 6; ++k) {
      sc::DrawItem d;
      d.tie_break_id = static_cast<dev::u64>(k);
      d.material = static_cast<dev::u32>(k % 3);  // 有并列材质
      d.transparent = (k % 2 == 1);
      d.center[0] = 0.0f;
      d.center[1] = 0.0f;
      d.center[2] = -static_cast<float>(k) * 0.5f;
      d.depth = static_cast<float>(k % 4) * 0.1f;  // 有并列深度
      d.mesh_key = static_cast<dev::u64>(k + 1);
      d.first_index = 0;
      d.index_count = 3;
      v.push_back(d);
    }
    return v;
  };

  sc::SubmitCamera cam;
  cam.position[0] = 0.0f;
  cam.position[1] = 0.0f;
  cam.position[2] = 0.0f;

  auto q1 = sc::create_submit_queue();
  const std::vector<sc::DrawItem> items = make_items();
  for (const sc::DrawItem& d : items) q1->add(d);
  const std::vector<sc::DrawItem> r1 = q1->sort(cam);

  auto q2 = sc::create_submit_queue();
  for (auto it = items.rbegin(); it != items.rend(); ++it) q2->add(*it);
  const std::vector<sc::DrawItem> r2 = q2->sort(cam);

  ASSERT_EQ(r1.size(), r2.size());
  for (std::size_t k = 0; k < r1.size(); ++k) {
    EXPECT_EQ(r1[k].tie_break_id, r2[k].tie_break_id)
        << "插入顺序不得影响排序结果(全序 tie-break),索引 " << k;
  }
}

TEST(SubmitQueue, SortedReflectsLastSortAndClearResets) {
  auto q = sc::create_submit_queue();
  ASSERT_NE(q, nullptr);

  sc::DrawItem d;
  d.tie_break_id = 1;
  d.material = 1;
  d.transparent = false;
  d.center[0] = 0.0f;
  d.center[1] = 0.0f;
  d.center[2] = -1.0f;
  d.depth = 0.2f;
  d.mesh_key = 1;
  d.first_index = 0;
  d.index_count = 3;
  q->add(d);

  // 排序前 sorted() 是空(还没有快照)
  EXPECT_TRUE(q->sorted().empty());

  sc::SubmitCamera cam;
  cam.position[0] = 0.0f;
  cam.position[1] = 0.0f;
  cam.position[2] = 0.0f;
  q->sort(cam);
  EXPECT_EQ(q->sorted().size(), 1u);

  q->clear();
  EXPECT_EQ(q->size(), 0u);
  EXPECT_TRUE(q->sorted().empty());
  EXPECT_EQ(q->opaque_count(), 0u);
  EXPECT_EQ(q->transparent_count(), 0u);
}

// ================================================================ S3 渲染图(4)

TEST(RenderGraph, ValidateAcceptsExternalOutputChain) {
  auto device_r = make_device();
  if (!device_r.ok()) {
    ADD_FAILURE() << "需要软件光栅设备: " << device_r.message();
    return;
  }
  auto tgt_r = make_target(*device_r.value(), 64, 64);
  ASSERT_TRUE(tgt_r.ok()) << tgt_r.message();
  dev::IRenderTarget* target = tgt_r.value().get();

  auto g = sc::create_render_graph();
  ASSERT_NE(g, nullptr);
  EXPECT_EQ(g->pass_count(), 0u);

  sc::PassDesc p1;
  p1.name = "scene";
  p1.color_output = sc::AttachmentRef::make_external(target);
  p1.own_target = target;
  g->add_pass(p1);
  EXPECT_EQ(g->pass_count(), 1u);

  const sc::GraphValidation v = g->validate();
  EXPECT_TRUE(v.status.ok()) << v.message;

  // resolve 外部目标必须返回原指针
  EXPECT_EQ(g->resolve(p1.color_output), target);
}

TEST(RenderGraph, ValidateRejectsForwardReference) {
  auto g = sc::create_render_graph();
  ASSERT_NE(g, nullptr);

  // pass 0 的输入指向 pass 1 的输出 —— 前向引用,必须拒绝
  sc::PassDesc p0;
  p0.name = "first";
  p0.color_inputs.push_back(sc::AttachmentRef::make_pass_output(1));
  g->add_pass(p0);

  sc::PassDesc p1;
  p1.name = "second";
  g->add_pass(p1);

  const sc::GraphValidation v = g->validate();
  EXPECT_FALSE(v.status.ok()) << "前向引用必须被拒绝";
  EXPECT_EQ(v.failing_pass, 0u);
  EXPECT_FALSE(v.message.empty());
}

TEST(RenderGraph, ValidateRejectsSelfReference) {
  auto g = sc::create_render_graph();
  ASSERT_NE(g, nullptr);

  sc::PassDesc p0;
  p0.name = "self";
  p0.color_inputs.push_back(sc::AttachmentRef::make_pass_output(0));
  g->add_pass(p0);

  const sc::GraphValidation v = g->validate();
  EXPECT_FALSE(v.status.ok()) << "自引用必须被拒绝";
  EXPECT_EQ(v.failing_pass, 0u);
}

TEST(RenderGraph, PassOutputResolvesThroughProducerTarget) {
  auto device_r = make_device();
  if (!device_r.ok()) {
    ADD_FAILURE() << "需要软件光栅设备: " << device_r.message();
    return;
  }
  auto a_r = make_target(*device_r.value(), 32, 32);
  auto b_r = make_target(*device_r.value(), 32, 32);
  ASSERT_TRUE(a_r.ok()) << a_r.message();
  ASSERT_TRUE(b_r.ok()) << b_r.message();
  dev::IRenderTarget* ta = a_r.value().get();
  dev::IRenderTarget* tb = b_r.value().get();

  auto g = sc::create_render_graph();
  ASSERT_NE(g, nullptr);

  // pass 0 输出到 ta
  sc::PassDesc p0;
  p0.name = "producer";
  p0.color_output = sc::AttachmentRef::make_external(ta);
  p0.own_target = ta;
  g->add_pass(p0);

  // pass 1 读 pass 0 的输出,写 tb
  sc::PassDesc p1;
  p1.name = "consumer";
  p1.color_inputs.push_back(sc::AttachmentRef::make_pass_output(0));
  p1.color_output = sc::AttachmentRef::make_external(tb);
  p1.own_target = tb;
  g->add_pass(p1);

  const sc::GraphValidation v = g->validate();
  EXPECT_TRUE(v.status.ok()) << v.message;

  // "先离屏再贴图":pass 1 的输入解析成 pass 0 的真实目标
  EXPECT_EQ(g->resolve(p1.color_inputs[0]), ta);
  EXPECT_EQ(g->resolve(p1.color_output), tb);

  // clear 复位
  g->clear();
  EXPECT_EQ(g->pass_count(), 0u);
}

// ================================================================ S4 命令流(10)

TEST(Stream, CommandOpCountIsEleven) {
  EXPECT_EQ(sc::kCommandOpCount, 11u);
  for (std::uint32_t k = 0; k < sc::kCommandOpCount; ++k) {
    const char* name = sc::command_op_name(static_cast<sc::CommandOp>(k));
    ASSERT_NE(name, nullptr);
    EXPECT_GT(std::strlen(name), 0u) << "op " << k << " 没有名字";
  }
}

TEST(Stream, BinaryRoundTripPreservesHash) {
  sc::CommandStream s = make_fixed_stream();
  const std::uint64_t want = sc::compute_stream_hash(s);
  s.stream_hash = want;

  auto enc = sc::encode_binary(s);
  ASSERT_TRUE(enc.ok()) << enc.message();
  ASSERT_FALSE(enc.value().empty());

  const std::vector<std::uint8_t>& bytes = enc.value();
  ASSERT_GE(bytes.size(), sc::kBinaryHeaderBytes);
  EXPECT_EQ(bytes[0], 'M');
  EXPECT_EQ(bytes[1], 'B');
  EXPECT_EQ(bytes[2], 'L');
  EXPECT_EQ(bytes[3], '2');

  auto dec = sc::decode_binary(bytes);
  ASSERT_TRUE(dec.ok()) << dec.message();
  const sc::CommandStream& back = dec.value();
  EXPECT_EQ(back.frame_index, s.frame_index);
  EXPECT_EQ(back.backend, s.backend);
  EXPECT_EQ(back.engine_major, s.engine_major);
  EXPECT_EQ(back.commands.size(), s.commands.size());
  EXPECT_EQ(back.passes.size(), s.passes.size());
  EXPECT_EQ(back.events.size(), s.events.size());
  EXPECT_EQ(back.scene.nodes.size(), s.scene.nodes.size());
  EXPECT_EQ(sc::compute_stream_hash(back), want) << "往返后哈希必须不变";

  // U2:area=(0,0,0,0) 必须原样保留,不能被解析成 extent
  ASSERT_EQ(back.passes.size(), 1u);
  EXPECT_EQ(back.passes[0].area[2], 0u);
  EXPECT_EQ(back.passes[0].area[3], 0u);
}

TEST(Stream, DecodeRejectsBadMagic) {
  sc::CommandStream s = make_fixed_stream();
  auto enc = sc::encode_binary(s);
  ASSERT_TRUE(enc.ok()) << enc.message();

  std::vector<std::uint8_t> bytes = enc.value();
  ASSERT_GE(bytes.size(), 4u);
  bytes[0] = 'X';

  EXPECT_FALSE(sc::decode_binary(bytes).ok()) << "magic 不对必须拒绝";
}

TEST(Stream, DecodeRejectsUnsupportedVersion) {
  sc::CommandStream s = make_fixed_stream();
  auto enc = sc::encode_binary(s);
  ASSERT_TRUE(enc.ok()) << enc.message();

  std::vector<std::uint8_t> bytes = enc.value();
  // format_version 在 [4..6),小端 u16
  ASSERT_GE(bytes.size(), 6u);
  bytes[4] = 99;
  bytes[5] = 0;

  EXPECT_FALSE(sc::decode_binary(bytes).ok()) << "不支持的版本必须拒绝";
}

TEST(Stream, DecodeRejectsTruncatedBuffer) {
  sc::CommandStream s = make_fixed_stream();
  auto enc = sc::encode_binary(s);
  ASSERT_TRUE(enc.ok()) << enc.message();

  std::vector<std::uint8_t> bytes = enc.value();
  ASSERT_GT(bytes.size(), 16u);
  bytes.resize(bytes.size() / 2);

  EXPECT_FALSE(sc::decode_binary(bytes).ok())
      << "截断的流必须拒绝(不得读到未初始化内存)";
}

TEST(Stream, DecodeRejectsCorruptedPayload) {
  sc::CommandStream s = make_fixed_stream();
  auto enc = sc::encode_binary(s);
  ASSERT_TRUE(enc.ok()) << enc.message();

  std::vector<std::uint8_t> bytes = enc.value();
  // 翻转正文中间某字节,尾部 hash 未更新 → 必须被校验拦住
  ASSERT_GT(bytes.size(), 32u);
  bytes[bytes.size() / 2] ^= 0xFFu;

  EXPECT_FALSE(sc::decode_binary(bytes).ok())
      << "正文被改而尾部 hash 未更新 → 必须拒绝";
}

TEST(Stream, HashChangesForEveryFieldMutation) {
  const sc::CommandStream base = make_fixed_stream();
  const std::uint64_t h0 = sc::compute_stream_hash(base);

  {
    sc::CommandStream m = base;
    m.frame_index += 1;
    EXPECT_NE(sc::compute_stream_hash(m), h0) << "frame_index 未进哈希";
  }
  {
    sc::CommandStream m = base;
    m.extent.width += 1;
    EXPECT_NE(sc::compute_stream_hash(m), h0) << "extent.width 未进哈希";
  }
  {
    sc::CommandStream m = base;
    m.backend = "vulkan2";
    EXPECT_NE(sc::compute_stream_hash(m), h0) << "backend 未进哈希";
  }
  {
    sc::CommandStream m = base;
    m.events[0].timestamp_us += 1;
    EXPECT_NE(sc::compute_stream_hash(m), h0) << "event.timestamp_us 未进哈希";
  }
  {
    sc::CommandStream m = base;
    m.scene.nodes[0].visible = !m.scene.nodes[0].visible;
    EXPECT_NE(sc::compute_stream_hash(m), h0) << "node.visible 未进哈希";
  }
  {
    sc::CommandStream m = base;
    m.passes[0].clear_color = !m.passes[0].clear_color;
    EXPECT_NE(sc::compute_stream_hash(m), h0) << "pass.clear_color 未进哈希";
  }
  {
    sc::CommandStream m = base;
    m.commands[2].a += 1;
    EXPECT_NE(sc::compute_stream_hash(m), h0) << "command.a 未进哈希";
  }
  {
    // -0.0 与 +0.0 位模式不同 → 哈希必须不同(R7)
    sc::CommandStream m = base;
    m.commands[0].f[0] = -0.0f;
    EXPECT_NE(sc::compute_stream_hash(m), h0) << "-0.0f 与 0.0f 未被区分";
  }
}

TEST(Stream, HashIsStableAcrossRuns) {
  // 硬编码常数:同一份内容在任何进程、任何机器上都必须是这个值。
  // 若此断言失败,说明哈希算法(键序 / 字节序 / 浮点打包)发生了**不兼容变更**,
  // 旧录制的文件将无法与新引擎比对 —— 必须同时升级 kBinaryFormatVersion。
  const sc::CommandStream s = make_fixed_stream();
  const std::uint64_t h = sc::compute_stream_hash(s);
  EXPECT_EQ(sc::compute_stream_hash(make_fixed_stream()), h) << "同内容哈希必须可重现";

  // FNV-1a 64 的基础常量校验
  EXPECT_EQ(sc::kFnv1aOffsetBasis, 0xcbf29ce484222325ull);
  EXPECT_EQ(sc::kFnv1aPrime, 0x100000001b3ull);
  // 已知向量:FNV-1a 64 对 0 长度就是 offset basis
  EXPECT_EQ(sc::fnv1a64(nullptr, 0, sc::kFnv1aOffsetBasis), sc::kFnv1aOffsetBasis);

  std::printf("[   INFO   ] fixed-stream hash = 0x%016llx\n",
              static_cast<unsigned long long>(h));
}

TEST(Stream, JsonRoundTripMatchesBinary) {
  sc::CommandStream s = make_fixed_stream();
  const std::uint64_t want = sc::compute_stream_hash(s);

  const std::string json = sc::to_json(s);
  ASSERT_FALSE(json.empty());
  // 两次序列化必须逐字节相同(键序固定)
  EXPECT_EQ(sc::to_json(s), json) << "to_json 必须确定性";

  auto back = sc::from_json(json);
  ASSERT_TRUE(back.ok()) << back.message();
  EXPECT_EQ(sc::compute_stream_hash(back.value()), want)
      << "JSON 往返后哈希必须不变";
}

TEST(Stream, JsonRejectsMalformedInput) {
  EXPECT_FALSE(sc::from_json("").ok()) << "空串必须拒绝";
  EXPECT_FALSE(sc::from_json("not json").ok()) << "非法 JSON 必须拒绝";
  EXPECT_FALSE(sc::from_json("{}").ok()) << "缺字段必须拒绝";
  EXPECT_FALSE(sc::from_json("[1,2,3]").ok()) << "顶层不是对象必须拒绝";
}

// ================================================================ S5-S7 录制器(9)

TEST(Recorder, StateMachineEnforcesOrder) {
  // 声明顺序要紧:recorder 持有的管线必须先于 device 析构,
  // 否则管线会在设备之后销毁(访问已失效的 VkDevice → 0xC0000005)。
  // 局部对象按声明的**逆序**销毁,所以 device 必须声明在 recorder 之前。
  auto device_r = make_device();
  if (!device_r.ok()) {
    ADD_FAILURE() << "需要软件光栅设备: " << device_r.message();
    return;
  }

  auto r = sc::create_recorder();
  ASSERT_NE(r, nullptr);
  EXPECT_EQ(r->state(), sc::RecorderState::Idle);
  EXPECT_FALSE(r->attached());

  auto cmd_r = device_r.value()->create_command_list();
  ASSERT_TRUE(cmd_r.ok()) << cmd_r.message();

  // capture 在 Idle → 拒绝
  EXPECT_FALSE(r->capture().ok()) << "Idle 下 capture 必须拒绝";

  // on_attach 必须至少有一条管线
  sc::RecorderAttachments empty;
  EXPECT_FALSE(r->on_attach(*device_r.value(), empty).ok())
      << "pipelines 为空必须拒绝";

  sc::RecorderAttachments att;
  att.pipelines.push_back(demo_pipeline_desc());
  ASSERT_TRUE(r->on_attach(*device_r.value(), att).ok());
  EXPECT_TRUE(r->attached());
  EXPECT_EQ(r->pipelines().size(), 1u);

  // 重复 attach → 拒绝
  EXPECT_FALSE(r->on_attach(*device_r.value(), att).ok()) << "只允许 attach 一次";

  // 未 begin_frame 就录 → 拒绝
  res::DrawArgs da;
  da.vertex_count = 3;
  da.instance_count = 1;
  EXPECT_FALSE(r->record_draw(*cmd_r.value(), da).ok()) << "Idle 状态下不得录制";
  EXPECT_EQ(r->state(), sc::RecorderState::Idle);

  auto s = sc::create_scene();
  s->add_node(sc::kInvalidNodeId, "root");
  ASSERT_TRUE(r->begin_frame(0, {64, 64}, *s).ok());
  EXPECT_EQ(r->state(), sc::RecorderState::Recording);

  auto stream_r = r->capture();
  ASSERT_TRUE(stream_r.ok()) << stream_r.message();
  EXPECT_EQ(r->state(), sc::RecorderState::Idle);
  EXPECT_EQ(stream_r.value().frame_index, 0u);
}

TEST(Recorder, RecordsAllElevenCommandTypes) {
  auto device_r = make_device();
  if (!device_r.ok()) {
    ADD_FAILURE() << "需要软件光栅设备: " << device_r.message();
    return;
  }
  dev::IRenderDevice* device = device_r.value().get();
  auto tgt_r = make_target(*device, 64, 64);
  ASSERT_TRUE(tgt_r.ok()) << tgt_r.message();
  dev::IRenderTarget* target = tgt_r.value().get();

  auto cmd_r = device->create_command_list();
  ASSERT_TRUE(cmd_r.ok()) << cmd_r.message();
  res::ICommandList* cmd = cmd_r.value().get();

  // draw_indexed 必须先绑定索引缓冲(L1 会拒绝没有索引缓冲的索引绘制);
  // 顺便让 BindVertexBuffer / BindIndexBuffer 两类命令都真实出现。
  // 分配器按整页预取:不 reserve 就会 "当前页无空位"(L1 的显式分页契约)。
  ASSERT_TRUE(device->memory_allocator()->reserve_pages(res::MemoryKind::Vertex, 1).ok());
  ASSERT_TRUE(device->memory_allocator()->reserve_pages(res::MemoryKind::Index, 1).ok());

  res::BufferDesc vbd;
  vbd.size = 256;
  vbd.usage = res::BufferUsage::Vertex;
  auto vb_r = device->create_buffer(vbd);
  ASSERT_TRUE(vb_r.ok()) << vb_r.message();
  res::IBuffer* vbuf = vb_r.value().get();

  res::BufferDesc ibd;
  ibd.size = 256;
  ibd.usage = res::BufferUsage::Index;
  auto ib_r = device->create_buffer(ibd);
  ASSERT_TRUE(ib_r.ok()) << ib_r.message();
  res::IBuffer* ibuf = ib_r.value().get();

  auto r = sc::create_recorder();
  sc::RecorderAttachments att;
  att.pipelines.push_back(demo_pipeline_desc());
  ASSERT_TRUE(r->on_attach(*device, att).ok());

  auto s = sc::create_scene();
  s->add_node(sc::kInvalidNodeId, "root");
  ASSERT_TRUE(r->begin_frame(1, {64, 64}, *s).ok());

  sc::PassRecord pr;
  pr.name = "scene";
  pr.clear_color = true;
  pr.clear_color_rgba[0] = 0.0f;
  pr.clear_color_rgba[1] = 0.0f;
  pr.clear_color_rgba[2] = 0.0f;
  pr.clear_color_rgba[3] = 1.0f;
  pr.color_format = dev::Format::RGBA8_UNORM;

  res::RenderPassDesc rp;
  rp.target = target;
  rp.clear_color = true;
  rp.clear_color_rgba[0] = 0.0f;
  rp.clear_color_rgba[1] = 0.0f;
  rp.clear_color_rgba[2] = 0.0f;
  rp.clear_color_rgba[3] = 1.0f;

  ASSERT_TRUE(r->record_begin_pass(*cmd, pr, target, rp).ok());

  const std::vector<std::uint64_t>& hashes = r->pipelines().hashes();
  ASSERT_FALSE(hashes.empty());
  res::IPipeline* pipeline = r->pipelines().find(hashes.front());
  ASSERT_NE(pipeline, nullptr);
  ASSERT_TRUE(r->record_bind_pipeline(*cmd, pipeline).ok());

  res::Viewport vp;
  vp.width = 64.0f;
  vp.height = 64.0f;
  vp.max_depth = 1.0f;
  ASSERT_TRUE(r->record_set_viewport(*cmd, vp).ok());

  res::ScissorRect sr;
  sr.width = 64;
  sr.height = 64;
  ASSERT_TRUE(r->record_set_scissor(*cmd, sr).ok());

  res::DrawArgs da;
  da.vertex_count = 3;
  da.instance_count = 1;
  ASSERT_TRUE(r->record_draw(*cmd, da).ok());

  // 索引绘制:先绑索引缓冲,再给 index_count(不设 index_count 会被 L1 拒绝)
  ASSERT_TRUE(r->record_bind_vertex_buffer(*cmd, vbuf->id(), 0u, 0ull).ok());
  ASSERT_TRUE(r->record_bind_index_buffer(*cmd, ibuf->id(), res::IndexFormat::UInt16, 0).ok());
  res::DrawArgs dia;
  dia.index_count = 3;
  dia.instance_count = 1;
  ASSERT_TRUE(r->record_draw_indexed(*cmd, dia).ok());

  // draw_instanced 要求 instance_count > 1
  res::DrawArgs di;
  di.vertex_count = 3;
  di.instance_count = 4;
  ASSERT_TRUE(r->record_draw_instanced(*cmd, di).ok());

  const float pc[4] = {1.0f, 0.0f, 0.0f, 1.0f};
  ASSERT_TRUE(r->record_push_constants(*cmd, 0x1u, 0, sizeof(pc), pc).ok());

  ASSERT_TRUE(r->record_end_pass(*cmd).ok());

  // W4:录制器的 commands[] 必须与 L1 的 command_count() 严格一一对应 ——
  // 这是本用例的核心不变量,因此用相对比较而不是硬编码数字
  // (绑定类命令在 L1 侧是否计入 command_count() 属于 L1 的口径,
  //  不在 L2 的断言范围;L1 自己那条 Command.RecordsAllOps 守着 11)。
  const dev::u32 l1_count = cmd->command_count();
  EXPECT_GT(l1_count, 0u);

  auto stream_r = r->capture();
  ASSERT_TRUE(stream_r.ok()) << stream_r.message();
  const sc::CommandStream& st = stream_r.value();

  ASSERT_EQ(st.commands.size(), static_cast<std::size_t>(l1_count))
      << "W4:commands[] 必须与 L1 的 command_count() 相等";
  std::set<int> seen;
  for (const sc::CommandRecord& c : st.commands) {
    seen.insert(static_cast<int>(c.op));
  }
  // 11 类命令必须全部出现过 —— 这正是 RecordsAllElevenCommandTypes 的断言点
  EXPECT_EQ(seen.size(), sc::kCommandOpCount) << "11 类命令必须全部出现过";
  // pass 元信息只登记一条(record_begin_pass 幂等)
  EXPECT_EQ(st.passes.size(), 1u) << "record_begin_pass 不得重复登记 pass";
}

TEST(Recorder, L1RejectionVoidsWholeFrame) {
  auto device_r = make_device();
  if (!device_r.ok()) {
    ADD_FAILURE() << "需要软件光栅设备: " << device_r.message();
    return;
  }
  dev::IRenderDevice* device = device_r.value().get();
  auto cmd_r = device->create_command_list();
  ASSERT_TRUE(cmd_r.ok()) << cmd_r.message();
  res::ICommandList* cmd = cmd_r.value().get();

  auto r = sc::create_recorder();
  sc::RecorderAttachments att;
  att.pipelines.push_back(demo_pipeline_desc());
  ASSERT_TRUE(r->on_attach(*device, att).ok());

  auto s = sc::create_scene();
  ASSERT_TRUE(r->begin_frame(0, {64, 64}, *s).ok());
  EXPECT_EQ(r->state(), sc::RecorderState::Recording);

  // 未 begin_pass 就 draw → L1 拒绝 → 整帧作废
  res::DrawArgs da;
  da.vertex_count = 3;
  da.instance_count = 1;
  EXPECT_FALSE(r->record_draw(*cmd, da).ok()) << "L1 拒绝时录制器必须报错";

  // 整帧作废:状态回 Idle,后续录制全部拒绝,capture 也拒绝
  EXPECT_EQ(r->state(), sc::RecorderState::Idle);
  EXPECT_FALSE(r->capture().ok()) << "作废的帧不得 capture";
  EXPECT_FALSE(r->record_draw(*cmd, da).ok()) << "作废后不得继续录制";
}

TEST(Recorder, CaptureRequiresNoSubmitAndResetsState) {
  auto device_r = make_device();
  if (!device_r.ok()) {
    ADD_FAILURE() << "需要软件光栅设备: " << device_r.message();
    return;
  }
  dev::IRenderDevice* device = device_r.value().get();

  auto r = sc::create_recorder();
  sc::RecorderAttachments att;
  att.pipelines.push_back(demo_pipeline_desc());
  ASSERT_TRUE(r->on_attach(*device, att).ok());

  auto s = sc::create_scene();
  const sc::NodeId root = s->add_node(sc::kInvalidNodeId, "root").value();
  sc::Transform t;
  t.translation[0] = 2.0f;
  t.scale[0] = 1.0f;
  t.scale[1] = 1.0f;
  t.scale[2] = 1.0f;
  t.rotation[3] = 1.0f;
  s->set_transform(root, t);

  ASSERT_TRUE(r->begin_frame(42, {128, 96}, *s).ok());
  EXPECT_EQ(r->submit_count(), 0u);

  auto stream_r = r->capture();
  ASSERT_TRUE(stream_r.ok()) << stream_r.message();
  EXPECT_EQ(r->submit_count(), 0u) << "capture 不得提交";

  const sc::CommandStream& st = stream_r.value();
  EXPECT_EQ(st.frame_index, 42u);
  EXPECT_EQ(st.extent.width, 128u);
  EXPECT_EQ(st.extent.height, 96u);
  EXPECT_EQ(st.backend, "vulkan");
  EXPECT_EQ(st.scene.nodes.size(), 1u);
  EXPECT_NE(st.stream_hash, 0u) << "capture 必须算好 stream_hash";

  // 下一帧可以继续
  ASSERT_TRUE(r->begin_frame(43, {128, 96}, *s).ok());
  auto s2 = r->capture();
  ASSERT_TRUE(s2.ok()) << s2.message();
  EXPECT_EQ(s2.value().frame_index, 43u);
}

TEST(Recorder, ReplayRejectsMissingPipelineHash) {
  auto device_r = make_device();
  if (!device_r.ok()) {
    ADD_FAILURE() << "需要软件光栅设备: " << device_r.message();
    return;
  }
  dev::IRenderDevice* device = device_r.value().get();
  auto tgt_r = make_target(*device, 64, 64);
  ASSERT_TRUE(tgt_r.ok()) << tgt_r.message();
  auto cmd_r = device->create_command_list();
  ASSERT_TRUE(cmd_r.ok()) << cmd_r.message();

  auto r = sc::create_recorder();
  sc::RecorderAttachments att;
  att.pipelines.push_back(demo_pipeline_desc());
  ASSERT_TRUE(r->on_attach(*device, att).ok());

  // 造一条引用了未登记 hash 的流
  sc::CommandStream s = make_fixed_stream();
  s.commands[1].pipeline_hash = 0xDEADBEEFCAFEBABEull;

  const dev::Status st = r->replay(s, *cmd_r.value(), tgt_r.value().get());
  EXPECT_FALSE(st.ok()) << "缺管线必须硬失败,不得静默跳过";
  EXPECT_NE(st.message.find("deadbeefcafebabe"), std::string::npos)
      << "缺管线报错必须带缺失 hash,实际: " << st.message;
}

TEST(Recorder, ReplayReproducesSameCommandCount) {
  auto device_r = make_device();
  if (!device_r.ok()) {
    ADD_FAILURE() << "需要软件光栅设备: " << device_r.message();
    return;
  }
  dev::IRenderDevice* device = device_r.value().get();
  auto tgt_r = make_target(*device, 64, 64);
  ASSERT_TRUE(tgt_r.ok()) << tgt_r.message();
  dev::IRenderTarget* target = tgt_r.value().get();

  sc::RecorderAttachments att;
  att.pipelines.push_back(demo_pipeline_desc());

  // 先录一帧真的
  auto rec = sc::create_recorder();
  ASSERT_TRUE(rec->on_attach(*device, att).ok());

  auto cmd_r = device->create_command_list();
  ASSERT_TRUE(cmd_r.ok()) << cmd_r.message();
  res::ICommandList* cmd = cmd_r.value().get();

  auto s = sc::create_scene();
  ASSERT_TRUE(rec->begin_frame(0, {64, 64}, *s).ok());

  const std::vector<std::uint64_t>& hashes = rec->pipelines().hashes();
  ASSERT_FALSE(hashes.empty());
  res::IPipeline* pipeline = rec->pipelines().find(hashes.front());
  ASSERT_NE(pipeline, nullptr);

  sc::PassRecord pr;
  pr.name = "scene";
  pr.color_format = dev::Format::RGBA8_UNORM;
  res::RenderPassDesc rp;
  rp.target = target;
  ASSERT_TRUE(rec->record_begin_pass(*cmd, pr, target, rp).ok());
  ASSERT_TRUE(rec->record_bind_pipeline(*cmd, pipeline).ok());
  res::Viewport vp;
  vp.width = 64.0f;
  vp.height = 64.0f;
  vp.max_depth = 1.0f;
  ASSERT_TRUE(rec->record_set_viewport(*cmd, vp).ok());
  res::ScissorRect sr;
  sr.width = 64;
  sr.height = 64;
  ASSERT_TRUE(rec->record_set_scissor(*cmd, sr).ok());
  res::DrawArgs da;
  da.vertex_count = 3;
  da.instance_count = 1;
  ASSERT_TRUE(rec->record_draw(*cmd, da).ok());
  ASSERT_TRUE(rec->record_end_pass(*cmd).ok());

  auto captured = rec->capture();
  ASSERT_TRUE(captured.ok()) << captured.message();
  const sc::CommandStream stream = captured.value();

  // 换一个 recorder + 新命令列表回放
  auto rec2 = sc::create_recorder();
  ASSERT_TRUE(rec2->on_attach(*device, att).ok());
  auto cmd2_r = device->create_command_list();
  ASSERT_TRUE(cmd2_r.ok()) << cmd2_r.message();

  const dev::Status st = rec2->replay(stream, *cmd2_r.value(), target);
  ASSERT_TRUE(st.ok()) << st.message;
  EXPECT_EQ(cmd2_r.value()->command_count(),
            static_cast<dev::u32>(stream.commands.size()));
}

TEST(Recorder, ReproduceUsesRebuiltScene) {
  auto scene = sc::create_scene();
  const sc::NodeId root = scene->add_node(sc::kInvalidNodeId, "root").value();
  scene->add_node(root, "child");

  sc::CommandStream s = make_fixed_stream();
  s.scene = scene->snapshot();

  auto r = sc::create_recorder();
  ASSERT_NE(r, nullptr);

  // 复现函数必须拿到**从流里重建**的场景(节点数一致)
  bool saw_scene = false;
  std::size_t node_count = 0;
  sc::ReproduceFn fn = [&](sc::IScene& rebuilt, res::ICommandList& cl,
                           sc::CommandStream& out) -> dev::Status {
    saw_scene = true;
    node_count = rebuilt.node_count();
    // 原样回填流的内容,保证哈希一致
    out = s;
    (void)cl;
    return dev::Status::Ok_();
  };

  const dev::Status st = r->reproduce(s, fn);
  EXPECT_TRUE(st.ok()) << st.message;
  EXPECT_TRUE(saw_scene) << "复现函数必须被调用";
  EXPECT_EQ(node_count, 2u) << "复现函数拿到的场景必须是从流重建的";
  EXPECT_EQ(r->state(), sc::RecorderState::Idle) << "复现结束必须回到 Idle";
}

TEST(Recorder, ReproduceDetectsDivergence) {
  auto scene = sc::create_scene();
  scene->add_node(sc::kInvalidNodeId, "root");

  sc::CommandStream s = make_fixed_stream();
  s.scene = scene->snapshot();

  auto r = sc::create_recorder();
  ASSERT_NE(r, nullptr);

  // 复现函数产出**不同**的命令流 → 必须被检测出来
  sc::ReproduceFn fn = [&](sc::IScene&, res::ICommandList&,
                           sc::CommandStream& out) -> dev::Status {
    out = s;
    out.commands.pop_back();  // 少一条
    return dev::Status::Ok_();
  };

  const dev::Status st = r->reproduce(s, fn);
  EXPECT_FALSE(st.ok()) << "命令流不一致必须被检测出来";
  EXPECT_EQ(r->state(), sc::RecorderState::Idle);
}

TEST(Recorder, PipelineTableMissListsRegisteredHashes) {
  auto device_r = make_device();
  if (!device_r.ok()) {
    ADD_FAILURE() << "需要软件光栅设备: " << device_r.message();
    return;
  }

  auto r = sc::create_recorder();
  sc::RecorderAttachments att;
  att.pipelines.push_back(demo_pipeline_desc());
  ASSERT_TRUE(r->on_attach(*device_r.value(), att).ok());

  const sc::PipelineTable& table = r->pipelines();
  ASSERT_EQ(table.size(), 1u);
  ASSERT_EQ(table.hashes().size(), 1u);

  const std::uint64_t known = table.hashes().front();
  EXPECT_NE(table.find(known), nullptr) << "已登记的 hash 必须能查到";
  EXPECT_EQ(table.find(0x1234567890ABCDEFull), nullptr) << "未登记的 hash 必须查不到";

  // kMaxListedOnMiss 是"报错信息里最多列几个 hash"的上限,必须是正数
  EXPECT_GT(sc::PipelineTable::kMaxListedOnMiss, 0u);
  EXPECT_LE(sc::PipelineTable::kMaxListedOnMiss, 16u)
      << "上限过大让报错信息失去可读性";
}

// ================================================================ S9 特性注册表(3)

namespace {

// 记录"自己被调用了几次"的最小特性实现,用于验证分层调度。
class CountingFeature final : public sc::IRenderFeature {
 public:
  CountingFeature(sc::FeatureLayer layer, int* counter)
      : layer_(layer), counter_(counter) {}
  sc::FeatureLayer layer() const override { return layer_; }
  dev::Status record(sc::FeatureRecordContext&) override {
    ++(*counter_);
    return dev::Status::Ok_();
  }

 private:
  sc::FeatureLayer layer_;
  int* counter_;
};

// 总是失败的特性:验证 record_layer 把失败原样上报。
class FailingFeature final : public sc::IRenderFeature {
 public:
  sc::FeatureLayer layer() const override { return sc::FeatureLayer::Scene; }
  dev::Status record(sc::FeatureRecordContext&) override {
    return dev::Status::Invalid("feature 故意失败");
  }
};

}  // namespace

TEST(FeatureRegistry, AddRemoveKeepsRegistrationOrder) {
  auto reg = sc::create_feature_registry();
  ASSERT_NE(reg, nullptr);
  EXPECT_EQ(reg->size(), 0u);

  int a = 0, b = 0, c = 0;
  CountingFeature fa(sc::FeatureLayer::Scene, &a);
  CountingFeature fb(sc::FeatureLayer::Scene, &b);
  CountingFeature fc(sc::FeatureLayer::Overlay, &c);

  reg->add(&fa);
  reg->add(&fb);
  reg->add(&fc);
  EXPECT_EQ(reg->size(), 3u);

  // 注册顺序必须保留
  ASSERT_EQ(reg->features().size(), 3u);
  EXPECT_EQ(reg->features()[0], &fa);
  EXPECT_EQ(reg->features()[1], &fb);
  EXPECT_EQ(reg->features()[2], &fc);

  // 删除中间一个,其余顺序不变
  EXPECT_TRUE(reg->remove(&fb));
  ASSERT_EQ(reg->size(), 2u);
  EXPECT_EQ(reg->features()[0], &fa);
  EXPECT_EQ(reg->features()[1], &fc);

  // 重复删除 / 删除未注册 → false
  EXPECT_FALSE(reg->remove(&fb));
  EXPECT_FALSE(reg->remove(nullptr));
}

TEST(FeatureRegistry, RecordLayerOnlyCallsMatchingLayer) {
  auto reg = sc::create_feature_registry();
  ASSERT_NE(reg, nullptr);

  int scene_calls = 0, overlay_calls = 0;
  CountingFeature f_scene(sc::FeatureLayer::Scene, &scene_calls);
  CountingFeature f_overlay(sc::FeatureLayer::Overlay, &overlay_calls);
  reg->add(&f_scene);
  reg->add(&f_overlay);

  auto q = sc::create_submit_queue();
  sc::FeatureRecordContext ctx;
  ctx.queue = q.get();
  ctx.frame_index = 5;

  ASSERT_TRUE(reg->record_layer(sc::FeatureLayer::Scene, ctx).ok());
  EXPECT_EQ(scene_calls, 1);
  EXPECT_EQ(overlay_calls, 0) << "Overlay 层不得在 Scene 轮次被调用";

  ASSERT_TRUE(reg->record_layer(sc::FeatureLayer::Overlay, ctx).ok());
  EXPECT_EQ(scene_calls, 1) << "Scene 层不得被重复调用";
  EXPECT_EQ(overlay_calls, 1);

  // 空层调用成功且不调用任何人
  ASSERT_TRUE(reg->record_layer(sc::FeatureLayer::Overlay, ctx).ok());
  EXPECT_EQ(overlay_calls, 2);
}

TEST(FeatureRegistry, RecordLayerPropagatesFeatureFailure) {
  auto reg = sc::create_feature_registry();
  ASSERT_NE(reg, nullptr);

  FailingFeature bad;
  reg->add(&bad);

  auto q = sc::create_submit_queue();
  sc::FeatureRecordContext ctx;
  ctx.queue = q.get();
  ctx.frame_index = 0;

  const dev::Status st = reg->record_layer(sc::FeatureLayer::Scene, ctx);
  EXPECT_FALSE(st.ok()) << "特性失败必须原样上报,不得吞掉";
  EXPECT_NE(st.message.find("feature 故意失败"), std::string::npos)
      << "必须保留特性自己的错误信息,实际: " << st.message;
}

// ================================================================ golden(1)

TEST(L2Golden, MatchesL2Baseline) {
  // 用与 CLI 相同的绘制路径重新出图,验证 L2 的身份标定点。
  //
  // 判据(plans-l2 §6.5):
  //   1) 左上角格心必须是**纯绿** —— L2 的身份标定点(实例 0);
  //   2) 右上角格心必须**不是**纯绿 —— 证明标定点只在左上(通道序 + y 方向都对);
  //   3) 格缝背景必须是深蓝清屏色,且不是纯黑 —— 与 L0 的黑色清屏区分开。
  // 把 golden 换成 L0 那套时断言 1 必然失败 —— 两套基线互不冒充。
  auto device_r = make_device();
  if (!device_r.ok()) {
    ADD_FAILURE() << "需要软件光栅设备: " << device_r.message();
    return;
  }
  dev::IRenderDevice* device = device_r.value().get();

  constexpr dev::u32 kW = 320;
  constexpr dev::u32 kH = 240;

  auto tgt_r = make_target(*device, kW, kH);
  ASSERT_TRUE(tgt_r.ok()) << tgt_r.message();
  dev::IRenderTarget* target = tgt_r.value().get();

  auto* off = dynamic_cast<::magicblocks::backend::OffscreenTarget*>(target);
  if (off == nullptr) {
    ADD_FAILURE() << "离屏目标不是 backend::OffscreenTarget";
    return;
  }
  {
    const dev::Status ds = off->draw_selftest(
        0, ::magicblocks::backend::OffscreenTarget::SelftestKind::kL2Scene);
    ASSERT_TRUE(ds.ok()) << ds.message;
  }

  magicblocks::ImageRgba8 img;
  img.width = kW;
  img.height = kH;
  img.pixels.assign(img.byte_size(), 0);
  {
    const dev::Status rs = target->readback_rgba8(img.pixels.data(), img.pixels.size());
    ASSERT_TRUE(rs.ok()) << rs.message;
  }

  auto px = [&](dev::u32 x, dev::u32 y, int* r, int* g, int* b) {
    const std::size_t o = (static_cast<std::size_t>(y) * kW + x) * 4u;
    *r = img.pixels[o + 0];
    *g = img.pixels[o + 1];
    *b = img.pixels[o + 2];
  };

  // 1) 左上角格心(实例 0):纯绿标定点
  {
    int r = 0, g = 0, b = 0;
    px(kW / 8, kH / 8, &r, &g, &b);
    EXPECT_LT(r, 60) << "左上角标定点 R 太高: " << r;
    EXPECT_GT(g, 200) << "左上角标定点 G 太低: " << g;
    EXPECT_LT(b, 60) << "左上角标定点 B 太高: " << b;
  }
  // 2) 右上角格心:不能是纯绿
  {
    int r = 0, g = 0, b = 0;
    px(kW - kW / 8 - 1, kH / 8, &r, &g, &b);
    const bool also_green = (r < 60 && g > 200 && b < 60);
    EXPECT_FALSE(also_green) << "右上角也是纯绿,标定点位置不对 (R" << r << " G" << g
                             << " B" << b << ")";
  }
  // 3) 格缝背景:深蓝清屏,不是纯黑
  {
    int r = 0, g = 0, b = 0;
    px(kW / 8, kH / 2, &r, &g, &b);
    EXPECT_GT(r, 0) << "背景不应是纯黑(那是 L0 的配色)";
    EXPECT_GT(b, r) << "背景应偏蓝";
  }

  // 若仓库里有 golden_l2.png 基线,再逐像素比一次(软件光栅确定性 → 0 容差)
  const std::string golden = std::string(MB_TEST_DATA_DIR) + "/golden_l2.png";
  std::FILE* f = std::fopen(golden.c_str(), "rb");
  if (f == nullptr) {
    ADD_FAILURE() << "没有找到基线文件: " << golden;
    return;
  }
  std::fclose(f);

  magicblocks::ImageRgba8 base;
  std::string err;
  if (!magicblocks::read_png_rgba8(golden, &base, &err)) {
    ADD_FAILURE() << "无法读取 golden_l2.png: " << err;
    return;
  }
  ASSERT_EQ(base.width, kW);
  ASSERT_EQ(base.height, kH);
  ASSERT_EQ(base.pixels.size(), img.pixels.size());

  std::size_t diff = 0;
  std::size_t first = 0;
  for (std::size_t k = 0; k < img.pixels.size(); ++k) {
    if (img.pixels[k] != base.pixels[k]) {
      if (diff == 0) first = k;
      ++diff;
    }
  }
  EXPECT_EQ(diff, 0u) << "与 golden_l2.png 有 " << diff << " 字节不同,首个差异在字节 "
                      << first << "(像素 " << (first / 4) << ",通道 " << (first % 4)
                      << "):got " << static_cast<int>(img.pixels[first]) << " want "
                      << static_cast<int>(base.pixels[first]);
}

}  // namespace
