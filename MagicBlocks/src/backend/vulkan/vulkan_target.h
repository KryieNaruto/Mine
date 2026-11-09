// MagicBlocks —— Vulkan 后端:渲染目标(Window / Offscreen 同构)
//
// L0 的离屏通路 = 一张颜色附件(+可选深度)+ 可读回。
// 这里只做「分配/布局/读回」的 plumbing,**不做**场景、渲染图、排序
// (那些是 L2 的职责;L2 落地时本文件不承载任何编排逻辑)。
#pragma once

#include <cstdint>
#include <memory>

#include "backend/vulkan/vk_common.h"
#include "backend/vulkan/vulkan_frame_target.h"
#include "magicblocks/device/render_target.h"

namespace magicblocks::backend {

using Status = device::Status;

// 后端内共享的"一次性命令提交"工具(wait-idle 语义,够 L0 用)。
class VulkanPagingAllocator;

class VulkanContext {
 public:
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice phys = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  std::uint32_t queue_family = 0;
  VkCommandPool cmd_pool = VK_NULL_HANDLE;
  VkPhysicalDeviceProperties props{};
  VkPhysicalDeviceMemoryProperties mem{};
  // L1 分页分配器(非拥有):由 VulkanRenderDevice 持有,存活期覆盖所有资源与目标。
  // 所有显存分配都必须经它(G4:全仓零裸分配)。
  VulkanPagingAllocator* allocator = nullptr;
};

// 找一段满足 typeBits 与 flags 的内存类型;找不到返回 UINT32_MAX。
std::uint32_t find_memory_type(const VulkanContext& ctx, std::uint32_t type_bits,
                               VkMemoryPropertyFlags flags);

// 创建一张图像并由 L1 分页分配器出内存(G4:全仓零裸分配)。
// 返回 Status;失败时不留半成品。out_page 是分配器给该图像的专用页 id,
// 销毁图像后必须调 release_image_page(out_page)。
//
// samples:MSAA 采样数,缺省 VK_SAMPLE_COUNT_1_BIT(既有调用点因此逐字节不变)。
// samples > 1 的图像不能当 TransferSrc 读回(Vulkan 禁止非 1 采样图像做
// vkCmdCopyImageToBuffer),所以 MSAA 通路必须另配一张单采样 resolve 图像。
Status create_image(const VulkanContext& ctx, VkFormat fmt, VkImageUsageFlags usage,
                    VkImageLayout initial_layout, std::uint32_t w, std::uint32_t h,
                    VkImage* out_image, std::uint32_t* out_page,
                    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT);

// OffscreenTarget:纯离屏渲染目标。
//
// ⚠️ 历史坑(2026-11-06 F-SK 收尾):这里曾经写作
//     `class OffscreenTarget final : public device::IRenderTarget, public IFrameTargetImage`
//   —— 用了 **private** 继承。后果是 `submit_device_frame` 里的
//     `dynamic_cast<IFrameTargetImage*>(&target)` **恒为 nullptr**(private 基类
//   对 dynamic_cast 不可见),于是每一帧都报
//     「submit_device_frame: 第 0 条命令(begin_pass): 目标不提供可用的颜色附件图像」。
//   编译器不会报错(C++ 允许私有基类),而 base 顺序变化(先 IRenderTarget)
//   又会把 vtable 的布局改掉、让别的 dynamic_cast 落到错误的 vptr 上 ——
//   只能靠真实提交路径的测试抓到。所以:**这两个基类必须 public**。
class OffscreenTarget final : public device::IRenderTarget, public IFrameTargetImage {
 public:
  OffscreenTarget(const VulkanContext& ctx, const device::TargetDesc& desc) : ctx_(ctx), desc_(desc) {}
  ~OffscreenTarget() override;

  Status init();

  device::TargetDesc::Kind kind() const override { return device::TargetDesc::Kind::Offscreen; }
  device::Extent2D extent() const override { return desc_.extent; }
  device::Format color_format() const override { return desc_.format; }
  device::u32 samples() const override { return desc_.samples; }

  device::AttachmentView color_attachment() const override;
  device::AttachmentView depth_attachment() const override;

  Status readback_rgba8(std::uint8_t* out, std::size_t size_bytes) const override;

  // 自检绘制:画 4x4 色块(实例化,无几何着色器)。
  //   kL0Default → fullscreen.vert/frag,纯红标定点 + 黑清屏(L0 golden)
  //   kL2Scene   → l2_scene.vert/frag,纯绿标定点 + 深蓝清屏(L2 golden)
  // 两套输出逐像素不同,保证 golden_l0 与 golden_l2 不会互相冒充。
  enum class SelftestKind { kL0Default, kL2Scene };
  Status draw_selftest(std::uint64_t frame_index,
                       SelftestKind kind = SelftestKind::kL0Default);

  // L0 内部用:交给离屏绘制通路
  //
  // MSAA 语义(重要):color_ 是**渲染用的颜色附件**,samples>1 时它是多重采样图像;
  // resolve_image() 才是单采样、可读回的那张。所以:
  //   - render pass / framebuffer 的附件 0 挂 color_ 的视图
  //   - pResolveAttachments 挂 resolve_image()
  //   - readback_rgba8 读固定读 resolve_image()(samples==1 时两者是同一张图)
  VkImage color_image() const { return color_; }
  // MSAA 的单采样 resolve 图像。samples == 1 时**等于** color_(同一张图),
  // 这样读回路径与历史完全一致,不会多出一张没用上的图像。
  VkImage resolve_image() const { return resolve_ != VK_NULL_HANDLE ? resolve_ : color_; }
  // L3 设备帧提交用:深度附件图像(with_depth=false 时为 VK_NULL_HANDLE)。
  // 管线/pass 的深度附件形状必须与它一致(vulkan_device_frame.cpp 的 PassShape)。
  VkImage depth_image() const { return depth_; }
  const VulkanContext& ctx() const { return ctx_; }

  // IFrameTargetImage:让 submit_device_frame 不必强转具体类型。
  // frame_color_image() 返回的是**多重采样**那张(渲染用,pass 的附件 0);
  // frame_resolve_image() 返回单采样那张(pass 的 resolve 附件)。
  //
  // ⚠️ frame_resolve_image() **必须重写**。基类默认实现是
  // `return frame_color_image();`(为交换链兜底,那边本来就没有 resolve)。
  // 少了这个重写,MSAA 时 framebuffer 的附件 1 会拿到那张 4 采样图,
  // 而 render pass 声明附件 1 是 1 采样 → 采样数不匹配 → vkCmdDraw 被
  // **静默丢弃**,画面只剩清屏色(不报错,极难查)。本文件 2026-11-09 栽过这一跤。
  VkImage frame_color_image() const override { return color_; }
  VkImage frame_resolve_image() const override { return resolve_image(); }
  VkImage frame_depth_image() const override { return depth_; }
  bool frame_from_swapchain() const override { return false; }

 private:
  VulkanContext ctx_;
  device::TargetDesc desc_{};
  VkImage color_ = VK_NULL_HANDLE;
  std::uint32_t color_page_ = 0xFFFFFFFFu;  // 分配器图像页 id(不是 VkDeviceMemory)
  VkImage depth_ = VK_NULL_HANDLE;
  std::uint32_t depth_page_ = 0xFFFFFFFFu;
  // MSAA 的单采样 resolve 图像。samples == 1 时为 VK_NULL_HANDLE(不额外分配),
  // resolve_image() 直接回落到 color_,历史路径零变化。
  VkImage resolve_ = VK_NULL_HANDLE;
  std::uint32_t resolve_page_ = 0xFFFFFFFFu;
};

VkFormat to_vk_format(device::Format f);
VkFormat to_vk_depth_format(device::DepthFormat f);

}  // namespace magicblocks::backend
