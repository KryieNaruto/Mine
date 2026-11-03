// MagicBlocks —— Xcb surface TU 的内部声明
#pragma once

#include <cstdint>

#include "backend/vulkan/vulkan_target.h"

namespace magicblocks::backend {

device::Status create_xcb_surface(const VulkanContext& ctx, void* connection, std::uintptr_t window,
                                  VkSurfaceKHR* out_surface);

}  // namespace magicblocks::backend
