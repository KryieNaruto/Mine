// MagicBlocks —— Vulkan 后端:设备实现 + 工厂(spec L0 D1/D2/D7/D8)
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "backend/vulkan/vk_common.h"
#include "backend/vulkan/vulkan_allocator.h"
#include "backend/vulkan/vulkan_frame_sync.h"
#include "backend/vulkan/vulkan_instance.h"
#include "backend/vulkan/vulkan_pipeline.h"
#include "backend/vulkan/vulkan_target.h"
#include "magicblocks/device/render_device.h"
namespace magicblocks::backend {

// 前置声明：bind_known_buffers 只需要引用，不需要完整类型；
// 完整定义在 vulkan_command.h，设备实现文件里再 include。
class VulkanCommandList;

class VulkanRenderDevice final : public device::IRenderDevice {
 public:
  VulkanRenderDevice() = default;
  ~VulkanRenderDevice() override;

  // 两段式:先 init(会做能力探测与强筛),再使用。
  device::Status init(const device::DeviceDesc& desc, const std::string& device_override);

  device::BackendType backend() const override { return device::BackendType::Vulkan; }
  const device::DeviceCaps& caps() const override { return caps_; }

  device::Result<std::unique_ptr<device::IRenderTarget>> create_offscreen_target(
      const device::TargetDesc& desc) override;
  device::Result<std::unique_ptr<device::IRenderTarget>> create_window_target(
      const device::TargetDesc& desc) override;
  device::Result<std::unique_ptr<device::ISwapchain>> create_swapchain(
      const device::NativeSurface& surface, device::Extent2D extent) override;

  device::IFrameSync* frame_sync() override { return frame_sync_.get(); }

  // ---- L1 资源层 ----
  resources::IMemoryAllocator* memory_allocator() override { return allocator_.get(); }
  device::Result<std::unique_ptr<resources::IBuffer>> create_buffer(
      const resources::BufferDesc& desc) override;
  device::Result<std::unique_ptr<resources::ITexture>> create_texture(
      const resources::TextureDesc& desc) override;
  device::Result<std::unique_ptr<resources::ISampler>> create_sampler(
      const resources::SamplerDesc& desc) override;
  device::Result<std::unique_ptr<resources::IPipeline>> create_pipeline(
      const resources::PipelineDesc& desc) override;
  // 后端内部：按指定附件形状建管线。
  // 为什么需要：VkPipeline 绑定的 VkRenderPass 必须与 submit_device_frame
  // 用来 begin_pass 的 render pass **逐项兼容**（颜色格式/采样数/深度）。
  // 公共 create_pipeline(desc) 固定用 RGBA8 离屏形状，渲染到交换链
  // （BGRA8 或 RGBA8，由 surface 决定）时必须用这条入口。
  // 公共面(include/)不能出现 AttachmentShape，因为它带 VkFormat 语义，
  // 所以只在本后端头里暴露 —— 调用方是同目录的 src/android/。
  device::Result<std::unique_ptr<resources::IPipeline>> create_pipeline_for_shape(
      const resources::PipelineDesc& desc, const AttachmentShape& shape);
  device::Result<std::unique_ptr<resources::ICommandList>> create_command_list() override;

  // L0→L1:把已录制完成的命令列表真正编码成 vkCmd* 并提交。
  // 内部转调 submit_device_frame()(那条通路早就实现完整,只是从来没接到
  // 公开接口上,导致 L2/L3 录完命令后无法出图)。
  device::Status submit_command_list(resources::ICommandList& list) override;

  device::Status notify_lifecycle(const device::AppLifecycleEvent& ev) override;
  device::Status wait_idle() override;

  // 测试/CLI 用:直接拿到 context 与帧同步的具体类型
  const VulkanContext& context() const { return ctx_; }
  VulkanFrameSync* frame_sync_impl() { return frame_sync_.get(); }
  // L1:分页分配器(公共面通过 IRenderDevice::memory_allocator() 取接口指针)
  VulkanPagingAllocator* allocator_impl() { return allocator_.get(); }
  // 校验层的可计量判据(Device.CreateAndDestroy / RunsFifteenFrames)
  std::uint32_t validation_error_count() const { return inst_.error_count; }
  std::uint32_t validation_message_count() const { return inst_.message_count; }

 private:
  // 设备创建的缓冲资源句柄 → 实现指针。
  //
  // 为什么需要:公共面 resources::ICommandList::bind_vertex_buffer 只收
  // ResourceId(值语义,M3,这是正确的公共设计),命令列表自己拿不到 IBuffer*;
  // 而设备帧提交必须拿到 VkBuffer 才能 vkCmdBindVertexBuffers。这张表就是
  // 两者之间缺失的那座桥 —— 提交前按命令里的句柄把指针补进命令列表。
  void bind_known_buffers(VulkanCommandList& list) const;

  InstanceBundle inst_{};
  VulkanContext ctx_{};
  device::DeviceCaps caps_{};
  // L1 分页分配器。必须**晚于** ctx_ 构造、**早于** VkDevice 销毁:
  // 它在析构时会 vmaDestroyAllocator,而 VMA 持有 VkDevice。
  std::unique_ptr<VulkanPagingAllocator> allocator_;
  // 注意:frame_sync_ 必须**晚于**每个帧槽对应的 VkDevice 之后再析构,
  // 所以这里用 unique_ptr 并在 ~VulkanRenderDevice 里显式先 reset,
  // 保证 "先销毁 fence、再销毁 VkDevice" 的顺序。
  std::unique_ptr<VulkanFrameSync> frame_sync_;
  bool logical_ready_ = false;
  // ResourceId → 缓冲实现指针(见 bind_known_buffers 的说明)。
  // 设备不拥有这些对象(所有权在调用方的 unique_ptr),这里只存裸指针;
  // 缓冲析构时通过 WeakBuffer 回调把自己摘掉,避免悬垂。
  std::vector<std::pair<resources::ResourceId, resources::IBuffer*>> known_buffers_;
};

// 测试钩子:强制把枚举到的设备"假装"改名(用于 RefusesPhysicalGpu)。
// 空串 = 不覆盖。仅影响强筛判定,不改变真实设备。
void set_physical_device_override(const std::string& name);
const std::string& physical_device_override();

// 工厂:由 core/device_factory.cpp 调用(公共面不出现 Vk*)。
device::Result<std::unique_ptr<device::IRenderDevice>> create_vulkan_device(
    const device::DeviceDesc& desc);

}  // namespace magicblocks::backend
