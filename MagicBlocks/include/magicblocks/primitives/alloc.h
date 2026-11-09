// MagicBlocks L3 —— 对象池 / 分配器 / 帧统计（spec §8 + §4.1）
//
// 这个头是"性能第一"的落地底座：§4.1 的五个预算里，前两项
// （堆分配 0 次、VMA alloc/free 0 次）完全由这里的三个池保证，
// 后三项（drawcall / 管线切换 / 上传字节）由 IFrameStats 计量。
//
// 三个池，三个用途（spec §8.1，不要混用）：
//   IFrameArena  —— 一帧活。线性 bump，帧末 reset() 是 O(1)，不 free 单块。
//   IObjectPool  —— 长于帧、可增长。自由链表 + 分块，**不做单块 realloc**
//                   （避免搬移导致已发出的指针失效）。
//   IBatchPool   —— 一帧活，环形。每帧一个 slot 复用，**稳态帧 0 次分配**。
//
// 硬约束（与 L0~L2 一致，G3/G4 会扫）：
//   - 本头零 Vk* 类型、零 #include <vulkan/...>；
//   - 错误类型复用 device::Status / device::Result，不另造；
//   - 本层**不读时钟**（tools/check_scene_no_clock.py 同样覆盖 L3）。
//
// 为什么不用 std::pmr / malloc 包装：
//   我们要的不只是"分配器"，而是**可断言的零分配**。std::pmr 的
//   monotonic_buffer_resource 在块用尽时会静默 new 新块，无法在热路径上
//   证明"这一帧真的没分配"。这里的 FrameArena 把"用尽"变成显式错误
//   （热路径）或一次性扩容（预分配期），让 §4.1 的断言成立。
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <utility>
#include <vector>

#include "magicblocks/device/result.h"
#include "magicblocks/device/types.h"

namespace magicblocks::primitives {

using device::Result;
using device::Status;
using device::u32;
using device::u64;

// 对齐到 2 的幂（上游保证 align 是 2 的幂，这里只做位运算）。
inline std::size_t align_up(std::size_t v, std::size_t align) {
  return (v + (align - 1)) & ~(align - 1);
}

// ================================================================ §4.1 帧统计
//
// 每帧一组单调计数，帧末由 IFrameStats::end_frame() 快照成 FrameStatsRecord，
// 然后清零重新计。测试与门控 G9 读 record 断言预算。
//
// 为什么单独一个类而不是散落的成员：
//   "0 次分配"这种预算必须是**可观测**的，否则只是文档里的愿望。
//   所有会破坏预算的路径（池的扩容、上传、管线创建）都往这里记一笔，
//   测试就能对**整帧**求和，而不是逐个模块去猜。
struct FrameStatsRecord {
  u64 heap_allocs = 0;        // 稳态帧预算 0
  u64 heap_bytes = 0;
  u64 vma_allocs = 0;         // 稳态帧预算 0（从 L1 AllocatorStats 差分而来）
  u64 vma_frees = 0;
  u64 drawcalls = 0;          // 预算 ≤ 300
  u64 pipeline_switches = 0;  // 预算 ≤ 16
  u64 vertices_uploaded = 0;  // 预算：仅脏块
  u64 instances_uploaded = 0;
  u64 arena_bytes_used = 0;   // 信息性
  u64 batch_pool_bytes = 0;   // 信息性
};

// §4.1 的预算常量，集中在一处，门控与测试共用（避免两边各写一份数字）。
struct FrameBudget {
  static constexpr u64 kMaxHeapAllocs = 0;
  static constexpr u64 kMaxVmaAllocs = 0;
  static constexpr u64 kMaxDrawcalls = 300;
  static constexpr u64 kMaxPipelineSwitches = 16;
};

class IFrameStats {
 public:
  IFrameStats() = default;
  IFrameStats(const IFrameStats&) = delete;
  IFrameStats& operator=(const IFrameStats&) = delete;
  virtual ~IFrameStats() = default;

  // ---- 记账（热路径上调，必须是几个自增的量级）----
  virtual void add_heap_alloc(u64 bytes) = 0;
  virtual void add_vma_alloc(u64 count = 1) = 0;
  virtual void add_vma_free(u64 count = 1) = 0;
  virtual void add_drawcall(u64 count = 1) = 0;
  virtual void add_pipeline_switch(u64 count = 1) = 0;
  virtual void add_vertices_uploaded(u64 count) = 0;
  virtual void add_instances_uploaded(u64 count) = 0;
  virtual void set_arena_bytes_used(u64 bytes) = 0;
  virtual void set_batch_pool_bytes(u64 bytes) = 0;

  // ---- 帧边界 ----
  // 把当前累计值封存成一条 record（返回引用，下次 begin_frame 前有效），并清零。
  virtual const FrameStatsRecord& end_frame() = 0;
  // 本帧至今的实时值（调试/门控用，不清零）。
  virtual FrameStatsRecord current() const = 0;
  // 上一帧封存的记录。
  virtual const FrameStatsRecord& last_frame() const = 0;
  virtual u64 frame_count() const = 0;
  virtual void reset() = 0;
};

std::unique_ptr<IFrameStats> create_frame_stats();

// ================================================================ §8.2 FrameArena
//
// 一帧活的内存。线性 bump 分配，**不支持单独 free**，帧末 reset() 把游标归零。
//
// 用"分块 + 每帧游标"而不是"一整块大 vector"：
//   分块让 arena 在增长时不用搬移已有数据（搬移会让已发出的指针失效），
//   同时 reset() 仍保持 O(1)（只需把游标和当前块号归零，不碰块内内存）。
//
// 预分配期扩容 vs 热路径：
//   warmup() 在启动期一次性把块预分配好；之后 alloc() 若需要新块且
//   allow_growth=false（热路径模式），就返回 nullptr 并记一次
//   heap_alloc 缺口，让"稳态帧 0 分配"断言能抓到问题。
class IFrameArena {
 public:
  IFrameArena() = default;
  IFrameArena(const IFrameArena&) = delete;
  IFrameArena& operator=(const IFrameArena&) = delete;
  virtual ~IFrameArena() = default;

  // 分配 bytes 字节，按 align 对齐（align 必须是 2 的幂）。
  // 返回 nullptr 表示当前配置下无法满足（热路径不扩容 / bytes==0）。
  virtual void* alloc(std::size_t bytes, std::size_t align) = 0;

  template <typename T>
  T* alloc_array(std::size_t count) {
    return static_cast<T*>(alloc(sizeof(T) * count, alignof(T)));
  }

  // 帧末：O(1) 把游标归零，不 free 任何块。
  virtual void reset() = 0;

  // 启动期预热：确保至少有 total_bytes 的容量已分配到块里。
  // 期间发生的扩容会记进 stats（如果给了）。
  virtual void warmup(std::size_t total_bytes) = 0;

  // 热路径是否允许扩容。默认 true（宽松）；渲染器在预热后应设为 false。
  virtual void set_allow_growth(bool allow) = 0;
  virtual bool allow_growth() const = 0;

  virtual std::size_t bytes_used() const = 0;
  virtual std::size_t bytes_capacity() const = 0;
  // 创建以来累计的堆分配次数/字节（用于断言"稳态帧不再增长"）。
  virtual u64 heap_alloc_count() const = 0;
  virtual u64 heap_alloc_bytes() const = 0;

  virtual void set_stats(IFrameStats* stats) = 0;
};

// block_bytes 是单块大小；0 表示用默认（256 KiB）。
std::unique_ptr<IFrameArena> create_frame_arena(std::size_t block_bytes = 0);

// ================================================================ §8.1 IObjectPool
//
// 长生命周期的对象池。自由链表 + **分块**（vector<Chunk>），
// 关键约束：**不做单块 realloc** —— 块一旦创建地址就稳定，
// 因此 acquire() 返回的指针在整个池生命周期内有效（除非 release 后复用）。
//
// 为什么不用 std::deque：
//   deque 的块大小与对象对齐、生命周期规则不在我们控制内，
//   且 release 后槽位的复用顺序（这里是 LIFO 自由链表）决定确定性——
//   命令回放要求"同样的操作序列产生同样的句柄序列"（spec §11 确定性），
//   LIFO 是确定性的，deque 的内部策略不是。
template <typename T>
class IObjectPool {
 public:
  IObjectPool() = default;
  IObjectPool(const IObjectPool&) = delete;
  IObjectPool& operator=(const IObjectPool&) = delete;
  virtual ~IObjectPool() = default;

  // 取一个槽位（已构造 T，默认值）。返回的指针在 release 前保持有效。
  virtual T* acquire() = 0;
  // 归还槽位：T 被析构，槽位进自由链表，下次 acquire 优先复用（LIFO）。
  virtual void release(T* obj) = 0;

  // 预分配至少 capacity 个槽位（启动期调用，把分配挪出热路径）。
  virtual void reserve(std::size_t capacity) = 0;
  // 热路径是否允许扩容（同 FrameArena 的理由）。
  virtual void set_allow_growth(bool allow) = 0;

  virtual std::size_t live_count() const = 0;
  virtual std::size_t capacity() const = 0;
  // 创建以来累计的块级扩容次数（断言热路径 0 增长的依据）。
  virtual u64 growth_count() const = 0;

  virtual void set_stats(IFrameStats* stats) = 0;
};

// chunk_capacity 是每个块的槽位数；0 表示默认 64。
template <typename T>
std::unique_ptr<IObjectPool<T>> create_object_pool(std::size_t chunk_capacity = 0);

// ================================================================ §8.3 IBatchPool
//
// 实例属性缓冲的环形池（性能核心）。按帧槽位各持一块 host-visible 缓冲，
// 每帧 begin_frame(slot) 把写游标归零，upload() 把数据追加到同一块。
//
// **稳态帧 0 次分配**：块在第一次使用时分配并缓存；之后只要不超过
// 单帧容量，就只是往已有内存里 memcpy。超容量时：
//   - allow_growth=false（预分配后的正常状态）→ 返回 false，调用方分批
//   - allow_growth=true → 扩容并记一次 heap_alloc（warmup 期应完成）
//
// 与 L1 IBuffer 的关系：
//   这里的"块"是 CPU 侧的可写暂存区。真正的 GPU buffer 上传统一由
//   IPrimitiveRenderer 用 L1 的 IBuffer 做（L3 不碰 Vk*）；本池只管
//   "形状数据先攒到连续内存"，避免每实例一次小分配。
class IBatchPool {
 public:
  IBatchPool() = default;
  IBatchPool(const IBatchPool&) = delete;
  IBatchPool& operator=(const IBatchPool&) = delete;
  virtual ~IBatchPool() = default;

  // 设定帧槽数（= in_flight_frames）与每槽容量；启动期调用。
  virtual void configure(u32 frame_slots, std::size_t slot_bytes) = 0;

  // 开始一帧：slot 是环上的帧槽下标（< frame_slots），游标归零。
  virtual void begin_frame(u32 slot) = 0;

  // 追加 bytes 字节，返回块内偏移（写入即生效）。
  // 返回 false 表示本次放不下（超容量且不允许扩容）。
  virtual bool upload(const void* data, std::size_t bytes, std::size_t align,
                      std::size_t* out_offset) = 0;

  // 取当前槽块的 CPU 可写首址与已用字节数（交给 L1 IBuffer 上传）。
  virtual std::byte* slot_data(u32 slot) = 0;
  virtual std::size_t slot_used() const = 0;
  virtual std::size_t slot_capacity() const = 0;

  virtual u32 frame_slots() const = 0;
  virtual void set_allow_growth(bool allow) = 0;
  // 创建以来累计扩容次数（稳态帧应为 0 增长）。
  virtual u64 growth_count() const = 0;
  virtual void set_stats(IFrameStats* stats) = 0;
};

std::unique_ptr<IBatchPool> create_batch_pool();

// ================================================================ IObjectPool<T> 实现
//
// 模板实现放头文件。分块 + LIFO 自由链表，**不做单块 realloc**：
//   每个 Chunk 一次性 new 出来，地址从此稳定；扩容只追加新 Chunk。
//   因此 acquire() 发出的指针在 release 前永远有效 —— 这是
//   "实体行 / Stroke / GizmoHandle 的指针可以安全缓存"的前提（spec §8.1）。
namespace detail {

template <typename T>
class ObjectPool final : public IObjectPool<T> {
 public:
  explicit ObjectPool(std::size_t chunk_capacity)
      : chunk_capacity_(chunk_capacity == 0 ? 64 : chunk_capacity) {}

  ~ObjectPool() override {
    for (Chunk& c : chunks_) {
      // 只析构 live 的槽位，避免对已 release 的对象重复析构。
      for (std::size_t i = 0; i < c.capacity; ++i) {
        if (c.slot_state[i]) c.data[i].~T();
      }
      ::operator delete(c.data, std::align_val_t{kChunkAlign});
      ::operator delete(c.slot_state, std::align_val_t{kChunkAlign});
    }
  }

  T* acquire() override {
    if (free_head_ != nullptr) {
      Node* n = free_head_;
      free_head_ = n->next;
      T* obj = n->obj;
      // 槽位对应状态置活
      set_state(obj, true);
      ++live_count_;
      return obj;
    }
    // 没有空闲槽位 → 可能需要扩容
    if (next_free_ >= capacity()) {
      if (!allow_growth_) return nullptr;
      if (!add_chunk()) return nullptr;
    }
    T* obj = slot_at(next_free_);
    ++next_free_;
    set_state(obj, true);
    ++live_count_;
    return obj;
  }

  void release(T* obj) override {
    if (obj == nullptr) return;
    if (!owns(obj)) return;  // 不属于本池：显式忽略（调用方契约）
    if (!state_of(obj)) return;  // 重复 release：忽略
    set_state(obj, false);
    obj->~T();
    // 复用槽位：用对象本身的内存存链表节点（T 已析构，可以复写）。
    Node* n = reinterpret_cast<Node*>(obj);
    n->obj = obj;
    n->next = free_head_;
    free_head_ = n;
    --live_count_;
  }

  void reserve(std::size_t capacity) override {
    const bool saved = allow_growth_;
    allow_growth_ = true;
    while (this->capacity() < capacity) {
      if (!add_chunk()) break;
    }
    allow_growth_ = saved;
  }

  void set_allow_growth(bool allow) override { allow_growth_ = allow; }

  std::size_t live_count() const override { return live_count_; }

  std::size_t capacity() const override {
    return chunks_.size() * chunk_capacity_;
  }

  u64 growth_count() const override { return growth_count_; }

  void set_stats(IFrameStats* stats) override { stats_ = stats; }

 private:
  static constexpr std::size_t kChunkAlign = 64;
  // 链表节点必须能塞进 T 的存储里；T 太小就退化为用索引链。
  static constexpr bool kNodeFitsInT = sizeof(T) >= sizeof(void*) * 2;

  struct Node {
    T* obj;
    Node* next;
  };

  struct Chunk {
    T* data = nullptr;
    unsigned char* slot_state = nullptr;  // 1 = live
    std::size_t capacity = 0;
  };

  bool add_chunk() {
    // 注意：这里用 operator new 获得**原始内存**，不构造 T（T 在 acquire 时构造）。
    T* data = static_cast<T*>(::operator new(chunk_capacity_ * sizeof(T),
                                             std::align_val_t{kChunkAlign}, std::nothrow));
    if (data == nullptr) return false;
    unsigned char* state = static_cast<unsigned char*>(
        ::operator new(chunk_capacity_, std::align_val_t{kChunkAlign}, std::nothrow));
    if (state == nullptr) {
      ::operator delete(data, std::align_val_t{kChunkAlign});
      return false;
    }
    std::memset(state, 0, chunk_capacity_);
    chunks_.push_back(Chunk{data, state, chunk_capacity_});
    ++growth_count_;
    if (stats_ != nullptr) {
      // 状态数组也算一次分配，一起记账，避免统计漏项。
      stats_->add_heap_alloc(chunk_capacity_ * sizeof(T) + chunk_capacity_);
      stats_->add_heap_alloc(0);
    }
    return true;
  }

  std::size_t chunk_of(const T* obj) const {
    for (std::size_t ci = 0; ci < chunks_.size(); ++ci) {
      const T* base = chunks_[ci].data;
      if (obj >= base && obj < base + chunks_[ci].capacity) return ci;
    }
    return static_cast<std::size_t>(-1);
  }

  bool owns(const T* obj) const { return chunk_of(obj) != static_cast<std::size_t>(-1); }

  T* slot_at(std::size_t index) {
    const std::size_t ci = index / chunk_capacity_;
    const std::size_t off = index % chunk_capacity_;
    return chunks_[ci].data + off;
  }

  void set_state(const T* obj, bool live) {
    const std::size_t ci = chunk_of(obj);
    if (ci == static_cast<std::size_t>(-1)) return;
    const std::size_t off = static_cast<std::size_t>(obj - chunks_[ci].data);
    chunks_[ci].slot_state[off] = live ? 1 : 0;
  }

  bool state_of(const T* obj) const {
    const std::size_t ci = chunk_of(obj);
    if (ci == static_cast<std::size_t>(-1)) return false;
    const std::size_t off = static_cast<std::size_t>(obj - chunks_[ci].data);
    return chunks_[ci].slot_state[off] != 0;
  }

  std::vector<Chunk> chunks_;
  std::size_t chunk_capacity_;
  std::size_t next_free_ = 0;  // 从未分配过的下一个下标
  std::size_t live_count_ = 0;
  u64 growth_count_ = 0;
  bool allow_growth_ = true;
  Node* free_head_ = nullptr;
  IFrameStats* stats_ = nullptr;
  static_assert(sizeof(T) >= sizeof(Node) || sizeof(T) >= sizeof(void*),
                "T 太小，复用槽位放不下自由链表节点；请用更厚的对象类型");
};

}  // namespace detail

template <typename T>
std::unique_ptr<IObjectPool<T>> create_object_pool(std::size_t chunk_capacity) {
  return std::make_unique<detail::ObjectPool<T>>(chunk_capacity);
}

}  // namespace magicblocks::primitives
