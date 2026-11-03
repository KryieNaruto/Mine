// MagicBlocks —— Vulkan 后端:Xcb surface TU(Linux)
//
// 非 Linux 构建时本文件的函数体被整体排除,由 vulkan_swapchain_xcb_stub.cpp 提供定义。
#include "backend/vulkan/vulkan_swapchain_xcb.h"

#include "backend/vulkan/vulkan_swapchain.h"

#if defined(VK_USE_PLATFORM_XCB_KHR)
#include <xcb/xcb.h>
#endif

namespace magicblocks::backend {

#if defined(VK_USE_PLATFORM_XCB_KHR)
Status create_xcb_surface(const VulkanContext& ctx, void* connection, std::uintptr_t window,
                          VkSurfaceKHR* out_surface) {
  if (!out_surface) return Status::Invalid("create_xcb_surface: out 为空");
  if (!connection) return Status::Invalid("create_xcb_surface: xcb_connection_t 为空");
  VkXcbSurfaceCreateInfoKHR sci{};
  sci.sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR;
  sci.connection = static_cast<xcb_connection_t*>(connection);
  sci.window = static_cast<xcb_window_t>(window);
  VkResult r = vkCreateXcbSurfaceKHR(ctx.instance, &sci, nullptr, out_surface);
  if (r != VK_SUCCESS) return Status::InitFailed(fmt_err("vkCreateXcbSurfaceKHR", r));
  return Status::Ok_();
}
#endif

}  // namespace magicblocks::backend
