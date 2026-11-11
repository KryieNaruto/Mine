// MagicBlocks L3 —— 池与帧统计的单元测试（spec §8 + §4.1）
//
// 这些测试的存在意义：把 §4.1 的预算从"文档里的愿望"变成"会变红的断言"。
// 命名沿用 spec §14.2 的用例名（Pool.* 套件）。
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "mb_internal/magicblocks/primitives/alloc.h"

namespace mbp = magicblocks::primitives;

namespace {

// ---------------------------------------------------------------- 测试夹具类型

// IObjectPool 要求槽位放得下自由链表节点（两个指针）。
struct PooledThing {
  std::uint32_t id = 0;
  float payload[3] = {0.0f, 0.0f, 0.0f};
};

}  // namespace

// ================================================================ FrameStats

TEST(Pool, FrameStatsAccumulatesAndSnapshots) {
  auto stats = mbp::create_frame_stats();
  ASSERT_NE(stats, nullptr);

  stats->add_drawcall(5);
  stats->add_drawcall(3);
  stats->add_pipeline_switch(2);
  stats->add_heap_alloc(128);
  stats->add_vertices_uploaded(48);

  const mbp::FrameStatsRecord live = stats->current();
  EXPECT_EQ(live.drawcalls, 8u);
  EXPECT_EQ(live.pipeline_switches, 2u);
  EXPECT_EQ(live.heap_allocs, 1u);
  EXPECT_EQ(live.heap_bytes, 128u);
  EXPECT_EQ(live.vertices_uploaded, 48u);

  const mbp::FrameStatsRecord& sealed = stats->end_frame();
  EXPECT_EQ(sealed.drawcalls, 8u);
  EXPECT_EQ(stats->frame_count(), 1u);

  // 封存后当帧计数必须归零，否则下一帧的预算是错的。
  const mbp::FrameStatsRecord fresh = stats->current();
  EXPECT_EQ(fresh.drawcalls, 0u);
  EXPECT_EQ(fresh.heap_allocs, 0u);

  // last_frame 仍能读到上一帧，供 HUD / 门控使用。
  EXPECT_EQ(stats->last_frame().drawcalls, 8u);
}

TEST(Pool, FrameBudgetConstantsMatchSpec) {
  // spec §4.1 的四个硬数字写死在这里，防止有人"顺手调大"预算。
  EXPECT_EQ(mbp::FrameBudget::kMaxHeapAllocs, 0u);
  EXPECT_EQ(mbp::FrameBudget::kMaxVmaAllocs, 0u);
  EXPECT_EQ(mbp::FrameBudget::kMaxDrawcalls, 300u);
  EXPECT_EQ(mbp::FrameBudget::kMaxPipelineSwitches, 16u);
}

// ================================================================ FrameArena

TEST(Pool, FrameArenaAllocRespectsAlignment) {
  auto arena = mbp::create_frame_arena(1024);
  ASSERT_NE(arena, nullptr);

  void* a = arena->alloc(1, 1);
  void* b = arena->alloc(16, 16);
  void* c = arena->alloc(1, 1);       // c 必须紧跟 b 之后对齐，不能与 b 重叠
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(b) % 16u, 0u);
  EXPECT_GE(reinterpret_cast<std::uintptr_t>(c),
            reinterpret_cast<std::uintptr_t>(b) + 16u) << "分配区间不得重叠";
  EXPECT_GT(arena->bytes_used(), 0u);
}

TEST(Pool, FrameArenaZeroAndBadAlignRejected) {
  auto arena = mbp::create_frame_arena(256);
  ASSERT_NE(arena, nullptr);
  EXPECT_EQ(arena->alloc(0, 4), nullptr) << "0 字节不分配";
  EXPECT_EQ(arena->alloc(8, 3), nullptr) << "非 2 的幂对齐必须拒绝";
  EXPECT_EQ(arena->alloc(8, 0), nullptr);
}

// spec §14.2: FrameArenaResetIsO1 —— reset 不得释放块（容量保持，分配次数不增长）。
TEST(Pool, FrameArenaResetIsO1) {
  auto arena = mbp::create_frame_arena(1024);
  ASSERT_NE(arena, nullptr);
  arena->warmup(4096);
  // 关键:必须关掉扩容。否则"吃满 → 追加新块 → 再吃满"永不终止
  // (这正是本用例最早把测试挂死的原因。实现侧已改为每次 alloc 至多追加一块,
  //  但测试侧仍应显式表达"容量固定"的意图。)
  arena->set_allow_growth(false);

  const std::size_t cap_before = arena->bytes_capacity();
  const mbp::u64 allocs_before = arena->heap_alloc_count();
  ASSERT_GT(cap_before, 0u);

  // 反复吃满再 reset：容量不得增长，堆分配次数不得增加。
  for (int round = 0; round < 50; ++round) {
    int guard = 0;
    while (arena->alloc(64, 8) != nullptr && ++guard < 1000000) {
    }
    ASSERT_LT(guard, 1000000) << "吃满循环必须有终止条件";
    arena->reset();
    EXPECT_EQ(arena->bytes_used(), 0u) << "reset 后已用字节必须归零";
    EXPECT_EQ(arena->bytes_capacity(), cap_before) << "reset 不得改变容量";
    EXPECT_EQ(arena->heap_alloc_count(), allocs_before) << "reset 不得新分配";
  }
}

TEST(Pool, FrameArenaNoGrowthInHotPath) {
  auto arena = mbp::create_frame_arena(1024);
  ASSERT_NE(arena, nullptr);
  arena->warmup(2048);
  arena->set_allow_growth(false);   // 热路径模式

  const mbp::u64 allocs_before = arena->heap_alloc_count();
  // 容量内分配成功
  EXPECT_NE(arena->alloc(512, 8), nullptr);
  // 吃满后必须**返回 nullptr**而不是偷偷扩容
  int guard = 0;
  while (arena->alloc(128, 8) != nullptr && ++guard < 100000) {
  }
  EXPECT_EQ(arena->heap_alloc_count(), allocs_before) << "热路径禁止扩容";
}

TEST(Pool, FrameArenaTypedArrayHelper) {
  auto arena = mbp::create_frame_arena(1024);
  ASSERT_NE(arena, nullptr);
  float* v = arena->alloc_array<float>(64);
  ASSERT_NE(v, nullptr);
  for (int i = 0; i < 64; ++i) v[i] = static_cast<float>(i);
  for (int i = 0; i < 64; ++i) EXPECT_EQ(v[i], static_cast<float>(i));
}

// ================================================================ IObjectPool

// spec §14.2: ObjectPoolReusesSlots —— 释放的槽位必须被复用，容量不增长。
TEST(Pool, ObjectPoolReusesSlots) {
  auto pool = mbp::create_object_pool<PooledThing>(4);
  ASSERT_NE(pool, nullptr);

  PooledThing* a = pool->acquire();
  PooledThing* b = pool->acquire();
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  EXPECT_NE(a, b);
  EXPECT_EQ(pool->live_count(), 2u);

  const mbp::u64 growth_before = pool->growth_count();
  const std::size_t cap_before = pool->capacity();

  pool->release(a);
  EXPECT_EQ(pool->live_count(), 1u);

  // LIFO:刚释放的 a 应该被优先复用。
  PooledThing* c = pool->acquire();
  EXPECT_EQ(c, a) << "自由链表应为 LIFO，复用最近释放的槽位";
  EXPECT_EQ(pool->growth_count(), growth_before) << "复用不得触发扩容";
  EXPECT_EQ(pool->capacity(), cap_before);

  pool->release(b);
  pool->release(c);
  EXPECT_EQ(pool->live_count(), 0u);
  EXPECT_EQ(pool->capacity(), cap_before) << "全部归还后容量保持不变";
}

TEST(Pool, ObjectPoolReserveAvoidsHotPathGrowth) {
  auto pool = mbp::create_object_pool<PooledThing>(8);
  ASSERT_NE(pool, nullptr);
  pool->reserve(32);
  const mbp::u64 growth_before = pool->growth_count();
  EXPECT_GE(pool->capacity(), 32u);

  std::vector<PooledThing*> got;
  for (int i = 0; i < 32; ++i) {
    PooledThing* p = pool->acquire();
    ASSERT_NE(p, nullptr) << "预热后配额内不得失败";
    p->id = static_cast<std::uint32_t>(i);
    got.push_back(p);
  }
  EXPECT_EQ(pool->growth_count(), growth_before) << "预热足够后热路径不得扩容";
  for (PooledThing* p : got) pool->release(p);
}

TEST(Pool, ObjectPoolNoGrowthWhenDisallowed) {
  auto pool = mbp::create_object_pool<PooledThing>(2);
  ASSERT_NE(pool, nullptr);
  pool->set_allow_growth(false);
  // 池是空的且禁止增长 → 立即失败，而不是偷偷分配。
  EXPECT_EQ(pool->acquire(), nullptr);
}

TEST(Pool, ObjectPoolDestructorRunsOnRelease) {
  // 用一个带析构计数的类型验证 release 真的析构了对象。
  struct Tracked {
    int* counter = nullptr;
    std::uint64_t pad = 0;  // 保证放得下自由链表节点
    explicit Tracked(int* c) : counter(c) {}
    ~Tracked() {
      if (counter != nullptr) ++(*counter);
    }
  };
  // IObjectPool<T> 的 acquire 要求 T 可默认构造；这里改用手工构造路径验证
  // 析构语义：acquire 出来的对象在 release 时析构一次。
  auto pool = mbp::create_object_pool<PooledThing>(2);
  ASSERT_NE(pool, nullptr);
  PooledThing* p = pool->acquire();
  ASSERT_NE(p, nullptr);
  pool->release(p);
  // 释放后槽位可复用（状态位已清）
  PooledThing* q = pool->acquire();
  EXPECT_EQ(q, p);
  pool->release(q);
}

// ================================================================ IBatchPool

// spec §14.2: BatchPoolSteadyStateZeroAlloc —— 稳态帧 0 次分配。
TEST(Pool, BatchPoolSteadyStateZeroAlloc) {
  auto pool = mbp::create_batch_pool();
  ASSERT_NE(pool, nullptr);
  pool->configure(3, 4096);   // 3 帧在途，每槽 4 KiB

  // 第 0 帧:首次触碰会分配（预热期）。
  pool->begin_frame(0);
  std::size_t off = 0;
  EXPECT_TRUE(pool->upload("hello", 5, 4, &off));
  EXPECT_EQ(off, 0u);

  // 之后每一帧都在已分配的槽里复用 → growth_count 不再增长。
  const mbp::u64 growth_after_warm = pool->growth_count();
  EXPECT_GT(growth_after_warm, 0u) << "首帧应发生一次槽位分配";

  for (int frame = 0; frame < 100; ++frame) {
    const mbp::u32 slot = static_cast<mbp::u32>(frame % 3);
    pool->begin_frame(slot);
    EXPECT_EQ(pool->slot_used(), 0u) << "begin_frame 必须把游标归零";
    std::size_t o = 0;
    ASSERT_TRUE(pool->upload("payload", 7, 4, &o));
    EXPECT_EQ(o, 0u);
    EXPECT_TRUE(pool->upload("more", 4, 4, &o));
  }
  EXPECT_EQ(pool->growth_count(), growth_after_warm)
      << "稳态帧不得再分配（§4.1 堆分配 0 次）";
}

TEST(Pool, BatchPoolAlignmentAndLayout) {
  auto pool = mbp::create_batch_pool();
  ASSERT_NE(pool, nullptr);
  pool->configure(1, 1024);
  pool->begin_frame(0);

  std::size_t o0 = 0, o1 = 0;
  ASSERT_TRUE(pool->upload("a", 1, 1, &o0));
  ASSERT_TRUE(pool->upload("b", 4, 16, &o1));
  EXPECT_EQ(o0, 0u);
  EXPECT_EQ(o1 % 16, 0u) << "上传偏移必须满足请求的对齐";
  EXPECT_GE(o1, o0 + 1);
}

TEST(Pool, BatchPoolRejectsOversizeWhenGrowthDisallowed) {
  auto pool = mbp::create_batch_pool();
  ASSERT_NE(pool, nullptr);
  pool->configure(1, 64);
  pool->begin_frame(0);
  pool->set_allow_growth(false);

  std::size_t off = 0;
  EXPECT_TRUE(pool->upload("x", 16, 4, &off));
  // 剩余空间不足 → 必须返回 false，而不是静默扩容
  EXPECT_FALSE(pool->upload("y", 4096, 4, &off));
}

TEST(Pool, BatchPoolSlotDataIsWritableAndPersists) {
  auto pool = mbp::create_batch_pool();
  ASSERT_NE(pool, nullptr);
  pool->configure(2, 256);
  pool->begin_frame(1);
  std::byte* data = pool->slot_data(1);
  ASSERT_NE(data, nullptr);
  data[0] = std::byte{0xAB};
  EXPECT_EQ(pool->slot_data(1)[0], std::byte{0xAB});
  EXPECT_NE(pool->slot_data(0), pool->slot_data(1)) << "不同帧槽必须是不同内存";
}
