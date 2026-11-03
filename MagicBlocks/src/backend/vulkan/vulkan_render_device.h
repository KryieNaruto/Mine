// MagicBlocks —— Vulkan 后端:设备实现 + 工厂(spec L0 D1/D2/D7/D8)
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "backend/vulkan/vk_common.h"
#include "backend/vulkan/vulkan_allocator.h"
#include "backend/vulkan/vulkan_frame_sync.h"
#include "backend/vulkan/vulkan_instance.h"
#include "backend/vulkan/vulkan_target.h"
#include "magicblocks/device/render_device.h"
namespace magicblocks::backend {

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
  device::Result<std::unique_ptr<resources::ICommandList>> create_command_list() override;

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
};

// 测试钩子:强制把枚举到的设备"假装"改名(用于 RefusesPhysicalGpu)。
// 空串 = 不覆盖。仅影响强筛判定,不改变真实设备。
void set_physical_device_override(const std::string& name);
const std::string& physical_device_override();

// 工厂:由 core/device_factory.cpp 调用(公共面不出现 Vk*)。
device::Result<std::unique_ptr<device::IRenderDevice>> create_vulkan_device(
    const device::DeviceDesc& desc);

}  // namespace magicblocks::backend
