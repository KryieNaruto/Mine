// MagicBlocks —— Vulkan 后端:L1 分页内存分配器实现(VMA v3.5.0 + 桶内空闲链表)
//
// ============================================================================
// 【VMA 唯一注入点】全仓只有本 TU 可以 include <vk_mem_alloc.h>,也只有本 TU 可以
// #define VMA_IMPLEMENTATION。门控 tools/check_no_raw_alloc.py 对这两条做结构性断言
// (第 1 条 + 第 2 条)。因此 VmaAllocator / VmaAllocation / VmaAllocationInfo /
// VkMemoryRequirements 只出现在本文件里;vulkan_allocator.h 对外只见
// std::unique_ptr<Impl>(pimpl)。
// ============================================================================
#include "backend/vulkan/vulkan_allocator.h"

#include <cstring>

// CMake 已按变体给出 VMA 的 -I(third_party/_install/vma-v3.5.0/<variant>/include)。
// 下面这段是**兜底**:只在 <vk_mem_alloc.h> 真的找不到时才派生一条指向池的相对路径,
// 绝不覆盖正常的 include 搜索。理由:本 TU 是全仓唯一带 VMA_IMPLEMENTATION 的文件,
// 若有人把它单独喂给编译器做语法检查(或新工程忘了加 -I),我们希望报出真实错误,
// 而不是被 include 失败挡住。
// VK_IMPLICIT_DEDICATED_ALLOCATION_HEADER 同时给兜底路径配套设置:让 VMA 能探测到
// VK_KHR_dedicated_allocation 扩展头,于是 spec §6.2-7「不建 dedicated allocation」
// 可以由我们显式 VMA_ALLOCATOR_CREATE_KHR_DEDICATED_ALLOCATION_BIT 保证,
// 而不是靠"VMA 没探测到扩展所以恰好不会 dedicated"这种环境偶然。
#if !defined(__has_include)
#define __has_include(x) 0
#endif
// VMA_IMPLEMENTATION 必须在本 TU **唯一的** VMA include 之前定义(全仓唯一)。
// 注意:写在 include 之后虽然"看起来"定义了,但 VMA 头里的实现段早已按未定义处理过,
// 结果是"只有声明没有实现",到链接期才炸(LNK2019)。所以必须在下面这段之前。
// VMA 是**第三方全内联头**,在 /W4 下会吐 ~20 条 C4100/C4189/C4324(它自己是 /W3 构建的)。
// 抑制范围**严格限定在 VMA include 前后**,不留到本 TU 其余部分 ——
// 否则我们自己的分配器代码会被静默降级,门控形同虚设。
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4100)  // 未引用的形参(VMA 的虚函数空实现)
#pragma warning(disable : 4189)  // 局部变量已初始化但未引用
#pragma warning(disable : 4324)  // 结构因对齐说明符被填充
#endif
#define VMA_IMPLEMENTATION
// Android 的 libvulkan.so 只导出 Vulkan **1.0** 符号：vkGetBufferMemoryRequirements2 /
// vkBindBufferMemory2 / vkGetPhysicalDeviceProperties2 等 1.1 核心函数在链接期直接
// undefined symbol。
//
// VMA 3.5.0 在「未定义 VMA_STATIC_VULKAN_FUNCTIONS 且未定义 VK_NO_PROTOTYPES」时
// **默认把 VMA_STATIC_VULKAN_FUNCTIONS 设为 1**，于是走 ImportVulkanFunctions_Static()，
// 在 1.1 分支里把这些函数当链接符号取地址 —— 这就是 undefined symbol 的来源。
// 所以 Android 上必须显式把 STATIC 压成 0，逼 VMA 走 DYNAMIC 分支
// （通过我们填的 pVulkanFunctions 用 vkGetInstanceProcAddr/vkGetDeviceProcAddr 解析）。
#if defined(__ANDROID__)
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#endif
// 注意三点:
//   1. __has_include 的探测形式必须和实际 #include 的形式**完全一致**
//      (<...> 与 "..." 的搜索起点不同,探到的未必是真正被包含的那个)。
//   2. 本文件**只有这一处** VMA include —— 再多写一条 #include <vk_mem_alloc.h>
//      会让 __has_include(<vk_mem_alloc.h>) 在没加 -I 时又变回 fatal C1083,
//      兜底就白写了。
//   3. 两条兜底分支必须让 VMA 的 __has_include 也命中 VK_KHR_dedicated_allocation
//      扩展头,否则 VMA 会关掉 dedicated 相关代码,我们那句显式
//      VMA_ALLOCATOR_CREATE_KHR_DEDICATED_ALLOCATION_BIT 会被 VMA_ASSERT 打中。
#if !__has_include(<vk_mem_alloc.h>)
#if __has_include("../../third_party/_install/vma-v3.5.0/release/include/vk_mem_alloc.h")
#define VK_IMPLICIT_DEDICATED_ALLOCATION_HEADER \
  "../../third_party/_install/vma-v3.5.0/release/include/vulkan/vulkan.h"
#include "../../third_party/_install/vma-v3.5.0/release/include/vk_mem_alloc.h"  // VMA 唯一 include
#elif __has_include("../third_party/_install/vma-v3.5.0/release/include/vk_mem_alloc.h")
#define VK_IMPLICIT_DEDICATED_ALLOCATION_HEADER \
  "../third_party/_install/vma-v3.5.0/release/include/vulkan/vulkan.h"
#include "../third_party/_install/vma-v3.5.0/release/include/vk_mem_alloc.h"  // VMA 唯一 include
#elif __has_include("../../../third_party/_install/vma-v3.5.0/release/include/vk_mem_alloc.h")
#define VK_IMPLICIT_DEDICATED_ALLOCATION_HEADER \
  "../../../third_party/_install/vma-v3.5.0/release/include/vulkan/vulkan.h"
#include "../../../third_party/_install/vma-v3.5.0/release/include/vk_mem_alloc.h"  // VMA 唯一 include
#else
#include <vk_mem_alloc.h>  // VMA 唯一 include;让编译器报出原始缺失错误
#endif
#else
#include <vk_mem_alloc.h>  // VMA 唯一 include
#endif

#if defined(_MSC_VER)
#pragma warning(pop)  // 第三方 VMA 头到此为止,下面恢复 /W4 
#endif

#include <algorithm>
#include <string>
#include <vector>

#include "backend/vulkan/vk_common.h"  // fmt_err / VulkanContext:Vk 类型只在本 TU 出现
#include "backend/vulkan/vulkan_resource_common.h"  // 页物理内存注册表(sync_page_memory)

namespace magicblocks::backend {

namespace {

using device::Result;
using device::Status;

// 桶数 = static_cast<u32>(MemoryKind::Staging) + 1;与 AllocatorStats::kMemoryKindCount 对齐。
constexpr u32 kKindCount = resources::AllocatorStats::kMemoryKindCount;
// 链表里表示"没有"的下标。
constexpr u32 kNoLink = 0xFFFFFFFFu;
constexpr u32 kNoPage = 0xFFFFFFFFu;
// 页内 64 KiB 槽位上限。设备页 64 MB/64 KiB = 1024 槽,Host 页 8 MB/64 KiB = 128 槽,
// 离上限都很远;这条断言让"有人把页调到 4 GiB+"在构造期就红,而不是让 offset 在
// pack() 里静默超宽(rulings 裁定 3)。
constexpr u64 kMaxSlotsPerPage = 65536;
// 全部桶都受预算硬约束(M6:不静默降级)。
constexpr u32 kBudgetAllKinds = (1u << kKindCount) - 1u;
// reserve_pages 单次请求的页数上界:防手滑一次要上千页把显存吃光。
constexpr u32 kMaxPagesPerReserve = 256;

u32 kind_index(MemoryKind k) { return static_cast<u32>(k); }

bool kind_in_range(MemoryKind k) { return kind_index(k) < kKindCount; }

u64 align_up(u64 value, u64 alignment) {
  if (alignment <= 1) return value;
  return (value + alignment - 1) / alignment * alignment;
}

bool is_power_of_two(u64 v) { return v != 0 && (v & (v - 1)) == 0; }

const char* kind_name(MemoryKind k) {
  switch (k) {
    case MemoryKind::Vertex: return "Vertex";
    case MemoryKind::Index: return "Index";
    case MemoryKind::Uniform: return "Uniform";
    case MemoryKind::Texture: return "Texture";
    case MemoryKind::Staging: return "Staging";
  }
  return "?";
}

// M6 报错文案:必须同时出现「需要 / 已有 / 预算」三个数
// (plans-l1 §6 Allocator.OverBudgetFails 会断言这三个数都在 message 里)。
std::string budget_message(MemoryKind k, u64 need_this, u64 need_total, u64 have, u64 budget) {
  return std::string("内存预算不足: 需要 ") + std::to_string(need_total) + " 字节(本次 " +
         std::to_string(need_this) + " 字节);已有 " + std::to_string(have) + " 字节;预算 " +
         std::to_string(budget) + " 字节 [桶 " + kind_name(k) + "]";
}

}  // namespace

// ============================================================================
// Impl:全部 VMA 状态与页/桶状态(本 TU 外不可见)。
// ============================================================================
struct VulkanPagingAllocator::Impl {
  // 反向指针:页注册表要按 IMemoryAllocator* 记页,而 Impl 只是它的成员,
  // 所以在 create_page 里需要 owner 才能调 sync_page_memory。
  VulkanPagingAllocator* owner = nullptr;

  // ------------------------------------------------------------ 子块表示
  //
  // 页内子块统一用「槽位下标 = 页内几何位置」表达:子块 i 的**几何起点**
  // geom(i) = i * 64 KiB(所有子块都从 64 KiB 边界开始,这是 rulings 裁定 3 的
  // 直接后果:任何成功返回的 ResourceId::offset 必是 64 KiB 的整数倍)。
  // 子块条目数 ≤ 页槽位数(设备页 1024、Host 页 128),页有空洞时条目被删掉、
  // geom 由下标推出 —— 所以"删条目"不影响几何关系。
  //
  // 空闲链表:每页一条严格按 offset 升序的单链表(prev/next 是页内下标),
  // 因为页内子块天然按 geom 升序,插入位置左右两根是 O(1) 推出的。
  struct Slot {
    u64 offset = 0;   // 子块起点(64 KiB 对齐)
    u64 size = 0;     // 活块 = 请求字节数;空闲块 = 0(容量见 cap)
    u64 cap = 0;      // 本子块的几何容量(到下一个子块起点为止;最后一个到页尾)
    u64 id = 0;       // 逻辑槽位号(活块才有意义)
    u32 prev_free = kNoLink;
    u32 next_free = kNoLink;
    bool in_free_list = false;
  };

  struct Page {
    u32 id = kNoPage;
    MemoryKind kind = MemoryKind::Vertex;
    resources::PageKind page_kind = resources::PageKind::Device;
    u64 page_size = 0;
    u64 slot_count = 0;     // 64 KiB 槽位数 = page_size / kPackGranularity
    u64 bytes_in_use = 0;   // 活子块 size 之和(不含对齐填充)
    u64 bytes_reserved = 0; // 已切出区间之和(含对齐填充)
    u32 live = 0;
    u64 idle_frames = 0;    // 连续空置帧数
    bool in_flight = false; // 有在途帧引用 → 不得退回 VMA
    bool host_mapped = false;
    // **本页当前有几个 vmaMapMemory 建立的映射**(VMA 内部 m_MapCount 的副本)。
    // VMA 的 unmap 不幂等:多还一次 → BlockAllocUnmap 断言;少还一次 →
    // ~VmaAllocation_T 断言。两者都 abort 进程(真机表现为启动即闪退)。
    //
    // 注意 VMA_ALLOCATION_CREATE_MAPPED_BIT 走的是 **FLAG_PERSISTENT_MAP**,
    // 它**不**增加 m_MapCount(见 vk_mem_alloc.h 的 InitBlockAllocation /
    // GetMappedData:"m_MapCount != 0 || IsPersistentMap()")。所以持久映射
    // 不占计数、释放时也不需要 unmap —— 只有我们需要显式 vmaMapMemory 时才 +1。
    int vma_map_count = 0;
    // 图像页(G4 替代 L0 裸分配的专用页):整页绑一张 VkImage,不参与桶内切分与 LRU。
    // 生命周期由 allocate_image_page / release_image_page 显式管理。
    bool image_bound = false;
    // 图像页绑的是 VkBuffer 而不是 VkImage(读回缓冲)。释放路径两者一致,
    // 但绑定必须调用正确的 vmaBind{Buffer,Image}Memory。
    bool buffer_bound = false;
    // 子块,按 offset 升序(**不变量**:slots[i].offset < slots[i+1].offset)。
    std::vector<Slot> slots;
    // 整页内存:VMA 绑定态由 VmaAllocationInfo::pMappedData 给出;
    // 纯软件态由下面这块 vector 模拟(可读写,便于单测验证映射)。
    std::uint8_t* host_ptr = nullptr;
    std::vector<std::uint8_t> host_storage;
    // VMA 句柄(纯软件态保持 VK_NULL_HANDLE)。
    VmaAllocation vma_allocation = VK_NULL_HANDLE;

    // 子块 i 的几何起点(64 KiB 粒子)。
    u64 geom(u32 i) const { return static_cast<u64>(i) * ResourceId::kPackGranularity; }
    // 子块 i 的几何终点 = 下一个子块起点(或页尾)。
    u64 geom_end(u32 i) const { return slots[i].offset + slots[i].cap; }
    // 子块 i 的几何容量(到下一个子块起点为止)。
    u64 geom_capacity(u32 i) const { return slots[i].cap; }
  };

  // ------------------------------------------------------------ 按桶

  struct Bucket {
    std::vector<u32> page_idx;  // 桶 → pages 下标(按创建顺序 = LRU 顺序)
    // 粗粒度缓存:桶内最大空闲块大小。释放/切分时标记 dirty,下一次 first-fit
    // 需要时重算 —— 目的是让"桶里根本没有够大的空块"这种常见拒绝走 O(1) 早退。
    u64 max_free = 0;
    bool dirty = true;
  };

  std::vector<Page> pages;
  Bucket buckets[kKindCount];
  u32 next_page_id = 0;

  // ------------------------------------------------------------ 配置与状态

  AllocatorConfig cfg{};
  AllocatorHandles handles{};
  bool propagate_frame_index = false;   // 纯软件态恒 false:没有 VMA 可推
  bool frame_index_push_enabled = true; // vmaSetCurrentFrameIndex 的诊断开关
  bool vma_frame_requested = true;      // 上一次 begin_frame 是否真的推给了 VMA
  u64 current_frame = 0;
  u64 logical_id_hi = 0;  // 单调递增的逻辑槽位号(M3)
  u32 budget_kinds = kBudgetAllKinds;
  bool inited = false;

  // ------------------------------------------------------------ VMA(全仓唯一持有者)

  VmaAllocator vma = VK_NULL_HANDLE;
  bool vma_ready = false;

  // ------------------------------------------------------------ 计数

  resources::AllocatorStats stats{};

  // ------------------------------------------------------------ 小工具

  Page* page_of(u32 page_id) {
    if (page_id >= pages.size()) return nullptr;
    Page& p = pages[page_id];
    return p.id == page_id ? &p : nullptr;
  }
  const Page* page_of(u32 page_id) const {
    if (page_id >= pages.size()) return nullptr;
    const Page& p = pages[page_id];
    return p.id == page_id ? &p : nullptr;
  }

  static bool is_alive(const Slot& s) { return !s.in_free_list; }

  static u64 free_bytes(const Page& p) {
    u64 sum = 0;
    for (u32 i = 0; i < p.slots.size(); ++i) {
      if (p.slots[i].in_free_list) sum += p.geom_capacity(i);
    }
    return sum;
  }

  static u32 free_block_count(const Page& p) {
    u32 n = 0;
    for (const Slot& s : p.slots) {
      if (s.in_free_list) ++n;
    }
    return n;
  }

  static u64 largest_free(const Page& p) {
    u64 best = 0;
    for (u32 i = 0; i < p.slots.size(); ++i) {
      if (p.slots[i].in_free_list) best = (std::max)(best, p.geom_capacity(i));
    }
    return best;
  }

  // 把 slots[idx] 从页内空闲链表里摘掉。O(1)。
  void unlink_free(Page& p, u32 idx) {
    Slot& s = p.slots[idx];
    if (!s.in_free_list) return;
    if (s.prev_free != kNoLink) p.slots[s.prev_free].next_free = s.next_free;
    if (s.next_free != kNoLink) p.slots[s.next_free].prev_free = s.prev_free;
    s.in_free_list = false;
    s.prev_free = kNoLink;
    s.next_free = kNoLink;
    buckets[kind_index(p.kind)].dirty = true;
  }

  // 把 slots[idx] 插回页内空闲链表。页内子块按 offset 升序,左右两根是 idx-1/idx+1
  // 中"同样是空闲块"的那些 → 插入位置 O(1) 确定,这就是 M4 的 O(1) 合并/插入。
  void link_free(Page& p, u32 idx) {
    Slot& s = p.slots[idx];
    s.in_free_list = true;
    s.prev_free = (idx > 0 && p.slots[idx - 1].in_free_list) ? idx - 1 : kNoLink;
    s.next_free =
        (idx + 1 < p.slots.size() && p.slots[idx + 1].in_free_list) ? idx + 1 : kNoLink;
    if (s.prev_free != kNoLink) p.slots[s.prev_free].next_free = idx;
    if (s.next_free != kNoLink) p.slots[s.next_free].prev_free = idx;
    buckets[kind_index(p.kind)].dirty = true;
  }


  // 页内 first-fit:在空闲块里找一个 offset 满足对齐、且余量放得下 envelope 的起点。
  // alignment 已由调用方保证是 64 KiB 的整数倍(≤ 页大小),所以候选起点仍在
  // 本空闲块内。返回命中的子块下标并通过 out_offset 给出起点。
  bool fit_in_page(Page& p, u64 envelope, u64 alignment, u64* out_offset, u32* out_slot) {
    for (u32 i = 0; i < p.slots.size(); ++i) {
      Slot& s = p.slots[i];
      if (!s.in_free_list) continue;
      const u64 start = align_up(s.offset, alignment);
      const u64 pad = start - s.offset;
      const u64 cap = p.geom_capacity(i);
      if (pad <= cap && envelope <= cap - pad) {
        *out_offset = start;
        *out_slot = i;
        return true;
      }
    }
    return false;
  }

  // 在页里切出 offset 处的子块。前提:slots[slot] 是**该起点上的空闲块**
  // (fit_in_page 已保证),且本块几何容量放得下。
  //
  // 统一按"删掉原空闲块 → 重建 [前残段][活块][后残段]"处理。之所以不用
  // "就地改造 + 中间插入"的省事写法:那会让"条目下标 == 64 KiB 几何位置"这条不变量
  // 在中间插入时失效,而页内链表指针全是按条目下标存的 —— 一次手算错就留下野指针,
  // 而且只在 release 之后才炸。重建写法慢一点,但每一步都容易验证。
  void carve(Page& p, u32 slot, u64 offset, u64 size, u64 id) {
    const u64 block_offset = p.slots[slot].offset;
    const u64 block_end = p.geom_end(slot);
    const u64 tail = block_end - offset;  // 从活块起点到本块终点的剩余空间
    const u64 live_cap = align_up(size, ResourceId::kPackGranularity);
    const bool has_front = offset > block_offset;
    const bool has_back = tail > live_cap;
    const u64 front_len = offset - block_offset;
    const u64 back_len = block_end - (offset + live_cap);

    std::vector<Slot> rebuilt;
    rebuilt.reserve(p.slots.size() + 2);
    for (u32 i = 0; i < slot; ++i) rebuilt.push_back(p.slots[i]);
    if (has_front) {
      Slot front;
      front.offset = block_offset;
      front.cap = front_len;
      front.in_free_list = true;
      rebuilt.push_back(front);
    }
    Slot live;
    live.offset = offset;
    live.size = size;
    live.cap = live_cap;
    live.id = id;
    live.in_free_list = false;
    rebuilt.push_back(live);
    if (has_back) {
      Slot back;
      back.offset = offset + live_cap;
      back.cap = back_len;
      back.in_free_list = true;
      rebuilt.push_back(back);
    }
    for (u32 i = slot + 1; i < p.slots.size(); ++i) rebuilt.push_back(p.slots[i]);
    p.slots = std::move(rebuilt);
    rebuild_free_list(p);

    p.bytes_in_use += size;
    p.bytes_reserved += live_cap;
    ++p.live;
    p.idle_frames = 0;
    buckets[kind_index(p.kind)].dirty = true;
  }

  // 全量重建页内空闲链表。页内子块按 offset 升序,所以一趟线性扫描就能把 prev/next
  // 串成严格有序的链 —— 这是"插入/删除后指针一定自洽"的兜底,代价 O(页内条目数)
  // (设备页 ≤ 1024)。合并动作本身仍是常数次操作,M4 的 merge_count 不受影响。
  static void rebuild_free_list(Page& p) {
    u32 prev = kNoLink;
    for (u32 i = 0; i < p.slots.size(); ++i) {
      Slot& s = p.slots[i];
      if (!s.in_free_list) {
        s.prev_free = kNoLink;
        s.next_free = kNoLink;
        continue;
      }
      s.prev_free = prev;
      s.next_free = kNoLink;
      if (prev != kNoLink) p.slots[prev].next_free = i;
      prev = i;
    }
  }

  // 把活子块 slots[idx] 还成空闲块,并与左右相邻空闲块合并。返回合并次数。
  u32 release_slot(Page& p, u32 idx) {
    Slot& s = p.slots[idx];
    p.bytes_in_use -= s.size;
    p.bytes_reserved -= p.geom_capacity(idx);
    --p.live;
    s.id = 0;
    s.size = 0;
    s.in_free_list = true;

    u32 merges = 0;
    // 左并:左邻是空闲块 → 把本块的容量并给左邻,再删掉自己。
    if (idx > 0 && p.slots[idx - 1].in_free_list) {
      p.slots[idx - 1].cap += p.slots[idx].cap;
      p.slots.erase(p.slots.begin() + idx);
      --idx;
      ++merges;
    }
    // 右并:当前块与右邻都是空闲块 → 把右邻容量并进来,再删掉右邻。
    if (idx + 1 < p.slots.size() && p.slots[idx + 1].in_free_list) {
      p.slots[idx].cap += p.slots[idx + 1].cap;
      p.slots.erase(p.slots.begin() + idx + 1);
      ++merges;
    }
    rebuild_free_list(p);
    buckets[kind_index(p.kind)].dirty = true;
    stats.merge_count += merges;
    return merges;
  }

  // ------------------------------------------------------------ 页的物理申请 / 归还(VMA 接触面)

  Status alloc_page_storage(Page* p) {
    p->host_ptr = nullptr;
    p->host_mapped = false;
    p->vma_allocation = VK_NULL_HANDLE;
    if (!vma_ready || vma == VK_NULL_HANDLE) {
      // 纯软件态:std::vector 模拟一整页(单测要验证 Host 页映射可读写)。
      // 计数值**照样** +1 —— vma_alloc_count 是 M5 的硬断言对象,必须能在没有
      // Vulkan 设备的 ctest 里被差分测量。唯一区别:没有真实设备内存。
      if (p->page_kind == resources::PageKind::HostVisible) {
        p->host_storage.assign(static_cast<std::size_t>(p->page_size), 0);
        p->host_ptr = p->host_storage.data();
        p->host_mapped = true;
      }
      // 设备页在 CPU 侧不可见:host_ptr 保持 nullptr(M2 的 Device/Host 页分工)。
      ++stats.vma_alloc_count;
      return Status::Ok_();
    }

    // ---- 真正的 VMA 路径 ----
    VkMemoryRequirements req{};
    req.size = static_cast<VkDeviceSize>(p->page_size);
    req.alignment = ResourceId::kPackGranularity;
    // memoryTypeBits 是「这块资源**允许**落在哪些内存类型」的位图。VMA 拿它和
    // requiredFlags 做交集挑内存类型;填 0 等于「一个都不允许」,VMA 于是必然
    // 返回 VK_ERROR_FEATURE_NOT_PRESENT(SwiftShader 上这条 100% 复现)。
    // 页是后端自造的裸内存,不对应任何 VkBuffer/VkImage,所以「全部内存类型
    // 都允许」,真正的约束交给 requiredFlags。这也是 vmaAllocateMemory 与
    // vmaCreateBuffer(它自己填 req)之间唯一的语义差别。
    req.memoryTypeBits = 0xFFFFFFFFu;

    VmaAllocationCreateInfo aci{};
    if (p->page_kind == resources::PageKind::Device) {
      // 设备页:DEVICE_LOCAL。不开 VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT
      // (spec §6.2-7「不创建 dedicated allocation」):64 MB 页本身也不该独占
      // 一块 VkDeviceMemory。
      //
      // 用 VMA_MEMORY_USAGE_GPU_ONLY 而不是 AUTO_PREFER_DEVICE:AUTO_* 系列要求
      // 分配器建时带上 VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT 等扩展标志,
      // 否则 VMA 直接返回 VK_ERROR_FEATURE_NOT_PRESENT(在 SwiftShader 上 100% 复现,
      // 与 size/alignment/memoryTypeBits 都无关)。传统 usage 值没有这个前提。
      aci.usage = VMA_MEMORY_USAGE_GPU_ONLY;
      aci.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    } else {
      // Host 页:HOST_VISIBLE|HOST_COHERENT + MAPPED 常驻(spec §6.2-7)。
      aci.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
      aci.requiredFlags =
          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
      aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                  VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    }

    VmaAllocation allocation = VK_NULL_HANDLE;
    VmaAllocationInfo info{};
    const VkResult r = vmaAllocateMemory(vma, &req, &aci, &allocation, &info);
    ++stats.vma_alloc_count;  // M5:只有真正调用了 vma* 才 +1
    if (r != VK_SUCCESS) {
      return Status::OOM(fmt_err("vmaAllocateMemory(整页)", r));
    }
    p->vma_allocation = allocation;
    // 只有 Host 页要常驻映射;设备页不给 CPU 指针(即使驱动给了 pMappedData)。
    if (p->page_kind == resources::PageKind::HostVisible) {
      p->host_ptr = static_cast<std::uint8_t*>(info.pMappedData);
      p->host_mapped = info.pMappedData != nullptr;
      // MAPPED_BIT = FLAG_PERSISTENT_MAP,**不计入** m_MapCount,这里保持 0。
    }
    return Status::Ok_();
  }

  void free_page_storage(Page* p) {
    if (!vma_ready || vma == VK_NULL_HANDLE) {
      p->host_storage.clear();
      p->host_storage.shrink_to_fit();
      p->host_ptr = nullptr;
      p->host_mapped = false;
      p->vma_map_count = 0;
      ++stats.vma_free_count;
      return;
    }
    if (p->vma_allocation != VK_NULL_HANDLE) {
      // 释放前把还活着的映射**精确**还清:vmaFreeMemory 不会替我们 unmap,
      // 而 VMA 的 unmap 既不幂等、又对残留映射直接 abort。按 vma_map_count
      // 逐次还,一次不多一次不少。
      while (p->vma_map_count > 0) {
        vmaUnmapMemory(vma, p->vma_allocation);
        --p->vma_map_count;
      }
      vmaFreeMemory(vma, p->vma_allocation);
      ++stats.vma_free_count;  // M5:只有真正调用了 vma* 才 +1
    }
    p->vma_allocation = VK_NULL_HANDLE;
    p->host_ptr = nullptr;
    p->host_mapped = false;
    p->vma_map_count = 0;
  }

  // 建一张新页(软件状态 + 物理存储)。**只有 reserve_pages 调它** ——
  // 单资源 allocate 永不进入本函数(M2/M4「大块优先,单资源不触发 VMA」)。
  Status create_page(MemoryKind kind, u32* out_index) {
    Page p;
    p.id = next_page_id++;
    p.kind = kind;
    p.page_kind = resources::page_kind_of(kind);
    p.page_size = (p.page_kind == resources::PageKind::Device) ? cfg.device_page_size
                                                               : cfg.host_page_size;
    p.slot_count = p.page_size / ResourceId::kPackGranularity;
    Slot whole;
    whole.offset = 0;
    whole.cap = p.page_size;
    whole.in_free_list = true;
    p.slots.push_back(whole);
    Status s = alloc_page_storage(&p);
    if (!s.ok()) return s;
    pages.push_back(std::move(p));
    const u32 index = static_cast<u32>(pages.size() - 1);
    buckets[kind_index(kind)].page_idx.push_back(index);
    buckets[kind_index(kind)].dirty = true;
    stats.page_count = live_page_count();
    // 把页的 (VkDeviceMemory, offset, size) 登记进后端页注册表:
    // VkBuffer/VkImage 只能按「整块内存 + 偏移」绑定,资源层要绑定到子块
    // 就得先拿到页的物理内存句柄。不登记的话,所有设备态路径(纹理上传/
    // 读回、缓冲绑定)都会报 "读回页未登记 VkDeviceMemory"。
    // 软件态 page_memory_handle 返回 false,register 侧自然跳过。
    sync_page_memory(owner, index);
    *out_index = index;
    return Status::Ok_();
  }

  // 物理回收一张页:标记为墓碑(id = kNoPage)但**保留下标**,让
  // "page_id == pages 下标" 这条不变量一直成立(句柄可预测、validate 简单)。
  // L1 的页数量级是几十张,墓碑不会成为问题。
  void drop_page(u32 index) {
    Page& p = pages[index];
    // 页的物理内存已经(或即将)被 VMA 退回,注册表里必须同步作废 ——
    // 否则后续 page_binding() 会拿到悬空的 VkDeviceMemory 去绑定。
    forget_page_memory(index);
    Bucket& b = buckets[kind_index(p.kind)];
    for (u32& v : b.page_idx) {
      if (v == index) {
        v = kNoPage;
        break;
      }
    }
    p.id = kNoPage;
    p.slots.clear();
    p.live = 0;
    p.bytes_in_use = 0;
    p.bytes_reserved = 0;
    p.idle_frames = 0;
    p.in_flight = false;
    stats.page_count = live_page_count();
    b.dirty = true;
  }

  u64 live_page_count() const {
    u64 n = 0;
    for (const Page& p : pages) {
      if (p.id != kNoPage) ++n;
    }
    return n;
  }

  // 当前活页占用的物理字节总数(预算判定用)。
  u64 resident_page_bytes() const {
    u64 n = 0;
    for (const Page& p : pages) {
      if (p.id != kNoPage) n += p.page_size;
    }
    return n;
  }

  // 「已有」:物理驻留字节(页面整页算)。
  u64 budget_have() const { return resident_page_bytes(); }

  bool budget_ok(MemoryKind k, u64 need_bytes, std::string* msg) const {
    if (!kind_in_range(k)) return true;
    const u32 bit = 1u << kind_index(k);
    if ((budget_kinds & bit) == 0) return true;  // 该桶不参加硬预算
    if (cfg.memory_budget == 0) return true;     // 未提供预算 → 不检查(与 DeviceCaps 同义)
    const u64 have = budget_have();
    if (have + need_bytes <= cfg.memory_budget) return true;
    if (msg) *msg = budget_message(k, need_bytes, have + need_bytes, have, cfg.memory_budget);
    return false;
  }

  // 桶内 first-fit。返回命中的 {页下标, 子块下标, offset};失败返回 false。
  bool first_fit(MemoryKind kind, u64 envelope, u64 alignment, u32* out_page_idx, u32* out_slot,
                 u64* out_offset) {
    // 粗粒度早退:桶内最大空闲块都放不下 → 直接 O(1) 拒绝(缓存失效时才重算)。
    if (bucket_max_free(kind) < envelope) return false;
    Bucket& b = buckets[kind_index(kind)];
    for (u32 k = 0; k < b.page_idx.size(); ++k) {
      const u32 pi = b.page_idx[k];
      if (pi == kNoPage) continue;
      Page& p = pages[pi];
      u64 off = 0;
      u32 slot = kNoLink;
      if (fit_in_page(p, envelope, alignment, &off, &slot)) {
        *out_page_idx = pi;
        *out_slot = slot;
        *out_offset = off;
        return true;
      }
    }
    return false;
  }

  // 桶内最大空闲块(需要时重算并缓存)。
  u64 bucket_max_free(MemoryKind kind) {
    Bucket& b = buckets[kind_index(kind)];
    if (!b.dirty) return b.max_free;
    u64 best = 0;
    for (u32 pi : b.page_idx) {
      if (pi == kNoPage) continue;
      best = (std::max)(best, largest_free(pages[pi]));
    }
    b.max_free = best;
    b.dirty = false;
    return best;
  }

  // 从统计的"当前占用"字段按实测重算(validate 与 stats 都依赖它保持自洽)。
  void recompute_in_use() {
    u64 in_use = 0;
    u64 reserved = 0;
    u64 bucket_in_use[kKindCount] = {};
    for (const Page& p : pages) {
      if (p.id == kNoPage) continue;
      in_use += p.bytes_in_use;
      reserved += p.bytes_reserved;
      bucket_in_use[kind_index(p.kind)] += p.bytes_in_use;
    }
    stats.bytes_in_use = in_use;
    stats.bytes_reserved = reserved;
    for (u32 i = 0; i < kKindCount; ++i) stats.bucket_bytes_in_use[i] = bucket_in_use[i];
    stats.page_count = live_page_count();
  }

  // ------------------------------------------------------------ VMA 初始化

  Status init_vma() {
    if (handles.device == 0) {
      // 纯软件态:没有 VMA 可建。**这不是降级**,而是单测形态(见头文件注释)。
      vma_ready = false;
      propagate_frame_index = false;
      return Status::Ok_();
    }

    VmaAllocatorCreateInfo aci{};
    aci.vulkanApiVersion = VK_API_VERSION_1_1;
    aci.instance = reinterpret_cast<VkInstance>(handles.instance);
    aci.physicalDevice = reinterpret_cast<VkPhysicalDevice>(handles.physical_device);
    aci.device = reinterpret_cast<VkDevice>(handles.device);
    // spec §6.2-7「不建 dedicated allocation」:显式关掉 KHR_dedicated_allocation,
    // 而不是指望"扩展没探测到" —— 后者是环境相关的偶然,不是保证。
    aci.flags = VMA_ALLOCATOR_CREATE_KHR_DEDICATED_ALLOCATION_BIT;
    // spec §6.2-7:bufferDeviceAddress 只在**设备真的启用了该特性**时才打开。
    //
    // 曾经的写法是"物理设备支持就置 VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT",
    // 这是错的:VMA 一旦看到这个 flag,就会给**每一次** vkAllocateMemory 挂上
    // VkMemoryAllocateFlagsInfo{ VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT },
    // 而逻辑设备创建时并没有启用 bufferDeviceAddress(实例请求的是 API 1.1,
    // 该特性要么靠 1.2 核心、要么靠显式启用 VK_KHR_buffer_device_address 扩展),
    // 于是校验层每次分配都报:
    //   VUID-VkMemoryAllocateFlagsInfo-flags-parameter
    //   VUID-VkMemoryAllocateInfo-flags-03331 (bufferDeviceAddress feature is not enabled)
    // "物理设备支持" ≠ "本设备启用了"。两者必须一致,否则就是永久刷屏的校验错误。
    //
    // 现在这一段**不置位**:设备侧没有启用该特性,分配侧就不该用它的 flag。
    // 要真正用上 bda(为 buffer reference 提速),得先在
    // vulkan_render_device.cpp 里把特性连同 VK_KHR_buffer_device_address 扩展
    // 一起启用,再把开关传进来 —— 两边必须成对修改。
    // (曾尝试直接在设备创建里塞 pNext 链启用,导致 vkCreateDevice 后 SEH
    //  0xc0000005,已回退;那需要配合 API 版本升级单独验证,不属于本次范围。)
    // API 版本仍是 1.1:见上面的 aci.vulkanApiVersion 注释。
    // Android 的 libvulkan.so 只导出 Vulkan **1.0** 符号：vkGetPhysicalDeviceFeatures2、
    // vkGetBufferMemoryRequirements2、vkBindBufferMemory2 等 1.1 核心函数在链接期是
    // undefined symbol。VMA 3.5.0 的 VMA_DYNAMIC_VULKAN_FUNCTIONS 默认为 1，但它要求
    // 调用方**显式填 pVulkanFunctions**——不填就不会走函数指针，仍是直连链接符号。
    // 所以这里用 loader 的 vkGetInstanceProcAddr / vkGetDeviceProcAddr 把 1.1 那几项
    // 从函数表里解析出来。Windows 上不需要（loader 直接导出 1.1 符号）。
    VmaVulkanFunctions vma_funcs{};
#if defined(__ANDROID__)
    vma_funcs.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    vma_funcs.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    aci.pVulkanFunctions = &vma_funcs;
#else
    (void)vma_funcs;
#endif

    const VkResult r = vmaCreateAllocator(&aci, &vma);
    if (r != VK_SUCCESS) {
      vma = VK_NULL_HANDLE;
      vma_ready = false;
      // 硬失败:绝不退化成"每个资源一次裸 vkAllocateMemory"(G4 的唯一内存路径)。
      return Status::InitFailed(fmt_err("vmaCreateAllocator", r));
    }
    vma_ready = true;
    propagate_frame_index = cfg.propagate_frame_index_to_vma;
    return Status::Ok_();
  }
};

// ============================================================================
// 构造 / 析构
// ============================================================================

VulkanPagingAllocator::VulkanPagingAllocator() : impl_(std::make_unique<Impl>()) {
  impl_->owner = this;
}

VulkanPagingAllocator::VulkanPagingAllocator(const AllocatorConfig& config)
    : impl_(std::make_unique<Impl>()) {
  impl_->cfg = config;
  impl_->owner = this;
}

VulkanPagingAllocator::VulkanPagingAllocator(const AllocatorHandles& handles)
    : impl_(std::make_unique<Impl>()) {
  impl_->handles = handles;
  impl_->owner = this;
}

VulkanPagingAllocator::VulkanPagingAllocator(const AllocatorHandles& handles,
                                             const AllocatorConfig& config)
    : impl_(std::make_unique<Impl>()) {
  impl_->handles = handles;
  impl_->cfg = config;
  impl_->owner = this;
}

VulkanPagingAllocator::~VulkanPagingAllocator() {
  if (!impl_) return;
  // 逐页把物理存储还掉(纯软件态对应 vector 释放,绑定态对应 vmaFreeMemory),
  // 再销毁 VmaAllocator。顺序反了会让页还回去时 VmaAllocator 已不存在。
  for (Impl::Page& p : impl_->pages) {
    if (p.id == kNoPage) continue;
    impl_->free_page_storage(&p);
  }
  impl_->pages.clear();
  if (impl_->vma_ready && impl_->vma != VK_NULL_HANDLE) {
    vmaDestroyAllocator(impl_->vma);
    impl_->vma = VK_NULL_HANDLE;
    impl_->vma_ready = false;
  }
}

// ============================================================================
// init:校验配置 → 建 VMA → 首组预取
// ============================================================================

device::Status VulkanPagingAllocator::init() {
  Impl& m = *impl_;

  // ---- 配置校验(D8:非法配置立刻报错,不"尽量跑") ----
  for (u64 size : {m.cfg.device_page_size, m.cfg.host_page_size}) {
    if (size == 0) return Status::Invalid("分配器:页大小不能为 0");
    if (size % ResourceId::kPackGranularity != 0) {
      return Status::Invalid("分配器:页大小必须是 64 KiB 的整数倍,得到 " + std::to_string(size));
    }
    if (size / ResourceId::kPackGranularity > kMaxSlotsPerPage) {
      return Status::Invalid("分配器:页内 64 KiB 槽位数超过 " + std::to_string(kMaxSlotsPerPage) +
                             ",得到 " + std::to_string(size / ResourceId::kPackGranularity));
    }
  }
  if (m.inited) return Status::Ok_();

  // ---- VMA ----
  Status s = m.init_vma();
  if (!s.ok()) return s;
  m.inited = true;

  // ---- 首组预取(spec §6.2 第 1 条:启动时按预算预留首组大页) ----
  if (m.cfg.initial_pages_per_kind > 0) {
    for (u32 k = 0; k < kKindCount; ++k) {
      s = reserve_pages(static_cast<MemoryKind>(k), m.cfg.initial_pages_per_kind);
      if (!s.ok()) return s;
    }
  }
  return Status::Ok_();
}

// ============================================================================
// allocate / free(M2 / M3 / M6)
// ============================================================================

device::Result<resources::AllocResult> VulkanPagingAllocator::allocate(MemoryKind kind, u64 size,
                                                                       u64 align) {
  Impl& m = *impl_;

  // ---- 参数校验 ----
  if (!kind_in_range(kind)) {
    return Status::Invalid("allocate: 非法 MemoryKind");
  }
  if (size == 0) return Status::Invalid("allocate: size 必须 > 0");
  if (!is_power_of_two(align)) {
    // D8 / memory.h 契约:align 必须是 2 的幂且 >= 1,不满足直接报错而非"尽力对齐"。
    return Status::Invalid("allocate: align 必须是 2 的幂且 >= 1,得到 " + std::to_string(align));
  }

  // ---- 对齐铁律(rulings 裁定 3) ----
  // 实际对齐 = max(调用方 align, kPackGranularity)。这**不是**可选的优化:
  // 命令流的 pack() 要求 offset % 64 KiB == 0,放松对齐会让它在运行期静默失败。
  // 同时也不允许超过页大小(那会让槽位粒度失效)。
  const u64 page_bytes = m.cfg.device_page_size;  // 兜底上界;真正判定见下
  const u64 kind_page_bytes =
      resources::page_kind_of(kind) == resources::PageKind::Device ? m.cfg.device_page_size
                                                                  : m.cfg.host_page_size;
  const u64 k64 = ResourceId::kPackGranularity;
  u64 effective_align = (std::max)(align, k64);
  if (effective_align > kind_page_bytes) {
    return Status::Invalid("allocate: 对齐 " + std::to_string(align) + " 超过页大小 " +
                           std::to_string(kind_page_bytes) + "(需要/已有/预算不适用,属参数错误)");
  }
  (void)page_bytes;

  // 子块占用的几何区间是 align_up(size, 64 KiB):粒度对齐的代价(裁到 rulings 裁定 3)。
  const u64 envelope = align_up(size, k64);
  if (envelope > kind_page_bytes) {
    // 单资源比一整页还大:在「分页 + 偏移子分配」模型下无法表达
    // (spec M2:资源全部从内存页切偏移)。这是明确的 Unsupported,不是静默降级。
    return Status::Unsupported("allocate: 单资源需要 " + std::to_string(envelope) +
                               " 字节的 64 KiB 槽位,超过整页大小 " +
                               std::to_string(kind_page_bytes) + " 字节(不允许单资源独占 VMA 分配)");
  }

  // ---- 预算(M6):先判定再动手,避免"先分配后失败"的半成品 ----
  std::string msg;
  if (!m.budget_ok(kind, 0, &msg)) {
    return Status::OOM(msg);
  }

  // ---- 桶内 first-fit(热路径:只动空闲链表,绝不触发 VMA) ----
  u32 page_idx = kNoPage;
  u32 slot = kNoLink;
  u64 offset = 0;
  if (!m.first_fit(kind, envelope, effective_align, &page_idx, &slot, &offset)) {
    // 桶里没有能装下的空块。**不**在这里向 VMA 要页(M2/M4「大块优先,单资源永不
    // 直接触发 VMA」):由上层用 reserve_pages() 一次性整页预取。
    // 报错里带上"需要/已有/预算",与预算错误同一个诊断口径。
    const u64 have = m.budget_have();
    return Status::OOM(std::string("内存不足: 需要 ") + std::to_string(size) + " 字节(需 " +
                       std::to_string(envelope) + " 字节 64 KiB 槽位);已有 " +
                       std::to_string(have) + " 字节;预算 " +
                       std::to_string(m.cfg.memory_budget) + " 字节 [桶 " + kind_name(kind) +
                       "];当前页无空位 —— 请先 reserve_pages() 预取整页");
  }

  Impl::Page& page = m.pages[page_idx];
  const u64 id = m.logical_id_hi++;
  m.carve(page, slot, offset, size, id);

  // ---- 统计 ----
  ++m.stats.allocation_count;
  ++m.stats.bucket_alloc_count[kind_index(kind)];
  m.stats.bytes_in_use += size;
  m.stats.bytes_reserved += envelope;
  m.stats.bucket_bytes_in_use[kind_index(kind)] += size;

  resources::AllocResult out;
  out.id.page_id = page.id;
  out.id.offset = static_cast<u32>(offset);
  out.id.size = size;
  out.id.id = id;
  // Host 页 MAPPED 常驻 → 给出可直接写的首址;设备页 CPU 不可见 → nullptr(不算失败)。
  out.mapped = page.host_ptr != nullptr
                   ? static_cast<void*>(page.host_ptr + offset)
                   : nullptr;
  return out;
}

device::Status VulkanPagingAllocator::free(const ResourceId& id) {
  Impl& m = *impl_;

  // 句柄非法 / 已释放 / 不属于本分配器 → InvalidArgument(显式报错,不静默忽略)。
  if (!id.valid()) return Status::Invalid("free: 句柄非法(page_id/size 无效)");
  Impl::Page* page = m.page_of(id.page_id);
  if (page == nullptr) {
    return Status::Invalid("free: 页 " + std::to_string(id.page_id) + " 不存在(已回收或不属于本分配器)");
  }
  for (u32 i = 0; i < page->slots.size(); ++i) {
    const Impl::Slot& s = page->slots[i];
    // 命中条件:同一 offset 的**活块**且 id/size 全等 —— 用 id 做一致性校验(M3)。
    if (!s.in_free_list && s.offset == id.offset && s.id == id.id && s.size == id.size) {
      m.release_slot(*page, i);
      ++m.stats.free_count;
      m.stats.bytes_in_use -= id.size;
      m.stats.bytes_reserved -= 0;  // release_slot 已在页内扣减
      m.stats.bucket_bytes_in_use[kind_index(page->kind)] -= id.size;
      // bytes_reserved 以页为准重算一遍,避免"几何容量"与"envelope"两套账漂移。
      m.recompute_in_use();
      return Status::Ok_();
    }
  }
  return Status::Invalid("free: 页 " + std::to_string(id.page_id) + " 上找不到活子块 offset=" +
                         std::to_string(id.offset) + " id=" + std::to_string(id.id) +
                         "(可能已释放)");
}

// ============================================================================
// 映射(mapped_ptr / map_page / unmap_page)
// ============================================================================

void* VulkanPagingAllocator::mapped_ptr(const ResourceId& id, u64 offset) const {
  const Impl& m = *impl_;
  const Impl::Page* page = m.page_of(id.page_id);
  if (page == nullptr || page->host_ptr == nullptr) return nullptr;
  // 越界一律 nullptr(D8:不给"差不多能用"的越界指针)。
  if (offset >= id.size) return nullptr;
  return const_cast<std::uint8_t*>(page->host_ptr) + id.offset + offset;
}

void* VulkanPagingAllocator::map_page(u32 page_id) {
  Impl& m = *impl_;
  Impl::Page* page = m.page_of(page_id);
  if (page == nullptr) return nullptr;
  // Host 页 MAPPED 常驻 → 直接给整页首址;设备页 CPU 不可见 → nullptr。
  if (page->host_ptr != nullptr) return page->host_ptr;
  if (page->page_kind != resources::PageKind::HostVisible) return nullptr;
  if (!m.vma_ready || m.vma == VK_NULL_HANDLE || page->vma_allocation == VK_NULL_HANDLE) {
    return nullptr;
  }
  // 走到这里说明 Host 页建的时候没拿到常驻映射(不该发生,但别硬断言崩溃)。
  void* mapped = nullptr;
  if (vmaMapMemory(m.vma, page->vma_allocation, &mapped) != VK_SUCCESS) return nullptr;
  page->host_ptr = static_cast<std::uint8_t*>(mapped);
  page->host_mapped = mapped != nullptr;
  if (mapped != nullptr) ++page->vma_map_count;
  return mapped;
}

device::Status VulkanPagingAllocator::unmap_page(u32 page_id) {
  Impl& m = *impl_;
  Impl::Page* page = m.page_of(page_id);
  if (page == nullptr) return Status::Invalid("unmap_page: 页不存在");
  // 常驻映射(MAPPED_BIT / 纯软件态)没有对应的 unmap,属幂等成功。
  if (!m.vma_ready || m.vma == VK_NULL_HANDLE) return Status::Ok_();
  if (page->vma_allocation == VK_NULL_HANDLE) return Status::Ok_();
  if (!page->host_mapped) return Status::Ok_();
  // 只还我们 vmaMapMemory 出来的那一次。持久映射(MAPPED_BIT)不进 m_MapCount,
  // 计数为 0 时不能调 vmaUnmapMemory —— 那会减一个不存在的映射并断言。
  if (page->vma_map_count <= 0) return Status::Ok_();
  vmaUnmapMemory(m.vma, page->vma_allocation);
  --page->vma_map_count;
  page->host_ptr = nullptr;
  page->host_mapped = false;
  return Status::Ok_();
}

// ============================================================================
// 页管理(reserve_pages / page_count / page_size / page_desc)
// ============================================================================

device::Status VulkanPagingAllocator::reserve_pages(MemoryKind kind, u32 count) {
  Impl& m = *impl_;
  if (!kind_in_range(kind)) return Status::Invalid("reserve_pages: 非法 MemoryKind");
  if (count > kMaxPagesPerReserve) {
    return Status::Invalid("reserve_pages: 单次页数上限 " + std::to_string(kMaxPagesPerReserve) +
                           ",得到 " + std::to_string(count));
  }
  if (count == 0) return Status::Ok_();

  // 页大小在配置校验里已保证是 64 KiB 的正整数倍;这里再算一次预算。
  const u64 per_page = resources::page_kind_of(kind) == resources::PageKind::Device
                           ? m.cfg.device_page_size
                           : m.cfg.host_page_size;
  const u64 need = per_page * static_cast<u64>(count);
  std::string msg;
  if (!m.budget_ok(kind, need, &msg)) {
    return Status::OOM(msg);
  }

  // 串行建页:中途失败就把本次已建的页回滚,不留半成品(spec D8)。
  std::vector<u32> created;
  created.reserve(count);
  for (u32 i = 0; i < count; ++i) {
    u32 idx = kNoPage;
    Status s = m.create_page(kind, &idx);
    if (!s.ok()) {
      for (u32 back : created) {
        m.free_page_storage(&m.pages[back]);
        m.drop_page(back);
        m.stats.vma_alloc_count = m.stats.vma_alloc_count;  // 建页时已计入;回滚不再减
      }
      return s;
    }
    created.push_back(idx);
  }
  m.recompute_in_use();
  return Status::Ok_();
}

u32 VulkanPagingAllocator::page_count(MemoryKind kind) const {
  if (!kind_in_range(kind)) return 0;
  const Impl& m = *impl_;
  u32 n = 0;
  for (u32 pi : m.buckets[kind_index(kind)].page_idx) {
    if (pi != kNoPage && m.pages[pi].id != kNoPage) ++n;
  }
  return n;
}

u64 VulkanPagingAllocator::page_size(MemoryKind kind) const {
  if (!kind_in_range(kind)) return 0;
  const Impl& m = *impl_;
  return resources::page_kind_of(kind) == resources::PageKind::Device ? m.cfg.device_page_size
                                                                      : m.cfg.host_page_size;
}

bool VulkanPagingAllocator::page_desc(u32 page_id, resources::PageDesc* out) const {
  if (out == nullptr) return false;
  const Impl& m = *impl_;
  const Impl::Page* page = m.page_of(page_id);
  if (page == nullptr) return false;  // 页不存在时 out 保持不变
  resources::PageDesc d;
  d.page_id = page->id;
  d.kind = page->page_kind;
  d.page_size = page->page_size;
  d.bytes_in_use = page->bytes_in_use;
  d.bytes_reserved = page->bytes_reserved;
  d.live_allocations = page->live;
  d.free_block_count = Impl::free_block_count(*page);
  d.idle_frames = page->idle_frames;
  d.in_flight = page->in_flight;
  *out = d;
  return true;
}

// ============================================================================
// 帧边界(M4 / spec §6.2-5)
// ============================================================================

void VulkanPagingAllocator::begin_frame(u64 frame_index) {
  Impl& m = *impl_;
  m.current_frame = frame_index;

  // spec §6.2 第 5 条:把帧号推给 VMA,让它自己的延迟释放与引擎帧边界一致。
  // 纯软件态 / 诊断开关关闭时不推(页面空置计数照常推进)。
  m.vma_frame_requested = false;
  if (m.propagate_frame_index && m.frame_index_push_enabled && m.vma_ready &&
      m.vma != VK_NULL_HANDLE) {
    vmaSetCurrentFrameIndex(m.vma, static_cast<std::uint32_t>(frame_index));
    m.vma_frame_requested = true;
  }

  // 推进页空置计数:完全空置的页 +1 帧,被使用的页清零。
  for (Impl::Page& p : m.pages) {
    if (p.id == kNoPage) continue;
    const bool idle = p.live == 0 && Impl::free_bytes(p) >= p.page_size;
    p.idle_frames = idle ? (p.idle_frames + 1) : 0;
  }

  // 帧边界顺手做一次整页 LRU 回收;失败不打断渲染(下一帧还会重试),
  // 但一定要报 —— 这里没有返回值,所以只在 trim() 里返回错误。
  (void)trim();
}

void VulkanPagingAllocator::set_page_in_flight(u32 page_id, bool in_flight) {
  Impl& m = *impl_;
  Impl::Page* page = m.page_of(page_id);
  if (page == nullptr) return;  // 页不存在:与"已回收"等价,不算错误
  page->in_flight = in_flight;
}

u32 VulkanPagingAllocator::page_idle_frames() const {
  return static_cast<u32>(impl_->cfg.page_idle_frames);
}

device::Status VulkanPagingAllocator::trim() {
  Impl& m = *impl_;
  // 整页 LRU(M6:不做页内碎片整理):空置满 cfg.page_idle_frames 帧、
  // 无在途帧引用 → 整页退回 VMA(vma_free_count / evict_count 各 +1)。
  const u64 threshold = m.cfg.page_idle_frames;
  u32 evicted = 0;
  for (u32 i = 0; i < m.pages.size(); ++i) {
    Impl::Page& p = m.pages[i];
    if (p.id == kNoPage) continue;
    if (p.live != 0) continue;                 // 还有活子块:不能退
    if (p.in_flight) continue;                 // 有在途帧引用:不能退(M4 硬条件)
    if (Impl::free_bytes(p) < p.page_size) continue;  // 碎片没满(理论上不会出现)
    if (p.idle_frames < threshold) continue;   // 空置还不够久
    m.free_page_storage(&p);
    m.drop_page(i);
    ++m.stats.vma_free_count;  // 与 free_page_storage 的计数一致(纯软件态/绑定态都走这条)
    ++m.stats.evict_count;
    ++evicted;
  }
  // 上面 free_page_storage 在绑定态已经 +1 了 vma_free_count;纯软件态也 +1。
  // 这里再 +1 会把每次淘汰记成两次 —— 于是把多记的那一份扣回来,
  // 保证「一次淘汰 = vma_free_count +1」(plans-l1 §6 PageEvictionAfterIdle 断言)。
  if (evicted > 0) {
    m.stats.vma_free_count -= evicted;
  }
  if (evicted > 0) m.recompute_in_use();
  return Status::Ok_();
}

// ============================================================================
// 预算 / 统计 / 自检
// ============================================================================

resources::AllocatorStats VulkanPagingAllocator::stats() const {
  resources::AllocatorStats s = impl_->stats;
  // 让"当前占用"字段永远是实测值,而不是维护出来的累计值:
  // 这样 validate() 里的「统计与实测一致」才真的在验证东西。
  const Impl& m = *impl_;
  s.page_count = m.live_page_count();
  s.memory_budget = m.cfg.memory_budget;
  return s;
}

void VulkanPagingAllocator::reset_counters() {
  // 清零**累计**计数(差分断言的起点),不改变当前占用。
  // M5 的断言形态是「取两次 stats 求差」,所以这里只需保证 vma_* / merge / evict /
  // allocation / free / bucket_alloc 这些单调计数归零即可。
  Impl& m = *impl_;
  m.stats.vma_alloc_count = 0;
  m.stats.vma_free_count = 0;
  m.stats.allocation_count = 0;
  m.stats.free_count = 0;
  m.stats.merge_count = 0;
  m.stats.evict_count = 0;
  for (u32 i = 0; i < kKindCount; ++i) m.stats.bucket_alloc_count[i] = 0;
  m.recompute_in_use();
}

u64 VulkanPagingAllocator::memory_budget() const { return impl_->cfg.memory_budget; }

void VulkanPagingAllocator::set_memory_budget(u64 bytes) { impl_->cfg.memory_budget = bytes; }

u32 VulkanPagingAllocator::require_budget_check() const { return impl_->budget_kinds; }

device::Status VulkanPagingAllocator::validate() const {
  const Impl& m = *impl_;

  // ---- 1. 页 id 与下标一致(保留下标的不变量) + 页内几何 ----
  for (u32 i = 0; i < m.pages.size(); ++i) {
    const Impl::Page& p = m.pages[i];
    if (p.id == kNoPage) continue;
    if (p.id != i) {
      return Status::Invalid("validate: 页 " + std::to_string(p.id) + " 的下标是 " +
                             std::to_string(i) + ",违反 page_id == pages 下标 不变量");
    }
    if (p.slot_count == 0 || p.slot_count > kMaxSlotsPerPage) {
      return Status::Invalid("validate: 页 " + std::to_string(p.id) + " 槽位数非法");
    }
    // 子块严格按 offset 升序、互不重叠、且都在页内。
    u64 prev_end = 0;
    u64 reserved = 0;
    u64 in_use = 0;
    u32 live = 0;
    for (u32 k = 0; k < p.slots.size(); ++k) {
      const Impl::Slot& s = p.slots[k];
      // 子块必须从 64 KiB 边界开始 —— 这是 rulings 裁定 3 的硬约束。
      if (!s.in_free_list && k == 0 && s.offset != 0) {
        return Status::Invalid("validate: 页 " + std::to_string(p.id) + " 首个子块不从 0 开始");
      }
      if ((s.offset % ResourceId::kPackGranularity) != 0) {
        return Status::Invalid("validate: 页 " + std::to_string(p.id) + " 子块 " +
                               std::to_string(k) + " 的 offset " + std::to_string(s.offset) +
                               " 未按 64 KiB 对齐");
      }
      // 子块按 offset 升序、互不重叠、首尾相接地覆盖整页(容量 = 到下一个子块起点)。
      if (s.offset != prev_end) {
        return Status::Invalid("validate: 页 " + std::to_string(p.id) + " 子块 " +
                               std::to_string(k) + " 的 offset " + std::to_string(s.offset) +
                               " 与前一个子块终点 " + std::to_string(prev_end) + " 不连续");
      }
      if (s.cap == 0 || s.offset + s.cap > p.page_size) {
        return Status::Invalid("validate: 页 " + std::to_string(p.id) + " 子块容量越界");
      }
      if (s.in_free_list) {
        if (s.size != 0) {
          return Status::Invalid("validate: 页 " + std::to_string(p.id) + " 空闲块携带了非零 size");
        }
      } else {
        // 活块真实字节数落在 (上一槽位容量, 64 KiB 粒度的本槽容量] 之内。
        if (s.size == 0 || s.size > s.cap) {
          return Status::Invalid("validate: 页 " + std::to_string(p.id) + " 活子块 size " +
                                 std::to_string(s.size) + " 超过容量 " + std::to_string(s.cap));
        }
        in_use += s.size;
        reserved += s.cap;
        ++live;
      }
      prev_end = s.offset + s.cap;
    }
    if (prev_end != p.page_size) {
      return Status::Invalid("validate: 页 " + std::to_string(p.id) + " 子块未覆盖整页");
    }
    if (in_use != p.bytes_in_use) {
      return Status::Invalid("validate: 页 " + std::to_string(p.id) + " bytes_in_use 记账 " +
                             std::to_string(p.bytes_in_use) + " != 实测 " + std::to_string(in_use));
    }
    if (reserved != p.bytes_reserved) {
      return Status::Invalid("validate: 页 " + std::to_string(p.id) + " bytes_reserved 记账 " +
                             std::to_string(p.bytes_reserved) + " != 实测 " + std::to_string(reserved));
    }
    if (live != p.live) {
      return Status::Invalid("validate: 页 " + std::to_string(p.id) + " live 记账 " +
                             std::to_string(p.live) + " != 实测 " + std::to_string(live));
    }

    // ---- 2. 页内空闲链表与子块数组互相自洽 ----
    // 链表必须按 offset 升序、且恰好覆盖"in_free_list == true"的那些子块。
    u32 chain_len = 0;
    u64 last_offset = 0;
    bool first = true;
    u32 cur = kNoLink;
    // 找链表头:prev_free == kNoLink 的空闲块。
    for (u32 k = 0; k < p.slots.size(); ++k) {
      if (p.slots[k].in_free_list && p.slots[k].prev_free == kNoLink) {
        cur = k;
        break;
      }
    }
    while (cur != kNoLink) {
      if (cur >= p.slots.size()) return Status::Invalid("validate: 空闲链表指针越界");
      const Impl::Slot& s = p.slots[cur];
      if (!s.in_free_list) return Status::Invalid("validate: 空闲链表指向了活子块");
      if (!first && s.offset <= last_offset) {
        return Status::Invalid("validate: 空闲链表未按 offset 升序");
      }
      first = false;
      last_offset = s.offset;
      cur = s.next_free;
      if (++chain_len > p.slots.size()) {
        return Status::Invalid("validate: 空闲链表成环");
      }
    }
    u32 expect_len = Impl::free_block_count(p);
    if (chain_len != expect_len) {
      return Status::Invalid("validate: 页 " + std::to_string(p.id) + " 空闲链表长度 " +
                             std::to_string(chain_len) + " != 空闲块数 " + std::to_string(expect_len));
    }
  }

  // ---- 3. 桶 → 页的索引自洽 ----
  for (u32 k = 0; k < kKindCount; ++k) {
    for (u32 pi : m.buckets[k].page_idx) {
      if (pi == kNoPage) continue;
      if (pi >= m.pages.size()) return Status::Invalid("validate: 桶索引越界");
      const Impl::Page& p = m.pages[pi];
      if (p.id == kNoPage) return Status::Invalid("validate: 桶索引指向墓碑页");
      if (kind_index(p.kind) != k) return Status::Invalid("validate: 桶索引与页的 kind 不一致");
    }
  }

  // ---- 4. 统计与实测一致 ----
  u64 in_use = 0;
  u64 reserved = 0;
  u64 bucket_in_use[kKindCount] = {};
  for (const Impl::Page& p : m.pages) {
    if (p.id == kNoPage) continue;
    in_use += p.bytes_in_use;
    reserved += p.bytes_reserved;
    bucket_in_use[kind_index(p.kind)] += p.bytes_in_use;
  }
  if (in_use != m.stats.bytes_in_use) {
    return Status::Invalid("validate: stats.bytes_in_use " + std::to_string(m.stats.bytes_in_use) +
                           " != 实测 " + std::to_string(in_use));
  }
  if (reserved != m.stats.bytes_reserved) {
    return Status::Invalid("validate: stats.bytes_reserved " + std::to_string(m.stats.bytes_reserved) +
                           " != 实测 " + std::to_string(reserved));
  }
  for (u32 k = 0; k < kKindCount; ++k) {
    if (bucket_in_use[k] != m.stats.bucket_bytes_in_use[k]) {
      return Status::Invalid("validate: 桶 " + std::to_string(k) + " 的字节计数与实测不一致");
    }
  }
  if (m.live_page_count() != m.stats.page_count) {
    return Status::Invalid("validate: stats.page_count 与实测页数不一致");
  }
  // 页的物理占用不能超过预算(若提供预算)。
  if (m.cfg.memory_budget != 0 && m.resident_page_bytes() > m.cfg.memory_budget) {
    return Status::Invalid("validate: 驻留页字节 " + std::to_string(m.resident_page_bytes()) +
                           " 超过预算 " + std::to_string(m.cfg.memory_budget));
  }
  return Status::Ok_();
}

// ============================================================================
// 单测/后端查询
// ============================================================================

bool VulkanPagingAllocator::has_vma() const { return impl_->vma_ready; }

bool VulkanPagingAllocator::vma_frame_index_enabled() const { return impl_->vma_frame_requested; }

u32 VulkanPagingAllocator::bucket_free_block_count(MemoryKind kind) const {
  if (!kind_in_range(kind)) return 0;
  const Impl& m = *impl_;
  u32 n = 0;
  for (u32 pi : m.buckets[kind_index(kind)].page_idx) {
    if (pi == kNoPage) continue;
    n += Impl::free_block_count(m.pages[pi]);
  }
  return n;
}

u64 VulkanPagingAllocator::largest_free_block(MemoryKind kind) const {
  if (!kind_in_range(kind)) return 0;
  const Impl& m = *impl_;
  u64 best = 0;
  for (u32 pi : m.buckets[kind_index(kind)].page_idx) {
    if (pi == kNoPage) continue;
    best = (std::max)(best, Impl::largest_free(m.pages[pi]));
  }
  return best;
}

u32 VulkanPagingAllocator::free_extents(MemoryKind kind, std::vector<FreeBlockInfo>* out) const {
  if (!kind_in_range(kind)) return 0;
  const Impl& m = *impl_;
  if (out) out->clear();
  u32 n = 0;
  for (u32 pi : m.buckets[kind_index(kind)].page_idx) {
    if (pi == kNoPage) continue;
    const Impl::Page& p = m.pages[pi];
    for (u32 i = 0; i < p.slots.size(); ++i) {
      if (!p.slots[i].in_free_list) continue;
      ++n;
      if (out) {
        FreeBlockInfo info;
        info.offset = p.slots[i].offset;
        info.size = p.geom_capacity(i);
        out->push_back(info);
      }
    }
  }
  if (out) std::sort(out->begin(), out->end(), [](const FreeBlockInfo& a, const FreeBlockInfo& b) {
    return a.offset < b.offset;
  });
  return n;
}

u32 VulkanPagingAllocator::listed_slots(MemoryKind kind, std::vector<SubAllocInfo>* out) const {
  if (!kind_in_range(kind)) return 0;
  const Impl& m = *impl_;
  if (out) out->clear();
  u32 n = 0;
  for (u32 pi : m.buckets[kind_index(kind)].page_idx) {
    if (pi == kNoPage) continue;
    const Impl::Page& p = m.pages[pi];
    for (u32 i = 0; i < p.slots.size(); ++i) {
      const Impl::Slot& s = p.slots[i];
      ++n;
      if (out) {
        SubAllocInfo info;
        info.kind = p.kind;
        info.alive = !s.in_free_list;
        info.id.page_id = p.id;
        info.id.offset = static_cast<u32>(s.offset);
        info.id.size = s.in_free_list ? 0 : s.size;
        info.id.id = s.in_free_list ? 0 : s.id;
        out->push_back(info);
      }
    }
  }
  if (out) {
    std::sort(out->begin(), out->end(), [](const SubAllocInfo& a, const SubAllocInfo& b) {
      if (a.id.page_id != b.id.page_id) return a.id.page_id < b.id.page_id;
      return a.id.offset < b.id.offset;
    });
  }
  return n;
}

u32 VulkanPagingAllocator::page_total() const {
  return static_cast<u32>(impl_->live_page_count());
}

bool VulkanPagingAllocator::page_desc_at(u32 index, resources::PageDesc* out) const {
  if (out == nullptr) return false;
  const Impl& m = *impl_;
  if (index >= m.pages.size()) return false;
  return page_desc(m.pages[index].id, out);
}

bool VulkanPagingAllocator::page_kind(u32 page_id, MemoryKind* kind) const {
  if (kind == nullptr) return false;
  const Impl& m = *impl_;
  const Impl::Page* p = m.page_of(page_id);
  if (p == nullptr) return false;
  *kind = p->kind;
  return true;
}

u32 VulkanPagingAllocator::page_slot_capacity(u32 page_id) const {
  const Impl& m = *impl_;
  const Impl::Page* p = m.page_of(page_id);
  return p == nullptr ? 0 : static_cast<u32>(p->slot_count);
}

u32 VulkanPagingAllocator::page_slots_used(u32 page_id) const {
  const Impl& m = *impl_;
  const Impl::Page* p = m.page_of(page_id);
  if (p == nullptr) return 0;
  u32 used = 0;
  for (const Impl::Slot& s : p->slots) {
    if (!s.in_free_list) ++used;
  }
  return used;
}

void VulkanPagingAllocator::set_vma_frame_index_enabled(bool enabled) {
  impl_->frame_index_push_enabled = enabled;
}

u64 VulkanPagingAllocator::next_logical_id() const { return impl_->logical_id_hi; }

// ---------------------------------------------------------------- 图像绑定(G4)
//
// L0 的 vulkan_target.cpp 原来为颜色/深度附件与读回缓冲直接调 vkAllocateMemory。
// L1 之后全仓零裸分配(G4),这些位置必须由分配器出内存。与桶内子分配的区别:
//   - VkImage 必须整块绑定到一个 VmaAllocation 的 offset 0,**不能**与别的资源共享,
//     所以图像走"专用整页"(仍是一次 vmaAllocateMemory,计入 vma_alloc_count);
//   - 这类页不进桶、不进 LRU、不切 64 KiB 槽位,由创建者在销毁时显式 release。

device::Result<VulkanPagingAllocator::ImagePageResult>
VulkanPagingAllocator::allocate_image_page(const ImagePageRequest& req) {
  Impl& m = *impl_;
  if (req.size == 0) return Status::Invalid("allocate_image_page: size 必须 > 0");

  Impl::Page p;
  p.id = m.next_page_id++;
  // 图像页借 Texture 桶占位(仅用于页内记账,不参与该桶的空闲链复用)。
  p.kind = MemoryKind::Texture;
  p.page_kind = req.device_local ? resources::PageKind::Device : resources::PageKind::HostVisible;
  p.page_size = req.size;
  p.slot_count = 0;  // 不切槽位
  p.image_bound = true;
  p.slots.clear();

  if (!m.vma_ready || m.vma == VK_NULL_HANDLE) {
    // 纯软件态(单测):用 vector 模拟整页,让图像绑定路径在没有 GPU 时也能被验证。
    p.host_storage.assign(static_cast<std::size_t>(req.size), 0);
    if (!req.device_local) p.host_ptr = p.host_storage.data();
    ++m.stats.vma_alloc_count;
  } else {
    VkMemoryRequirements mem_req{};
    mem_req.size = static_cast<VkDeviceSize>(req.size);
    mem_req.alignment = req.alignment == 0 ? 1 : static_cast<VkDeviceSize>(req.alignment);
    mem_req.memoryTypeBits = req.memory_type_bits;

    VmaAllocationCreateInfo aci{};
    if (req.device_local) {
      // 同 create_page:AUTO_* usage 需要分配器带扩展标志,否则 FEATURE_NOT_PRESENT。
      aci.usage = VMA_MEMORY_USAGE_GPU_ONLY;
      aci.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    } else {
      aci.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
      aci.requiredFlags =
          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
      aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                  VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
    }
    VmaAllocation allocation = VK_NULL_HANDLE;
    VmaAllocationInfo info{};
    const VkResult r = vmaAllocateMemory(m.vma, &mem_req, &aci, &allocation, &info);
    ++m.stats.vma_alloc_count;  // M5:真实 vma* 调用才 +1
    if (r != VK_SUCCESS) return Status::OOM(fmt_err("vmaAllocateMemory(图像页)", r));
    p.vma_allocation = allocation;
  // 映射:分配时已带 VMA_ALLOCATION_CREATE_MAPPED_BIT,VMA 会把地址写进
  // info.pMappedData。**优先用它**,不要无脑再 vmaMapMemory 一次 —— 那会让
  // MapCount 变成 2,释放时必须还两次,是个纯粹自找的对称负担。
  // 只在 pMappedData 为空时(某些主机可见堆不保证常驻)才自己 map。
  if (!p.host_ptr && m.vma_ready && m.vma != VK_NULL_HANDLE) {
    if (info.pMappedData != nullptr) {
      p.host_ptr = static_cast<std::uint8_t*>(info.pMappedData);
      p.host_mapped = true;
      // 持久映射,不占 m_MapCount。
    } else {
      void* vma_mapped = nullptr;
      if (vmaMapMemory(m.vma, p.vma_allocation, &vma_mapped) == VK_SUCCESS) {
        p.host_ptr = static_cast<std::uint8_t*>(vma_mapped);
        p.host_mapped = vma_mapped != nullptr;
        if (vma_mapped != nullptr) p.vma_map_count = 1;
      }
    }
  }
  }

  m.pages.push_back(std::move(p));
  const u32 index = static_cast<u32>(m.pages.size() - 1);
  // 保持 "page_id == pages 下标" 的不变量(与 create_page 一致),句柄可预测。
  m.pages[index].id = index;
  m.stats.page_count = m.live_page_count();
  // 图像页不计入桶占用(bytes_in_use/bytes_reserved 只统计桶内子块)。
  ++m.stats.allocation_count;
  return ImagePageResult{m.pages[index].id};
}

device::Status VulkanPagingAllocator::bind_image_page(u32 page_id, std::uintptr_t image) {
  Impl& m = *impl_;
  Impl::Page* p = m.page_of(page_id);
  if (p == nullptr) return Status::Invalid("bind_image_page: 页不存在");
  if (!p->image_bound) return Status::Invalid("bind_image_page: 该页不是图像页");
  if (image == 0) return Status::Invalid("bind_image_page: image 句柄为空");
  if (!m.vma_ready || m.vma == VK_NULL_HANDLE) return Status::Ok_();  // 纯软件态无绑定

  const VkImage vk_image = reinterpret_cast<VkImage>(image);
  const VkResult r = vmaBindImageMemory(m.vma, p->vma_allocation, vk_image);
  if (r != VK_SUCCESS) return Status::InitFailed(fmt_err("vmaBindImageMemory", r));
  return Status::Ok_();
}

device::Status VulkanPagingAllocator::bind_buffer_page(u32 page_id, std::uintptr_t buffer) {
  Impl& m = *impl_;
  Impl::Page* p = m.page_of(page_id);
  if (p == nullptr) return Status::Invalid("bind_buffer_page: 页不存在");
  if (!p->image_bound) return Status::Invalid("bind_buffer_page: 该页不是专用页");
  if (buffer == 0) return Status::Invalid("bind_buffer_page: buffer 句柄为空");
  if (!m.vma_ready || m.vma == VK_NULL_HANDLE) {
    p->buffer_bound = true;  // 纯软件态无真实绑定
    return Status::Ok_();
  }

  const VkBuffer vk_buffer = reinterpret_cast<VkBuffer>(buffer);
  const VkResult r = vmaBindBufferMemory(m.vma, p->vma_allocation, vk_buffer);
  if (r != VK_SUCCESS) return Status::InitFailed(fmt_err("vmaBindBufferMemory", r));
  p->buffer_bound = true;
  return Status::Ok_();
}

device::Status VulkanPagingAllocator::release_image_page(u32 page_id) {
  Impl& m = *impl_;
  const Impl::Page* found = m.page_of(page_id);
  if (found == nullptr) return Status::Invalid("release_image_page: 页不存在");
  if (!found->image_bound) return Status::Invalid("release_image_page: 该页不是图像页");

  // 找到下标(保持 id == 下标的不变量)
  u32 index = 0;
  for (u32 i = 0; i < m.pages.size(); ++i) {
    if (m.pages[i].id == page_id) {
      index = i;
      break;
    }
  }
  Impl::Page& p = m.pages[index];
  if (m.vma_ready && m.vma != VK_NULL_HANDLE && p.vma_allocation != VK_NULL_HANDLE) {
    // 同 free_page_storage:按 vma_map_count 精确还清映射再 free。
    // 少还一次 → ~VmaAllocation_T 断言;多还一次 → BlockAllocUnmap 断言。
    while (p.vma_map_count > 0) {
      vmaUnmapMemory(m.vma, p.vma_allocation);
      --p.vma_map_count;
    }
    vmaFreeMemory(m.vma, p.vma_allocation);
    ++m.stats.vma_free_count;
  } else if (!m.vma_ready) {
    ++m.stats.vma_free_count;
  }
  p.vma_allocation = VK_NULL_HANDLE;
  p.host_storage.clear();
  p.host_storage.shrink_to_fit();
  p.host_ptr = nullptr;
  p.host_mapped = false;
  p.vma_map_count = 0;
  p.id = kNoPage;
  p.image_bound = false;
  p.buffer_bound = false;
  m.stats.page_count = m.live_page_count();
  ++m.stats.free_count;
  return Status::Ok_();
}

void* VulkanPagingAllocator::image_page_mapped_ptr(u32 page_id) const {
  const Impl& m = *impl_;
  const Impl::Page* p = m.page_of(page_id);
  if (p == nullptr || !p->image_bound) return nullptr;
  return p->host_ptr;
}

void* VulkanPagingAllocator::map_image_page(u32 page_id) {
  Impl& m = *impl_;
  Impl::Page* p = m.page_of(page_id);
  if (p == nullptr || !p->image_bound) return nullptr;
  if (p->host_ptr != nullptr) return p->host_ptr;  // 幂等
  if (p->page_kind != resources::PageKind::HostVisible) return nullptr;
  if (!m.vma_ready || m.vma == VK_NULL_HANDLE || p->vma_allocation == VK_NULL_HANDLE) {
    return nullptr;
  }
  void* mapped = nullptr;
  if (vmaMapMemory(m.vma, p->vma_allocation, &mapped) != VK_SUCCESS) return nullptr;
  p->host_ptr = static_cast<std::uint8_t*>(mapped);
  p->host_mapped = mapped != nullptr;
  return p->host_ptr;
}

void VulkanPagingAllocator::unmap_image_page(u32 page_id) {
  Impl& m = *impl_;
  Impl::Page* p = m.page_of(page_id);
  if (p == nullptr || !p->image_bound) return;
  if (p->host_ptr == nullptr) return;
  if (m.vma_ready && m.vma != VK_NULL_HANDLE && p->vma_allocation != VK_NULL_HANDLE) {
    // 同 unmap_page:持久映射不进 m_MapCount,计数为 0 时不要调 vmaUnmapMemory。
    if (p->vma_map_count > 0) {
      vmaUnmapMemory(m.vma, p->vma_allocation);
      --p->vma_map_count;
    }
  }
  p->host_ptr = nullptr;
  p->host_mapped = false;
}

u64 VulkanPagingAllocator::image_page_size(u32 page_id) const {
  const Impl& m = *impl_;
  const Impl::Page* p = m.page_of(page_id);
  return p == nullptr ? 0 : p->page_size;
}

// ============================================================================
// 桶页绑定指针(L1-C 的 VulkanBuffer / VulkanTexture 用)
// ============================================================================
//
// VMA 绑定态下页是一整块 VmaAllocation;这里把它的 VkDeviceMemory 与页首偏移
// 以 std::uintptr_t 报出去。**为什么不用 VmaAllocationInfo::offset**:那要求
// vmaGetAllocationInfo(不透明指针得再暴露一遍),而页内偏移实际上恒为 0
// ——页是 VMA 整页分配的、不与别的 VmaAllocation 共享 VkDeviceMemory。
// 纯软件态没有 VkDeviceMemory,直接返回 false(调用方据此走"无 Vk 对象"路径)。
bool VulkanPagingAllocator::page_memory_handle(u32 page_id, std::uintptr_t* out_memory,
                                               u64* out_offset) const {
  const Impl& m = *impl_;
  const Impl::Page* p = m.page_of(page_id);
  if (p == nullptr) return false;
  if (!m.vma_ready || m.vma == VK_NULL_HANDLE || p->vma_allocation == VK_NULL_HANDLE) {
    return false;
  }
  VmaAllocationInfo info{};
  vmaGetAllocationInfo(m.vma, p->vma_allocation, &info);
  if (info.deviceMemory == VK_NULL_HANDLE) return false;
  if (out_memory) {
    *out_memory = reinterpret_cast<std::uintptr_t>(info.deviceMemory);
  }
  if (out_offset) *out_offset = static_cast<u64>(info.offset);
  return true;
}

}  // namespace magicblocks::backend
