// MagicBlocks —— Vulkan 后端:交换链实现
#include "backend/vulkan/vulkan_swapchain.h"

#include <algorithm>

#include "backend/vulkan/vulkan_frame_target.h"
#include "backend/vulkan/vulkan_swapchain_android.h"
#include "backend/vulkan/vulkan_swapchain_win32.h"
#include "backend/vulkan/vulkan_swapchain_xcb.h"

namespace magicblocks::backend {

using namespace magicblocks::device;

namespace {

// surface 图像只读视图:swapchain 拥有图像,这里只描述大小/格式。
// 同时实现 IFrameTargetImage —— submit_device_frame 需要拿到这张 VkImage
// 才能把 render pass 建在它上面(pad 上的主渲染路径)。
class SwapchainImageView final : public IRenderTarget, public IFrameTargetImage {
 public:
  SwapchainImageView(Extent2D e, VkImage image, Format fmt) : extent_(e), image_(image), format_(fmt) {}
  TargetDesc::Kind kind() const override { return TargetDesc::Kind::Window; }
  Extent2D extent() const override { return extent_; }
  // 必须报**真实**格式：管线创建时用这个格式建 render pass，
  // 若这里写死 BGRA8 而实际交换链是 RGBA8，管线与 pass 不兼容，
  // vkCmdDraw 会被静默丢弃（表现为纯清屏色）。
  Format color_format() const override { return format_; }
  u32 samples() const override { return 1; }
  AttachmentView color_attachment() const override {
    return AttachmentView{extent_, format_, true};
  }
  AttachmentView depth_attachment() const override { return AttachmentView{}; }
  Status readback_rgba8(std::uint8_t*, std::size_t) const override {
    return Status::Unsupported("swapchain 图像不支持 CPU 读回;离屏渲染请用 create_offscreen_target");
  }

  // IFrameTargetImage。交换链图像由 VkSwapchainKHR 持有，这里只借用句柄。
  VkImage frame_color_image() const override { return image_; }
  VkImage frame_depth_image() const override { return VK_NULL_HANDLE; }  // 交换链无深度
  bool frame_from_swapchain() const override { return true; }

 private:
  Extent2D extent_;
  VkImage image_ = VK_NULL_HANDLE;
  Format format_ = Format::RGBA8_UNORM;
};

}  // namespace

VulkanSwapchain::VulkanSwapchain(const VulkanContext& ctx, const NativeSurface& surface)
    : ctx_(ctx), native_(surface) {}

VulkanSwapchain::~VulkanSwapchain() {
  views_.clear();
  if (acquire_fence_ != VK_NULL_HANDLE) {
    vkDestroyFence(ctx_.device, acquire_fence_, nullptr);
    acquire_fence_ = VK_NULL_HANDLE;
  }
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

  Status cs = create_swapchain_objects(extent);
  if (!cs.ok()) return cs;

  // acquire 用 fence:保证下一帧覆盖写之前 GPU 已经用完这张图。
  if (acquire_fence_ == VK_NULL_HANDLE) {
    VkFenceCreateInfo fi{};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;  // 首帧不应等待
    const VkResult r = vkCreateFence(ctx_.device, &fi, nullptr, &acquire_fence_);
    if (r != VK_SUCCESS) return Status::InitFailed(fmt_err("vkCreateFence", r));
  }
  return Status::Ok_();
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
    // 每张交换链图像对应一个视图，且必须带上自己的 VkImage ——
    // submit_device_frame 就是靠它把 render pass 建在这张图上。
    views_.push_back(std::make_unique<SwapchainImageView>(extent_, images_[i], format_));
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

// ---- 上屏三件事:acquire / present ----
//
// 这两个动作此前在后端**完全没有实现**(ISwapchain 只建了 image 就结束了),
// 所以"上屏"一直是空的。Android SurfaceView 必须真的走
// vkAcquireNextImageKHR + vkQueuePresentKHR,这里补上。
//
// 关键约束:**不读回任何像素**。acquire 只取图,present 只交图。

ISwapchain::ImageResult VulkanSwapchain::acquire_next_image() {
  ImageResult out{};
  if (swapchain_ == VK_NULL_HANDLE || !surface_) {
    out.status = ImageStatus::SurfaceLost;
    out.message = "acquire: swapchain/surface 不可用";
    return out;
  }
  if (images_.empty() || images_.size() != views_.size()) {
    out.status = ImageStatus::Failed;
    out.message = "acquire: image 数组不一致";
    return out;
  }

  // 用 fence 保证 GPU 已经把旧帧用完再覆盖这张图。
  // 超时给 1 秒:UI 线程不允许永久阻塞,超时就如实报失败让上层决定。
  std::uint32_t idx = 0;
  const VkResult r = vkAcquireNextImageKHR(ctx_.device, swapchain_, 1'000'000'000ull,
                                           VK_NULL_HANDLE, acquire_fence_, &idx);
  if (r == VK_ERROR_OUT_OF_DATE_KHR) {
    out.status = ImageStatus::OutOfDate;
    out.message = "acquire: OUT_OF_DATE";
    return out;
  }
  if (r == VK_ERROR_SURFACE_LOST_KHR) {
    out.status = ImageStatus::SurfaceLost;
    out.message = "acquire: SURFACE_LOST";
    surface_lost_ = true;
    return out;
  }
  if (r == VK_TIMEOUT) {
    out.status = ImageStatus::Failed;
    out.message = "acquire: 超时(1s)";
    return out;
  }
  if (r != VK_SUCCESS) {
    out.status = ImageStatus::Failed;
    out.message = fmt_err("vkAcquireNextImageKHR", r);
    return out;
  }

  // 等上一帧的 GPU 工作结束,这样这张 image 可以立刻被覆盖写。
  if (acquire_fence_ != VK_NULL_HANDLE) {
    vkWaitForFences(ctx_.device, 1, &acquire_fence_, VK_TRUE, 1'000'000'000ull);
    vkResetFences(ctx_.device, 1, &acquire_fence_);
  }

  if (idx >= views_.size()) {
    out.status = ImageStatus::Failed;
    out.message = "acquire: 索引越界";
    return out;
  }
  current_index_ = idx;
  out.status = ImageStatus::Ok;
  out.target = views_[idx].get();
  return out;
}

device::ISwapchain::ImageStatus VulkanSwapchain::present_current() {
  if (swapchain_ == VK_NULL_HANDLE || !surface_) return ImageStatus::SurfaceLost;

  VkPresentInfoKHR pi{};
  pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
  pi.swapchainCount = 1;
  pi.pSwapchains = &swapchain_;
  pi.pImageIndices = &current_index_;
  // 不等待信号量:本 SDK 在 submit 时已用 fence 保证顺序,见 vulkan_device_frame.cpp。

  const VkResult r = vkQueuePresentKHR(ctx_.queue, &pi);
  if (r == VK_ERROR_OUT_OF_DATE_KHR) return ImageStatus::OutOfDate;
  if (r == VK_ERROR_SURFACE_LOST_KHR) {
    surface_lost_ = true;
    return ImageStatus::SurfaceLost;
  }
  if (r != VK_SUCCESS) return ImageStatus::Failed;
  return ImageStatus::Ok;
}

}  // namespace magicblocks::backend
