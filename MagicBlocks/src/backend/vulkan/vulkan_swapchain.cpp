// MagicBlocks —— Vulkan 后端:交换链实现
#include "backend/vulkan/vulkan_swapchain.h"

#include <algorithm>
#include <cstdio>

#if defined(__ANDROID__)
#include <android/log.h>
#endif

#include "backend/vulkan/vulkan_allocator.h"
#include "backend/vulkan/vulkan_frame_target.h"
#include "backend/vulkan/vulkan_resource_common.h"  // color_format_supports_samples(MSAA 兜底防线)
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
  // msaa = 交换链图中的那张"GPU 侧 resolve 目标"；samples>1 时
  // frame_color_image() 改报独立的多采样图，由 render pass 在末尾 resolve 回 msaa。
  SwapchainImageView(Extent2D e, VkImage image, Format fmt, VkImage msaa = VK_NULL_HANDLE,
                     u32 samples = 1)
      : extent_(e), image_(image), format_(fmt), msaa_(msaa), samples_(samples) {}
  TargetDesc::Kind kind() const override { return TargetDesc::Kind::Window; }
  Extent2D extent() const override { return extent_; }
  // 必须报**真实**格式：管线创建时用这个格式建 render pass，
  // 若这里写死 BGRA8 而实际交换链是 RGBA8，管线与 pass 不兼容，
  // vkCmdDraw 会被静默丢弃（表现为纯清屏色）。
  Format color_format() const override { return format_; }
  u32 samples() const override { return samples_; }
  AttachmentView color_attachment() const override {
    return AttachmentView{extent_, format_, true};
  }
  AttachmentView depth_attachment() const override { return AttachmentView{}; }
  Status readback_rgba8(std::uint8_t*, std::size_t) const override {
    return Status::Unsupported("swapchain 图像不支持 CPU 读回;离屏渲染请用 create_offscreen_target");
  }

  // IFrameTargetImage。交换链图像由 VkSwapchainKHR 持有，这里只借用句柄。
  //
  // 开了 MSAA 后这两个必须分开报，否则 MSAA 等于没开：
  //   frame_color_image()   → 多采样图（render pass 往这里画，每像素 N 个采样）
  //   frame_resolve_image() → 交换链图（pass 末尾 resolve 的目标，也是上屏的那张）
  // 拿到的是**同一张**的话，pass 会退化成单采样，棱边照旧是锯齿。
  VkImage frame_color_image() const override {
    return samples_ > 1 && msaa_ != VK_NULL_HANDLE ? msaa_ : image_;
  }
  VkImage frame_resolve_image() const override { return image_; }
  VkImage frame_depth_image() const override { return VK_NULL_HANDLE; }  // 交换链无深度
  bool frame_from_swapchain() const override { return true; }

 private:
  Extent2D extent_;
  VkImage image_ = VK_NULL_HANDLE;
  Format format_ = Format::RGBA8_UNORM;
  VkImage msaa_ = VK_NULL_HANDLE;
  u32 samples_ = 1;
};

}  // namespace

VulkanSwapchain::VulkanSwapchain(const VulkanContext& ctx, const NativeSurface& surface)
    : ctx_(ctx), native_(surface) {}

VulkanSwapchain::~VulkanSwapchain() {
  // present 只是把图交给显示队列,GPU 可能还在用它。规范要求销毁交换链前
  // 所有已 acquire 的 presentable image 的使用都已完成,否则校验层报
  //   VUID-vkDestroySwapchainKHR-swapchain-01282: All uses of presentable
  //   images acquired from swapchain must have completed execution
  // 所以在拆任何东西之前先等设备空闲(P0 的析构路径,代价可接受)。
  if (ctx_.device != VK_NULL_HANDLE) {
    vkDeviceWaitIdle(ctx_.device);
  }
  views_.clear();
  destroy_msaa_images();  // 必须先于 swapchain_ 销毁：MS 图是独立图像
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
    pre_transform_ = static_cast<std::uint32_t>(caps.currentTransform);
  scci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  scci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
  scci.clipped = VK_TRUE;
  r = vkCreateSwapchainKHR(ctx_.device, &scci, nullptr, &swapchain_);
  if (r != VK_SUCCESS) return Status::InitFailed(fmt_err("vkCreateSwapchainKHR", r));

  std::uint32_t ic = 0;
  vkGetSwapchainImagesKHR(ctx_.device, swapchain_, &ic, nullptr);
  images_.resize(ic);
  if (ic) vkGetSwapchainImagesKHR(ctx_.device, swapchain_, &ic, images_.data());

  // ---- MSAA：每张交换链图配一张自己的 MS 图 ----
  //
  // 共用一张 MS 图的后果：swapchain 图有 3~4 张，GPU 可能同时持有 2~3 帧在飞。
  // 它们都往同一块 MS 缓冲里画，下一帧 begin 时的 clearLoadOp CLEAR 会把上一帧
  // 还没 resolve 的内容清掉 —— resolve 出去的是别的帧的画面（鬼影）。
  // 所以 MS 附件必须和交换链图一一对应、同生共死。
  (void)destroy_msaa_images();
  msaa_samples_ = 1;
  if (requested_msaa_ > 1) {
    VkSampleCountFlagBits bits = VK_SAMPLE_COUNT_1_BIT;
    switch (requested_msaa_) {
      case 2: bits = VK_SAMPLE_COUNT_2_BIT; break;
      case 4: bits = VK_SAMPLE_COUNT_4_BIT; break;
      case 8: bits = VK_SAMPLE_COUNT_8_BIT; break;
      case 16: bits = VK_SAMPLE_COUNT_16_BIT; break;
      default: bits = VK_SAMPLE_COUNT_1_BIT; break;
    }

    msaa_images_.assign(ic, VK_NULL_HANDLE);
    msaa_pages_.assign(ic, 0xFFFFFFFFu);
    Status msaa_status = Status::Ok_();
    bool ok_msaa = (bits != VK_SAMPLE_COUNT_1_BIT);
    if (!ok_msaa) {
      msaa_status = Status::Unsupported("采样数不是合法的 Vulkan 采样数(只支持 2/4/8/16)");
    }
    // 第二层防线:建图前先按**本交换链真实颜色格式**复核一次采样数支持。
    //
    // 为什么不能只靠 vkCreateImage 的返回值:SwiftShader 这类驱动在
    // "格式不支持该采样数"时会**返回 VK_SUCCESS 并给出一个无效图像**,只有验证层
    // 会打印错误 —— 而验证层在正式版和手机上是关闭的。也就是说,光看返回值会在
    // 用户设备上静默降级:UI 显示 8x,实际 MS 图不能用,棱边依旧锯齿且零日志。
    //
    // 这里主动查,不依赖任何调试组件,任何构建、任何设备都生效。
    if (ok_msaa && !color_format_supports_samples(ctx_.phys, chosen.format, requested_msaa_)) {
      char buf[192];
      std::snprintf(buf, sizeof(buf),
                    "颜色格式 %u 不支持 %ux 采样(驱动会「成功」返回一张无效图,所以必须在建图前拦)",
                    static_cast<unsigned>(chosen.format), static_cast<unsigned>(requested_msaa_));
      msaa_status = Status::Unsupported(buf);
      ok_msaa = false;
    }
    for (std::uint32_t i = 0; ok_msaa && i < ic; ++i) {
      msaa_status = create_image(ctx_, chosen.format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                 VK_IMAGE_LAYOUT_UNDEFINED, extent_.width, extent_.height,
                                 &msaa_images_[i], &msaa_pages_[i], bits);
      ok_msaa = msaa_status.ok();
    }

    if (ok_msaa) {
      msaa_samples_ = requested_msaa_;
    } else {
      // 绝不因为 MSAA 建不出来而挡住上屏：退回 1x，但必须留下原因。
      // 以前这里是静默的，用户开了 ×4 发现"还是有锯齿"却查不到为什么，
      // 只能靠盲猜。这条日志就是那种情况下的唯一线索。
      destroy_msaa_images();
      msaa_samples_ = 1;
      const char* why = msaa_status.message.empty() ? "未知原因" : msaa_status.message.c_str();
#if defined(__ANDROID__)
      __android_log_print(ANDROID_LOG_WARN, "MagicBlocks",
                          "[swapchain] MSAA 请求 %ux 但退回 1x，原因: %s", requested_msaa_, why);
#else
      std::fprintf(stderr, "[swapchain] MSAA 请求 %ux 但退回 1x，原因: %s\n", requested_msaa_, why);
#endif
    }
  }

  views_.clear();
  for (std::uint32_t i = 0; i < ic; ++i) {
    // 每张交换链图像对应一个视图，且必须带上自己的 VkImage ——
    // submit_device_frame 就是靠它把 render pass 建在这张图上。
    // MS 图也按 i 取，不能所有 view 共用 msaa_images_[0]。
    const VkImage msaa_for_i = (i < msaa_images_.size()) ? msaa_images_[i] : VK_NULL_HANDLE;
    views_.push_back(
        std::make_unique<SwapchainImageView>(extent_, images_[i], format_, msaa_for_i, msaa_samples_));
  }
  return Status::Ok_();
}

Status VulkanSwapchain::destroy_swapchain() {
  views_.clear();
  destroy_msaa_images();
  images_.clear();
  if (swapchain_) {
    vkDeviceWaitIdle(ctx_.device);
    vkDestroySwapchainKHR(ctx_.device, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
  }
  return Status::Ok_();
}

// 销毁所有 MS 图并把分页分配器的页全部还回去。
//
// 泄漏点在这里：每张 MS 图都占一个专用页，少 release 一次就是一块显存
// 在 swapchain 重建（转屏/后台恢复）时永久丢失。所以销毁和 release
// 必须在同一个循环里成对出现，不能只写其中一个。
void VulkanSwapchain::destroy_msaa_images() {
  for (std::size_t i = 0; i < msaa_images_.size(); ++i) {
    if (msaa_images_[i] != VK_NULL_HANDLE) {
      vkDestroyImage(ctx_.device, msaa_images_[i], nullptr);
    }
  }
  for (std::size_t i = 0; i < msaa_pages_.size(); ++i) {
    if (msaa_pages_[i] != 0xFFFFFFFFu && ctx_.allocator != nullptr) {
      (void)ctx_.allocator->release_image_page(msaa_pages_[i]);
    }
  }
  msaa_images_.clear();
  msaa_pages_.clear();
  msaa_samples_ = 1;
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
  // 必须读 current_index_，不能硬编码 0。
  //
  // acquire_next_image() 返回的是 views_[idx]，idx 由驱动决定，可能不是 0。
  // 这里写死 views_[0] 意味着：只要交换链有 2 张以上图，UI 层拿到的就永远是
  // 最初那张，多帧在飞时渲染到错误的图上（画了但看不见 / 上屏内容串帧）。
  // 曾经的实现就是 views_[0]，且 acquire 里明明已经存了 current_index_ 也没人读它。
  return target_at(current_index_);
}

IRenderTarget* VulkanSwapchain::target_at(std::uint32_t index) {
  // 把"索引 → 目标"单独拎出来（而不是内联在 current_target 里）是为了可测：
  // 软件驱动在隐藏窗口下可能永远只返回 0 号索引，那种环境里
  // current_target() 硬编码 0 也能"碰巧"对上，测试就抓不到。
  // 有了 target_at(i)，测试可以直接问"1 号和 0 号是不是同一个对象"。
  if (index >= views_.size()) return nullptr;
  return views_[index].get();
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
  //
  // ⚠️ 传进 vkAcquireNextImageKHR 的 fence 在调用时必须处于 **unsignaled**
  // 状态(VUID-vkAcquireNextImageKHR-fence-01287)。上一次 acquire 会把它置为
  // signaled,所以每次 acquire 前必须先 wait 再 reset,否则校验层每轮报错。
  if (acquire_fence_ != VK_NULL_HANDLE) {
    vkWaitForFences(ctx_.device, 1, &acquire_fence_, VK_TRUE, 1'000'000'000ull);
    vkResetFences(ctx_.device, 1, &acquire_fence_);
  }
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
