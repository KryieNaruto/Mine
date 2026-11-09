// MagicBlocks —— Vulkan 后端:L1 资源对象层共用的小工具
//
// 集中三件事,避免 buffer / texture / command 各写一份又互相漂移:
//   1. **页内存绑定**:VkBuffer / VkImage 的 VkDeviceMemory 只能来自分页分配器的
//      整页 VMA 分配(裁决 3 的"VMA 唯一注入点"要求那些 VMA 句柄不在本层泄露)。
//      分配器的头刻意不暴露任何 VmaAllocation,因此 binding 走一层"后端内部注册表":
//      分配器在 reserve_pages() 成功时把 {page_id → (VkDeviceMemory, offset)} 记进来,
//      本层的 bind_* 只查表。以偏移绑定页面内存,不需要 vkGet*MemoryRequirements2。
//   2. **单队列一次性提交(带 fence)**:R5 的 staging→GPU 拷贝、纹理上传/读回的
//      布局转换都走这里;替代 L0 的 vkQueueWaitIdle 语义,热路径只等自己的 fence。
//   3. **格式 ↔ 字节数 / 深度判定**:读回口径(行序自左上、RGBA8)依赖它。
//
// 前置条件:所有绑定前必须由调用方对相应桶调用过 reserve_pages();没有注册记录时
// bind_* 返回 InvalidArgument 并给出明确 message(不静默传 VK_NULL_HANDLE)。
#pragma once

#include <cstdint>
#include <mutex>
#include <string>

#include "backend/vulkan/vk_common.h"
#include "backend/vulkan/vulkan_target.h"  // Status 别名 + VulkanContext(与 L0 后端同口径)
#include "magicblocks/device/types.h"
#include "magicblocks/resources/handles.h"
#include "magicblocks/resources/memory.h"

namespace magicblocks::backend {

using Status = device::Status;
// VulkanContext / fmt_err 定义在 vulkan_target.h 与 vk_common.h。

// 外部小锁:注册表是进程级单例,这里给"同一进程多个设备"留一点余地。
// 单队列单线程(G5)下不会真争用,只是为了不让数据竞争成为未定义行为。
std::mutex& page_registry_mutex();

// ---------------------------------------------------------------- 页内存注册表

// 一页对应的宿主 VkDeviceMemory 与它在其中占的区间。
// offset 一般是 0(VMA 整页分配),保留区间信息是为了将来 VMA 子分配页仍能正确绑定。
struct PageBinding {
  VkDeviceMemory memory = VK_NULL_HANDLE;
  std::uint64_t offset = 0;  // 页首址在 memory 内的偏移
  std::uint64_t size = 0;    // 页字节数
};

// 登记一页(VulkanPagingAllocator::reserve_pages 成功后调用)。
void register_page_memory(std::uint32_t page_id, const PageBinding& binding);
// 从一个分配器查页的宿主内存并登记(推荐入口:自动判定分配器实现与 VMA 绑定态,
// 纯软件态 / 别的分配器实现时什么也不做 —— 视图层本来就不需要 Vk 绑定)。
void sync_page_memory(resources::IMemoryAllocator* allocator, std::uint32_t page_id);
// 注销一页(页退回 VMA 时调用)。
void forget_page_memory(std::uint32_t page_id);
// 查一页的绑定;未登记返回 false(不改变 out)。
bool page_binding(std::uint32_t page_id, PageBinding* out);
// 已登记的页数(单测观测"页注册表是否随页回收一起清空")。
std::uint32_t registered_page_count();
// 清空注册表(设备销毁时兜底;必须在所有页都销毁之后)。
void clear_page_memory();

// 把 VkBuffer 绑定到 id 所在页的 memory + 页内偏移(id.offset)。
// 页未登记 / 缓冲过界 → InvalidArgument,并释放调用方的 VkBuffer(失败不留半成品)。
Status bind_buffer_memory(VkDevice device, VkBuffer buffer, const resources::ResourceId& id);
// 同上,给图像用。
Status bind_image_memory(VkDevice device, VkImage image, const resources::ResourceId& id);

// ---------------------------------------------------------------- 一次性提交(G5 单队列)

// 录制一段命令并同步执行到底(内部 fence + wait)。fn 的返回值即整体结果;
// 返回非 Ok 时不执行命令缓冲(避免把半截录制提交上去)。
// 用 VulkanContext 的 cmd_pool(带 RESET_COMMAND_BUFFER_BIT),不另建命令池。
template <typename Fn>
Status submit_once_sync(const VulkanContext& ctx, const char* what, Fn&& fn) {
  if (ctx.device == VK_NULL_HANDLE || ctx.queue == VK_NULL_HANDLE) {
    return Status::InitFailed(std::string(what) + ": 设备未初始化");
  }
  VkCommandBufferAllocateInfo cai{};
  cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cai.commandPool = ctx.cmd_pool;
  cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cai.commandBufferCount = 1;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkResult r = vkAllocateCommandBuffers(ctx.device, &cai, &cmd);
  if (r != VK_SUCCESS) return Status::InitFailed(fmt_err(what, r));

  VkCommandBufferBeginInfo bi{};
  bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cmd, &bi);
  Status s = fn(cmd);
  vkEndCommandBuffer(cmd);

  if (s.ok()) {
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    r = vkCreateFence(ctx.device, &fci, nullptr, &fence);
    if (r != VK_SUCCESS) {
      s = Status::InitFailed(fmt_err(what, r));
    } else {
      VkSubmitInfo si{};
      si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
      si.commandBufferCount = 1;
      si.pCommandBuffers = &cmd;
      r = vkQueueSubmit(ctx.queue, 1, &si, fence);
      if (r == VK_SUCCESS) r = vkWaitForFences(ctx.device, 1, &fence, VK_TRUE, UINT64_MAX);
      vkDestroyFence(ctx.device, fence, nullptr);
      if (r != VK_SUCCESS) s = Status::InitFailed(fmt_err(what, r));
    }
  }
  vkFreeCommandBuffers(ctx.device, ctx.cmd_pool, 1, &cmd);
  return s;
}

// ---------------------------------------------------------------- 格式工具

// 公共 Format → VkFormat(与 vulkan_target.cpp 的 to_vk_format 同表,
// 这里只给 L1 资源用,避免为了一个函数改 L0 头的函数签名)。
VkFormat vk_format_of(device::Format f);

// 采样数(N) → VkSampleCountFlagBits。
//
// 起初它是 vulkan_pipeline.cpp 匿名 namespace 里的私有函数,MSAA 接线时
// vulkan_draw.cpp 的 VkPipelineMultisampleStateCreateInfo 也要用同一张表,
// 于是提到这里由多处共用 —— **全仓只此一份**,不要在任何 .cpp 里再复制。
// 未知/非法取值(0、3、非 2 的幂)返回 VK_SAMPLE_COUNT_1_BIT;需要"拒绝"
// 语义的调用方必须自己先校验(见 VulkanPipeline::validate)。
VkSampleCountFlagBits to_vk_samples(std::uint32_t n);

// 单像素字节数(RGBA8/BGRA8 = 4、R8 = 1、D16 = 2、D24S8 = 4、D32 = 4)。
// 未知格式返回 0(调用方必须当成 InvalidArgument)。
std::uint32_t format_pixel_bytes(device::Format f);

// 是否深度(或深度+模板)格式。L1 的 ITexture::readback_rgba8 对这类格式返回 Unsupported。
bool is_depth_format(device::Format f);

// 该格式能否按 RGBA8 口径读回(TransferDst / Sampled 的 RGBA8 与 BGRA8 都可以;
// R8 与深度格式不行)。读回时若需要通道重排会在 readback 里做一次显式转换。
bool can_readback_rgba8(device::Format f);

}  // namespace magicblocks::backend
