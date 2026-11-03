// MagicBlocks —— Vulkan 后端:Win32 surface 的**占位实现**(非 Windows 构建)
//
// 与 xcb_stub 同理:保证 init() 在三平台上无 #if,且缺符号不会变成链接错误。
#include "backend/vulkan/vulkan_swapchain_win32.h"

#include "backend/vulkan/vulkan_swapchain.h"

namespace magicblocks::backend {

#if !defined(VK_USE_PLATFORM_WIN32_KHR)
Status create_win32_surface(const VulkanContext& ctx, void* hwnd, std::uintptr_t hinstance,
                            VkSurfaceKHR* out_surface) {
  (void)ctx;
  (void)hwnd;
  (void)hinstance;
  (void)out_surface;
  return Status::Unsupported("create_win32_surface: 本构建未启用 Win32 surface");
}
#endif

}  // namespace magicblocks::backend
