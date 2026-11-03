// MagicBlocks —— Vulkan 后端:实例与校验层
#include "backend/vulkan/vulkan_instance.h"

#include <cstring>

namespace magicblocks::backend {

namespace {

VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*types*/,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void* user_data) {
  auto* b = static_cast<InstanceBundle*>(user_data);
  if (!b) return VK_FALSE;
  b->message_count++;
  if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
    b->error_count++;
  }
  (void)data;
  return VK_FALSE;  // 不中止
}

bool has_layer(const char* name) {
  std::uint32_t n = 0;
  if (vkEnumerateInstanceLayerProperties(&n, nullptr) != VK_SUCCESS || n == 0) return false;
  std::vector<VkLayerProperties> props(n);
  if (vkEnumerateInstanceLayerProperties(&n, props.data()) != VK_SUCCESS) return false;
  for (const auto& p : props) {
    if (std::strcmp(p.layerName, name) == 0) return true;
  }
  return false;
}

bool has_instance_extension(const char* name) {
  std::uint32_t n = 0;
  if (vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr) != VK_SUCCESS || n == 0) return false;
  std::vector<VkExtensionProperties> exts(n);
  if (vkEnumerateInstanceExtensionProperties(nullptr, &n, exts.data()) != VK_SUCCESS) return false;
  for (const auto& e : exts) {
    if (std::strcmp(e.extensionName, name) == 0) return true;
  }
  return false;
}

}  // namespace

const char* platform_surface_extension() {
#if defined(VK_USE_PLATFORM_WIN32_KHR)
  return VK_KHR_WIN32_SURFACE_EXTENSION_NAME;
#elif defined(VK_USE_PLATFORM_XCB_KHR)
  return VK_KHR_XCB_SURFACE_EXTENSION_NAME;
#else
  return nullptr;
#endif
}

bool create_instance(bool enable_validation, std::uint32_t api_version, InstanceBundle* out,
                     std::string* err) {
  VkApplicationInfo app{};
  app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app.pApplicationName = "MagicBlocks";
  app.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
  app.pEngineName = "MagicBlocks";
  app.engineVersion = VK_MAKE_VERSION(1, 0, 0);
  // SwiftShader 实测 apiVersion=1.3.0;请求 1.0 保证跨驱动可用
  app.apiVersion = VK_MAKE_VERSION(1, 0, 0);
  (void)api_version;

  std::vector<const char*> exts;
  exts.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
  if (const char* plat = platform_surface_extension()) exts.push_back(plat);

  std::vector<const char*> layers;
  bool validation = false;
  if (enable_validation && has_layer("VK_LAYER_KHRONOS_validation")) {
    layers.push_back("VK_LAYER_KHRONOS_validation");
    validation = true;
    if (has_instance_extension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
      exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }
  }

  // best_practices 让校验层把「对象未销毁」等泄漏问题报为 error/warning —— 可计量判据。
  VkValidationFeatureEnableEXT enables[] = {VK_VALIDATION_FEATURE_ENABLE_BEST_PRACTICES_EXT};
  VkValidationFeaturesEXT vf{};
  vf.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT;
  vf.enabledValidationFeatureCount = validation ? 1u : 0u;
  vf.pEnabledValidationFeatures = validation ? enables : nullptr;

  VkInstanceCreateInfo ci{};
  ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  ci.pNext = validation ? &vf : nullptr;
  ci.pApplicationInfo = &app;
  ci.enabledExtensionCount = static_cast<std::uint32_t>(exts.size());
  ci.ppEnabledExtensionNames = exts.data();
  ci.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
  ci.ppEnabledLayerNames = layers.empty() ? nullptr : layers.data();

  if (vkCreateInstance(&ci, nullptr, &out->instance) != VK_SUCCESS) {
    if (err) *err = fmt_err("vkCreateInstance", VK_ERROR_INITIALIZATION_FAILED);
    return false;
  }

  if (validation && has_instance_extension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
    VkDebugUtilsMessengerCreateInfoEXT mci{};
    mci.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    mci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
    mci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                      VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                      VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    mci.pfnUserCallback = &debug_callback;
    mci.pUserData = out;
    auto create_fn = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(out->instance, "vkCreateDebugUtilsMessengerEXT"));
    if (create_fn) create_fn(out->instance, &mci, nullptr, &out->messenger);
  }
  return true;
}

void destroy_instance(InstanceBundle* b) {
  if (!b->instance) return;
  if (b->messenger) {
    auto fn = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(b->instance, "vkDestroyDebugUtilsMessengerEXT"));
    if (fn) fn(b->instance, b->messenger, nullptr);
    b->messenger = VK_NULL_HANDLE;
  }
  vkDestroyInstance(b->instance, nullptr);
  b->instance = VK_NULL_HANDLE;
}

}  // namespace magicblocks::backend
