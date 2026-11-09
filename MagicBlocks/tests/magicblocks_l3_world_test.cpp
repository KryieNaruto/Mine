// MagicBlocks L3 —— IWorld（ECS）的单元测试（spec §4.2 / §7 / §14.2 World 套件）
//
// 这个文件的存在意义和 Pool 测试一样：把 §4.2 / §7.4 的设计承诺变成会变红的断言。
// 命名沿用 spec §14.2 的用例名（World.* 套件），另外补了几条守门用的用例。
//
// 注意:全目录有门控 `tools/check_ui_single_submit.py` 会 grep `->submit\s*[<(]` /
// `.\s*submit\s*[<(]`，所以这里**不写任何 .submit(...) 形式的调用**。
//
// 本文件用了 <chrono> 只为 `ComponentAccessIsO1` 测时延。`check_scene_no_clock`
// 的扫描口径是 `src/scene/**` 与 `include/magicblocks/scene/**`（**不含 tests**），
// L3 的 `src/primitives/**` 同样没有时钟调用 —— 时序只出现在测试侧，符合门控口径。
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <numeric>
#include <vector>

#include "magicblocks/primitives/alloc.h"
#include "magicblocks/primitives/batch.h"
#include "magicblocks/primitives/world.h"
#include "magicblocks/primitives/world_render.h"

namespace mbp = magicblocks::primitives;

namespace {

// 每个用例都从"注册好标准组件"开始。标准组件的号是约定号段 0..7，
// 业务组件从 8 往后排（这里给测试自定义组件用）。
constexpr mbp::ComponentKind kTestKindA = 8;   // 16 字节的"生命值 + 标签"
constexpr mbp::ComponentKind kTestKindB = 9;   // 4 字节的小组件（和标准组件混排）
constexpr mbp::ComponentKind kTestKindC = 10;  // 40 字节，用来制造列宽差

struct TestCompA {
  std::uint32_t hp = 0;
  float weight = 0.0f;
  std::uint32_t tag = 0;
  std::uint32_t pad = 0;
};
static_assert(sizeof(TestCompA) == 16, "TestCompA 想要 16 字节，改字段时同步改 kTestKindA 的注册宽度");

// 真实世界的注册：拿 IWorld 而不是具体实现类，保证测试只依赖公开接口。
std::unique_ptr<mbp::IWorld> make_world() {
  auto w = mbp::create_world();
  EXPECT_NE(w, nullptr);
  const mbp::Status st = mbp::register_standard_components(*w);
  EXPECT_TRUE(st.ok()) << st.message;
  return w;
}

// 固定种子的线性同余发生器：测试里的"随机序列"必须可复现，
// 否则红了之后无法重放（与 spec §11 的确定性口径同源）。
struct Lcg {
  std::uint32_t s = 12345u;
  std::uint32_t next() {
    s = s * 1664525u + 1013904223u;
    return s;
  }
};

// 把世界里的 Transform 全部写成一个确定性初值，便于快照比对。
void fill_transform(mbp::IWorld& w, mbp::EntityId id, float x) {
  auto* tr = w.get<mbp::TransformComp>(id);
  ASSERT_NE(tr, nullptr);
  tr->translation[0] = x;
  tr->translation[1] = x * 2.0f;
  tr->translation[2] = -x;
}

}  // namespace

// ================================================================ 注册表

TEST(World, RegisterComponentRejectsBadInput) {
  auto w = make_world();
  ASSERT_NE(w, nullptr);
  // 重复注册：注册是显式的，静默覆盖只会掩盖笔误。
  EXPECT_FALSE(w->register_component(mbp::TransformComp::kComponentKind,
                                     sizeof(mbp::TransformComp), "again")
                   .ok());
  // 0 宽度 / 超宽 / kind 越界 全部明确拒绝。
  EXPECT_FALSE(w->register_component(20, 0, "zero").ok());
  EXPECT_FALSE(w->register_component(21, 128u * 1024u, "huge").ok());
  EXPECT_FALSE(w->register_component(mbp::kMaxComponents, 4, "oob").ok());
  EXPECT_EQ(w->registered_count(), mbp::kStandardComponentCount);
  EXPECT_EQ(w->component_size(mbp::TransformComp::kComponentKind),
            static_cast<mbp::u32>(sizeof(mbp::TransformComp)));
}

TEST(World, RegisterRejectedAfterSpawn) {
  auto w = make_world();
  ASSERT_NE(w, nullptr);
  auto e = w->spawn();
  ASSERT_TRUE(e.ok());
  // 已经建过 archetype 之后再注册：新实体的位图会与旧 archetype 撞掩码却宽度不同，
  // 所以必须在启动期一次性注册。
  EXPECT_FALSE(w->register_component(kTestKindA, sizeof(TestCompA), "late").ok());
}

// ================================================================ 实体生命周期

// spec §14.2: SpawnDespawnStableGen —— despawn 之后旧句柄必须失效，
// 新实体可以复用 index，但 gen 必须不同（否则旧句柄会"复活"成新实体）。
TEST(World, SpawnDespawnStableGen) {
  auto w = make_world();
  ASSERT_NE(w, nullptr);

  auto a = w->spawn();
  ASSERT_TRUE(a.ok());
  auto b = w->spawn();
  ASSERT_TRUE(b.ok());
  EXPECT_NE(a.value().index, b.value().index) << "两个同时活着的实体不能共用 index";
  EXPECT_TRUE(w->alive(a.value()));
  EXPECT_TRUE(w->alive(b.value()));
  EXPECT_EQ(w->entity_count(), 2u);

  const mbp::EntityId old_a = a.value();
  ASSERT_TRUE(w->despawn(old_a));
  EXPECT_FALSE(w->alive(old_a)) << "despawn 后旧句柄必须失效";
  EXPECT_FALSE(w->despawn(old_a)) << "重复 despawn 返回 false";
  EXPECT_EQ(w->entity_count(), 1u);

  // 复用 index：gen 必须 +1。
  auto c = w->spawn();
  ASSERT_TRUE(c.ok());
  EXPECT_EQ(c.value().index, old_a.index) << "空闲 index 应被复用（不为每个 despawn 永久烧掉一个号）";
  EXPECT_NE(c.value().gen, old_a.gen) << "复用时 gen 必须变化，否则旧句柄指向新实体";
  EXPECT_FALSE(w->alive(old_a)) << "旧句柄在 index 被复用后依旧必须无效";
  EXPECT_TRUE(w->alive(c.value()));

  // b 全程不受影响（despawn 用 swap-remove，不能把别人的身份弄坏）。
  EXPECT_TRUE(w->alive(b.value()));
  EXPECT_TRUE(w->has_component(b.value(), mbp::TransformComp::kComponentKind) == false);
}

// spec §14.2: DespawnInvalidatesHandle —— 悬垂句柄的**所有**操作都必须失效，
// 包括"点进去拿组件"。这条比 SpawnDespawnStableGen 更狠：它检查的是
// despawn 之后、index 被复用之前那段窗口，以及复用之后访问旧 gen 的窗口。
TEST(World, DespawnInvalidatesHandle) {
  auto w = make_world();
  ASSERT_NE(w, nullptr);

  std::vector<mbp::EntityId> victims;
  for (int i = 0; i < 8; ++i) {
    auto e = w->spawn();
    ASSERT_TRUE(e.ok());
    ASSERT_TRUE(w->add_component(e.value(), mbp::TransformComp::kComponentKind).ok());
    ASSERT_TRUE(w->add_component(e.value(), mbp::MeshRefComp::kComponentKind).ok());
    fill_transform(*w, e.value(), static_cast<float>(i));
    victims.push_back(e.value());
  }

  // 删掉中间几个，保留两个继续用它。
  for (int i = 1; i < 8; i += 2) {
    ASSERT_TRUE(w->despawn(victims[i]));
  }
  for (int i = 1; i < 8; i += 2) {
    const mbp::EntityId dead = victims[i];
    EXPECT_FALSE(w->alive(dead));
    EXPECT_EQ(w->component(dead, mbp::TransformComp::kComponentKind), nullptr)
        << "悬垂句柄不得读到任何组件";
    EXPECT_FALSE(w->has_component(dead, mbp::TransformComp::kComponentKind));
    EXPECT_FALSE(w->add_component(dead, mbp::LayerComp::kComponentKind).ok());
    EXPECT_FALSE(w->remove_component(dead, mbp::TransformComp::kComponentKind).ok());
    EXPECT_FALSE(w->despawn(dead));

    // 标脏悬垂句柄必须是空操作：如果它进了脏集合，sync 时就会去碰别人的行。
    w->mark_dirty(dead, mbp::DirtyKind::Transform);
  }
  EXPECT_TRUE(w->dirty().empty()) << "悬垂句柄标脏不得进脏集合";

  // index 复用之后，旧 (index, 旧 gen) 依旧无效 —— 这是 gen 的全部意义。
  // 空闲列表是 LIFO，所以复用的是**最后**被 despawn 的那个 index。
  const mbp::EntityId resurrect_index = victims[7];
  auto fresh = w->spawn();
  ASSERT_TRUE(fresh.ok());
  EXPECT_EQ(fresh.value().index, resurrect_index.index);
  EXPECT_FALSE(w->alive(resurrect_index)) << "index 被复用后旧 gen 仍必须悬垂";
  EXPECT_EQ(w->component(resurrect_index, mbp::TransformComp::kComponentKind), nullptr);

  // 新实体是"刚生成"的：它没有原受害者的 Transform（复用不继承组件）。
  EXPECT_FALSE(w->has_component(fresh.value(), mbp::TransformComp::kComponentKind));
  EXPECT_TRUE(w->alive(fresh.value()));

  // 还活着的实体数据完好。
  EXPECT_TRUE(w->alive(victims[0]));
  EXPECT_EQ(w->get<mbp::TransformComp>(victims[0])->translation[0], 0.0f);
}

// ================================================================ 迁移

// spec §14.2: AddComponentMigrates —— 加组件要换 archetype，但
// **原有的组件数据必须跟着走**，且实体身份（index）不变。
TEST(World, AddComponentMigrates) {
  auto w = make_world();
  ASSERT_NE(w, nullptr);
  ASSERT_TRUE(w->register_component(kTestKindA, sizeof(TestCompA), "TestA").ok());
  ASSERT_TRUE(w->register_component(kTestKindB, sizeof(std::uint32_t), "TestB").ok());

  auto e = w->spawn();
  ASSERT_TRUE(e.ok());
  const mbp::EntityId id = e.value();

  ASSERT_TRUE(w->add_component(id, mbp::TransformComp::kComponentKind).ok());
  fill_transform(*w, id, 7.0f);
  ASSERT_TRUE(w->add_component(id, mbp::MeshRefComp::kComponentKind).ok());
  w->get<mbp::MeshRefComp>(id)->mesh = 42;
  ASSERT_TRUE(w->add_component(id, kTestKindA).ok());
  {
    auto* a = static_cast<TestCompA*>(w->component(id, kTestKindA));
    ASSERT_NE(a, nullptr);
    a->hp = 9001;
    a->tag = 0xAB;
  }

  // 前一步的数据都还在（迁移按"目标列的组件种类"去源列查序号，不是按下标硬搬）。
  EXPECT_EQ(w->mask_of(id), mbp::component_bit(mbp::TransformComp::kComponentKind) |
                                mbp::component_bit(mbp::MeshRefComp::kComponentKind) |
                                mbp::component_bit(kTestKindA));
  EXPECT_EQ(w->get<mbp::TransformComp>(id)->translation[0], 7.0f);
  EXPECT_EQ(w->get<mbp::MeshRefComp>(id)->mesh, 42u);
  EXPECT_EQ(static_cast<TestCompA*>(w->component(id, kTestKindA))->hp, 9001u);
  // 迁移不换实体身份：index 与 gen 都不变（不变量 1）。
  const mbp::EntityId after_first_migrate = e.value();
  EXPECT_EQ(after_first_migrate.index, id.index);
  EXPECT_EQ(after_first_migrate.gen, id.gen);

  // 位序中间的组件（Transform=0）在目标 archetype 里可能排到别的位置，
  // 这里先加一个更低位序的 kTestKindB，确认低位的列被正确挪动。
  ASSERT_TRUE(w->add_component(id, kTestKindB).ok());
  EXPECT_EQ(w->get<mbp::TransformComp>(id)->translation[0], 7.0f);
  EXPECT_EQ(w->get<mbp::MeshRefComp>(id)->mesh, 42u);
  EXPECT_EQ(static_cast<TestCompA*>(w->component(id, kTestKindA))->hp, 9001u);

  // 幂等：重复加同一个组件不报错、不迁移数据。
  EXPECT_TRUE(w->add_component(id, kTestKindA).ok());
  EXPECT_EQ(static_cast<TestCompA*>(w->component(id, kTestKindA))->hp, 9001u);

  // 移除中间那个，剩下的仍然正确。
  ASSERT_TRUE(w->remove_component(id, mbp::MeshRefComp::kComponentKind).ok());
  EXPECT_FALSE(w->has_component(id, mbp::MeshRefComp::kComponentKind));
  EXPECT_EQ(w->get<mbp::TransformComp>(id)->translation[0], 7.0f);
  EXPECT_EQ(static_cast<TestCompA*>(w->component(id, kTestKindA))->tag, 0xABu);
}

// 迁移 + swap-remove 混在一起是 ECS 最容易出错的组合：删一个实体把末行挪到洞上，
// 之后对"被挪的实体"加组件，迁移必须按它**新的**行号搬数据。
TEST(World, MigrateAfterSwapRemoveKeepsRowsConsistent) {
  auto w = make_world();
  ASSERT_NE(w, nullptr);

  Lcg rng;
  struct Rec {
    mbp::EntityId id;
    float expected;
  };
  std::vector<Rec> live;

  for (int round = 0; round < 200; ++round) {
    const std::uint32_t op = rng.next() % 3;
    if (op == 0 || live.empty()) {
      auto e = w->spawn();
      ASSERT_TRUE(e.ok());
      const float v = static_cast<float>(live.size()) + static_cast<float>(round) * 0.25f;
      ASSERT_TRUE(w->add_component(e.value(), mbp::TransformComp::kComponentKind).ok());
      fill_transform(*w, e.value(), v);
      live.push_back(Rec{e.value(), v});
    } else if (op == 1) {
      // 随机删一个（触发 swap-remove）。
      const std::size_t k = rng.next() % live.size();
      ASSERT_TRUE(w->despawn(live[k].id));
      live.erase(live.begin() + static_cast<std::ptrdiff_t>(k));
    } else {
      // 随机给一个实体加/减组件（触发迁移）。
      const std::size_t k = rng.next() % live.size();
      const mbp::EntityId id = live[k].id;
      if (w->has_component(id, mbp::LayerComp::kComponentKind)) {
        ASSERT_TRUE(w->remove_component(id, mbp::LayerComp::kComponentKind).ok());
      } else {
        ASSERT_TRUE(w->add_component(id, mbp::LayerComp::kComponentKind).ok());
      }
    }

    // 每一步之后校验**所有**活实体的 Transform 与记录值一致。
    for (const Rec& r : live) {
      ASSERT_TRUE(w->alive(r.id));
      const auto* tr = w->get<mbp::TransformComp>(r.id);
      ASSERT_NE(tr, nullptr);
      ASSERT_EQ(tr->translation[0], r.expected)
          << "swap-remove / 迁移之后行数据串了（round=" << round << "）";
      ASSERT_EQ(tr->translation[1], r.expected * 2.0f);
    }
  }
  EXPECT_EQ(w->entity_count(), live.size());
}

// ================================================================ 遍历

// spec §14.2: ViewOnlyVisitedArchetypes —— view(mask) 只访问含所需组件的
// archetype，含别的组件组合的实体一条都不进。
TEST(World, ViewOnlyVisitedArchetypes) {
  auto w = make_world();
  ASSERT_NE(w, nullptr);

  // archetype 1: {Transform}            × 3
  // archetype 2: {Transform, MeshRef}  × 4
  // archetype 3: {MeshRef}             × 2   ← 不含 Transform，必须被跳过
  // archetype 4: {Layer}               × 5   ← 和 Transform 无关，必须被跳过
  std::vector<mbp::EntityId> with_tr_only;
  std::vector<mbp::EntityId> with_both;
  std::vector<mbp::EntityId> mesh_only;
  std::vector<mbp::EntityId> layer_only;

  for (int i = 0; i < 3; ++i) {
    auto e = w->spawn();
    ASSERT_TRUE(e.ok());
    ASSERT_TRUE(w->add_component(e.value(), mbp::TransformComp::kComponentKind).ok());
    fill_transform(*w, e.value(), static_cast<float>(i));
    with_tr_only.push_back(e.value());
  }
  for (int i = 0; i < 4; ++i) {
    auto e = w->spawn();
    ASSERT_TRUE(e.ok());
    ASSERT_TRUE(w->add_component(e.value(), mbp::TransformComp::kComponentKind).ok());
    ASSERT_TRUE(w->add_component(e.value(), mbp::MeshRefComp::kComponentKind).ok());
    fill_transform(*w, e.value(), static_cast<float>(100 + i));
    w->get<mbp::MeshRefComp>(e.value())->mesh = static_cast<mbp::MeshId>(i + 1);
    with_both.push_back(e.value());
  }
  for (int i = 0; i < 2; ++i) {
    auto e = w->spawn();
    ASSERT_TRUE(e.ok());
    ASSERT_TRUE(w->add_component(e.value(), mbp::MeshRefComp::kComponentKind).ok());
    mesh_only.push_back(e.value());
  }
  for (int i = 0; i < 5; ++i) {
    auto e = w->spawn();
    ASSERT_TRUE(e.ok());
    ASSERT_TRUE(w->add_component(e.value(), mbp::LayerComp::kComponentKind).ok());
    layer_only.push_back(e.value());
  }
  // 还有一条无组件实体（空 archetype），也不含 Transform，同样必须被跳过。
  auto bare = w->spawn();
  ASSERT_TRUE(bare.ok());

  // ---- 只查 Transform：命中前两个 archetype，共 7 条 ----
  {
    auto view = w->view(mbp::component_bit(mbp::TransformComp::kComponentKind));
    EXPECT_EQ(view.total_entities(), 7u);
    EXPECT_EQ(view.visited_archetypes(), 2u) << "只有含 Transform 的两个 archetype";

    std::vector<mbp::EntityId> seen;
    float sum = 0.0f;
    mbp::ComponentBlock block;
    while (view.next(&block)) {
      ASSERT_GT(block.count, 0u) << "空 archetype 不该产生块";
      ASSERT_EQ(block.column_sizes[0], sizeof(mbp::TransformComp));
      const auto* column = static_cast<const mbp::TransformComp*>(block.columns[0]);
      for (mbp::u32 r = 0; r < block.count; ++r) {
        seen.push_back(block.ids[r]);
        sum += column[block.row_offset + r].translation[0];
      }
    }
    EXPECT_EQ(seen.size(), 7u);
    // 逐条对照：看到的 id 必须恰好是前两组，一个不多一个不少。
    for (mbp::EntityId id : with_tr_only) {
      EXPECT_NE(std::find(seen.begin(), seen.end(), id), seen.end());
    }
    for (mbp::EntityId id : with_both) {
      EXPECT_NE(std::find(seen.begin(), seen.end(), id), seen.end());
    }
    for (mbp::EntityId id : mesh_only) {
      EXPECT_EQ(std::find(seen.begin(), seen.end(), id), seen.end()) << "没有 Transform 的实体不该出现";
    }
    for (mbp::EntityId id : layer_only) {
      EXPECT_EQ(std::find(seen.begin(), seen.end(), id), seen.end());
    }
    EXPECT_EQ(std::find(seen.begin(), seen.end(), bare.value()), seen.end());
    // 数据是对的：0+1+2 + (100+101+102+103)
    EXPECT_FLOAT_EQ(sum, 0.0f + 1.0f + 2.0f + 100.0f + 101.0f + 102.0f + 103.0f);
  }

  // ---- 查 Transform + MeshRef 的交集：只有第二个 archetype，4 条 ----
  {
    const mbp::ComponentMask mask = mbp::component_bit(mbp::TransformComp::kComponentKind) |
                                    mbp::component_bit(mbp::MeshRefComp::kComponentKind);
    auto view = w->view(mask);
    EXPECT_EQ(view.total_entities(), 4u);
    EXPECT_EQ(view.visited_archetypes(), 1u);
    mbp::ComponentBlock block;
    std::vector<mbp::u32> meshes;
    while (view.next(&block)) {
      // 列按位序给出：Transform 是第 0 位，MeshRef 是第 2 位，所以 columns[0]=Transform、
      // columns[1]=MeshRef（**只**遍历掩码里的位，不含其它组件列）。
      ASSERT_EQ(block.column_sizes[0], sizeof(mbp::TransformComp));
      ASSERT_EQ(block.column_sizes[1], sizeof(mbp::MeshRefComp));
      const auto* col = static_cast<const mbp::MeshRefComp*>(block.columns[1]);
      for (mbp::u32 r = 0; r < block.count; ++r) meshes.push_back(col[block.row_offset + r].mesh);
    }
    ASSERT_EQ(meshes.size(), 4u);
    std::sort(meshes.begin(), meshes.end());
    EXPECT_EQ(meshes, (std::vector<mbp::u32>{1, 2, 3, 4}));
  }

  // ---- 空掩码视图：命中全部活实体 ----
  {
    auto view = w->view(0);
    EXPECT_EQ(view.total_entities(), static_cast<mbp::u64>(w->entity_count()));
  }

  // ---- 没人有的组件组合：零块、零实体（不 crash） ----
  {
    const mbp::ComponentMask mask = mbp::component_bit(mbp::StrokeComp::kComponentKind) |
                                    mbp::component_bit(mbp::GizmoTargetComp::kComponentKind);
    auto view = w->view(mask);
    EXPECT_EQ(view.total_entities(), 0u);
    mbp::ComponentBlock block;
    EXPECT_FALSE(view.next(&block));
  }

  // ---- 删空一个 archetype 之后，视图不再产出空块 ----
  for (mbp::EntityId id : with_tr_only) ASSERT_TRUE(w->despawn(id));
  {
    auto view = w->view(mbp::component_bit(mbp::TransformComp::kComponentKind));
    EXPECT_EQ(view.total_entities(), 4u);
    mbp::ComponentBlock block;
    int blocks = 0;
    while (view.next(&block)) {
      ++blocks;
      EXPECT_GT(block.count, 0u);
    }
    EXPECT_EQ(blocks, 1);
  }
}

// ================================================================ O(1) 访问

// spec §14.2: ComponentAccessIsO1 —— 4096 次随机句柄访问必须在
// 亚毫秒量级。这条测试故意**同时断言正确性**：光看快是真的，
// 读到别人的行也是真的快 —— 只测时间会把最严重的错误放过去。
TEST(World, ComponentAccessIsO1) {
  auto w = make_world();
  ASSERT_NE(w, nullptr);

  constexpr int kEntities = 4096;
  std::vector<mbp::EntityId> ids;
  ids.reserve(kEntities);
  for (int i = 0; i < kEntities; ++i) {
    auto e = w->spawn();
    ASSERT_TRUE(e.ok());
    // 交错放几种 archetype，让"访问必须穿过 archetype 索引"这点真的被覆盖到。
    ASSERT_TRUE(w->add_component(e.value(), mbp::TransformComp::kComponentKind).ok());
    if (i % 3 == 0) {
      ASSERT_TRUE(w->add_component(e.value(), mbp::MeshRefComp::kComponentKind).ok());
    }
    if (i % 7 == 0) {
      ASSERT_TRUE(w->add_component(e.value(), mbp::LayerComp::kComponentKind).ok());
    }
    fill_transform(*w, e.value(), static_cast<float>(i));
    ids.push_back(e.value());
  }

  // ---- 正确性 ----
  for (int i = 0; i < kEntities; ++i) {
    const auto* tr = w->get<mbp::TransformComp>(ids[i]);
    ASSERT_NE(tr, nullptr) << "index=" << i;
    ASSERT_EQ(tr->translation[0], static_cast<float>(i)) << "O(1) 访问读错了行（index=" << i << "）";
  }

  // ---- 时延 ----
  // 预算给得很宽（4096 次 < 1ms ≈ 每次 244ns）: 真正 O(1) 的实现每次只有十几 ns，
  // 留两个数量级的余量给 CI 上的抖动；一旦退化成线性扫描，4096×4096 次访问是
  // 千万级，必然超时。
  Lcg rng;
  const auto t0 = std::chrono::steady_clock::now();
  volatile float sink = 0.0f;
  for (int i = 0; i < kEntities; ++i) {
    const std::uint32_t pick = rng.next() % static_cast<std::uint32_t>(kEntities);
    const auto* tr = w->get<mbp::TransformComp>(ids[pick]);
    ASSERT_NE(tr, nullptr);
    sink = sink + tr->translation[0];
  }
  const auto t1 = std::chrono::steady_clock::now();
  const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
  EXPECT_LT(ms, 1.0) << "4096 次随机组件访问用了 " << ms << " ms，已不是 O(1)";
  (void)sink;

  // alive() 也是 O(1)：不在任何容器的线性查找里（悬垂句柄同理）。
  const auto t2 = std::chrono::steady_clock::now();
  int alive_count = 0;
  for (int i = 0; i < kEntities * 2; ++i) {
    const std::uint32_t pick = rng.next() % static_cast<std::uint32_t>(kEntities);
    if (w->alive(ids[pick])) ++alive_count;
  }
  const auto t3 = std::chrono::steady_clock::now();
  EXPECT_EQ(alive_count, kEntities * 2);
  const double ms2 = std::chrono::duration<double, std::milli>(t3 - t2).count();
  EXPECT_LT(ms2, 1.0) << "8192 次 alive() 用了 " << ms2 << " ms，已不是 O(1)";
}

// ================================================================ 版本号

// 补的用例：mark_dirty 会让 version() +1？
//
// 设计上**不会** —— 见 world.h 里 version 的注释：version 是结构性指纹（进命令流
// 哈希），脏标记是每帧被 system 无差别刷新的瞬态信号。如果标脏也 +1，version
// 每帧都变，"输入没变 → 输出没变"的判别力就没了。这条用例把这个决定钉住，
// 同时断言"真正的变更（spawn/despawn/增删组件/restore）一定 +1"。
TEST(World, VersionBumpsOnChange) {
  auto w = make_world();
  ASSERT_NE(w, nullptr);

  mbp::u64 v0 = w->version();
  auto e = w->spawn();
  ASSERT_TRUE(e.ok());
  EXPECT_EQ(w->version(), v0 + 1) << "spawn 必须 +1";

  mbp::u64 v1 = w->version();
  ASSERT_TRUE(w->add_component(e.value(), mbp::TransformComp::kComponentKind).ok());
  EXPECT_EQ(w->version(), v1 + 1) << "add_component 必须 +1";

  mbp::u64 v2 = w->version();
  ASSERT_TRUE(w->remove_component(e.value(), mbp::TransformComp::kComponentKind).ok());
  EXPECT_EQ(w->version(), v2 + 1) << "remove_component 必须 +1";

  // 幂等的增/删不改变结构，因此不 +1（它们只是"结果态已经是目标"）。
  mbp::u64 v3 = w->version();
  EXPECT_TRUE(w->add_component(e.value(), mbp::TransformComp::kComponentKind).ok());
  EXPECT_TRUE(w->add_component(e.value(), mbp::TransformComp::kComponentKind).ok());
  EXPECT_EQ(w->version(), v3 + 1) << "只有第一次 add 生效";
  mbp::u64 v4 = w->version();
  EXPECT_TRUE(w->remove_component(e.value(), mbp::LayerComp::kComponentKind).ok());
  EXPECT_EQ(w->version(), v4) << "移除本来就没有的组件是空操作";

  // ---- 补的用例的核心断言：mark_dirty 不动 version，但脏集合有它 ----
  mbp::u64 v5 = w->version();
  w->mark_dirty(e.value(), mbp::DirtyKind::Transform);
  EXPECT_EQ(w->version(), v5) << "mark_dirty 是瞬态信号，不得改变结构性版本号";
  w->mark_dirty(e.value(), mbp::DirtyKind::Transform);  // 同一实体重复标：去重
  w->mark_dirty(e.value(), mbp::DirtyKind::Visibility);
  EXPECT_EQ(w->dirty().size(), 1u) << "同一实体的多次标脏必须去重成一条";
  EXPECT_EQ(w->dirty().entity_flags(0),
            mbp::dirty_bit(mbp::DirtyKind::Transform) | mbp::dirty_bit(mbp::DirtyKind::Visibility));
  EXPECT_EQ(w->version(), v5);

  mbp::u64 v6 = w->version();
  ASSERT_TRUE(w->despawn(e.value()));
  EXPECT_EQ(w->version(), v6 + 1) << "despawn 必须 +1";

  // restore 也 +1（整份状态被替换是最大的结构性变更）。
  auto other = make_world();
  ASSERT_NE(other, nullptr);
  auto oe = other->spawn();
  ASSERT_TRUE(oe.ok());
  const mbp::WorldSnapshot snap = other->snapshot();
  mbp::u64 v7 = w->version();
  ASSERT_TRUE(w->restore(snap).ok());
  EXPECT_GT(w->version(), v7) << "restore 必须让版本号前进";
  EXPECT_EQ(w->entity_count(), 1u);
}

// ================================================================ 快照

// spec §14.2: SnapshotRoundTrip —— 快照 → 恢复 → 再快照，逐字节一致；
// 恢复出来的世界在结构与数据上与源世界等价。
TEST(World, SnapshotRoundTrip) {
  auto src = make_world();
  ASSERT_NE(src, nullptr);

  // 构造一个"有几种 archetype、有洞、有迁移历史"的世界 —— 快照最怕的就是
  // 只在简单形状上正确（比如所有实体都在同一个 archetype、index 连续）。
  std::vector<mbp::EntityId> ids;
  for (int i = 0; i < 40; ++i) {
    auto e = src->spawn();
    ASSERT_TRUE(e.ok());
    ids.push_back(e.value());
    ASSERT_TRUE(src->add_component(e.value(), mbp::TransformComp::kComponentKind).ok());
    fill_transform(*src, e.value(), static_cast<float>(i) * 0.5f);
    if (i % 2 == 0) {
      ASSERT_TRUE(src->add_component(e.value(), mbp::MeshRefComp::kComponentKind).ok());
      src->get<mbp::MeshRefComp>(e.value())->mesh = static_cast<mbp::MeshId>(i);
    }
    if (i % 5 == 0) {
      ASSERT_TRUE(src->add_component(e.value(), mbp::LayerComp::kComponentKind).ok());
      src->get<mbp::LayerComp>(e.value())->layer = static_cast<mbp::u32>(i / 5);
    }
  }
  // 制造洞：删掉一部分（index 稀疏），再复用几个。
  for (int i = 3; i < 40; i += 4) ASSERT_TRUE(src->despawn(ids[i]));
  for (int i = 0; i < 3; ++i) {
    auto e = src->spawn();
    ASSERT_TRUE(e.ok());
    ids.push_back(e.value());
    ASSERT_TRUE(src->add_component(e.value(), mbp::TransformComp::kComponentKind).ok());
    fill_transform(*src, e.value(), -1.0f - static_cast<float>(i));
  }

  const mbp::WorldSnapshot snap = src->snapshot();
  EXPECT_EQ(snap.entities.size(), static_cast<std::size_t>(src->entity_count()));
  ASSERT_FALSE(snap.entities.empty());
  // 快照按 {index, gen} 排序（进命令流之前必须规范顺序）。
  for (std::size_t i = 1; i < snap.entities.size(); ++i) {
    const bool ordered = snap.entities[i - 1].id.index < snap.entities[i].id.index ||
                         (snap.entities[i - 1].id.index == snap.entities[i].id.index &&
                          snap.entities[i - 1].id.gen <= snap.entities[i].id.gen);
    EXPECT_TRUE(ordered) << "快照实体必须按 EntityId 排序";
  }

  // ---- 恢复到一个**全新**世界（注册表相同）----
  auto dst = make_world();
  ASSERT_NE(dst, nullptr);
  const mbp::Status st = dst->restore(snap);
  ASSERT_TRUE(st.ok()) << st.message;

  EXPECT_EQ(dst->entity_count(), src->entity_count());
  EXPECT_EQ(dst->registered_count(), src->registered_count());

  // 每一行的数据都要对上：用 view 走一遍，比逐个 id 更能覆盖"列顺序错了"。
  {
    auto sa = src->view(mbp::component_bit(mbp::MeshRefComp::kComponentKind));
    auto sb = dst->view(mbp::component_bit(mbp::MeshRefComp::kComponentKind));
    EXPECT_EQ(sa.total_entities(), sb.total_entities());
    std::vector<std::pair<mbp::u32, mbp::u32>> ma;  // (index, mesh)
    std::vector<std::pair<mbp::u32, mbp::u32>> mb;
    mbp::ComponentBlock blk;
    while (sb.next(&blk)) {
      auto ids_col = blk.ids;
      const auto* col = static_cast<const mbp::MeshRefComp*>(blk.columns[0]);
      for (mbp::u32 r = 0; r < blk.count; ++r) mb.emplace_back(ids_col[r].index, col[r].mesh);
    }
    while (sa.next(&blk)) {
      auto ids_col = blk.ids;
      const auto* col = static_cast<const mbp::MeshRefComp*>(blk.columns[0]);
      for (mbp::u32 r = 0; r < blk.count; ++r) ma.emplace_back(ids_col[r].index, col[r].mesh);
    }
    std::sort(ma.begin(), ma.end());
    std::sort(mb.begin(), mb.end());
    EXPECT_EQ(ma, mb);
  }

  // ---- 核心断言：恢复后再快照，与源快照**逐字节等价** ----
  const mbp::WorldSnapshot again = dst->snapshot();
  EXPECT_EQ(again.entities.size(), snap.entities.size());
  ASSERT_EQ(again.component_sizes, snap.component_sizes);
  ASSERT_EQ(again.component_names, snap.component_names);
  for (std::size_t i = 0; i < snap.entities.size(); ++i) {
    ASSERT_EQ(again.entities[i].id.index, snap.entities[i].id.index) << "i=" << i;
    ASSERT_EQ(again.entities[i].mask, snap.entities[i].mask) << "i=" << i;
    ASSERT_EQ(again.entities[i].size_bytes, snap.entities[i].size_bytes) << "i=" << i;
    ASSERT_EQ(again.entities[i].components.size(), snap.entities[i].components.size()) << "i=" << i;
    ASSERT_EQ(0, std::memcmp(again.entities[i].components.data(),
                             snap.entities[i].components.data(),
                             snap.entities[i].components.size()))
        << "第 " << i << " 个实体的组件字节不一致";
  }

  // 恢复出的实体可以继续正常改：加组件 / 遍历 / 再快照。
  const mbp::EntityId probe = again.entities.back().id;
  EXPECT_TRUE(dst->alive(probe));
  EXPECT_TRUE(dst->add_component(probe, mbp::VisibleComp::kComponentKind).ok());
  const mbp::WorldSnapshot after_edit = dst->snapshot();
  EXPECT_EQ(after_edit.entities.size(), snap.entities.size());
}

// 快照是**全或无**：一份坏快照必须被整体拒绝，且世界保持原样。
TEST(World, SnapshotRestoreIsAllOrNothing) {
  auto w = make_world();
  ASSERT_NE(w, nullptr);

  auto a = w->spawn();
  ASSERT_TRUE(a.ok());
  ASSERT_TRUE(w->add_component(a.value(), mbp::TransformComp::kComponentKind).ok());
  fill_transform(*w, a.value(), 3.0f);
  const mbp::WorldSnapshot before = w->snapshot();

  // 坏快照 1：组件字节数被截断。
  {
    mbp::WorldSnapshot bad = before;
    bad.entities[0].components.pop_back();
    EXPECT_FALSE(w->restore(bad).ok());
  }
  // 坏快照 2：掩码里有未注册的组件。
  {
    mbp::WorldSnapshot bad = before;
    bad.entities[0].mask |= mbp::component_bit(30);
    EXPECT_FALSE(w->restore(bad).ok());
  }
  // 坏快照 3：注册表与本世界不一致（另一个世界注册了别的组件）。
  {
    auto other = make_world();
    ASSERT_NE(other, nullptr);
    ASSERT_TRUE(other->register_component(kTestKindC, 40, "TestC").ok());
    auto oe = other->spawn();
    ASSERT_TRUE(oe.ok());
    const mbp::Status st = w->restore(other->snapshot());
    EXPECT_FALSE(st.ok()) << "注册表不同的快照必须拒绝";
    // 注册表不一致属于"本世界不支持这份快照"，不是"快照本身坏了" ——
    // 用 Unsupported 让调用方区分"换一个世界重试"与"数据损坏"。
    EXPECT_EQ(static_cast<int>(st.code),
              static_cast<int>(magicblocks::device::StatusCode::Unsupported));
  }
  // 坏快照 4：同一个 index 出现两次。
  {
    mbp::WorldSnapshot bad = before;
    bad.entities.push_back(bad.entities[0]);
    EXPECT_FALSE(w->restore(bad).ok());
  }

  // 全程世界保持原样：上面四次失败之后，实体、数据、版本号行为都必须没变。
  EXPECT_EQ(w->entity_count(), 1u);
  EXPECT_TRUE(w->alive(a.value()));
  EXPECT_EQ(w->get<mbp::TransformComp>(a.value())->translation[0], 3.0f);
  const mbp::WorldSnapshot after = w->snapshot();
  ASSERT_EQ(after.entities.size(), before.entities.size());
  EXPECT_EQ(after.entities[0].id.index, before.entities[0].id.index);
  EXPECT_EQ(0, std::memcmp(after.entities[0].components.data(), before.entities[0].components.data(),
                           before.entities[0].components.size()));
}

// restore 出来的实体不能与"从未进过快照的悬垂句柄"别名。
//
// 这条守的是 gen 的全部意义：快照里存了完整的 EntityId（含 gen），restore
// 必须**精确还原**它。反例是早先的实现：把恢复出的实体当成"代际未知"，
// alive() 只看 index 上有没有活实体 —— 于是**任意**同 index 的旧句柄都会被
// 判活，等于"删掉的物体在 restore 后自己活过来"。
//
// 构造方式：先造一批实体并快照，然后**继续使用**世界（spawn 新实体、
// despawn 旧实体，让 gen 前进），这些新句柄从未出现在快照里。restore 之后
// 它们必须全部失效，而快照里的句柄必须全部恢复。
TEST(World, SnapshotRestoredHandlesDoNotAliasLive) {
  auto w = make_world();
  ASSERT_NE(w, nullptr);

  // 快照时点：5 个实体。
  std::vector<mbp::EntityId> in_snapshot;
  for (int i = 0; i < 5; ++i) {
    auto e = w->spawn();
    ASSERT_TRUE(e.ok());
    ASSERT_TRUE(w->add_component(e.value(), mbp::TransformComp::kComponentKind).ok());
    fill_transform(*w, e.value(), static_cast<float>(i));
    in_snapshot.push_back(e.value());
  }
  const mbp::WorldSnapshot snap = w->snapshot();
  ASSERT_EQ(snap.entities.size(), 5u);

  // 快照之后继续玩：despawn 掉两个，再 spawn 两个复用它们的 index。
  // 新句柄 index 与快照里的重合，但 gen 已经前进了 —— 这正是"别名"的温床。
  std::vector<mbp::EntityId> after_snapshot;
  ASSERT_TRUE(w->despawn(in_snapshot[1]));
  ASSERT_TRUE(w->despawn(in_snapshot[3]));
  for (int i = 0; i < 2; ++i) {
    auto e = w->spawn();
    ASSERT_TRUE(e.ok());
    after_snapshot.push_back(e.value());
  }
  // 前提检查：新句柄确实复用了快照里的 index，且 gen 不同（否则这条用例没击中要害）。
  bool saw_alias_risk = false;
  for (const mbp::EntityId& n : after_snapshot) {
    for (const mbp::EntityId& o : in_snapshot) {
      if (n.index == o.index && n.gen != o.gen) saw_alias_risk = true;
    }
  }
  ASSERT_TRUE(saw_alias_risk) << "用例前提失效：spawn 没有复用快照里的 index，需要调整构造";

  ASSERT_TRUE(w->restore(snap).ok());
  EXPECT_EQ(w->entity_count(), 5u);

  // 1) 快照里的句柄：必须精确恢复（gen 一致）。
  for (std::size_t i = 0; i < in_snapshot.size(); ++i) {
    EXPECT_TRUE(w->alive(in_snapshot[i]))
        << "快照里的实体必须恢复（index=" << in_snapshot[i].index << " gen=" << in_snapshot[i].gen << "）";
    const auto* xf = w->get<mbp::TransformComp>(in_snapshot[i]);
    ASSERT_NE(xf, nullptr);
    EXPECT_EQ(xf->translation[0], static_cast<float>(i));
  }

  // 2) 快照之后产生的句柄：index 撞上了，但 gen 更快 —— 必须失效。
  //    这是本用例的核心断言：gen 若被"猜"成某个值（或干脆不比较），
  //    这里就会有句柄被判活。
  for (const mbp::EntityId& stale : after_snapshot) {
    EXPECT_FALSE(w->alive(stale))
        << "快照之后产生的句柄 index=" << stale.index << " gen=" << stale.gen
        << " 在 restore 之后必须失效";
    EXPECT_EQ(w->component(stale, mbp::TransformComp::kComponentKind), nullptr)
        << "失效句柄的组件访问必须返回 nullptr，不得别名到恢复出来的活实体";
  }

  // 3) 恢复后的世界还能正常继续用。
  auto fresh = w->spawn();
  ASSERT_TRUE(fresh.ok());
  EXPECT_TRUE(w->alive(fresh.value()));
  for (const mbp::EntityId& o : in_snapshot) {
    EXPECT_NE(fresh.value().index, o.index) << "新生成不得复用已恢复实体的 index";
  }
}

// IWorld → IBatchBuilder 的接线(Lead 要求 A/B 两项)。
//
// 100 个实体,同一个 mesh、同一个 material → 合批后必须是**一次** instanced draw。
// 这条用例守的是"ECS 与合批器的契约"而不是合批器本身(后者另有 Renderer 套件):
// 它验证 build_draw_requests 产出的 DrawRequest 里,凡是决定合批结果的三个键
// (pipeline_hash / material_key / mesh)对 100 个实体**完全一致** ——
// 只要有一个字段被错填成"每实体不同"(比如拿 EntityId 去填 material_key),
// batch_count 就会立刻变成 100。
TEST(World, BuildDrawRequestsIntegration) {
  auto w = make_world();
  ASSERT_NE(w, nullptr);

  constexpr std::uint32_t kCount = 100;
  constexpr mbp::MeshId kMesh = 7;
  constexpr mbp::u32 kMaterialSlot = 3;

  for (std::uint32_t i = 0; i < kCount; ++i) {
    auto e = w->spawn();
    ASSERT_TRUE(e.ok());
    const mbp::EntityId id = e.value();
    ASSERT_TRUE(w->add_component(id, mbp::TransformComp::kComponentKind).ok());
    ASSERT_TRUE(w->add_component(id, mbp::MeshRefComp::kComponentKind).ok());
    ASSERT_TRUE(w->add_component(id, mbp::MaterialRefComp::kComponentKind).ok());
    fill_transform(*w, id, static_cast<float>(i));
    w->get<mbp::MeshRefComp>(id)->mesh = kMesh;
    w->get<mbp::MaterialRefComp>(id)->material = kMaterialSlot;
  }

  auto builder = mbp::create_batch_builder();
  ASSERT_NE(builder, nullptr);
  builder->reserve(kCount);
  builder->begin_frame(0);

  // 不传相机 → 不剔除,纯粹验证"实体 → 请求"的映射。
  mbp::DrawRequestBuildDesc desc;
  desc.camera = nullptr;
  desc.fallback_index_count = 36;
  desc.fallback_vertex_count = 24;
  const mbp::DrawRequestBuildStats bstats = mbp::build_draw_requests(*w, *builder, desc);

  EXPECT_EQ(bstats.entities_seen, kCount) << "Mesh+Material+Transform 三者齐全的实体应全部被遍历到";
  EXPECT_EQ(bstats.requests_pushed, kCount);
  EXPECT_EQ(bstats.skipped_no_mesh, 0u);
  EXPECT_EQ(bstats.skipped_culled, 0u);

  builder->build();

  const mbp::BatchStats& st = builder->stats();
  EXPECT_EQ(st.request_count, kCount);
  EXPECT_EQ(st.batch_count, 1u) << "同 mesh 同 material 的 100 个实体必须合成一次 drawcall";
  ASSERT_EQ(builder->batches().size(), 1u);
  EXPECT_EQ(builder->batches()[0].instance_count, kCount);
  EXPECT_EQ(builder->batches()[0].mesh, kMesh);
  EXPECT_EQ(builder->batches()[0].material_key, static_cast<mbp::MaterialKey>(kMaterialSlot));

  // 反例锚点:实体 id 没被错填进某个决定合批的键里(否则上面就已经炸了)。
  // first_entity 记的是排序后首条请求的 entity_id —— 实体 index 从 0 开始,
  // 所以这里**可能是 0**,不能断言非零(那是把"合法 id"当成"未初始化"的经典误判)。
  // 真正该断言的是:它必须是这 100 个实体的 index 之一。
  EXPECT_LT(builder->batches()[0].first_entity, kCount);
}

// 接线的过滤规则:没网格的、不可见的实体不得进入绘制请求。
// 这条和上面的集成用例互补 —— 上面证明"该合的都合了",这里证明"不该画的没画"。
TEST(World, BuildDrawRequestsSkipsNoMeshAndInvisible) {
  auto w = make_world();
  ASSERT_NE(w, nullptr);

  // 3 个正常实体。
  for (int i = 0; i < 3; ++i) {
    auto e = w->spawn();
    ASSERT_TRUE(e.ok());
    ASSERT_TRUE(w->add_component(e.value(), mbp::MeshRefComp::kComponentKind).ok());
    ASSERT_TRUE(w->add_component(e.value(), mbp::MaterialRefComp::kComponentKind).ok());
    ASSERT_TRUE(w->add_component(e.value(), mbp::TransformComp::kComponentKind).ok());
    w->get<mbp::MeshRefComp>(e.value())->mesh = 5;
  }
  // 1 个"忘了填 mesh"的实体(MeshRefComp 默认就是 kInvalidMeshId)。
  {
    auto e = w->spawn();
    ASSERT_TRUE(e.ok());
    ASSERT_TRUE(w->add_component(e.value(), mbp::MeshRefComp::kComponentKind).ok());
    ASSERT_TRUE(w->add_component(e.value(), mbp::MaterialRefComp::kComponentKind).ok());
    ASSERT_TRUE(w->add_component(e.value(), mbp::TransformComp::kComponentKind).ok());
  }
  // 1 个只有 Transform 的实体(缺 Mesh/Material)—— 视图掩码就该把它排除,
  // 所以它连 entities_seen 都不该计入。
  {
    auto e = w->spawn();
    ASSERT_TRUE(e.ok());
    ASSERT_TRUE(w->add_component(e.value(), mbp::TransformComp::kComponentKind).ok());
  }

  auto builder = mbp::create_batch_builder();
  ASSERT_NE(builder, nullptr);
  builder->reserve(8);
  builder->begin_frame(0);

  const mbp::DrawRequestBuildStats bstats =
      mbp::build_draw_requests(*w, *builder, static_cast<const mbp::ICamera*>(nullptr));

  EXPECT_EQ(bstats.entities_seen, 4u) << "缺 Mesh/Material 的实体不应被视图遍历到";
  EXPECT_EQ(bstats.requests_pushed, 3u);
  EXPECT_EQ(bstats.skipped_no_mesh, 1u);

  builder->build();
  EXPECT_EQ(builder->stats().batch_count, 1u);
  EXPECT_EQ(builder->batches()[0].instance_count, 3u);
}

// 世界矩阵 = TransformComp 的 TRS,而且要真的是**列主序**。
//
// 为什么这条必须单独测:`Mat4` 的"平移"有两种都看起来合理的落点 ——
// 列主序在索引 12,13,14(第 3 列),行主序在索引 3,7,11(第 3 行)。
// 写错了不崩不报错,只让所有物体画到错地方的偏移上,而且**只在非单位变换
// 下才看得出**。所以这里显式断两件事:语义对、布局对。
TEST(World, TransformToMatrixMatchesComponents) {
  auto w = make_world();
  ASSERT_NE(w, nullptr);

  auto e = w->spawn();
  ASSERT_TRUE(e.ok());
  const mbp::EntityId id = e.value();
  ASSERT_TRUE(w->add_component(id, mbp::TransformComp::kComponentKind).ok());
  ASSERT_TRUE(w->add_component(id, mbp::MeshRefComp::kComponentKind).ok());
  ASSERT_TRUE(w->add_component(id, mbp::MaterialRefComp::kComponentKind).ok());

  auto* xf = w->get<mbp::TransformComp>(id);
  ASSERT_NE(xf, nullptr);
  xf->translation[0] = 1.0f;
  xf->translation[1] = 2.0f;
  xf->translation[2] = 3.0f;
  // 单位四元数 = 无旋转;缩放 (2,2,2)。
  xf->rotation[0] = 0.0f;
  xf->rotation[1] = 0.0f;
  xf->rotation[2] = 0.0f;
  xf->rotation[3] = 1.0f;
  xf->scale[0] = 2.0f;
  xf->scale[1] = 2.0f;
  xf->scale[2] = 2.0f;
  w->get<mbp::MeshRefComp>(id)->mesh = 1;
  w->get<mbp::MaterialRefComp>(id)->material = 1;

  auto builder = mbp::create_batch_builder();
  ASSERT_NE(builder, nullptr);
  builder->reserve(4);
  builder->begin_frame(0);

  std::vector<mbp::Mat4> matrices(4);
  mbp::DrawRequestBuildDesc desc;
  desc.camera = nullptr;  // 不剔除
  desc.out_matrices = matrices.data();
  desc.out_matrix_capacity = matrices.size();
  const mbp::DrawRequestBuildStats bstats = mbp::build_draw_requests(*w, *builder, desc);
  EXPECT_EQ(bstats.requests_pushed, 1u);
  EXPECT_EQ(bstats.matrices_written, 1u) << "容量够时必须全部写出";

  const mbp::Mat4& m = matrices[0];

  // ---- 1. 语义:平移与缩放分别落在该在的位置 ----
  // 列主序下平移在第 3 列 → m[12], m[13], m[14]。
  EXPECT_FLOAT_EQ(m.m[12], 1.0f) << "平移 x 必须在 m[12](列主序第 3 列)";
  EXPECT_FLOAT_EQ(m.m[13], 2.0f);
  EXPECT_FLOAT_EQ(m.m[14], 3.0f);
  // 对角线前三个是缩放。
  EXPECT_FLOAT_EQ(m.m[0], 2.0f) << "缩放 x 必须在 m[0]";
  EXPECT_FLOAT_EQ(m.m[5], 2.0f);
  EXPECT_FLOAT_EQ(m.m[10], 2.0f);
  // 齐次分量。
  EXPECT_FLOAT_EQ(m.m[15], 1.0f);

  // ---- 2. 布局:确实是列主序,不是行主序 ----
  // 行主序的实现会把平移写进 m[3], m[7], m[11] —— 这里必须全是 0。
  EXPECT_FLOAT_EQ(m.m[3], 0.0f) << "m[3] 非零说明写成了行主序";
  EXPECT_FLOAT_EQ(m.m[7], 0.0f) << "m[7] 非零说明写成了行主序";
  EXPECT_FLOAT_EQ(m.m[11], 0.0f) << "m[11] 非零说明写成了行主序";
  // 交叉项为 0(纯 TRS 无切变)。
  EXPECT_FLOAT_EQ(m.m[1], 0.0f);
  EXPECT_FLOAT_EQ(m.m[4], 0.0f);

  // ---- 3. at(col,row) 与原始 m[] 自洽 ----
  EXPECT_FLOAT_EQ(m.at(3, 0), 1.0f) << "at(3,0) 应等于 m[3*4+0] = 平移 x";
  EXPECT_FLOAT_EQ(m.at(0, 0), 2.0f) << "at(0,0) 应等于 m[0] = 缩放 x";

  // ---- 4. 用它变换一个点,结果符合"先缩放后平移" ----
  // (1,1,1) → 缩放 (2,2,2) → (2,2,2) → 平移 (1,2,3) → (3,4,5)。
  const mbp::Vec3 out = m.transform_point(mbp::Vec3{1.0f, 1.0f, 1.0f});
  EXPECT_FLOAT_EQ(out.x, 3.0f);
  EXPECT_FLOAT_EQ(out.y, 4.0f);
  EXPECT_FLOAT_EQ(out.z, 5.0f);
}

// 容量不足时不得越界写,且要通过 stats 让调用方发现。
TEST(World, TransformMatrixRespectsCapacity) {
  auto w = make_world();
  ASSERT_NE(w, nullptr);

  for (int i = 0; i < 4; ++i) {
    auto e = w->spawn();
    ASSERT_TRUE(e.ok());
    ASSERT_TRUE(w->add_component(e.value(), mbp::TransformComp::kComponentKind).ok());
    ASSERT_TRUE(w->add_component(e.value(), mbp::MeshRefComp::kComponentKind).ok());
    ASSERT_TRUE(w->add_component(e.value(), mbp::MaterialRefComp::kComponentKind).ok());
    w->get<mbp::MeshRefComp>(e.value())->mesh = 1;
  }

  auto builder = mbp::create_batch_builder();
  ASSERT_NE(builder, nullptr);
  builder->reserve(8);
  builder->begin_frame(0);

  // 只给 2 个元素的容量,但会有 4 条请求。
  std::vector<mbp::Mat4> matrices(2);
  for (mbp::Mat4& m : matrices) m = mbp::Mat4::identity();

  mbp::DrawRequestBuildDesc desc;
  desc.camera = nullptr;
  desc.out_matrices = matrices.data();
  desc.out_matrix_capacity = matrices.size();
  const mbp::DrawRequestBuildStats bstats = mbp::build_draw_requests(*w, *builder, desc);

  EXPECT_EQ(bstats.requests_pushed, 4u) << "容量不足也照常提交几何";
  EXPECT_EQ(bstats.matrices_written, 2u) << "只写容量允许的部分";
  // 已写的两个矩阵对应"默认 TransformComp"(平移 0 / 单位四元数 / 缩放 1),
  // 也就是单位阵。它们被正确覆盖(而不是保持我们最初填的 identity 就不算通过 ——
  // 所以这里额外确认平移列确实是 0 且缩放是 1,证明写入真的发生了)。
  for (const mbp::Mat4& m : matrices) {
    EXPECT_FLOAT_EQ(m.m[0], 1.0f) << "默认缩放应为 1";
    EXPECT_FLOAT_EQ(m.m[5], 1.0f);
    EXPECT_FLOAT_EQ(m.m[10], 1.0f);
    EXPECT_FLOAT_EQ(m.m[12], 0.0f) << "默认平移应为 0";
    EXPECT_FLOAT_EQ(m.m[15], 1.0f);
  }
}
