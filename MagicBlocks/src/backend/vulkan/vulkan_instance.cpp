// MagicBlocks —— Vulkan 后端:实例与校验层
#include "backend/vulkan/vulkan_instance.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace magicblocks::backend {

namespace {

// 校验层消息输出开关。
// MAGICBLOCKS_VALIDATION_QUIET=1 静音 INFO/VERBOSE(ERROR/WARNING 照常输出)，
// CI 里良性告警刷屏时可用；静音不影响计数。
bool validation_quiet() {
  const char* v = std::getenv("MAGICBLOCKS_VALIDATION_QUIET");
  return v != nullptr && v[0] == '1' && v[1] == '\0';
}

const char* severity_name(VkDebugUtilsMessageSeverityFlagBitsEXT s) {
  if (s & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) return "ERROR";
  if (s & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) return "WARN";
  if (s & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) return "INFO";
  return "VERBOSE";
}

VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT types,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void* user_data) {
  auto* b = static_cast<InstanceBundle*>(user_data);
  if (!b) return VK_FALSE;
  b->message_count++;
  const bool is_error = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0;
  const bool is_warning = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0;
  if (is_error) {
    b->error_count++;
  }

  // ERROR/WARNING 一律输出(这两档是判据)；INFO/VERBOSE 只在没静音时输出。
  if ((is_error || is_warning || !validation_quiet()) && data != nullptr) {
    const char* type = "GENERAL";
    if (types & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) {
      type = "VALIDATION";
    } else if (types & VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT) {
      type = "PERFORMANCE";
    }
    std::fprintf(stderr, "[vk-%s][%s] %s\n", severity_name(severity), type,
                 data->pMessage != nullptr ? data->pMessage : "(no message)");
    std::fflush(stderr);
  }
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
#if defined(VK_USE_PLATFORM_ANDROID_KHR)
  return VK_KHR_ANDROID_SURFACE_EXTENSION_NAME;
#elif defined(VK_USE_PLATFORM_WIN32_KHR)
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
  // 请求的 API 版本由调用方决定(**必须真的用上**,见下面 api_version 的说明)。
  // 早期版本这里硬编码 1.0 并 (void)api_version,导致:
  //   - VulkanPagingAllocator 的 VMA 声明 vulkanApiVersion = 1.1
  //   - L1/L2 资源路径调用 1.1 才提升为核心的 vkGetPhysicalDevice*2()
  // 两者与实例协商出的 1.0 不一致 → 驱动/VMA 内部状态被破坏 →
  // 表现为「创建设备后一切正常,但创建过命令列表再析构设备时堆损坏
  // (0xc0000374)」。崩在 vkDestroyDevice,根因在实例创建,极难定位。
  // 现在严格使用调用方给的版本(默认 1.1,SwiftShader 支持 1.3)。
  app.apiVersion = (api_version != 0) ? api_version : VK_API_VERSION_1_1;

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
  // 直接证据:层确实被交给 vkCreateInstance 了。
  // 不能用 message_count>0 做判据 —— 代码干净的时候就一条消息都没有。
  out->validation_enabled = validation;
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
