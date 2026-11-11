// MagicBlocks —— core/device_ops.h 的实现(backend 与公共面之间唯一被允许的翻译层)
//
// 这个文件是**唯一**被允许在 backend/ 之外写 `backend::` 的地方,理由见 device_ops.h。
// 门控(tools/check_layer_purity.py)对它仍然生效:它必须保持在"同一个文件里
// 把 backend 类型用完全,不把 backend 的**类型**漏进签名"这条线上 —— 所有对外
// 签名都只用公共面类型(IRenderDevice / ISwapchain / PipelineDesc / Result)。
#include "core/device_ops.h"

#include "backend/vulkan/vulkan_render_device.h"
#include "backend/vulkan/vulkan_swapchain.h"
#include "backend/vulkan/vulkan_target.h"
#include "backend/vulkan/vulkan_ui_overlay.h"

namespace magicblocks::core {

namespace {

// 是不是当前后端实现"的地方都走这里。不是 → nullptr,调用方退回公共面行为。
backend::VulkanRenderDevice* as_vulkan(device::IRenderDevice* d) {
  return dynamic_cast<backend::VulkanRenderDevice*>(d);
}
backend::VulkanSwapchain* as_vulkan(device::ISwapchain* s) {
  return dynamic_cast<backend::VulkanSwapchain*>(s);
}
const backend::VulkanSwapchain* as_vulkan(const device::ISwapchain* s) {
  return dynamic_cast<const backend::VulkanSwapchain*>(s);
}
backend::OffscreenTarget* as_offscreen(device::IRenderTarget* t) {
  return dynamic_cast<backend::OffscreenTarget*>(t);
}

// 把公共面的纯值枚举翻成 backend 的内部枚举。**这是唯一的一处翻译**:
// 往后端加一种自检图案时,只改这里和设备侧,公共面不动。
backend::OffscreenTarget::SelftestKind to_backend(SelftestKind k) {
  switch (k) {
    case SelftestKind::kL2Scene:
      return backend::OffscreenTarget::SelftestKind::kL2Scene;
    case SelftestKind::kL0Default:
    default:
      return backend::OffscreenTarget::SelftestKind::kL0Default;
  }
}

// OffscreenFrameRenderer 的实现:持有一个 backend::VulkanFrameRenderer。
//
// ⚠ 这里**必须**存 unique_ptr 而不是引用:VulkanFrameRenderer 的构造与析构
// 都要碰真实 GPU 资源,把它整体藏在这个 TU 里,上层连它多大都不知道。
class VulkanOffscreenFrameRenderer final : public OffscreenFrameRenderer {
 public:
  VulkanOffscreenFrameRenderer(const backend::VulkanContext& ctx, backend::OffscreenTarget* target)
      : renderer_(std::make_unique<backend::VulkanFrameRenderer>(ctx, target)) {}

  void reset_instances() override { renderer_->overlay_pass().reset_instances(); }

  device::Status append_rect(float x0, float y0, float x1, float y1, float r, float g, float b,
                             float a) override {
    return renderer_->overlay_pass().append_rect(x0, y0, x1, y1, r, g, b, a);
  }
  device::Status append_stroke(float x0, float y0, float x1, float y1, float r, float g, float b,
                               float a) override {
    return renderer_->overlay_pass().append_stroke(x0, y0, x1, y1, r, g, b, a);
  }
  device::Status append_line(float ax, float ay, float bx, float by, float width, float r, float g,
                             float b, float a) override {
    return renderer_->overlay_pass().append_line(ax, ay, bx, by, width, r, g, b, a);
  }
  bool render_to_memory(std::uint32_t width, std::uint32_t height, std::uint8_t* out_rgba8,
                        std::uint32_t row_bytes) override {
    return renderer_->render_to_memory(width, height, out_rgba8, row_bytes);
  }

 private:
  std::unique_ptr<backend::VulkanFrameRenderer> renderer_;
};

}  // namespace

device::Result<std::unique_ptr<device::IRenderDevice>> create_render_device(
    const device::DeviceDesc& desc) {
  // 只实现了 Vulkan;其它后端在 VulkanRenderDevice::init 里被拒为 Unsupported
  return backend::create_vulkan_device(desc);
}

device::Result<std::unique_ptr<device::ISwapchain>> create_swapchain_with_msaa(
    device::IRenderDevice* device, const device::NativeSurface& surface,
    device::Extent2D extent, std::uint32_t msaa_samples) {
  if (device == nullptr) return device::Status::Invalid("create_swapchain_with_msaa: device 为空");
  // 非当前后端:退回公共面入口(不带采样数),而不是假装支持 MSAA。
  auto* vkdev = as_vulkan(device);
  if (vkdev == nullptr) return device->create_swapchain(surface, extent);
  return vkdev->create_swapchain_with_msaa(surface, extent, msaa_samples);
}

std::uint32_t swapchain_msaa_samples(const device::ISwapchain* chain) {
  const auto* vk_chain = as_vulkan(chain);
  return vk_chain != nullptr ? vk_chain->msaa_samples() : 1u;
}

std::uint32_t swapchain_rotation_degrees(const device::ISwapchain* chain) {
  // 平台旋转标志(VK_SURFACE_TRANSFORM_*)在 VulkanSwapchain 里解码成角度,
  // 这样 VK_ 宏永远不会漏到 backend/ 之外。
  const auto* vk_chain = as_vulkan(chain);
  return vk_chain != nullptr ? vk_chain->surface_rotation_degrees() : 0u;
}

resources::Result<std::unique_ptr<resources::IPipeline>> create_swapchain_pipeline(
    device::IRenderDevice* device, const device::ISwapchain* chain,
    const resources::PipelineDesc& desc, std::uint32_t msaa_hint) {
  if (device == nullptr || chain == nullptr) {
    return resources::Status::Invalid("create_swapchain_pipeline: device/chain 为空");
  }
  auto* vkdev = as_vulkan(device);
  if (vkdev == nullptr) {
    // 非当前后端:没有"交换链形状"这个后端私有概念,退回公共管线工厂。
    // 这是**如实**的能力降级(而不是按交换链形状硬凑一个形状出来)。
    return device->create_pipeline(desc);
  }
  return vkdev->create_swapchain_pipeline(desc, *chain, msaa_hint);
}

device::Status draw_offscreen_selftest(device::IRenderTarget* target, std::uint64_t frame_index,
                                       SelftestKind kind) {
  if (target == nullptr) return device::Status::Invalid("draw_offscreen_selftest: target 为空");
  auto* off = as_offscreen(target);
  if (off == nullptr) {
    // 如实的能力降级:不是离屏目标就没有"自检图案"这回事。
    return device::Status::Unsupported("draw_offscreen_selftest: 目标不是当前后端的离屏目标");
  }
  return off->draw_selftest(frame_index, to_backend(kind));
}

device::Result<std::unique_ptr<OffscreenFrameRenderer>> create_offscreen_frame_renderer(
    device::IRenderDevice* device, device::IRenderTarget* target) {
  if (device == nullptr || target == nullptr) {
    return device::Status::Invalid("create_offscreen_frame_renderer: device/target 为空");
  }
  auto* vkdev = as_vulkan(device);
  auto* off = as_offscreen(target);
  if (vkdev == nullptr || off == nullptr) {
    return device::Status::Unsupported(
        "create_offscreen_frame_renderer: 当前后端没有离屏 UI 帧渲染器");
  }
  return std::unique_ptr<OffscreenFrameRenderer>(
      new VulkanOffscreenFrameRenderer(vkdev->context(), off));
}

std::uint64_t swapchain_current_image_value(const device::ISwapchain* chain) {
  // SwapchainImageProbe 是 backend 的私有钩子(见 backend/vulkan/vulkan_swapchain.h),
  // 在这里被问一次,出去就只剩一个 uint64。
  const backend::SwapchainImageProbe* probe =
      dynamic_cast<const backend::SwapchainImageProbe*>(chain);
  return probe != nullptr ? probe->current_image_value() : 0u;
}

}  // namespace magicblocks::core
