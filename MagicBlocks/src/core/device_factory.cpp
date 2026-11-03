// MagicBlocks —— 后端工厂入口(非 backend 文件,只做转发)
//
// G3 要求:公共面不出现 Vk*。本文件只把 create_render_device 转发给 Vulkan 实现,
// 里面没有任何 Vulkan 类型。
#include "magicblocks/device/render_device.h"

namespace magicblocks::backend {
device::Result<std::unique_ptr<device::IRenderDevice>> create_vulkan_device(
    const device::DeviceDesc& desc);
}  // namespace magicblocks::backend

namespace magicblocks::device {

Result<std::unique_ptr<IRenderDevice>> create_render_device(const DeviceDesc& desc) {
  // 只实现了 Vulkan;其它后端在 VulkanRenderDevice::init 里被拒为 Unsupported
  return backend::create_vulkan_device(desc);
}

}  // namespace magicblocks::device
