// MagicBlocks —— Vulkan 后端:L1 缓冲资源(R1 / R5)
//
// spec R1 / R5 / §6.3 的落地(plans-l1 §3, rulings 裁定 3):
//   - **句柄 = 偏移(M3)**:VulkanBuffer 持有一个 ResourceId,它是
//     VulkanPagingAllocator 从桶内空闲链表切出来的子块,**不单独走 VMA 分配**。
//     命令流记录这个 ResourceId(而不是 VkBuffer / 指针),于是回放时同一
//     page_id/offset 指向同一片内存。
//   - **CPU 写永远走映射,不用 vkMapMemory(G4)**:Host 页 MAPPED 常驻,
//     映射首址由分配器 allocate() 的 AllocResult.mapped 给出;设备页
//     (Vertex/Index/Uniform)CPU 不可见 → mapped() == nullptr,write() 返回
//     Unsupported。要往 GPU 侧写数据请用 Staging 用途 + upload()(R5)。
//   - **R5 上传队列**:upload() 从 MemoryKind::Staging 桶切一块暂存子块,
//     把数据 memcpy 进去后记进 pending 列表,等 flush_pending()(帧边界)统一
//     交给 GPU 副本;暂存缓冲本身也走分页分配器 —— 这就是
//     Upload.StagingGoesThroughAllocator 的判定依据。
//   - **销毁即归还子块(M4)**:析构只调 allocator.free(),不立即退 VMA。
//
// ---------------------------------------------------------------- 后端形态
// VkBuffer 需要 VkDevice 才能建:给 ctx(见 vulkan_render_device.h)即进入设备态
// (真实 VkBuffer + vkBindBufferMemory 绑定到该页的 VmaAllocation 的 offset);
// 不给 ctx 则进入纯软件态,用于无 Vulkan 设备的 ctest —— 句柄、桶、映射、上传
// 队列语义完全一致,只是没有 GPU 对象。
//
// 前置条件:页必须由本对象的 IMemoryAllocator 预取(reserve_pages);allocate()
// 在桶里没有空位时按 M2/M4 的"大块优先"原则返回 OutOfMemory,**不会**自己触发
// VMA 分配(那正是 G4 要杜绝的单资源裸分配路径)。
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "backend/vulkan/vk_common.h"
#include "backend/vulkan/vulkan_allocator.h"
#include "magicblocks/device/result.h"
#include "magicblocks/resources/buffer.h"
#include "magicblocks/resources/memory.h"
// VulkanContext / backend::Status 别名来自 L0 的后端公共头(必须在 resources 头之后,
// 这样 backend::Status 才不会抢在 device::Status 的声明之前被当成类型使用)。
#include "backend/vulkan/vulkan_target.h"

namespace magicblocks::backend {

using resources::BufferDesc;
using resources::BufferUsage;
using resources::IBuffer;
using resources::MemoryKind;
using resources::ResourceId;
using resources::u32;
using resources::u64;

class VulkanBuffer final : public IBuffer {
 public:
  // 纯软件态:用父分配器已预取的页,不建 VkBuffer。
  VulkanBuffer(resources::IMemoryAllocator* allocator, const BufferDesc& desc);
  // 设备态:额外建真实 VkBuffer 并绑定到子块所在页的 VkDeviceMemory + offset。
  VulkanBuffer(const VulkanContext& ctx, resources::IMemoryAllocator* allocator,
               const BufferDesc& desc);

  ~VulkanBuffer() override;

  VulkanBuffer(const VulkanBuffer&) = delete;
  VulkanBuffer& operator=(const VulkanBuffer&) = delete;

  // 两段式创建(与 OffscreenTarget::init() 同口径):构造只存参数,init() 报错。
  device::Status init();

  // ------------------------------------------------------------ IBuffer(R1)
  ResourceId id() const override { return id_; }
  const BufferDesc& desc() const override { return desc_; }
  u64 size() const override { return id_.size; }
  MemoryKind allocation_kind() const override { return kind_; }

  void* mapped() override { return mapped_; }
  const void* mapped() const override { return mapped_; }

  device::Status write(const void* data, u64 offset, u64 size) override;
  device::Status read(void* out, u64 offset, u64 size) const override;

  // R5:数据先进 Staging 桶的暂存子块,由 flush_pending() 统一提交。
  device::Status upload(const void* data, u64 size, u64 dst_offset = 0) override;
  void flush_pending() override;
  u64 pending_upload_bytes() const override { return pending_bytes_; }

  // ------------------------------------------------------------ 后端查询(gtest 用)
  // 是否建出了真实 VkBuffer(纯软件态恒 false)。
  bool has_vk_buffer() const { return vk_buffer_ != VK_NULL_HANDLE; }
  VkBuffer vk_buffer() const { return vk_buffer_; }
  u64 vk_buffer_offset() const { return vk_offset_; }

  // 本缓冲所在页号(== id().page_id 的便捷写法,断言"同页"用)。
  u32 page_id() const { return id_.page_id; }

  // R5 上传队列的可观测面:累积入队的上传次数 / 字节数(单调累计)。
  u64 upload_begin_count() const { return upload_begin_count_; }
  u64 upload_bytes_total() const { return upload_bytes_total_; }
  // 已提交(flush 并成功拷进设备态缓冲)的上传字节数。
  u64 uploaded_bytes_total() const { return uploaded_bytes_total_; }
  // 尚未提交的上传条数(每条 upload() 一笔)。
  u32 pending_upload_count() const { return static_cast<u32>(pending_.size()); }
  // 最近一次 flush_pending() 里第一条提交失败的原因;空串 = 全部成功。
  const std::string& upload_commit_error() const { return upload_commit_error_; }
  // 某条待提交上传的暂存子块句柄(index 越界返回默认构造的空句柄)。
  ResourceId pending_staging_id(u32 index) const;
  // 暂存子块从哪个桶切出来的(恒 MemoryKind::Staging;R5 的断言点)。
  MemoryKind pending_staging_kind() const { return MemoryKind::Staging; }

 private:
  struct PendingUpload {
    ResourceId staging{};  // 暂存子块句柄(Staging 桶)
    u64 dst_offset = 0;    // 目标缓冲内的写入偏移
    u64 size = 0;          // 字节数
  };

  device::Status allocate_block();
  // 把一条待提交上传真正落到目标缓冲:设备态走 vkCmdCopyBuffer,软件态走 memcpy。
  device::Status commit_upload(const PendingUpload& up);

  VulkanContext ctx_{};
  bool has_ctx_ = false;
  resources::IMemoryAllocator* allocator_ = nullptr;

  BufferDesc desc_{};
  MemoryKind kind_ = MemoryKind::Vertex;
  ResourceId id_{};
  void* mapped_ = nullptr;

  VkBuffer vk_buffer_ = VK_NULL_HANDLE;
  u64 vk_offset_ = 0;  // 子块在所属页 VkDeviceMemory 内的偏移(不用 vkBindBufferMemory2)

  std::vector<PendingUpload> pending_;
  std::string upload_commit_error_;  // 最近一次 flush 里第一条失败原因(空 = 全成功)
  u64 pending_bytes_ = 0;
  u64 upload_begin_count_ = 0;
  u64 upload_bytes_total_ = 0;
  u64 uploaded_bytes_total_ = 0;
};

}  // namespace magicblocks::backend
