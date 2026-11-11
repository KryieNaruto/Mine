// MagicBlocks —— Vulkan 后端:实例创建 + 平台 surface 扩展选择
#pragma once

#include <string>
#include <vector>

#include "backend/vulkan/vk_common.h"

namespace magicblocks::backend {

struct InstanceBundle {
  VkInstance instance = VK_NULL_HANDLE;
  VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
  std::uint32_t error_count = 0;     // 校验层 error 条数(可计量判据)
  std::uint32_t message_count = 0;   // 回调总数(消息数量本身不能证明层在不在)
  // 层是否真的被启用(实例创建时已知事实,不依赖是否产生过消息)。
  // 这才是「校验层就绪」的直接证据：层没挂上时此值为 false。
  bool validation_enabled = false;
};

// 创建实例。enable_validation=true 时启用 VK_LAYER_KHRONOS_validation,
// 并通过 VkValidationFeaturesEXT 打开 best_practices(可计量的泄漏判据)。
bool create_instance(bool enable_validation, std::uint32_t api_version, InstanceBundle* out,
                     std::string* err);

// 销毁实例及其 messenger。
void destroy_instance(InstanceBundle* b);

// 平台 surface 扩展名(RTII:win32 / xcb / wayland)。
const char* platform_surface_extension();

}  // namespace magicblocks::backend
