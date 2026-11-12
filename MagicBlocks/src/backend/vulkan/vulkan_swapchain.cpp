// MagicBlocks —— Vulkan 后端:交换链实现
#include "backend/vulkan/vulkan_swapchain.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#if defined(__ANDROID__)
#include <android/log.h>
#endif

#include "backend/vulkan/vulkan_allocator.h"
#include "backend/vulkan/vulkan_frame_target.h"
#include "backend/vulkan/vulkan_resource_common.h"  // color_format_supports_samples(MSAA 兜底防线)
#include "backend/vulkan/vulkan_swapchain_android.h"
#include "backend/vulkan/vulkan_swapchain_win32.h"
#include "backend/vulkan/vulkan_swapchain_xcb.h"
#include "backend/vulkan/vulkan_sync_core.h"

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
  // 【白名单第 2 处 / 全仓仅剩 2 处 vkDeviceWaitIdle 之一】
  //
  // present 只是把图交给显示队列,GPU 可能还在用它。规范要求销毁交换链前
  // 所有已 acquire 的 presentable image 的使用都已完成,否则校验层报
  //   VUID-vkDestroySwapchainKHR-swapchain-01282: All uses of presentable
  //   images acquired from swapchain must have completed execution
  //
  // 为什么这里**不能**像渲染路径那样改成槽位 fence:
  //   vkAcquireNextImageKHR / vkQueuePresentKHR 是**presentation 引擎**的工作。
  //   我们自己的 in_flight 槽位 fence 只覆盖"我们提交的那几批图形命令",
  //   覆盖不到"展示端还在读这张图"。规范里与之等价的信号是外部 presentation
  //   完成(需要 VK_KHR_present_wait / present fences 扩展,本项目未启用),
  //   所以在"交换链销毁"这个**不可逆、每设备一次**的时刻,device 级全停是唯一
  //   能同时覆盖 graphics 与 presentation 两条引擎的等待。销毁交换链不是每帧
  //   动作(rebuild() 只在 OUT_OF_DATE 触发),代价可接受。
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

  // 绑定设备的同步核心(非拥有,只登记指针):acquire 要拿它的槽位信号量。
  //
  // 为什么不把它放进 VulkanContext:ctx_ 是纯句柄容器、会被按值复制,
  // 而同步核心持有 VkSemaphore/VkFence/命令缓冲。见 vulkan_sync_core.h 文件头。
  // 找不到时(测试手工拼 VulkanContext 的旁路)acquire 退回"只取图、不带信号量",
  // 那条路径本来也没有 present 侧的等待,语义与改造前一致。
  frame_sync_ = VulkanSyncRegistry::find(ctx_.device);
  probe_.aligned = (frame_sync_ != nullptr && frame_sync_->ready());
  probe_.in_flight = frame_sync_ != nullptr ? frame_sync_->in_flight() : 0;
  probe_.slot = 0;

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
  // 优先 MAILBOX：它同样**不会撕裂**(Vulkan 规范里任何 present mode 都禁止撕裂，
  // 见 VkPresentModeKHR 的说明——IMMEDIATE 才是"可能撕裂"的那个，MAILBOX 不是)。
  // 区别只在"谁来丢弃多余帧"：
  //   FIFO    : 每个 vsync 吐一帧，多余的帧**排队**。队列满后 vkQueuePresentKHR 阻塞，
  //             进而 vkAcquireNextImageKHR 也拿不到图 —— 实测 acquire 要等 5.8ms。
  //   MAILBOX : 每个 vsync 吐一帧，多余的帧**被更新的帧顶掉**(只有一个待呈现槽)，
  //             于是 present 永不阻塞、acquire 几乎不等待 —— 实测 0.1ms，帧率 56→100+。
  // 代价是"生产多于刷新率时，被丢掉的帧用户看不到"——这是刻意的(降延迟)，
  // 不是画面错误。若某设备不支持 MAILBOX，按 FIFO 回退(FIFO 恒可用)。
  bool has_mailbox = false;
  bool has_immediate = false;
  for (auto m : pm) {
    if (m == VK_PRESENT_MODE_MAILBOX_KHR) has_mailbox = true;
    if (m == VK_PRESENT_MODE_IMMEDIATE_KHR) has_immediate = true;
  }
  modes_.push_back(PresentMode::Fifo);  // FIFO 恒可用
  if (has_mailbox) modes_.push_back(PresentMode::Mailbox);
  if (has_immediate) modes_.push_back(PresentMode::Immediate);
  // 默认 MAILBOX，不可用时回退 FIFO。见上面关于"MAILBOX 也不撕裂"的说明。
  present_mode_ = has_mailbox ? PresentMode::Mailbox : PresentMode::Fifo;

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
  // preTransform：**恒用 IDENTITY**，让合成器（SurfaceFlinger / DWM）去旋转。
  //
  // 曾经写的是 `scci.preTransform = caps.currentTransform`，而引擎从来没有按
  // preTransform 去旋转自己贴进去的内容 —— 等于向系统**撒谎**"我已经转过 90°，你
  // 别转了"。这台 pad 的面板物理竖置、应用横屏，currentTransform 恰好是
  // ROTATE_90(0x2)，于是合成器按"已旋转"去解读一块**未旋转**的 buffer。
  //
  // 实测症状：SurfaceView 层的 geomBufferSize=[1440 2160] 与
  // geomContentCrop=[2160 1440] 互相矛盾，合成后画面只剩中间一块 720x720 的
  // 死色方块，立方体完全不见 —— 用户看到的就是"旋转无效"，其实是**整帧都没上屏**。
  //
  // Vulkan 允许在 caps.supportedTransforms 里任选一个声明；IDENTITY 几乎恒被支持
  // （不支持时按流程回退到 currentTransform，那种情况下才需要真正实现旋转）。
  // 选 IDENTITY 后 imageExtent 就是 surface 的原始尺寸（2048/2160 等），
  // 内容按未旋转坐标系绘制，由合成器负责转到显示朝向。
  {
    VkSurfaceTransformFlagsKHR supported = caps.supportedTransforms;
    if ((supported & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) != 0) {
      scci.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    } else {
      scci.preTransform = caps.currentTransform;  // 兜底：只能按当前的来
    }
    pre_transform_ = static_cast<std::uint32_t>(scci.preTransform);
  }
  scci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  // 与上面 present_mode_ 的选择保持一致；MAILBOX 与 FIFO 都不会撕裂，
  // MAILBOX 只是把"多余帧排队"换成"多余帧被顶掉"，从而不再阻塞 acquire。
  scci.presentMode = (present_mode_ == PresentMode::Mailbox)      ? VK_PRESENT_MODE_MAILBOX_KHR
                     : (present_mode_ == PresentMode::Immediate)  ? VK_PRESENT_MODE_IMMEDIATE_KHR
                                                                  : VK_PRESENT_MODE_FIFO_KHR;
  scci.clipped = VK_TRUE;
  r = vkCreateSwapchainKHR(ctx_.device, &scci, nullptr, &swapchain_);
  // MAILBOX 虽被 surface 声明支持，个别驱动仍会拒绝它。此时回退 FIFO 重建一次，
  // 而不是让整个 Surface 初始化失败(那样用户看到的是黑屏，比 60fps 严重得多)。
  if (r != VK_SUCCESS && scci.presentMode != VK_PRESENT_MODE_FIFO_KHR) {
    scci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    present_mode_ = PresentMode::Fifo;
    r = vkCreateSwapchainKHR(ctx_.device, &scci, nullptr, &swapchain_);
  }
  if (r != VK_SUCCESS) return Status::InitFailed(fmt_err("vkCreateSwapchainKHR", r));

  std::uint32_t ic = 0;
  vkGetSwapchainImagesKHR(ctx_.device, swapchain_, &ic, nullptr);
  images_.resize(ic);
  if (ic) vkGetSwapchainImagesKHR(ctx_.device, swapchain_, &ic, images_.data());

#if defined(__ANDROID__)
  __android_log_print(ANDROID_LOG_INFO, "MagicBlocks",
                      "[swapchain] %ux%u images=%u preTransform=0x%x",
                      extent_.width, extent_.height, ic, pre_transform_);
#endif

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
  // 【2026-11-11】原来是 vkDeviceWaitIdle(ctx_.device) —— 一个 device 级全停,
  // 却只是为了"保证没人在用这些 presentable image / MS 图 / 视图"。
  //
  // 现在改成**等设备自己的在飞帧**:
  //   VulkanSyncCore::drain() 把 N 个槽位的 fence 全等一遍 —— 那正是本设备
  //   对交换链图发起过的全部写入(acquire→submit→present 都在同一队列上),
  //   与"设备在飞帧已排空"语义等价,但不是 queue/device 级全停。
  //
  // 为什么渲染路径可以这样做、而析构路径不行:rebuild() 只发生在 OUT_OF_DATE
  // (转屏 / 软键盘 / 分屏),此时我们自己的提交是唯一的图形使用者;而析构路径要
  // 面对 presentation 引擎仍在读图的情况(VUID-01282),那里仍需 device 级等待
  // (见 ~VulkanSwapchain 的注释)。
  Status idle = wait_device_frames_idle(ctx_);
  frame_acquired_ = false;
  views_.clear();
  destroy_msaa_images();
  images_.clear();
  if (swapchain_) {
    vkDestroySwapchainKHR(ctx_.device, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
  }
  return idle;
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

  // ---- 帧环对齐(2026-11-11)----
  //
  // 有设备同步核心时,本帧走它当前槽位:
  //   1) begin_frame():只等**该槽位**的 fence —— N 帧并行就靠这里"只等我这一格";
  //   2) vkAcquireNextImageKHR 把该槽的 image_available 传进去 —— GPU 侧规定
  //      "这张图在被我们写之前,必须等这个信号量",于是 submit 的 waitSemaphore
  //      与 acquire 的 signal 天然配对,不需要任何 CPU 等待。
  VkSemaphore acquire_sem = VK_NULL_HANDLE;
  VulkanSyncCore* core = VulkanSyncRegistry::find(ctx_.device);
  if (core != nullptr && core->ready()) {
    const Status bs = core->begin_frame(1'000'000'000ull);
    if (!bs.ok()) {
      out.status = ImageStatus::Failed;
      out.message = "acquire: " + bs.message;
      return out;
    }
    frame_sync_ = core;
    frame_slot_ = core->current_slot();
    acquire_sem = core->slot(frame_slot_).image_available;
    probe_.aligned = true;
    probe_.slot = frame_slot_;
    probe_.in_flight = core->in_flight();
  } else {
    frame_sync_ = nullptr;
    probe_.aligned = false;
    probe_.in_flight = 0;
  }

  // 用 fence 保证 GPU 已经把旧帧用完再覆盖这张图。
  //
  // ⚠️ 传进 vkAcquireNextImageKHR 的 fence 在调用时必须处于 **unsignaled**
  // 状态(VUID-vkAcquireNextImageKHR-fence-01287)。上一次 acquire 会把它置为
  // signaled,所以每次 acquire 前必须先 wait 再 reset,否则校验层每轮报错。
  //
  // 真机卡 1 秒的根因就在下面这两个等待上(2026-11-11):
  //   fence 建时带 SIGNALED(首帧不该等),于是第一轮 wait 立刻过、reset 后
  //   交给 vkAcquireNextImageKHR。但该调用在"有图可拿"时是**立即返回**的,
  //   并不保证把 fence 置为 signaled —— 驱动把信号化留给了真正完成的那一刻。
  //   下一轮再 wait 就等满 1000ms 超时。表现为稳定 1fps,且 CPU 几乎不耗
  //   (全在 vkWaitForFences 里睡),极易被误判成"渲染慢"。
  // 正解:图可用性本来就不需要 fence 守 —— vkAcquireNextImageKHR 返回成功
  //   就意味着这张图已经归我们独占并且 GPU 早已用完它。真正要等的是
  //   **上一帧的呈现**完成,那由 present 的 fence 负责,不该塞进 acquire。
  //   所以这里**不传 fence**(传 VK_NULL_HANDLE)。
  //
  // 【信号量不是可选的】上面的"不需要 fence"依然成立,但"不需要任何同步"不成立:
  //   Vulkan 要求 presentable image 在被写之前必须等到 acquire 的信号量。
  //   改造前这里传 VK_NULL_HANDLE,等于告诉驱动"我已经有权写了",于是
  //   在 present 还没把上一帧交还之前就可能开始覆盖 —— 校验层不报,画面撕裂/闪帧。
  std::uint32_t idx = 0;
  const VkResult r = vkAcquireNextImageKHR(ctx_.device, swapchain_, 1'000'000'000ull,
                                           acquire_sem, VK_NULL_HANDLE, &idx);
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

  // 不再需要 post-acquire 的 fence 等待:vkAcquireNextImageKHR 成功返回
  // 即代表这张图已独占;写入时机由上面那颗信号量在 GPU 侧守。

  if (idx >= views_.size()) {
    out.status = ImageStatus::Failed;
    out.message = "acquire: 索引越界";
    return out;
  }
  current_index_ = idx;
  frame_acquired_ = true;
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
  // 【2026-11-11】这里原来写着"不等待信号量:submit 时已用 fence 保证顺序"。
  // 那句话在改造前勉强成立,只因为 submit_device_frame() **同步**等到 GPU 跑完才返回。
  // 现在提交是异步的,若不设 pWaitSemaphores,展示端会在 GPU 还没画完时就拿走这张图。
  // 正确做法是等本帧槽位的 render_finished(由 vkQueueSubmit 的 signal 段产生)。
  if (frame_sync_ != nullptr && frame_acquired_) {
    FrameSlot& s = frame_sync_->slot(frame_slot_);
    if (s.render_finished != VK_NULL_HANDLE) {
      pi.waitSemaphoreCount = 1;
      pi.pWaitSemaphores = &s.render_finished;
    }
  }

  const VkResult r = vkQueuePresentKHR(ctx_.queue, &pi);
  // 本帧到此结束:下一个 acquire 会拿到下一个槽位,并只等那个槽位的 fence。
  // (不做任何 CPU 侧等待 —— present 的完成由 presentation 引擎与交换链归属保证。)
  frame_acquired_ = false;
  if (frame_sync_ != nullptr) {
    const Status es = frame_sync_->end_frame(VK_NULL_HANDLE);
    (void)es;  // 推进帧序号失败只会让槽位轮转停住,不影响本帧结果;下次 acquire 会发现。
  }
  if (r == VK_ERROR_OUT_OF_DATE_KHR) return ImageStatus::OutOfDate;
  if (r == VK_ERROR_SURFACE_LOST_KHR) {
    surface_lost_ = true;
    return ImageStatus::SurfaceLost;
  }
  // VK_SUBOPTIMAL_KHR 也是 1000001003（与 VK_ERROR_SURFACE_LOST_KHR 同值但语义相反，
  // 它是**正数** 1000001003，而 SURFACE_LOST 是负的常量比较）。上面那行用常量比较，
  // 只有真正的 SURFACE_LOST 会命中；走到这里若 r == VK_SUBOPTIMAL_KHR：
  //
  //   **这帧已经成功上屏了**，只是 surface 的当前朝向/尺寸与交换链不是最优匹配。
  //   Android 上声明 preTransform = IDENTITY 时必然出现这个返回值（surface 被
  //   旋转过，而交换链选择让合成器去转）。
  //
  // 以前没有这一支，`r != VK_SUCCESS` 把 SUBOPTIMAL 当成 Failed —— 每帧都报
  // "present 失败(status=4)" 并把已经画好、已经上屏的帧当丢弃处理，于是画面
  // 冻在最早那一帧上（表现为"旋转无效"）。SUBOPTIMAL 绝不能算失败。
  if (r == VK_SUBOPTIMAL_KHR) return ImageStatus::Ok;
  if (r != VK_SUCCESS) {
#if defined(__ANDROID__)
    __android_log_print(ANDROID_LOG_WARN, "MagicBlocks", "[swapchain] present VkResult=%d", (int)r);
#endif
    return ImageStatus::Failed;
  }
  return ImageStatus::Ok;
}

}  // namespace magicblocks::backend
