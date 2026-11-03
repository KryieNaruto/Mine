// MagicBlocks —— Vulkan 后端:渲染目标(Window / Offscreen 同构)
//
// L0 的离屏通路 = 一张颜色附件(+可选深度)+ 可读回。
// 这里只做「分配/布局/读回」的 plumbing,**不做**场景、渲染图、排序
// (那些是 L2 的职责;L2 落地时本文件不承载任何编排逻辑)。
#pragma once

#include <cstdint>
#include <memory>

#include "backend/vulkan/vk_common.h"
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
Status create_image(const VulkanContext& ctx, VkFormat fmt, VkImageUsageFlags usage,
                    VkImageLayout initial_layout, std::uint32_t w, std::uint32_t h,
                    VkImage* out_image, std::uint32_t* out_page);

// OffscreenTarget:纯离屏渲染目标。
class OffscreenTarget final : public device::IRenderTarget {
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
  VkImage color_image() const { return color_; }
  const VulkanContext& ctx() const { return ctx_; }

 private:
  VulkanContext ctx_;
  device::TargetDesc desc_{};
  VkImage color_ = VK_NULL_HANDLE;
  std::uint32_t color_page_ = 0xFFFFFFFFu;  // 分配器图像页 id(不是 VkDeviceMemory)
  VkImage depth_ = VK_NULL_HANDLE;
  std::uint32_t depth_page_ = 0xFFFFFFFFu;
};

VkFormat to_vk_format(device::Format f);
VkFormat to_vk_depth_format(device::DepthFormat f);

}  // namespace magicblocks::backend
