// MagicBlocks —— Vulkan 后端:L1 分页内存分配器(VMA v3.5.0 + 桶内空闲链表)
//
// spec §6 全条文的落地(plans-l1 §4):
//   - M2 分页 + 偏移子分配:设备页 64 MB(DEVICE_LOCAL)、Host 页 8 MB
//     (HOST_VISIBLE|HOST_COHERENT 且 MAPPED 常驻)。页内再按 MemoryKind 分桶,
//     桶内空闲链表 first-fit 切出 {offset, size} 给多个小资源共用一页。
//   - M4 稳定驻留:free() 只把子块还回本页(O(1) 相邻合并),**不**立即退 VMA;
//     页空置超过 page_idle_frames() 帧且无在途帧引用才整页退回 → 热路径 0 次 VMA 调用。
//   - M5 计数门槛:stats().vma_alloc_count / vma_free_count **只**在真正调用
//     vmaAllocateMemory* / vmaFreeMemory* 时 +1,其余路径全是软件计数。
//   - M6 预算与淘汰:超预算 → OutOfMemory,message 带「需要 / 已有 / 预算」三个数;
//     淘汰只做整页 LRU,不做页内碎片整理。
//
// ---------------------------------------------------------------- G4 铁律
// 本头**绝不** include <vk_mem_alloc.h>,也**不**暴露 VmaAllocator / VmaAllocation /
// VkDevice 等任何 Vulkan 类型:`vk_mem_alloc.h` 与 #define VMA_IMPLEMENTATION
// 只允许出现在 vulkan_allocator.cpp 这一个 TU(tools/check_no_raw_alloc.py 断言)。
// VMA 对象全部藏在 .cpp 里的 struct Impl(pimpl):本头只见 std::unique_ptr<Impl>。
//
// ---------------------------------------------------------------- 对齐铁律(rulings 裁定 3)
// 每个成功返回的 ResourceId 都满足 offset % 65536 == 0(= ResourceId::kPackGranularity):
// allocate() 把实际对齐取成 max(调用方 align, kPackGranularity),页也按 64 KiB 粒度
// 切成槽位。这是命令流 pack() 能成功的前提 —— 放松对齐会让 pack() 在运行期静默失败。
//
// ---------------------------------------------------------------- 线程模型
// spec G5:单队列单线程。本类**不做**内部加锁,跨线程调用属未定义行为。
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "mb_internal/magicblocks/resources/handles.h"
#include "mb_internal/magicblocks/resources/memory.h"

namespace magicblocks::backend {

using resources::MemoryKind;
using resources::PageDesc;
using resources::ResourceId;
using resources::u32;
using resources::u64;

// 子块的一次只读快照(单测查"同页 / 不重叠 / 合并效果"用)。
struct SubAllocInfo {
  ResourceId id{};                    // 真实的逻辑句柄(page_id / offset / size / id)
  MemoryKind kind = MemoryKind::Vertex;  // 所在桶
  bool alive = false;                 // false = 仍在空闲链表里(可复用)
};

// 桶内空闲块(快照,按 offset 升序)。
struct FreeBlockInfo {
  u64 offset = 0;
  u64 size = 0;
};

// 分配器构造参数。全部有默认值,所以 gtest 里可以 VulkanPagingAllocator allocator; 直接建。
struct AllocatorConfig {
  // 页尺寸(spec §6.1 / M2)。两个默认值都必须是 64 KiB 的整数倍 ——
  // 否则页内 64 KiB 粒子化的槽位切不满(构造函数会以 InvalidArgument 拒绝)。
  u64 device_page_size = 64ull * 1024 * 1024;  // 64 MB
  u64 host_page_size = 8ull * 1024 * 1024;     // 8 MB
  // M6 显存预算:0 = 未提供预算(不做预算检查,与 DeviceCaps.memory_budget == 0 同义)。
  // 现场请用 set_memory_budget(caps.memory_budget) 覆盖。
  u64 memory_budget = 0;
  // M4 页回收阈值:页空置满这么多帧才退回 VMA。默认 120(spec §6.2 第 4 条)。
  u64 page_idle_frames = 120;
  // 是否把 vmaSetCurrentFrameIndex 推给 VMA(spec §6.2 第 5 条)。
  // 无 VMA 的纯软件模式(单测)下自动失效:begin_frame 只推进页空置计数。
  bool propagate_frame_index_to_vma = true;
  // 初始预取页数:构造时用 reserve_pages 一次性把首组大页要下来(spec §6.2 第 1 条
  // 「进程启动时按预算预留首组大页」)。0 = 不预取。
  u32 initial_pages_per_kind = 0;
};

// 显式句柄形态:给出三个 Vulkan 句柄即进入 VMA 绑定态。
// 用 std::uintptr_t 承载是为了让本头不必 include vk_common.h(那会把 <vulkan/vulkan.h>
// 拖进来)—— 本头对 Vulkan 的唯一可见性是「整数句柄」,Vk* 类型解析只发生在 .cpp。
// 与 VulkanContext 的对应关系:instance = ctx.instance / physical_device = ctx.phys /
// device = ctx.device。用 allocator_handles_of(ctx)(见 vulkan_allocator_bind.h)转换。
struct AllocatorHandles {
  std::uintptr_t instance = 0;         // vkInstance
  std::uintptr_t physical_device = 0;  // vkPhysicalDevice(可为 0:VMA 只用它查扩展)
  std::uintptr_t device = 0;           // vkDevice(VMA 绑定态必须非 0)
};

// VulkanPagingAllocator = IMemoryAllocator 的唯一实现,也是**全仓唯一**持有
// VmaAllocator 的对象(在 .cpp 的 Impl 里)。
//
// 两种形态:
//   1. **VMA 绑定态**(生产):VulkanPagingAllocator allocator(ctx) 或 (Handles),
//      分配器只管「页从 VMA 来、页还回 VMA」。
//   2. **纯软件态**(单测):VulkanPagingAllocator allocator; 无 Vulkan 句柄,
//      页是 std::vector 模拟的一整页,页的 alloc/free **同样计入** stats 的
//      vma_alloc_count / vma_free_count —— 因为这两条计数是 M5 的硬断言对象,
//      必须能在没有 Vulkan 设备的 ctest 里被差分测量。区别只在:真实设备内存不存在。
class VulkanPagingAllocator final : public resources::IMemoryAllocator {
 public:
  // 纯软件态(无 VMA)。
  VulkanPagingAllocator();
  explicit VulkanPagingAllocator(const AllocatorConfig& config);

  // VMA 绑定态。两段式是为了与既有后端写法一致:构造不失败,失败在 init() 里报 Status。
  explicit VulkanPagingAllocator(const AllocatorHandles& handles);
  VulkanPagingAllocator(const AllocatorHandles& handles, const AllocatorConfig& config);

  ~VulkanPagingAllocator() override;

  VulkanPagingAllocator(const VulkanPagingAllocator&) = delete;
  VulkanPagingAllocator& operator=(const VulkanPagingAllocator&) = delete;

  // 校验配置 + 建 vmaCreateAllocator + 首组预取。无句柄时是纯软件态,只校验配置。
  // 失败:**硬失败,不做降级** —— 分配器拿不到 VMA 时不会静默退化成
  // "每个资源一次 vkAllocateMemory"(那正是 spec G4 要杜绝的路径)。
  device::Status init();

  // ------------------------------------------------------------ IMemoryAllocator
  device::Result<resources::AllocResult> allocate(MemoryKind kind, u64 size, u64 align) override;
  device::Status free(const ResourceId& id) override;
  void* mapped_ptr(const ResourceId& id, u64 offset = 0) const override;
  void* map_page(u32 page_id) override;
  device::Status unmap_page(u32 page_id) override;
  device::Status reserve_pages(MemoryKind kind, u32 count) override;
  u32 page_count(MemoryKind kind) const override;
  u64 page_size(MemoryKind kind) const override;
  bool page_desc(u32 page_id, PageDesc* out) const override;
  void begin_frame(u64 frame_index) override;
  void set_page_in_flight(u32 page_id, bool in_flight) override;
  u32 page_idle_frames() const override;
  device::Status trim() override;
  resources::AllocatorStats stats() const override;
  void reset_counters() override;
  u64 memory_budget() const override;
  void set_memory_budget(u64 bytes) override;
  device::Status validate() const override;
  u32 require_budget_check() const override;

  // ------------------------------------------------------------ 单测/后端查询(gtest 用)
  // 全部只读,不改变分配逻辑状态(除了明确标注诊断开关的两个 setter)。

  // VMA 绑定态是否建成功(纯软件态恒 false)。
  bool has_vma() const;
  // 当前是否还在给 VMA 推帧号。
  bool vma_frame_index_enabled() const;

  // 桶内空闲块数量(M4 合并断言:释放中间块后该数应减少)。
  u32 bucket_free_block_count(MemoryKind kind) const;
  // 桶内最大空闲块字节数(first-fit 只看这个 + 游标)。
  u64 largest_free_block(MemoryKind kind) const;
  // 桶内空闲链表快照(按 offset 升序)。out 为空指针时只返回节点数。
  u32 free_extents(MemoryKind kind, std::vector<FreeBlockInfo>* out) const;
  // 桶内全部子块快照(alive=true 是活子块,false 是空闲块),按 offset 升序。
  u32 listed_slots(MemoryKind kind, std::vector<SubAllocInfo>* out) const;

  // 全部页数(跨桶)与按索引取页快照。
  u32 page_total() const;
  bool page_desc_at(u32 index, PageDesc* out) const;
  // 页在哪个桶(页不存在返回 false,kind 保持不变)。
  bool page_kind(u32 page_id, MemoryKind* kind) const;
  // 页内 64 KiB 槽位总数与已占用槽位数(碎片率观测用)。
  u32 page_slot_capacity(u32 page_id) const;
  u32 page_slots_used(u32 page_id) const;

  // 诊断开关:关闭后 begin_frame 不再调 vmaSetCurrentFrameIndex。
  // 仅用于单测"无 VMA 时 begin_frame 仍然推进页空置计数"这条路径。
  void set_vma_frame_index_enabled(bool enabled);

  // 下一个要发的逻辑槽位号(单调递增、不复用,M3 的 id 语义)。
  u64 next_logical_id() const;

  // ------------------------------------------------------------ 图像绑定(G4:L0 裸分配的替代)
  // L0 的 vulkan_target.cpp 原本对颜色/深度附件与读回缓冲直接调 vkAllocateMemory。
  // L1 之后**全仓零裸分配**(G4),那些位置必须由分配器出内存,但图像还有两个额外要求:
  //   1. 图像必须由**一张独立的 VkDeviceMemory 块**承载(不能与别的资源共享同一块),
  //      因为 VkImage 只能整块绑定到 allocation 的 offset 0 —— 因此图像走"专用整页",
  //      它仍是 VMA 分配(计入 vma_alloc_count),只是不经桶内空闲链表切分。
  //   2. 绑定时需要 image 的 VkMemoryRequirements 来挑内存类型,这里用不透明参数传入,
  //      保证本头不出现 Vk* 类型(G3)。
  //
  // 语义:分配一块满足 req 的整页(float/devicemem),返回其 page_id;调用方再用
  // bind_image_page(page_id, image) 把它绑上去,并在销毁时 release_image_page(page_id)。
  // 这些页**不参与**桶内复用与 LRU 淘汰(附件生命周期由创建者显式管理)。
  struct ImagePageRequest {
    u64 size = 0;             // VkMemoryRequirements::size
    u64 alignment = 0;        // VkMemoryRequirements::alignment
    u32 memory_type_bits = 0;  // VkMemoryRequirements::memoryTypeBits
    bool device_local = true;  // true = DEVICE_LOCAL;false = HOST_VISIBLE|COHERENT(读回缓冲)
  };
  struct ImagePageResult {
    u32 page_id = ResourceId::kInvalidPage;
    bool valid() const { return page_id != ResourceId::kInvalidPage; }
  };
  device::Result<ImagePageResult> allocate_image_page(const ImagePageRequest& req);
  // 把 image 绑到该页的 VkDeviceMemory 上(offset 0)。失败返回 InitFailed 并保留页。
  device::Status bind_image_page(u32 page_id, std::uintptr_t image);
  // 把 buffer 绑到该页的 VkDeviceMemory 上(offset 0)。
  // 读回这类"整块绑定的 VkBuffer"必须走这里:vmaBindBufferMemory 与
  // vmaBindImageMemory 对句柄类型的要求不同,混用会让驱动拿到非法 VkImage 而崩。
  device::Status bind_buffer_page(u32 page_id, std::uintptr_t buffer);
  // 销毁图像后调用:把该页退回 VMA(计入 vma_free_count)。
  device::Status release_image_page(u32 page_id);
  // 该页的 Host 映射首址(读回缓冲用);设备页 / 未映射返回 nullptr。
  void* image_page_mapped_ptr(u32 page_id) const;
  u64 image_page_size(u32 page_id) const;
  // ---- 需要 CPU 侧写入的图像页(纹理上传 / 立方体贴图六面)----
  //
  // allocate_image_page 的 device_local=false 分支会返回一张 **未映射** 的
  // HOST_VISIBLE 页(host_ptr 为空):VMA 只在 VMA_ALLOCATION_CREATE_MAPPED_BIT
  // 时才预映射。图像页的既定用法是"整块交给 VkImage 直接采样",没人写过;
  // 而立方体贴图必须从 CPU 侧灌像素,所以这里补一对显式 map/unmap:
  //   - map_page 幂等:已映射则直接返回同一地址;非 HostVisible 页返回 nullptr;
  //   - unmap_page 与它成对,未映射时无操作(不算错误)。
  void* map_image_page(u32 page_id);
  void unmap_image_page(u32 page_id);

  // ------------------------------------------------------------ 桶页绑定指针(L1-C 资源对象用)
  // 桶内切出来的子块(VulkanBuffer / VulkanTexture)由 VkBuffer / VkImage 的
  // vkBind*Memory(..., memory, offset) **按偏移**绑定到所在页的整块 VkDeviceMemory 上。
  // 分配器头刻意不暴露 VmaAllocation(否则 vk_mem_alloc.h 的可见性会越过本 TU),
  // 于是这里给后端留一条"不透明句柄"通道:
  //   - 出参是 {页的 VkDeviceMemory(std::uintptr_t), 页首在该内存内的偏移};
  //   - 纯软件态 / 页不存在 / 未绑定 VMA 时统一返回 false(不抛、不崩);
  //   - 只有 src/backend/vulkan/vulkan_resource_common.cpp 才把它解释成 Vk* 类型。
  // 这不是公共面(include/magicblocks/** 的 IMemoryAllocator 里没有它),
  // 也不产生新的分配路径 —— 只是把"页已经有的那块内存"报给绑定层。
  bool page_memory_handle(u32 page_id, std::uintptr_t* out_memory, u64* out_offset) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace magicblocks::backend
