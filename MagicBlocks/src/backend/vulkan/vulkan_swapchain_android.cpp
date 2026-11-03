// MagicBlocks —— Vulkan 后端:Android surface TU(语法校验载体)
//
// A7 / Swapchain.AndroidTuCompiles:本文件在**所有平台**都参与编译校验
// (MB_ANDROID_TU_COMPILE_CHECK,默认 ON),这样 Android 分支不会在
// Windows/Linux CI 上悄悄腐烂。
//   - 有 NDK:MB_ANDROID_USE_NDK → 真实 <android/native_window.h> + <vulkan/vulkan_android.h>
//   - 无 NDK:MB_ANDROID_STUB_HEADER → stub/anativewindow.h + 本文件内的等价 ABI 声明
//
// 两条路径都只依赖 `ANativeWindow*` 这个指针类型,不引用任何 NDK 函数。
#include "backend/vulkan/vulkan_swapchain_android.h"

#include "backend/vulkan/vulkan_swapchain.h"

#if defined(MB_ANDROID_USE_NDK)
#include <android/native_window.h>
#include <vulkan/vulkan_android.h>
#else
#include "backend/vulkan/stub/anativewindow.h"

// 非 NDK 构建没有 vulkan_android.h:按 Khronos 官方定义复刻结构体与常量,
// 只做类型/字段/调用形状的校验,不产生任何运行期代码路径。
//   typedef struct VkAndroidSurfaceCreateInfoKHR {
//       VkStructureType sType;   // 1000008000
//       const void*     pNext;
//       ANativeWindow*  window;
//   } VkAndroidSurfaceCreateInfoKHR;
struct VkAndroidSurfaceCreateInfoKHR {
  VkStructureType sType;
  const void* pNext;
  ANativeWindow* window;
};
#define VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR \
  static_cast<VkStructureType>(1000008000)
#endif

namespace magicblocks::backend {

Status create_android_surface(const VulkanContext& ctx, device::NativeSurface::Kind kind,
                              void* window, VkSurfaceKHR* out_surface) {
  if (!out_surface) return device::Status::Invalid("create_android_surface: out 为空");
  if (kind != device::NativeSurface::Kind::Android) {
    return device::Status::Unsupported("create_android_surface: surface kind 不是 Android");
  }
  if (!window) return device::Status::Invalid("create_android_surface: ANativeWindow 为空");

  VkAndroidSurfaceCreateInfoKHR sci{};
  sci.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
  sci.window = static_cast<ANativeWindow*>(window);

#if defined(MB_ANDROID_USE_NDK)
  VkResult r = vkCreateAndroidSurfaceKHR(ctx.instance, &sci, nullptr, out_surface);
  if (r != VK_SUCCESS) return device::Status::InitFailed(fmt_err("vkCreateAndroidSurfaceKHR", r));
#else
  // 语法校验构建:无 loader 符号。参数形状已经校验完,这里明确失败而不是假装成功
  // (假成功会变成"兜底通过",违反不回退原则)。
  (void)ctx;
  (void)sci;
  return device::Status::Unsupported(
      "create_android_surface: 本构建无 Android loader 符号(仅语法校验)");
#endif
  return device::Status::Ok_();
}

}  // namespace magicblocks::backend
