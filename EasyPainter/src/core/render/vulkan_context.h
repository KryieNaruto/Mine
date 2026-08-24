#pragma once

#include <vulkan/vulkan.h>

namespace easypainter::render {

// RAII 的 Vulkan instance/device/queue(无 surface;headless 离屏与 windowed 共用)。
class VulkanContext {
 public:
  VulkanContext() = default;
  ~VulkanContext();
  VulkanContext(const VulkanContext&) = delete;
  VulkanContext& operator=(const VulkanContext&) = delete;

  // 创建 instance + 选物理设备 + 建逻辑设备/queue + 命令池。成功返回 true。
  bool init();

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
