// MagicBlocks —— Vulkan 后端:实例 / 物理设备选择 / 逻辑设备 / 能力探测
//
// 关键设计点(对应 spec L0 D1/D2/D8):
//  - 设备**强筛**:require_software_rasterizer 时只接受软件光栅设备,绝不回退物理 GPU;
//  - 能力探测是**纯读取**,不依赖 VMA;
//  - 所有失败路径返回带 code + message 的 Status。
#include "backend/vulkan/vk_common.h"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "backend/vulkan/vulkan_resource_common.h"  // 按格式复核采样数(见 collect_msaa)
#include "mb_internal/magicblocks/device/render_device.h"

namespace magicblocks::backend {

using namespace magicblocks::device;

std::string vk_result_str(VkResult r) {
  switch (r) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_EVENT_SET: return "VK_EVENT_SET";
    case VK_EVENT_RESET: return "VK_EVENT_RESET";
    case VK_INCOMPLETE: return "VK_INCOMPLETE";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
    default: return "VkResult(" + std::to_string(static_cast<int>(r)) + ")";
  }
}

std::string fmt_err(const char* what, VkResult r) {
  return std::string("[vk] ") + what + ": " + vk_result_str(r);
}

bool is_software_rasterizer_name(const char* name, std::uint32_t device_type) {
  if (!name) return false;
  if (device_type == VK_PHYSICAL_DEVICE_TYPE_CPU) return true;
  const std::string n(name);
  return n.find("SwiftShader") != std::string::npos || n.find("swiftshader") != std::string::npos ||
         n.find("llvmpipe") != std::string::npos || n.find("lavapipe") != std::string::npos;
}

namespace {

// 查询某格式的深度/模板支持
bool has_depth_stencil(VkPhysicalDevice phys, VkFormat fmt) {
  VkFormatProperties fp{};
  vkGetPhysicalDeviceFormatProperties(phys, fmt, &fp);
  return (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
}

bool has_color_attachment(VkPhysicalDevice phys, VkFormat fmt) {
  VkFormatProperties fp{};
  vkGetPhysicalDeviceFormatProperties(phys, fmt, &fp);
  return (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0;
}

// VkSampleCountFlags → 采样数列表(只报告可同时作为颜色与深度附件的等级)
//
// 两阶段(2026-11-10 修 8x 静默降级的教训,别再退回只看全局限制的版本):
//   1) 先用 framebufferColorSampleCounts & framebufferDepthSampleCounts 取"全局上限";
//   2) 再用 vkGetPhysicalDeviceImageFormatProperties 对候选颜色格式**逐个**查实际支持,
//      与第 1 步**取交集**。
//
// 只用第 1 步不够:那是设备全局上限,与格式无关。设备完全可能"全局允许 8x",
// 而某个具体颜色格式只支持 1x|4x —— caps 于是把 8x 报成"支持",用户在 UI 里选了 8x,
// vkCreateImage 失败(验证层只打印不拦,返回值仍是 VK_SUCCESS),MS 图无效且没有任何日志,
// 表现为"开了 8x 还是有锯齿"。**用户实际会点的那条路径必须是对的。**
//
// 取交集而不是并集:只有"所有本项目会用到的颜色格式都支持"的等级才算真的可用;
// 取并集等于换个位置重犯同一个错。
//
// ⚠️ 诚实交代本轮验证覆盖面(别让后来人以为它被证明过):
//   本机(Windows + SwiftShader)的全局 framebufferColorSampleCounts & ...Depth 里
//   **本来就不含 8x**(实测旧实现与修复后实现都输出 "caps levels: 1 4")。
//   所以:**本层在本机零行为变化、零判别力**,针对它的注入自检在本机不可能变红,
//   这一点已向团队负责人明确报告,没有用假注入充数。
//   它的价值在于"全局允许但某格式不允许"的设备上守门(例如将来加 HDR/半浮点格式)。
//   真正的、**本机可验证**的运行时防线是 vulkan_swapchain.cpp 里建图前的
//   color_format_supports_samples() 兜底 —— 那条注入(color_format_supports_samples
//   恒返回 true)会让 MsaaSwapchain.UnsupportedLevelRejectedAtSwapchain 变红。
void collect_msaa(VkPhysicalDevice phys, const VkPhysicalDeviceLimits& lim, std::vector<u32>* out) {
  const VkSampleCountFlags color = lim.framebufferColorSampleCounts;
  const VkSampleCountFlags depth = lim.framebufferDepthSampleCounts;

  // 第 2 步:候选颜色格式的实际采样数交集。查询失败的格式直接剔除;
  // 四格式全失败时退回第 1 步(极端设备上不能让整条 caps 变空)。
  constexpr std::size_t kProbeCount = sizeof(kMsaaProbeColorFormats) / sizeof(VkFormat);
  VkSampleCountFlags fmt_mask = ~static_cast<VkSampleCountFlags>(0);
  std::size_t probed = 0;
  for (std::size_t i = 0; i < kProbeCount; ++i) {
    const VkSampleCountFlags counts = query_color_sample_counts(phys, kMsaaProbeColorFormats[i]);
    if (counts == 0) continue;  // 该格式不支持,剔除
    fmt_mask &= counts;
    ++probed;
  }
  const bool format_filtered = probed > 0;
  if (!format_filtered) fmt_mask = ~static_cast<VkSampleCountFlags>(0);

  auto push = [&](VkSampleCountFlags bit, u32 n) {
    if ((color & bit) && (depth & bit) && (fmt_mask & bit)) out->push_back(n);
  };
  push(VK_SAMPLE_COUNT_1_BIT, 1);
  push(VK_SAMPLE_COUNT_2_BIT, 2);
  push(VK_SAMPLE_COUNT_4_BIT, 4);
  push(VK_SAMPLE_COUNT_8_BIT, 8);
  push(VK_SAMPLE_COUNT_16_BIT, 16);

  // 1x 是底线:多采样全被格式否决时也要能开窗口。
  if (out->empty()) out->push_back(1);

  if (!format_filtered) {
    std::fprintf(stderr,
                 "[caps] 颜色格式采样数查询全部失败(%u 个候选),supported_msaa_levels 退回"
                 "「只看设备全局限制」的结果 —— 该结果可能与具体格式不符,仅作兜底。\n",
                 static_cast<unsigned>(kProbeCount));
  }
}

}  // namespace

void fill_device_caps(VkPhysicalDevice phys, const VkPhysicalDeviceProperties& props,
                      const VkPhysicalDeviceMemoryProperties& mem, bool has_memory_budget_ext,
                      bool memory_budget_from_budget_ext, DeviceCaps* out) {
  if (!out) return;
  out->device_name = props.deviceName;
  out->driver_name = "";
  out->api_version = props.apiVersion;
  out->driver_version = props.driverVersion;

  // --- 深度格式(D2):只报告真正能当深度附件用的 ---
  out->depth_formats.clear();
  if (has_depth_stencil(phys, VK_FORMAT_D16_UNORM)) out->depth_formats.push_back(DepthFormat::D16_UNORM);
  if (has_depth_stencil(phys, VK_FORMAT_D24_UNORM_S8_UINT))
    out->depth_formats.push_back(DepthFormat::D24_UNORM_S8_UINT);
  if (has_depth_stencil(phys, VK_FORMAT_D32_SFLOAT)) out->depth_formats.push_back(DepthFormat::D32_SFLOAT);

  // --- MSAA ---
  out->supported_msaa_levels.clear();
  collect_msaa(phys, props.limits, &out->supported_msaa_levels);

  // --- 纹理格式 ---
  out->texture_formats.clear();
  if (has_color_attachment(phys, VK_FORMAT_R8G8B8A8_UNORM)) out->texture_formats.push_back(Format::RGBA8_UNORM);
  if (has_color_attachment(phys, VK_FORMAT_B8G8R8A8_UNORM)) out->texture_formats.push_back(Format::BGRA8_UNORM);
  if (has_color_attachment(phys, VK_FORMAT_R8_UNORM)) out->texture_formats.push_back(Format::R8_UNORM);

  // --- reverse-Z:靠深度格式浮点能力推断(L0 无独立扩展依赖) ---
  out->rev_z_supported = has_depth_stencil(phys, VK_FORMAT_D32_SFLOAT);

  out->max_push_constant_size = props.limits.maxPushConstantsSize;
  out->max_uniform_buffer_range = props.limits.maxUniformBufferRange;
  out->timestamp_queries = props.limits.timestampComputeAndGraphics != 0u;  // 压缩纹理:抽查 ET C2 与 BC1 任一可用
  out->compressed_textures = has_color_attachment(phys, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK) ||
                             has_color_attachment(phys, VK_FORMAT_BC1_RGB_UNORM_BLOCK);

  // --- 显存预算(A③/E1b:双来源) ---
  u64 heap_sum = 0;
  for (u32 i = 0; i < mem.memoryHeapCount; ++i) heap_sum += mem.memoryHeaps[i].size;
  out->memory_heap_total = heap_sum;
  if (memory_budget_from_budget_ext) {
    // 调用方已通过 VK_EXT_memory_budget 求和填了 memory_budget;此处只标记来源
    out->memory_budget_from_extension = true;
  } else {
    out->memory_budget = heap_sum;
    out->memory_budget_from_extension = false;
  }
  (void)has_memory_budget_ext;

  out->is_software_rasterizer = is_software_rasterizer_name(props.deviceName, props.deviceType);
}

}  // namespace magicblocks::backend
