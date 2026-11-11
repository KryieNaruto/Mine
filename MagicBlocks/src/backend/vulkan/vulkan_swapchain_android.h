// MagicBlocks —— Android surface TU 的内部声明(不对外)
#pragma once

#include "backend/vulkan/vulkan_target.h"
#include "mb_internal/magicblocks/device/types.h"

namespace magicblocks::backend {

device::Status create_android_surface(const VulkanContext& ctx, device::NativeSurface::Kind kind,
                                      void* window, VkSurfaceKHR* out_surface);

}  // namespace magicblocks::backend
