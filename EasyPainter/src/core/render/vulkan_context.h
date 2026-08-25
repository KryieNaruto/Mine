#pragma once

#include <vector>

#include <vulkan/vulkan.h>

namespace easypainter::render {

// RAII 的 Vulkan instance/device/queue/命令池。
// headless 离屏与 windowed(带 surface 扩展 + present 队列)共用。
class VulkanContext {
 public:
  VulkanContext() = default;
  ~VulkanContext();
  VulkanContext(const VulkanContext&) = delete;
  VulkanContext& operator=(const VulkanContext&) = delete;

  // 便捷:init_instance({}) + init_device(VK_NULL_HANDLE),headless 用。
  bool init();

  // 创建 instance(可带额外扩展,如 surface/xlib_surface)。成功返回 true。
  bool init_instance(const std::vector<const char*>& instance_extensions = {});

  // 选物理设备(需图形队列;surface 非空时要求该队列族支持 present)+ 建逻辑设备/queue + 命令池。
  // device_extensions 为逻辑设备扩展(如 windowed 需 VK_KHR_SWAPCHAIN_EXTENSION_NAME)。
  bool init_device(VkSurfaceKHR surface = VK_NULL_HANDLE,
                   const std::vector<const char*>& device_extensions = {});

  VkInstance instance() const { return instance_; }
  VkPhysicalDevice physical_device() const { return physical_device_; }
  VkDevice device() const { return device_; }
  uint32_t queue_family() const { return queue_family_; }
  VkQueue queue() const { return queue_; }
  VkCommandPool command_pool() const { return command_pool_; }

 private:
  VkInstance instance_ = VK_NULL_HANDLE;
  VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  uint32_t queue_family_ = 0;
  VkQueue queue_ = VK_NULL_HANDLE;
  VkCommandPool command_pool_ = VK_NULL_HANDLE;
};

}  // namespace easypainter::render
