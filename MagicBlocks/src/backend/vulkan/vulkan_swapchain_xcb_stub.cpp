// MagicBlocks —— Vulkan 后端:Xcb surface 的**占位实现**(非 Linux 构建)
//
// 目的:让 VulkanSwapchain::init() 无需 #if 分支——三个 create_*_surface 在任何平台
// 都有定义。非 Linux 构建下这里明确返回 Unsupported,不是"静默降级"。
#include "backend/vulkan/vulkan_swapchain_xcb.h"

#include "backend/vulkan/vulkan_swapchain.h"

namespace magicblocks::backend {

#if !defined(VK_USE_PLATFORM_XCB_KHR)
Status create_xcb_surface(const VulkanContext& ctx, void* connection, std::uintptr_t window,
                          VkSurfaceKHR* out_surface) {
  (void)ctx;
  (void)connection;
  (void)window;
  (void)out_surface;
  return Status::Unsupported("create_xcb_surface: 本构建未启用 Xcb surface");
}
#endif

}  // namespace magicblocks::backend
