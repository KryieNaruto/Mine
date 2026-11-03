// MagicBlocks —— Vulkan 后端公共件(backend 内部头,不属于 SDK 公共面)
//
// G3:所有 Vk* 类型只允许出现在 src/backend/** 内。
// 本文件把"公共头的 device 类型"与"Vulkan 句柄"之间的边界的**唯一**翻译点集中起来:
// 公共头只用 void*/uintptr_t,从不出现 Vk*。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

// VK_USE_PLATFORM_* 由 CMake 按 OS 注入(target_compile_definitions)
#include <vulkan/vulkan.h>

namespace magicblocks::device {
struct DeviceCaps;
}

namespace magicblocks::backend {

// 把 Vk 错误码转成人类可读串(错误日志用)。
std::string vk_result_str(VkResult r);

// 追加错误:统一格式 "[vk] <what>: <detail>"
std::string fmt_err(const char* what, VkResult r);

// 判断设备名是否软件光栅(SwiftShader / lavapipe / llvmpipe)。
bool is_software_rasterizer_name(const char* name, std::uint32_t device_type);

// 把物理设备能力填进公共 DeviceCaps(D2)。
void fill_device_caps(VkPhysicalDevice phys, const VkPhysicalDeviceProperties& props,
                      const VkPhysicalDeviceMemoryProperties& mem, bool has_memory_budget_ext,
                      bool memory_budget_from_budget_ext, device::DeviceCaps* out);

}  // namespace magicblocks::backend
