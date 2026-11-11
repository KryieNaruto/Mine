// MagicBlocks L3 —— 池与帧统计的实现（spec §8 + §4.1）
//
// 本 TU 只实现三个"非模板"部分：FrameStats / FrameArena / BatchPool。
// IObjectPool<T> 是模板，实现放在 alloc.h 底部（见该头）。
//
// 实测约定：所有扩容都记进 IFrameStats::add_heap_alloc，
// 这样门控 G9 能对**整帧**求和断言 §4.1 的"堆分配 0 次"，
// 而不是逐个模块猜哪个偷偷分配了。
#include "mb_internal/magicblocks/primitives/alloc.h"

#include <algorithm>

namespace magicblocks::primitives {
namespace {

// ---------------------------------------------------------------- FrameStats
class FrameStats final : public IFrameStats {
 public:
  void add_heap_alloc(u64 bytes) override {
    cur_.heap_allocs += 1;
    cur_.heap_bytes += bytes;
  }
  void add_vma_alloc(u64 count) override { cur_.vma_allocs += count; }
  void add_vma_free(u64 count) override { cur_.vma_frees += count; }
  void add_drawcall(u64 count) override { cur_.drawcalls += count; }
  void add_pipeline_switch(u64 count) override { cur_.pipeline_switches += count; }
  void add_vertices_uploaded(u64 count) override { cur_.vertices_uploaded += count; }
  void add_instances_uploaded(u64 count) override { cur_.instances_uploaded += count; }
  void set_arena_bytes_used(u64 bytes) override { cur_.arena_bytes_used = bytes; }
  void set_batch_pool_bytes(u64 bytes) override { cur_.batch_pool_bytes = bytes; }

  const FrameStatsRecord& end_frame() override {
    last_ = cur_;
    cur_ = FrameStatsRecord{};
    ++frame_count_;
    return last_;
  }

  FrameStatsRecord current() const override { return cur_; }
  const FrameStatsRecord& last_frame() const override { return last_; }
  u64 frame_count() const override { return frame_count_; }

  void reset() override {
    cur_ = FrameStatsRecord{};
    last_ = FrameStatsRecord{};
    frame_count_ = 0;
  }

 private:
  FrameStatsRecord cur_{};
  FrameStatsRecord last_{};
  u64 frame_count_ = 0;
};

// ---------------------------------------------------------------- FrameArena
//
// 分块线性分配器。每个 block 是一段独立 malloc 的连续内存；
// blocks_ 只增不减（reset 不回退 blocks_ 本身，只回退游标），
// 所以块地址在整个 arena 生命周期内稳定。
constexpr std::size_t kDefaultArenaBlock = 256u * 1024u;

class FrameArena final : public IFrameArena {
 public:
  explicit FrameArena(std::size_t block_bytes)
      : block_bytes_(block_bytes == 0 ? kDefaultArenaBlock : block_bytes) {}

  ~FrameArena() override {
    for (Block& b : blocks_) ::operator delete(b.mem, std::align_val_t{kBlockAlign});
  }

  void* alloc(std::size_t bytes, std::size_t align) override {
    if (bytes == 0) return nullptr;
    // align 必须是 2 的幂；调用方契约，这里兜底防御。
    if (align == 0 || (align & (align - 1)) != 0) return nullptr;
    if (align > kBlockAlign) align = kBlockAlign;

    // 先试当前块。current_ 指向"正在填的块"，游标 block_used_。
    while (current_ < blocks_.size()) {
      Block& b = blocks_[current_];
      const std::size_t off = align_up(block_used_, align);
      if (off + bytes <= b.size) {
        block_used_ = off + bytes;
        bytes_used_ += bytes;
        return static_cast<std::byte*>(b.mem) + off;
      }
      // 当前块放不下 → 跳到下一块（不回退，块内残留是允许的浪费）。
      ++current_;
      block_used_ = 0;
    }

    // 所有已分配的块用完了，需要新块。
    //
    // 注意：这里**不能**让 add_block 失败后继续往下走，也不能让热路径无限扩容。
    // 之前的实现踩过一个坑：reset() 后 current_ 归零，若 allow_growth 仍为 true，
    // 那么"吃满所有块 → 追加新块 → 再吃满 → 再追加"会无限循环，
    // 把内存吃光（表现为测试挂死）。所以这里只允许**每次 alloc 至多追加一个块**，
    // 新块必然装得下本次请求（size = max(block_bytes_, bytes + align)），
    // 一定能在本轮返回，不会循环。
    if (!allow_growth_) return nullptr;
    if (!add_block(std::max(block_bytes_, align_up(bytes, align)))) return nullptr;
    // add_block 已把 current_ 指向新块、block_used_ 归零，因此偏移就是 0。
    // 这里不写 off 变量，避免"新块里再对齐一次"造成语义含糊。
    Block& b = blocks_[current_];
    block_used_ = bytes;
    bytes_used_ += bytes;
    return static_cast<std::byte*>(b.mem);
  }

  void reset() override {
    current_ = 0;
    block_used_ = 0;
    bytes_used_ = 0;
    // 不 free、不 shrink：这正是"帧末 O(1)"的来源。
  }

  void warmup(std::size_t total_bytes) override {
    // warmup 期间允许扩容（这是预分配期，不是热路径）。
    const bool saved = allow_growth_;
    allow_growth_ = true;
    // add_block 会把 current_ 推到新块。warmup 只是**预留容量**，不该改变
    // "接下来从哪块开始分配"，所以这里保存并在结束后恢复 current_/block_used_。
    // （不恢复的话：warmup(4096) 会让前 3 块被永久跳过，等于白白浪费容量。）
    const std::size_t saved_current = current_;
    const std::size_t saved_used = block_used_;
    while (bytes_capacity() < total_bytes) {
      if (!add_block(block_bytes_)) break;
    }
    current_ = saved_current;
    block_used_ = saved_used;
    allow_growth_ = saved;
  }

  void set_allow_growth(bool allow) override { allow_growth_ = allow; }
  bool allow_growth() const override { return allow_growth_; }

  std::size_t bytes_used() const override { return bytes_used_; }

  std::size_t bytes_capacity() const override {
    std::size_t sum = 0;
    for (const Block& b : blocks_) sum += b.size;
    return sum;
  }

  u64 heap_alloc_count() const override { return heap_alloc_count_; }
  u64 heap_alloc_bytes() const override { return heap_alloc_bytes_; }

  void set_stats(IFrameStats* stats) override { stats_ = stats; }

 private:
  static constexpr std::size_t kBlockAlign = 64;  // >= alignof(max_align_t)

  struct Block {
    void* mem = nullptr;
    std::size_t size = 0;
  };

  bool add_block(std::size_t bytes) {
    void* mem = ::operator new(bytes, std::align_val_t{kBlockAlign}, std::nothrow);
    if (mem == nullptr) return false;
    blocks_.push_back(Block{mem, bytes});
    ++heap_alloc_count_;
    heap_alloc_bytes_ += bytes;
    if (stats_ != nullptr) stats_->add_heap_alloc(bytes);
    // 新块成为当前块
    current_ = blocks_.size() - 1;
    block_used_ = 0;
    return true;
  }

  std::size_t block_bytes_;
  std::vector<Block> blocks_;
  std::size_t current_ = 0;     // 正在填的块下标
  std::size_t block_used_ = 0;  // 当前块内已用字节
  std::size_t bytes_used_ = 0;  // 本次 reset 以来分配的字节总和
  bool allow_growth_ = true;
  u64 heap_alloc_count_ = 0;
  u64 heap_alloc_bytes_ = 0;
  IFrameStats* stats_ = nullptr;
};

// ---------------------------------------------------------------- BatchPool
//
// 帧槽环形实例缓冲。每槽一块独立内存，第一次 begin_frame 到该槽时按需分配；
// 之后复用。稳态帧只有 memcpy，没有分配。
constexpr std::size_t kDefaultBatchSlotBytes = 1u * 1024u * 1024u;  // 1 MiB

class BatchPool final : public IBatchPool {
 public:
  ~BatchPool() override { release_slots(); }

  void configure(u32 frame_slots, std::size_t slot_bytes) override {
    const u32 want = frame_slots == 0 ? 1u : frame_slots;
    const std::size_t bytes = slot_bytes == 0 ? kDefaultBatchSlotBytes : slot_bytes;

    // 配置是全量重建（只在启动/重配置期调用，不在热路径上）。
    //
    // **必须在这里一次性把所有帧槽分配好**：
    // 早先的实现是"槽位懒分配"（第一次用到某槽才 new），结果是前
    // in_flight_frames 帧每帧都新增一次 growth_count —— 看起来"稳态"，
    // 实际每帧都在分配，§4.1 的"堆分配 0 次"根本不成立。
    // 现在 configure 之后 slots_ 全就绪，begin_frame/upload 纯 memcpy。
    release_slots();
    frame_slots_ = want;
    slot_bytes_ = bytes;
    slots_.resize(frame_slots_);
    for (Slot& s : slots_) {
      if (!allocate_slot(s)) break;   // 失败则留空，upload 时再报错
    }
    used_ = 0;
    current_slot_ = 0;
  }

  void begin_frame(u32 slot) override {
    if (frame_slots_ == 0) return;
    current_slot_ = slot % frame_slots_;
    used_ = 0;
  }

  bool upload(const void* data, std::size_t bytes, std::size_t align,
              std::size_t* out_offset) override {
    if (bytes == 0) {
      if (out_offset != nullptr) *out_offset = used_;
      return true;
    }
    if (frame_slots_ == 0) return false;
    if (align == 0 || (align & (align - 1)) != 0) return false;
    if (align > kSlotAlign) align = kSlotAlign;

    Slot& s = slots_[current_slot_];
    // 首次使用才分配（此后稳态复用）。
    if (s.mem == nullptr) {
      if (!allocate_slot(s)) return false;
    }
    std::size_t off = align_up(used_, align);
    if (off + bytes > s.size) {
      if (!allow_growth_) return false;
      // 扩容：新块必须能装下 off+bytes。
      const std::size_t need = std::max(slot_bytes_, off + bytes + kSlotAlign);
      void* mem = ::operator new(need, std::align_val_t{kSlotAlign}, std::nothrow);
      if (mem == nullptr) return false;
      ::operator delete(s.mem, std::align_val_t{kSlotAlign});
      s.mem = mem;
      s.size = need;
      slot_bytes_ = need;
      ++growth_count_;
      if (stats_ != nullptr) stats_->add_heap_alloc(need);
      off = align_up(used_, align);
      if (off + bytes > s.size) return false;
    }
    if (data != nullptr) std::memcpy(static_cast<std::byte*>(s.mem) + off, data, bytes);
    used_ = off + bytes;
    if (out_offset != nullptr) *out_offset = off;
    return true;
  }

  std::byte* slot_data(u32 slot) override {
    if (frame_slots_ == 0) return nullptr;
    const u32 i = slot % frame_slots_;
    Slot& s = slots_[i];
    // configure() 之后所有槽位都应已就绪；这里只兜底（配置期分配失败的情况），
    // 正常路径不会走到 allocate_slot。
    if (s.mem == nullptr) {
      if (!allocate_slot(s)) return nullptr;
    }
    return static_cast<std::byte*>(s.mem);
  }

  std::size_t slot_used() const override { return used_; }
  std::size_t slot_capacity() const override {
    if (frame_slots_ == 0) return 0;
    return slots_[current_slot_].size;
  }

  u32 frame_slots() const override { return frame_slots_; }
  void set_allow_growth(bool allow) override { allow_growth_ = allow; }
  u64 growth_count() const override { return growth_count_; }
  void set_stats(IFrameStats* stats) override { stats_ = stats; }

 private:
  static constexpr std::size_t kSlotAlign = 64;

  struct Slot {
    void* mem = nullptr;
    std::size_t size = 0;
  };

  void release_slots() {
    for (Slot& s : slots_) {
      if (s.mem != nullptr) {
        ::operator delete(s.mem, std::align_val_t{kSlotAlign});
        s.mem = nullptr;
        s.size = 0;
      }
    }
    slots_.clear();
  }

  bool allocate_slot(Slot& s) {
    void* mem = ::operator new(slot_bytes_, std::align_val_t{kSlotAlign}, std::nothrow);
    if (mem == nullptr) return false;
    s.mem = mem;
    s.size = slot_bytes_;
    ++growth_count_;
    if (stats_ != nullptr) stats_->add_heap_alloc(slot_bytes_);
    return true;
  }

  std::vector<Slot> slots_;
  u32 frame_slots_ = 0;
  u32 current_slot_ = 0;
  std::size_t slot_bytes_ = kDefaultBatchSlotBytes;
  std::size_t used_ = 0;
  bool allow_growth_ = true;
  u64 growth_count_ = 0;
  IFrameStats* stats_ = nullptr;
};

}  // namespace

// ---------------------------------------------------------------- 工厂

std::unique_ptr<IFrameStats> create_frame_stats() {
  return std::make_unique<FrameStats>();
}

std::unique_ptr<IFrameArena> create_frame_arena(std::size_t block_bytes) {
  return std::make_unique<FrameArena>(block_bytes);
}

std::unique_ptr<IBatchPool> create_batch_pool() {
  return std::make_unique<BatchPool>();
}

}  // namespace magicblocks::primitives
