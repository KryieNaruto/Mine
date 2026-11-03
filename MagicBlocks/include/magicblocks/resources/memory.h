// MagicBlocks SDK —— L1 内存分配器(M1–M6)
//
// 设计约束(spec §6 / plans-l1 §4):
//   - **M1 抽象接口**:SDK **不暴露** vkAllocateMemory / VkDeviceMemory 之类的裸分配。
//     上层只通过 IMemoryAllocator 要一块 {page_id, offset, size} 的子块,
//     真实页(VMA 大块)只存在于 backend/vulkan 内部。
//   - **M2 分页 + 偏移子分配**:设备内存 64 MB 大页、Host 可见 8 MB 页;
//     页内按 MemoryKind 分桶,桶内空闲链表切出偏移给多个小资源共用一页。
//     因此 allocate() 永远不会为单个资源单独触发一次 VMA 分配。
//   - **M3 句柄 = 偏移**:返回的不是指针,而是可直接序列化进命令流的 ResourceId。
//   - **M4 稳定驻留**:free() 只把子块还回本页(O(1) 相邻合并),不立即退 VMA;
//     页空置超过 page_idle_frames() 帧且无在途帧引用才整页退回 ——
//     这正是**热路径 0 次 VMA 调用**的来源。
//   - **M5 计数门槛**:稳态帧 VMA alloc/free 次数必须为 0,用 stats() 的
//     vma_alloc_count / vma_free_count 增量断言,而不是"看起来没分配"。
//   - **M6 预算与淘汰**:超出 DeviceCaps.memory_budget → OutOfMemory,
//     message 带「需要 / 已有 / 预算」三个数,绝不静默降级(spec D8)。
//     require_budget_check() 明确列出"哪些桶的预算是硬的",避免"预算查询失败就
//     整段豁免"的静默降级;淘汰只做整页 LRU,不做页内碎片整理。
//
// 硬约束:本头零 Vk* 类型、零 #include <vulkan/...>(G3);错误类型复用
// device::Status / device::Result,不另造。
#pragma once

#include <cstdint>

#include "magicblocks/device/result.h"
#include "magicblocks/device/types.h"
#include "magicblocks/resources/handles.h"

namespace magicblocks::resources {

using device::Result;
using device::Status;
using device::u32;
using device::u64;

// ---------------------------------------------------------------- 页描述(M2)

// 一页 = 一次 VMA 大块分配。只读视图,由分配器内部维护,调用方不可改。
struct PageDesc {
  u32 page_id = ResourceId::kInvalidPage;
  PageKind kind = PageKind::Device;
  u64 page_size = 0;         // 整页字节数(设备 64 MB / Host 8 MB)
  u64 bytes_in_use = 0;      // 当前活子块字节总和(按子块 size 累计,不含对齐填充)
  u64 bytes_reserved = 0;    // 当前已切出区间总和(含对齐填充,用于算空闲率)
  u32 live_allocations = 0;  // 本页尚存的活子块数
  u32 free_block_count = 0;  // 本页空闲链表节点数(合并后减少)
  u64 idle_frames = 0;       // 连续空置帧数(== 0 表示有人用)
  bool in_flight = false;    // 是否有在途帧引用(有则**不得**退回 VMA)
};

// ---------------------------------------------------------------- 统计(M5 / M6)

// 计数全部是**单调累计**值(创建以来的总数),测试取两次差分判增量 == 0。
struct AllocatorStats {
  u64 vma_alloc_count = 0;  // 向 VMA 申请大页的次数(M5 硬断言对象)
  u64 vma_free_count = 0;   // 退回 VMA 的次数(M5 硬断言对象)
  u64 page_count = 0;       // 当前活页数
  u64 bytes_in_use = 0;     // 当前活子块字节总和(全桶合计)
  u64 bytes_reserved = 0;   // 当前已切出区间总和(全桶合计,含对齐填充)
  u64 memory_budget = 0;    // 来自 DeviceCaps.memory_budget,0 表示未提供
  u64 allocation_count = 0;  // 累计 allocate() 成功次数
  u64 free_count = 0;        // 累计 free() 成功次数
  u64 merge_count = 0;       // 累计相邻空闲块合并次数(M4 的 O(1) 合并证据)
  u64 evict_count = 0;       // 累计整页 LRU 退回次数(M6)
  // 按桶的明细:下标即 static_cast<u32>(MemoryKind),长度 kMemoryKindCount。
  static constexpr u32 kMemoryKindCount = 5;
  u64 bucket_bytes_in_use[kMemoryKindCount] = {};
  u64 bucket_alloc_count[kMemoryKindCount] = {};
};

// ---------------------------------------------------------------- 分配结果(M3)

// 一次分配的结果。mapped != nullptr 时是 CPU 可写的映射首地址
// (Host 页 MAPPED 常驻,设备页在 CPU 侧不可见 → nullptr,这不算失败)。
struct AllocResult {
  ResourceId id{};
  void* mapped = nullptr;

  bool valid() const { return id.valid(); }
};

// ---------------------------------------------------------------- 接口(M1)

class IMemoryAllocator {
 public:
  IMemoryAllocator() = default;
  IMemoryAllocator(const IMemoryAllocator&) = delete;
  IMemoryAllocator& operator=(const IMemoryAllocator&) = delete;
  virtual ~IMemoryAllocator() = default;

  // ---- 分配(M2):从桶内切偏移,永不单独触发 VMA。

  // size == 0 返回 InvalidArgument。
  // align 必须是 2 的幂且 >= 1;不满足返回 InvalidArgument 而非"尽力对齐"。
  // 同桶连续分配在页还有空间时**落在同一页**(同 page_id,offset 互不重叠)。
  // 超预算 → OutOfMemory(M6),message 里带 需要/已有/预算 三个数。
  virtual Result<AllocResult> allocate(MemoryKind kind, u64 size, u64 align) = 0;

  // 归还子块:与左右相邻空闲块 O(1) 合并,页不被立即退回 VMA(M4)。
  // 句柄非法/已释放/不属于本分配器 → InvalidArgument(显式报错,不静默忽略)。
  virtual Status free(const ResourceId& id) = 0;

  // 取子块的可映射指针;不可映射(设备页)返回 nullptr。offset 为子块内偏移。
  virtual void* mapped_ptr(const ResourceId& id, u64 offset = 0) const = 0;

  // 整页映射首址(Host 页常驻映射);设备页返回 nullptr。
  virtual void* map_page(u32 page_id) = 0;
  virtual Status unmap_page(u32 page_id) = 0;

  // ---- 页(M2 / M4 / M6)

  // 预取整页:初始化与切帧时批量预留,把 VMA 调用挪到热路径之外。
  virtual Status reserve_pages(MemoryKind kind, u32 count) = 0;
  virtual u32 page_count(MemoryKind kind) const = 0;
  // 页 64 MB / 8 MB 由 PageKind 决定;此接口按桶返回其所在页的尺寸。
  virtual u64 page_size(MemoryKind kind) const = 0;
  // page_id 不存在时 out 保持不变并返回 false。
  virtual bool page_desc(u32 page_id, PageDesc* out) const = 0;

  // ---- 帧边界(M4)

  // 推进分配器帧号:页空置计数 +1、触发整页 LRU 淘汰;backend 同时把帧号
  // 推给 VMA,让它的延迟释放与引擎帧边界对齐(spec §6.2 第 5 条)。
  virtual void begin_frame(u64 frame_index) = 0;
  // 页是否被在途帧引用(在途时**不得**退回 VMA)。
  virtual void set_page_in_flight(u32 page_id, bool in_flight) = 0;
  // 整页空置多少帧后才退回 VMA,默认 120(spec §6.2 第 4 条)。
  virtual u32 page_idle_frames() const = 0;
  // 帧号变化时按需清理(无在途引用且空置超阈值的页)。
  virtual Status trim() = 0;

  // ---- 预算与自检(M5 / M6)

  virtual AllocatorStats stats() const = 0;
  // 清零累计计数(差分断言的起点),不改变当前占用。
  virtual void reset_counters() = 0;
  virtual u64 memory_budget() const = 0;
  virtual void set_memory_budget(u64 bytes) = 0;
  // 自检:分配器内部一致性(页区间不重叠、空闲链表与活子块不重叠、
  // 统计与实测一致)。失败返回 InvalidArgument + 具体位置。
  virtual Status validate() const = 0;
  // 哪些桶受预算硬约束(over-budget 即 OutOfMemory,不静默降级)。
  // 返回位图:第 N 位 = static_cast<u32>(MemoryKind::N);默认全 1。
  virtual u32 require_budget_check() const = 0;
};

}  // namespace magicblocks::resources
