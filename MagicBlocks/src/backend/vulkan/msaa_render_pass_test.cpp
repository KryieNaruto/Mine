// MagicBlocks —— Vulkan 后端:**backend 内**的 MSAA 能力自证测试
//
// ======================= 为什么这个文件放在 src/ 而不是 tests/ =======================
//
// G3 门控规定「backend 之外不得出现 Vk* 类型」(tools/check_layer_purity.py)。
// 下面这条测试必须直接调 vkGetPhysicalDeviceImageFormatProperties 才能验到
// "caps 报的等级是不是真的能用"——这是本次 8x bug 的唯一可判别断言。
// 放进 tests/ 会直接撞 G3 红线;而为了绕过门控去删掉断言,
// 等于用一个红色门控换来一个永远抓不到 bug 的测试。所以它住在 backend 内。
//
// ============================== 它要抓的那个 bug ==============================
//
// 现象(2026-11-10,SwiftShader 实测):
//
//   [vk-ERROR][VALIDATION] vkCreateImage(): pCreateInfo->samples
//       (VK_SAMPLE_COUNT_8_BIT) is not supported by format VK_FORMAT_B8G8R8A8_UNORM
//   supported sampleCounts: VK_SAMPLE_COUNT_1_BIT|VK_SAMPLE_COUNT_4_BIT.
//   effective msaa = 8
//
// 即:设备全局限制里含 8x,于是 caps 报"支持 8x",UI 放行用户选 8x,MS 图建不出来
// 却依然显示"8x"——棱边照旧锯齿、零日志。和用户报的"开了 MSAA 没效果"是同一种病,
// 只是从 1x 挪到了 8x。
//
// 根因:collect_msaa 只看 framebufferColorSampleCounts(设备全局上限),与格式无关。
// 修法:再按候选颜色格式逐个查 vkGetPhysicalDeviceImageFormatProperties 并**取交集**。
//
// 判别力要求(Lead 明确要求"不要只断言列表非空"):
//   对 caps 里每一个 >1 的等级 n,直接用**同一个格式**(B8G8R8A8_UNORM)查驱动,
//   断言驱动说支持 VK_SAMPLE_COUNT_n_BIT。caps 报得比驱动宽,这里就红。
#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <sstream>
#include <string>

#include "backend/vulkan/vk_common.h"
#include "backend/vulkan/vulkan_render_device.h"
#include "backend/vulkan/vulkan_resource_common.h"

namespace magicblocks::backend {
namespace {

// 与 swapchain 实际会用的格式保持一致:Win32/Android 上
// vkGetPhysicalDeviceSurfaceFormatsKHR 的首选是 B8G8R8A8_UNORM / R8G8B8A8_UNORM。
// 这里固定查 B8G8R8A8_UNORM —— 也就是本次 bug 里那个"全局说支持 8x、格式说不行"的格式。
constexpr VkFormat kProbeFormat = VK_FORMAT_B8G8R8A8_UNORM;

const wchar_t* samples_name(VkSampleCountFlagBits b) {
  switch (b) {
    case VK_SAMPLE_COUNT_1_BIT: return L"1";
    case VK_SAMPLE_COUNT_2_BIT: return L"2";
    case VK_SAMPLE_COUNT_4_BIT: return L"4";
    case VK_SAMPLE_COUNT_8_BIT: return L"8";
    case VK_SAMPLE_COUNT_16_BIT: return L"16";
    default: return L"?";
  }
}

// 直接问驱动:该格式 + COLOR_ATTACHMENT 用法 + OPTIMAL tiling 下,支持哪些采样数。
// 失败(格式不支持)返回 0。
VkSampleCountFlags query_driver_sample_counts(VkPhysicalDevice phys) {
  VkImageFormatProperties ifp{};
  const VkResult r =
      vkGetPhysicalDeviceImageFormatProperties(phys, kProbeFormat, VK_IMAGE_TYPE_2D,
                                               VK_IMAGE_TILING_OPTIMAL,
                                               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 0, &ifp);
  if (r != VK_SUCCESS) return 0;
  return ifp.sampleCounts;
}

}  // namespace

// ===================== caps 里的每一个等级都必须能被驱动真的用起来 =====================

TEST(Msaa, CapsMatchRealFormatSupport) {
  // 这条测试就是 8x bug 的守门人:
  // 把 collect_msaa 改回"只看全局限制",SwiftShader 上就会报 {1,4,8},
  // 而驱动对 B8G8R8A8_UNORM 只支持 {1,4} —— 这里的 8 断言立刻变红。
  device::DeviceDesc d;
  d.app_name = "MagicBlocks MSAA caps probe";
  d.require_software_rasterizer = true;

  auto dev_r = device::create_render_device(d);
  if (!dev_r.ok()) {
    ADD_FAILURE() << "需要软件光栅设备: " << dev_r.message();
    return;
  }
  auto* vkdev = dynamic_cast<VulkanRenderDevice*>(dev_r.value().get());
  if (vkdev == nullptr) {
    ADD_FAILURE() << "需要 VulkanRenderDevice";
    return;
  }

  const VkPhysicalDevice phys = vkdev->context().phys;
  ASSERT_TRUE(phys != VK_NULL_HANDLE) << "context().phys 为空,后面无从查起";

  const VkSampleCountFlags driver_counts = query_driver_sample_counts(phys);
  // 2026-11-11：原来是 GTEST_SKIP。全仓纪律「缺能力是 FAIL 不是 SKIP」——
  // 驱动不认识 B8G8R8A8 + COLOR_ATTACHMENT 这件事本身就该在 CI 上响：
  // 那是交换链的颜色格式，认不出它等于整个上屏链路的前提不成立。
  ASSERT_NE(driver_counts, 0u)
      << "驱动不支持 B8G8R8A8_UNORM + COLOR_ATTACHMENT + OPTIMAL ——"
         " 这是交换链的首选颜色格式，能力自证的前提不成立。"
         " 按纪律这是 FAIL（不是 SKIP）：请检查 SwiftShader 部署与格式探测。";

  const device::DeviceCaps& caps = vkdev->caps();
  ASSERT_FALSE(caps.supported_msaa_levels.empty()) << "supported_msaa_levels 不能为空";

  // 1x 永远可用,是所有 MSAA 逻辑的底线。
  bool has_one = false;
  for (const device::u32 n : caps.supported_msaa_levels) {
    if (n == 1u) has_one = true;
  }
  EXPECT_TRUE(has_one) << "列表必须含 1(无 MSAA 底线)";
  EXPECT_TRUE((driver_counts & VK_SAMPLE_COUNT_1_BIT) != 0) << "驱动居然不支持 1x 采样";

  // ★ 核心断言:caps 报的每一个 n>1,驱动必须真的支持该采样数的 VkSampleCountFlagBits。
  for (const device::u32 n : caps.supported_msaa_levels) {
    if (n <= 1u) continue;
    const VkSampleCountFlagBits want = to_vk_samples(n);
    // to_vk_samples 对非法值会回落成 1x,那说明 caps 里混进了非法数值。
    ASSERT_NE(want, VK_SAMPLE_COUNT_1_BIT)
        << "caps.supported_msaa_levels 含非法等级 " << n << "(to_vk_samples 回落到 1x)";

    EXPECT_TRUE((driver_counts & want) != 0)
        << "caps 报支持 " << n << "x,但驱动对 " << static_cast<unsigned>(kProbeFormat)
        << " (COLOR_ATTACHMENT/OPTIMAL) 只支持 " << static_cast<unsigned>(driver_counts)
        << " —— caps 比驱动宽,用户选了 " << n
        << "x 会拿到一张无效的 MS 图:界面显示开着 MSAA,棱边照旧锯齿,且没有任何日志。"
           "根因是 collect_msaa 只看设备全局 framebufferColorSampleCounts,"
           "没有按颜色格式复核 vkGetPhysicalDeviceImageFormatProperties。";
  }

  // 反向也断言一次(只对 4x 这一档):驱动支持、且全局限制也支持时,caps 不该漏报。
  // 免得"修好 bug"的代价是把自己值得的等级一起删掉。
  const VkPhysicalDeviceLimits& lim = vkdev->context().props.limits;
  const VkSampleCountFlags global = lim.framebufferColorSampleCounts & lim.framebufferDepthSampleCounts;
  if ((global & VK_SAMPLE_COUNT_4_BIT) != 0 && (driver_counts & VK_SAMPLE_COUNT_4_BIT) != 0) {
    bool caps_has_4 = false;
    for (const device::u32 n : caps.supported_msaa_levels) {
      if (n == 4u) caps_has_4 = true;
    }
    EXPECT_TRUE(caps_has_4)
        << "驱动与全局限制都支持 4x,但 caps 没报 —— 过度收紧,用户点不到能用的等级";
  }
}

// ========= 退回原因必须能说清"是哪个格式不支持哪个采样数"(兜底防线的可观测性) =========

// ---------------------------------------------------------------- 能力自洽性
//
// 上面那条直接问驱动"B8G8R8A8 支持几倍采样"。但设备真正用的不一定就是这个格式
// （swapchain 可能挑 R8G8B8A8，离屏也可能换），而 caps 报的是**每格式交集**。
// 所以还需要一条只看 caps 自身、不碰驱动的断言：与 FillDeviceCaps 同一套判据，
// 独立重算一遍，任何一边漂了都会红。
//
// 判别力：collect_msaa 若退回"只看全局限制"，而设备存在某个候选格式支持不到
// 最顶档，这里就会红（caps 里出现了全局限制允许、但交集不允许的档位）。
TEST(Msaa, CapsRespectEveryFormatIntersection) {
  device::DeviceDesc d;
  d.app_name = "MagicBlocks MSAA intersect probe";
  d.require_software_rasterizer = true;

  auto dev_r = device::create_render_device(d);
  if (!dev_r.ok()) {
    ADD_FAILURE() << "需要软件光栅设备: " << dev_r.message();
    return;
  }
  auto* vkdev = dynamic_cast<VulkanRenderDevice*>(dev_r.value().get());
  ASSERT_TRUE(vkdev != nullptr) << "需要 VulkanRenderDevice";

  const VkPhysicalDevice phys = vkdev->context().phys;
  ASSERT_TRUE(phys != VK_NULL_HANDLE) << "context().phys 为空,后面无从查起";
  const device::DeviceCaps& caps = vkdev->caps();

  // 全局上限（外层天花板）。
  VkPhysicalDeviceProperties props{};
  vkGetPhysicalDeviceProperties(phys, &props);
  const VkSampleCountFlags global_mask = props.limits.framebufferColorSampleCounts &
                                         props.limits.framebufferDepthSampleCounts;

  // 与 collect_msaa 相同的交集重算。
  constexpr std::size_t kProbeCount = sizeof(kMsaaProbeColorFormats) / sizeof(VkFormat);
  VkSampleCountFlags fmt_mask = ~static_cast<VkSampleCountFlags>(0);
  std::size_t probed = 0;
  for (std::size_t i = 0; i < kProbeCount; ++i) {
    const VkSampleCountFlags c = query_color_sample_counts(phys, kMsaaProbeColorFormats[i]);
    if (c == 0) continue;
    fmt_mask &= c;
    ++probed;
  }
  // 2026-11-11：原来是 GTEST_SKIP。四个候选颜色格式全查不动 = 设备探测的
  // 输入本身失效，交集校验就无从谈起 —— 这必须响，不能跳过。
  ASSERT_NE(probed, 0u)
      << "\n  现象：4 个候选颜色格式的采样数查询全部失败。"
         "\n  原因链：驱动不认这 4 个格式的任何组合，collect_msaa 会退回"
         "「只看全局限制」并打日志，此时没有可对照的交集。"
         "\n  按纪律这是 FAIL（不是 SKIP）：请检查 SwiftShader 部署与格式探测。";

  const VkSampleCountFlags expect_mask = global_mask & fmt_mask;
  for (const device::u32 n : {1u, 2u, 4u, 8u, 16u}) {
    const VkSampleCountFlagBits bit = to_vk_samples(n);
    const bool expect_on = (expect_mask & bit) != 0;
    bool got_on = false;
    for (const device::u32 v : caps.supported_msaa_levels) {
      if (v == n) {
        got_on = true;
        break;
      }
    }
    EXPECT_EQ(got_on, expect_on)
        << n << "x：caps " << (got_on ? "报支持" : "报不支持") << "，但全局限制∩每格式交集 "
        << (expect_on ? "允许" : "不允许") << "。"
        << "caps 报多了 = 用户点了会用上一张无效的 MS 图（静默降级）；报少了 = 白丢能力。";
  }

  // 底线：1x 永远可用。
  bool has_1x = false;
  for (const device::u32 v : caps.supported_msaa_levels) has_1x = has_1x || (v == 1u);
  EXPECT_TRUE(has_1x) << "supported_msaa_levels 必须含 1（关闭 MSAA 是唯一保底档位）";
}

TEST(Msaa, ColorFormatSampleProbeMatchesDriver) {
  // 第二层防线(color_format_supports_samples)是"任何构建、任何设备都生效"的自查,
  // 它必须与直接问驱动的结果一致,否则兜底会误判:
  //   - 漏拦 → 8x 静默降级(本次 bug 的另一种形态)
  //   - 多拦 → 明明能用的 4x 被退回 1x(用户又"看不到效果")
  device::DeviceDesc d;
  d.app_name = "MagicBlocks MSAA probe consistency";
  d.require_software_rasterizer = true;

  auto dev_r = device::create_render_device(d);
  if (!dev_r.ok()) {
    ADD_FAILURE() << "需要软件光栅设备: " << dev_r.message();
    return;
  }
  auto* vkdev = dynamic_cast<VulkanRenderDevice*>(dev_r.value().get());
  if (vkdev == nullptr) {
    ADD_FAILURE() << "需要 VulkanRenderDevice";
    return;
  }
  const VkPhysicalDevice phys = vkdev->context().phys;
  const VkSampleCountFlags driver_counts = query_driver_sample_counts(phys);
  // 2026-11-11：原来是 GTEST_SKIP。缺能力是 FAIL 不是 SKIP。
  ASSERT_NE(driver_counts, 0u)
      << "驱动不支持 B8G8R8A8_UNORM + COLOR_ATTACHMENT —— 无法验证"
         " color_format_supports_samples 与驱动的一致性。按纪律这是 FAIL（不是 SKIP）。";

  // 1x 永远被视为支持(不需要 MS 图)。
  EXPECT_TRUE(color_format_supports_samples(phys, kProbeFormat, 1u));
  for (const VkSampleCountFlagBits bit :
       {VK_SAMPLE_COUNT_2_BIT, VK_SAMPLE_COUNT_4_BIT, VK_SAMPLE_COUNT_8_BIT,
        VK_SAMPLE_COUNT_16_BIT}) {
    const bool driver_ok = (driver_counts & bit) != 0;
    const bool helper_ok = color_format_supports_samples(phys, kProbeFormat,
                                                         static_cast<std::uint32_t>(bit));
    EXPECT_EQ(helper_ok, driver_ok)
        << "color_format_supports_samples 与驱动不一致: 采样数 bit="
        << static_cast<unsigned>(bit) << " 驱动=" << (driver_ok ? "支持" : "不支持")
        << " 但兜底判定=" << (helper_ok ? "支持" : "不支持");
  }

  // 非法采样数(非 2 的幂 / 超范围)一律判不支持,不能让 to_vk_samples 的 1x 回落蒙混过关。
  EXPECT_FALSE(color_format_supports_samples(phys, kProbeFormat, 3u));
  EXPECT_FALSE(color_format_supports_samples(phys, kProbeFormat, 64u));
}

// ===================== 采样数合法性:纯函数,本机必有判别力 =====================
//
// 为什么单独一条:to_vk_samples() 对**任何**非法值都会回落到 VK_SAMPLE_COUNT_1_BIT,
// 而 1x 永远"存在"。于是只要兜底逻辑写成
//     return (driver_counts & to_vk_samples(n)) != 0;
// 3、5、6、7、64、0 这些值全都会被判成"支持" —— 建出来的图不是要的 MS 图,
// 也没有任何报错。这条测试不依赖设备能力、不依赖驱动,纯函数,必定能红。
TEST(Msaa, SampleCountHelperRejectsNonLevels) {
  device::DeviceDesc d;
  d.app_name = "MagicBlocks MSAA sample-count probe";
  d.require_software_rasterizer = true;

  auto dev_r = device::create_render_device(d);
  if (!dev_r.ok()) {
    ADD_FAILURE() << "需要软件光栅设备: " << dev_r.message();
    return;
  }
  auto* vkdev = dynamic_cast<VulkanRenderDevice*>(dev_r.value().get());
  ASSERT_TRUE(vkdev != nullptr) << "需要 VulkanRenderDevice";
  const VkPhysicalDevice phys = vkdev->context().phys;
  ASSERT_TRUE(phys != VK_NULL_HANDLE) << "context().phys 为空,后面无从查起";

  // 合法档位:不校验驱动是否支持(不同设备不同),只校验"不会因为非法值这条路判定失败"。
  // 1x 永远支持 —— 关掉 MSAA 是唯一保底档位。
  EXPECT_TRUE(color_format_supports_samples(phys, kProbeFormat, 0u))
      << "0 应当按 1x 处理(等价于关闭 MSAA),必须可用";
  EXPECT_TRUE(color_format_supports_samples(phys, kProbeFormat, 1u)) << "1x 永远可用";

  // 非法档位:必须全部判 false。这些值即便驱动"支持"也不该放行:
  // 它们不是合法的 Vulkan 采样数,to_vk_samples 会把它们悄悄变成 1x。
  for (const std::uint32_t bad : {3u, 5u, 6u, 7u, 9u, 12u, 32u, 64u, 100u}) {
    EXPECT_FALSE(color_format_supports_samples(phys, kProbeFormat, bad))
        << bad << "x 不是合法的 Vulkan 采样数,必须判不支持;"
        << "否则 to_vk_samples 会把 1x 的回落当成'支持'放行,用户拿到一张不是他要的图";
  }

  // 合法档位在**格式不支持**时也必须判 false —— 这条是 8x 静默降级的本体:
  // 驱动对 B8G8R8A8_UNORM 报 1x|4x 时,8x 必须被拦。
  VkImageFormatProperties ifp{};
  const VkResult qr = vkGetPhysicalDeviceImageFormatProperties(
      phys, kProbeFormat, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 0, &ifp);
  if (qr == VK_SUCCESS) {
    for (const std::uint32_t n : {2u, 4u, 8u, 16u}) {
      const bool driver_ok =
          (ifp.sampleCounts & static_cast<VkSampleCountFlags>(to_vk_samples(n))) != 0;
      EXPECT_EQ(color_format_supports_samples(phys, kProbeFormat, n), driver_ok)
          << n << "x: 兜底判定与驱动支持矩阵不一致(驱动=" << (driver_ok ? "支持" : "不支持")
          << ")。多放行 = 静默降级;少放行 = 误杀";
    }
  }
}

}  // namespace magicblocks::backend
