// MagicBlocks —— Vulkan 后端:交换链(D3)
//
// 平台差异只在 surface 创建处:Win32 / Xcb 各一个 TU,公共面零平台宏。
#pragma once

#include <memory>
#include <vector>

#include "backend/vulkan/vk_common.h"
#include "backend/vulkan/vulkan_target.h"
#include "magicblocks/device/swapchain.h"

namespace magicblocks::backend {

// 不透明句柄探针:让 UI 层(`src/ui/`)能在**不认识 Vk*** 的前提下问到
// "当前 swapchain image 的句柄值"。句柄值只被当成不透明整数转发给后端内部,
// G3 分层因此保持干净(G3 只看源码里有没有 Vk 字样,这里只是一个 uint64)。
class SwapchainImageProbe {
 public:
  virtual ~SwapchainImageProbe() = default;
  virtual std::uint64_t current_image_value() const = 0;
};

class VulkanSwapchain final : public device::ISwapchain, public SwapchainImageProbe {
 public:
  VulkanSwapchain(const VulkanContext& ctx, const device::NativeSurface& surface);
  ~VulkanSwapchain() override;

  // 建 surface + swapchain。失败返回 Unsupported/InitFailed,不静默降级。
  device::Status init(device::Extent2D extent);

  device::Extent2D extent() const override { return extent_; }
  device::Format format() const override { return format_; }
  device::PresentMode present_mode() const override { return present_mode_; }
  std::vector<device::PresentMode> supported_present_modes() const override { return modes_; }

  device::Status rebuild(device::Extent2D new_extent) override;
  device::IRenderTarget* current_target() override;
  // 当前颜色附件的 Vulkan 句柄(交给 UI 层的帧渲染桩做离屏/上屏录制用)。
  // **backend 私有**:公共面看不到它,G3 分层不受影响。
  VkImage current_image() const {
    return current_index_ < images_.size() ? images_[current_index_] : VK_NULL_HANDLE;
  }
  std::uint32_t current_index() const { return current_index_; }
  std::uint64_t current_image_value() const override {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(current_image()));
  }

  // 内部:surface 是否已失效(lifecycle SurfaceDestroyed 后为 true)
  bool surface_alive() const { return surface_ != VK_NULL_HANDLE; }

  // 该 swapchain 的 preTransform（VkSurfaceTransformFlagBitsKHR 位值）。
  // **backend 私有**：只有 Android 渲染层用它补偿屏幕旋转造成的
  // 宽高比失真与手势方向错位。0 表示没取到（视同 IDENTITY）。
  std::uint32_t pre_transform() const { return pre_transform_; }

  // 上屏三件事(见 device/swapchain.h)
  device::ISwapchain::ImageResult acquire_next_image() override;
  device::ISwapchain::ImageStatus present_current() override;

 private:
  device::Status destroy_swapchain();
  device::Status create_swapchain_objects(device::Extent2D extent);

  VulkanContext ctx_;
  device::NativeSurface native_{};
  VkSurfaceKHR surface_ = VK_NULL_HANDLE;
  VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
  std::vector<VkImage> images_;
  std::uint32_t current_index_ = 0;
  device::Extent2D extent_{};
  device::Format format_ = device::Format::BGRA8_UNORM;
  device::PresentMode present_mode_ = device::PresentMode::Fifo;
  std::vector<device::PresentMode> modes_;
  // 每张 image 包一个只读 IRenderTarget 视图(不拥有图像)
  std::vector<std::unique_ptr<device::IRenderTarget>> views_;
  // acquire 用 fence:保证覆盖写之前 GPU 已经用完这张图。
  VkFence acquire_fence_ = VK_NULL_HANDLE;
  bool surface_lost_ = false;
  std::uint32_t pre_transform_ = 0;
};

}  // namespace magicblocks::backend
