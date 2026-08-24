#include "core/render/vulkan_context.h"

#include <cstdio>
#include <vector>

namespace easypainter::render {

namespace {
constexpr const char* kAppName = "easypainter";
}  // namespace

VulkanContext::~VulkanContext() {
  if (command_pool_ != VK_NULL_HANDLE) vkDestroyCommandPool(device_, command_pool_, nullptr);
  if (device_ != VK_NULL_HANDLE) vkDestroyDevice(device_, nullptr);
  if (instance_ != VK_NULL_HANDLE) vkDestroyInstance(instance_, nullptr);
}

bool VulkanContext::init() {
  VkApplicationInfo app{};
  app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app.pApplicationName = kAppName;
  app.applicationVersion = 1;
  app.pEngineName = kAppName;
  app.engineVersion = 1;
  app.apiVersion = VK_API_VERSION_1_0;

  VkInstanceCreateInfo ici{};
  ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  ici.pApplicationInfo = &app;
  if (vkCreateInstance(&ici, nullptr, &instance_) != VK_SUCCESS) {
    std::fprintf(stderr, "[vulkan] vkCreateInstance failed\n");
    return false;
  }

  uint32_t n = 0;
  if (vkEnumeratePhysicalDevices(instance_, &n, nullptr) != VK_SUCCESS || n == 0) {
    std::fprintf(stderr, "[vulkan] no physical device\n");
    return false;
  }
  std::vector<VkPhysicalDevice> devices(n);
  vkEnumeratePhysicalDevices(instance_, &n, devices.data());
  physical_device_ = devices[0];

  uint32_t qn = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &qn, nullptr);
  std::vector<VkQueueFamilyProperties> qprops(qn);
  vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &qn, qprops.data());
  bool found = false;
  for (uint32_t i = 0; i < qn; ++i) {
    if (qprops[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
      queue_family_ = i;
      found = true;
      break;
    }
  }
  if (!found) {
    std::fprintf(stderr, "[vulkan] no graphics queue family\n");
    return false;
  }

  const float priority = 1.0f;
  VkDeviceQueueCreateInfo qci{};
  qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  qci.queueFamilyIndex = queue_family_;
  qci.queueCount = 1;
  qci.pQueuePriorities = &priority;

  VkDeviceCreateInfo dci{};
  dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  if (vkCreateDevice(physical_device_, &dci, nullptr, &device_) != VK_SUCCESS) {
    std::fprintf(stderr, "[vulkan] vkCreateDevice failed\n");
    return false;
  }
  vkGetDeviceQueue(device_, queue_family_, 0, &queue_);

  VkCommandPoolCreateInfo cpi{};
  cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  cpi.queueFamilyIndex = queue_family_;
  if (vkCreateCommandPool(device_, &cpi, nullptr, &command_pool_) != VK_SUCCESS) {
    std::fprintf(stderr, "[vulkan] vkCreateCommandPool failed\n");
    return false;
  }
  return true;
}

}  // namespace easypainter::render
