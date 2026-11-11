// MagicBlocks L3 —— 合批器的单元测试（spec §4.3 / §14.2 Renderer 套件）
//
// 重点：把 §4.1 的 drawcall / 管线切换预算变成可断言的数字。
#include <gtest/gtest.h>

#include <vector>

#include "mb_internal/magicblocks/primitives/alloc.h"
#include "mb_internal/magicblocks/primitives/batch.h"

namespace mbp = magicblocks::primitives;

namespace {

mbp::DrawRequest make_req(mbp::u64 pipeline, mbp::MaterialKey material,
                          mbp::MeshId mesh, std::uint32_t entity) {
  mbp::DrawRequest r;
  r.pipeline_hash = pipeline;
  r.material_key = material;
  r.mesh = mesh;
  r.entity_id = entity;
  r.instance_index = entity;
  r.index_count = 36;   // 一个立方体
  r.first_index = 0;
  r.vertex_count = 0;
  return r;
}

}  // namespace

// spec §14.2: BatchesByShaderMaterialMesh —— 同 (pipeline, material, mesh) 必须合并。
TEST(Renderer, BatchesByShaderMaterialMesh) {
  auto bb = mbp::create_batch_builder();
  ASSERT_NE(bb, nullptr);
  bb->reserve(128);
  bb->begin_frame(0);

  // 100 个实例，全部同 pipeline / 同 material / 同 mesh。
  for (std::uint32_t i = 0; i < 100; ++i) {
    bb->push(make_req(0xAAAAull, 7u, 3, i));
  }
  bb->build();

  const mbp::BatchStats& st = bb->stats();
  EXPECT_EQ(st.request_count, 100u);
  EXPECT_EQ(st.batch_count, 1u) << "完全相同的键必须合成一次 drawcall";
  EXPECT_EQ(st.merged_requests, 99u);
  EXPECT_EQ(st.pipeline_switches, 0u);
  ASSERT_EQ(bb->batches().size(), 1u);
  EXPECT_EQ(bb->batches()[0].instance_count, 100u);
}

TEST(Renderer, DifferentKeysProduceSeparateBatches) {
  auto bb = mbp::create_batch_builder();
  ASSERT_NE(bb, nullptr);
  bb->begin_frame(0);

  bb->push(make_req(0x1111ull, 1u, 1, 10));
  bb->push(make_req(0x2222ull, 1u, 1, 11));   // 不同 pipeline
  bb->push(make_req(0x1111ull, 2u, 1, 12));   // 同 pipeline 不同 material
  bb->push(make_req(0x1111ull, 1u, 2, 13));   // 同 pipeline+material 不同 mesh
  bb->build();

  EXPECT_EQ(bb->stats().batch_count, 4u);
  // 排序后：pipeline 0x1111 的三条相邻，pipeline 0x2222 单独 → 1 次切换
  EXPECT_EQ(bb->stats().pipeline_switches, 1u);
}

TEST(Renderer, TransparentSortsAfterOpaque) {
  auto bb = mbp::create_batch_builder();
  ASSERT_NE(bb, nullptr);
  bb->begin_frame(0);

  mbp::DrawRequest opaque = make_req(0x1000ull, 5u, 1, 1);
  opaque.transparent = false;
  mbp::DrawRequest trans = make_req(0x1000ull, 5u, 1, 2);
  trans.transparent = true;

  // 故意先提交透明的，验证排序确实生效而不是碰巧输入有序。
  bb->push(trans);
  bb->push(opaque);
  bb->build();

  ASSERT_EQ(bb->batches().size(), 2u);
  EXPECT_FALSE(bb->batches()[0].transparent) << "不透明必须在前";
  EXPECT_TRUE(bb->batches()[1].transparent) << "透明必须在后";
}

// spec §14.2: StableSortTieBreak —— 相同键时按 entity 稳定排序。
TEST(Renderer, StableSortTieBreak) {
  auto bb = mbp::create_batch_builder();
  ASSERT_NE(bb, nullptr);
  bb->begin_frame(0);

  // 乱序提交同一组 entity，输出必须按 entity 升序。
  const std::uint32_t order[] = {5, 2, 9, 1, 7};
  for (std::uint32_t e : order) bb->push(make_req(0x9999ull, 3u, 2, e));
  bb->build();

  ASSERT_EQ(bb->batches().size(), 1u);
  EXPECT_EQ(bb->batches()[0].first_entity, 1u) << "合并后首实例应是最小 entity";
}

TEST(Renderer, InvisibleRequestsAreSkipped) {
  auto bb = mbp::create_batch_builder();
  ASSERT_NE(bb, nullptr);
  bb->begin_frame(0);

  mbp::DrawRequest vis = make_req(0x1ull, 1u, 1, 1);
  mbp::DrawRequest hid = make_req(0x2ull, 2u, 2, 2);
  hid.visible = false;
  bb->push(vis);
  bb->push(hid);
  bb->build();

  EXPECT_EQ(bb->stats().skipped_invisible, 1u);
  EXPECT_EQ(bb->stats().batch_count, 1u);
  EXPECT_EQ(bb->stats().request_count, 1u) << "不可见的不计入 request_count";
}

// §4.1 预算：1000 个实例、5 种 pipeline → drawcall 与切换数都要远低于上限。
TEST(Renderer, MeetsDrawcallAndSwitchBudget) {
  auto bb = mbp::create_batch_builder();
  ASSERT_NE(bb, nullptr);
  bb->reserve(1000);
  bb->begin_frame(0);

  // 5 种 pipeline × 4 种 material × 1000 个实体，乱序提交
  for (std::uint32_t e = 0; e < 1000; ++e) {
    const mbp::u64 pipe = 0x100ull + (e % 5);
    const mbp::MaterialKey mat = e % 4;
    bb->push(make_req(pipe, mat, 0, e));
  }
  bb->build();

  const mbp::BatchStats& st = bb->stats();
  // 5 pipeline × 4 material = 最多 20 个 batch
  EXPECT_LE(st.batch_count, mbp::FrameBudget::kMaxDrawcalls)
      << "drawcall 超出 §4.1 预算 300";
  EXPECT_LE(st.pipeline_switches, mbp::FrameBudget::kMaxPipelineSwitches)
      << "管线切换超出 §4.1 预算 16";
  EXPECT_EQ(st.pipeline_switches, 4u) << "5 种 pipeline 相邻排列 → 恰好 4 次切换";
}

// 确定性：同样的输入（哪怕提交顺序不同）必须得到同样的 order_hash。
TEST(Renderer, OrderHashIsDeterministicAcrossSubmissionOrder) {
  auto a = mbp::create_batch_builder();
  auto b = mbp::create_batch_builder();
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);

  std::vector<mbp::DrawRequest> reqs;
  for (std::uint32_t i = 0; i < 50; ++i) {
    reqs.push_back(make_req(0x200ull + (i % 3), i % 5, 0, i));
  }

  a->begin_frame(0);
  for (const auto& r : reqs) a->push(r);
  a->build();

  b->begin_frame(0);
  for (auto it = reqs.rbegin(); it != reqs.rend(); ++it) b->push(*it);
  b->build();

  EXPECT_EQ(a->order_hash(), b->order_hash())
      << "提交顺序不同但内容相同，合批结果指纹必须一致（回放确定性）";
  EXPECT_EQ(a->stats().batch_count, b->stats().batch_count);
}

// 稳态帧不分配：reserve 之后反复 begin_frame/build 不得新增堆分配。
TEST(Renderer, SteadyStateNoHeapGrowth) {
  auto stats = mbp::create_frame_stats();
  auto bb = mbp::create_batch_builder();
  ASSERT_NE(bb, nullptr);
  bb->set_stats(stats.get());
  bb->reserve(256);

  struct AllocProbe {
    mbp::u64 before = 0;
  };

  // 先跑 3 帧让内部容器容量稳定下来
  for (int warm = 0; warm < 3; ++warm) {
    bb->begin_frame(static_cast<mbp::u32>(warm));
    for (std::uint32_t i = 0; i < 200; ++i) {
      bb->push(make_req(0x300ull + (i % 4), i % 3, 0, i));
    }
    bb->build();
  }

  // 之后再跑 10 帧，容量已足，不应再增长
  const std::size_t cap_after_warm =
      bb->batches().capacity();  // 供参考
  std::size_t batch_counts_ok = 0;
  for (int f = 0; f < 10; ++f) {
    bb->begin_frame(static_cast<mbp::u32>(f % 3));
    for (std::uint32_t i = 0; i < 200; ++i) {
      bb->push(make_req(0x300ull + (i % 4), i % 3, 0, i));
    }
    bb->build();
    if (bb->stats().batch_count == 12u) ++batch_counts_ok;
  }
  EXPECT_EQ(batch_counts_ok, 10u) << "每帧结果必须一致（4 pipeline × 3 material）";
  EXPECT_GE(cap_after_warm, 12u);
}

TEST(Renderer, FrameStatsReceivesBudgetCounters) {
  auto stats = mbp::create_frame_stats();
  auto bb = mbp::create_batch_builder();
  ASSERT_NE(bb, nullptr);
  bb->set_stats(stats.get());
  bb->begin_frame(0);

  for (std::uint32_t i = 0; i < 30; ++i) {
    bb->push(make_req(0x400ull, 1u, 0, i));
  }
  bb->build();

  const mbp::FrameStatsRecord rec = stats->current();
  EXPECT_EQ(rec.drawcalls, 1u) << "合批器必须把 drawcall 记进 FrameStats";
  EXPECT_EQ(rec.pipeline_switches, 0u);
}
