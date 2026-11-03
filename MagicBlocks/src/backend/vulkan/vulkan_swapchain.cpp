// MagicBlocks —— Vulkan 后端:交换链实现
#include "backend/vulkan/vulkan_swapchain.h"

#include <algorithm>

#include "backend/vulkan/vulkan_swapchain_android.h"
#include "backend/vulkan/vulkan_swapchain_win32.h"
#include "backend/vulkan/vulkan_swapchain_xcb.h"

namespace magicblocks::backend {

using namespace magicblocks::device;

namespace {

// surface 图像只读视图:swapchain 拥有图像,这里只描述大小/格式。
class SwapchainImageView final : public IRenderTarget {
 public:
  explicit SwapchainImageView(Extent2D e) : extent_(e) {}
  TargetDesc::Kind kind() const override { return TargetDesc::Kind::Window; }
  Extent2D extent() const override { return extent_; }
  Format color_format() const override { return Format::BGRA8_UNORM; }
  u32 samples() const override { return 1; }
  AttachmentView color_attachment() const override {
    return AttachmentView{extent_, Format::BGRA8_UNORM, true};
  }
  AttachmentView depth_attachment() const override { return AttachmentView{}; }
  Status readback_rgba8(std::uint8_t*, std::size_t) const override {
    return Status::Unsupported("swapchain 图像不支持 CPU 读回;离屏渲染请用 create_offscreen_target");
  }

 private:
  Extent2D extent_;
};

}  // namespace

VulkanSwapchain::VulkanSwapchain(const VulkanContext& ctx, const NativeSurface& surface)
    : ctx_(ctx), native_(surface) {}

VulkanSwapchain::~VulkanSwapchain() {
  views_.clear();
  if (swapchain_) vkDestroySwapchainKHR(ctx_.device, swapchain_, nullptr);
  if (surface_) vkDestroySurfaceKHR(ctx_.instance, surface_, nullptr);
}

Status VulkanSwapchain::init(Extent2D extent) {
  if (native_.kind == NativeSurface::Kind::Headless) {
    return Status::Unsupported("swapchain: Headless surface 不创建交换链,请用离屏渲染目标");
  }

  // 平台差异只在 surface 创建处:各平台一个 TU,公共面零平台宏。
  // 注意:三个 create_*_surface 在本平台之外也总是有定义(只返回 Unsupported),
  // 这样 init() 不必用 #if 分支,链接也不会因缺符号失败。
  Status s = Status::Unsupported("swapchain: 不支持该 surface kind");
  switch (native_.kind) {
    case NativeSurface::Kind::Win32:
      s = create_win32_surface(ctx_, native_.a, native_.b, &surface_);
      break;
    case NativeSurface::Kind::Xcb:
      s = create_xcb_surface(ctx_, native_.a, native_.b, &surface_);
      break;
    case NativeSurface::Kind::Android:
      s = create_android_surface(ctx_, native_.kind, native_.a, &surface_);
      break;
    case NativeSurface::Kind::Wayland:
    case NativeSurface::Kind::Darwin:
      return Status::Unsupported("swapchain: 本构建未编译 Wayland/Darwin surface");
    case NativeSurface::Kind::Headless:
      break;  // 上面已返回
  }
  if (!s.ok()) return s;

  return create_swapchain_objects(extent);
}

Status VulkanSwapchain::create_swapchain_objects(Extent2D extent) {
  VkSurfaceCapabilitiesKHR caps{};
  VkResult r = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx_.phys, surface_, &caps);
  if (r != VK_SUCCESS) return Status::InitFailed(fmt_err("vkGetPhysicalDeviceSurfaceCapabilitiesKHR", r));

  // present modes
  std::uint32_t n = 0;
  vkGetPhysicalDeviceSurfacePresentModesKHR(ctx_.phys, surface_, &n, nullptr);
  std::vector<VkPresentModeKHR> pm(n);
  if (n) vkGetPhysicalDeviceSurfacePresentModesKHR(ctx_.phys, surface_, &n, pm.data());
  modes_.clear();
  bool has_mailbox = false;
  bool has_immediate = false;
  for (auto m : pm) {
    if (m == VK_PRESENT_MODE_MAILBOX_KHR) has_mailbox = true;
    if (m == VK_PRESENT_MODE_IMMEDIATE_KHR) has_immediate = true;
  }
  modes_.push_back(PresentMode::Fifo);  // FIFO 恒可用
  if (has_mailbox) modes_.push_back(PresentMode::Mailbox);
  if (has_immediate) modes_.push_back(PresentMode::Immediate);
  present_mode_ = PresentMode::Fifo;  // D3:默认 FIFO

  // extent:FIFO 下若 caps 给出确切值则以它为准
  if (caps.currentExtent.width != UINT32_MAX) {
    extent_ = {caps.currentExtent.width, caps.currentExtent.height};
  } else {
    extent_.width = std::clamp(extent.width, caps.minImageExtent.width, caps.maxImageExtent.width);
    extent_.height = std::clamp(extent.height, caps.minImageExtent.height, caps.maxImageExtent.height);
  }

  // format
  std::uint32_t fn = 0;
  vkGetPhysicalDeviceSurfaceFormatsKHR(ctx_.phys, surface_, &fn, nullptr);
  std::vector<VkSurfaceFormatKHR> fmts(fn);
  if (fn) vkGetPhysicalDeviceSurfaceFormatsKHR(ctx_.phys, surface_, &fn, fmts.data());
  VkSurfaceFormatKHR chosen{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
  if (fn > 0) {
    chosen = fmts[0];
    for (const auto& f : fmts) {
      if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) {
        chosen = f;
        break;
      }
    }
  }
  format_ = (chosen.format == VK_FORMAT_R8G8B8A8_UNORM) ? Format::RGBA8_UNORM : Format::BGRA8_UNORM;

  std::uint32_t image_count = caps.minImageCount + 1;
  if (caps.maxImageCount > 0 && image_count > caps.maxImageCount) image_count = caps.maxImageCount;

  VkSwapchainCreateInfoKHR scci{};
  scci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
  scci.surface = surface_;
  scci.minImageCount = image_count;
  scci.imageFormat = chosen.format;
  scci.imageColorSpace = chosen.colorSpace;
  scci.imageExtent = {extent_.width, extent_.height};
  scci.imageArrayLayers = 1;
  scci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  scci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  scci.preTransform = caps.currentTransform;
  scci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  scci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
  scci.clipped = VK_TRUE;
  r = vkCreateSwapchainKHR(ctx_.device, &scci, nullptr, &swapchain_);
  if (r != VK_SUCCESS) return Status::InitFailed(fmt_err("vkCreateSwapchainKHR", r));

  std::uint32_t ic = 0;
  vkGetSwapchainImagesKHR(ctx_.device, swapchain_, &ic, nullptr);
  images_.resize(ic);
  if (ic) vkGetSwapchainImagesKHR(ctx_.device, swapchain_, &ic, images_.data());
  views_.clear();
  for (std::uint32_t i = 0; i < ic; ++i) {
    views_.push_back(std::make_unique<SwapchainImageView>(extent_));
  }
  return Status::Ok_();
}

Status VulkanSwapchain::destroy_swapchain() {
  views_.clear();
  images_.clear();
  if (swapchain_) {
    vkDeviceWaitIdle(ctx_.device);
    vkDestroySwapchainKHR(ctx_.device, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
  }
  return Status::Ok_();
}

Status VulkanSwapchain::rebuild(Extent2D new_extent) {
  if (new_extent.width == 0 || new_extent.height == 0) {
    return Status::Invalid("rebuild: extent 必须 > 0");
  }
  if (!surface_) return Status::DeviceLost("rebuild: surface 已失效(SurfaceDestroyed 之后需重建)");
  Status s = destroy_swapchain();
  if (!s.ok()) return s;
  return create_swapchain_objects(new_extent);
}

IRenderTarget* VulkanSwapchain::current_target() {
  if (views_.empty()) return nullptr;
  return views_[0].get();
}

}  // namespace magicblocks::backend
