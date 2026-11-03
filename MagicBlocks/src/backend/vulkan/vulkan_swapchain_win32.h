// MagicBlocks —— Win32 surface TU 的内部声明
#pragma once

#include <cstdint>

#include "backend/vulkan/vulkan_target.h"

namespace magicblocks::backend {

device::Status create_win32_surface(const VulkanContext& ctx, void* hwnd,
                                    std::uintptr_t hinstance, VkSurfaceKHR* out_surface);

}  // namespace magicblocks::backend
