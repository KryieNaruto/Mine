// MagicBlocks —— Vulkan 后端:Win32 surface TU
//
// 只有本文件知道 HWND/HINSTANCE;公共面零平台宏。
// 非 Windows 构建时本文件的函数体被整体排除,由 vulkan_swapchain_win32_stub.cpp 提供定义。
#include "backend/vulkan/vulkan_swapchain_win32.h"

#include "backend/vulkan/vulkan_swapchain.h"

#if defined(VK_USE_PLATFORM_WIN32_KHR)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace magicblocks::backend {

#if defined(VK_USE_PLATFORM_WIN32_KHR)
Status create_win32_surface(const VulkanContext& ctx, void* hwnd, std::uintptr_t hinstance,
                            VkSurfaceKHR* out_surface) {
  if (!out_surface) return Status::Invalid("create_win32_surface: out 为空");
  if (!hwnd) return Status::Invalid("create_win32_surface: HWND 为空");
  VkWin32SurfaceCreateInfoKHR sci{};
  sci.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
  sci.hinstance = reinterpret_cast<HINSTANCE>(hinstance);
  sci.hwnd = reinterpret_cast<HWND>(hwnd);
  VkResult r = vkCreateWin32SurfaceKHR(ctx.instance, &sci, nullptr, out_surface);
  if (r != VK_SUCCESS) return Status::InitFailed(fmt_err("vkCreateWin32SurfaceKHR", r));
  return Status::Ok_();
}
#endif

}  // namespace magicblocks::backend
