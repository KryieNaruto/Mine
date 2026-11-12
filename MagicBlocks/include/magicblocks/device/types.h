// MagicBlocks SDK —— L0 device 层公共类型
//
// 设计约束(spec §2.1):本头属于 L2+ 可见的公共面,不得出现任何 Vulkan 类型;
// 平台句柄一律用 void*/uintptr_t 承载,由 backend/vulkan 内部解释。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace magicblocks::device {

using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i32 = std::int32_t;

// ---------------------------------------------------------------- 后端

// spec §9:本轮只落地 Vulkan;GLES / Metal 仅留枚举位。
enum class BackendType : std::uint8_t {
  Vulkan = 0,
  OpenGLES = 1,  // 未实现(本轮返回 Unsupported)
  Metal = 2,     // 未实现(本轮返回 Unsupported)
};

// ---------------------------------------------------------------- 基础几何

struct Extent2D {
  u32 width = 0;
  u32 height = 0;
  bool operator==(const Extent2D& o) const { return width == o.width && height == o.height; }
  bool operator!=(const Extent2D& o) const { return !(*this == o); }
};

// 本层只声明渲染目标用得到的格式(spec D2/D4)。
enum class Format : std::uint8_t {
  Unknown = 0,
  RGBA8_UNORM,
  BGRA8_UNORM,
  R8_UNORM,
  D16_UNORM,
  D24_UNORM_S8_UINT,
  D32_SFLOAT,
};

enum class PresentMode : std::uint8_t {
  Fifo = 0,     // 默认
  Mailbox = 1,  // 能力允许时可探测
  Immediate = 2,
};

// ---------------------------------------------------------------- 深度 / 光栅能力

enum class DepthFormat : std::uint8_t {
  D16_UNORM = 0,
  D24_UNORM_S8_UINT = 1,
  D32_SFLOAT = 2,
};

// ---------------------------------------------------------------- 设备描述(D1)

// DeviceDesc 只放"请求项";能力项在 DeviceCaps。两边命名有意不重名,
// 请求不可满足时 CreateRenderDevice 返回 Unsupported —— 不静默降级。
struct DeviceDesc {
  BackendType backend = BackendType::Vulkan;
  std::string app_name = "MagicBlocks";
  bool enable_validation = true;       // 打开校验层(默认开:验证层消息是默认能力)
  bool request_rev_z = false;          // 请求 reverse-Z(能力不足即 Unsupported)
  u32 requested_msaa = 1;              // 请求 MSAA 采样数(不在 supported_msaa_levels 即 Unsupported)
  u32 in_flight_frames = 3;            // 帧环大小,必须 ∈ {2,3}
  // 软件光栅强筛:true 时只接受 is_software_rasterizer 的设备,
  // 否则 CreateRenderDevice 返回 Unsupported(绝不回退到物理 GPU)。
  bool require_software_rasterizer = true;
};

// ---------------------------------------------------------------- 能力探测(D2)

// DeviceCaps 只放"能力项",与 DeviceDesc 的请求项零重名。
struct DeviceCaps {
  std::string device_name;
  std::string driver_name;
  u32 api_version = 0;              // 如 VK_MAKE_VERSION(1,3,0)
  u32 driver_version = 0;

  std::vector<DepthFormat> depth_formats;   // 至少含 D16_UNORM 或 D24_UNORM_S8_UINT
  std::vector<u32> supported_msaa_levels;   // 例如 {1,2,4,8}
  std::vector<Format> texture_formats;      // 至少含 RGBA8_UNORM
  bool rev_z_supported = false;
  u32 max_push_constant_size = 0;           // 移动端要求 >= 128
  u32 max_uniform_buffer_range = 0;
  bool timestamp_queries = false;
  bool compressed_textures = false;

  // 显存预算:优先 VK_EXT_memory_budget 的 heapBudget 之和;
  // 扩展不可用时取 VkMemoryHeap.size 之和。两者都 > 0。
  u64 memory_budget = 0;
  u64 memory_heap_total = 0;
  bool memory_budget_from_extension = false;  // true=来自 heapBudget;false=来自 heap size

  // 软件光栅标记:与 require_software_rasterizer 配合做设备强筛。
  bool is_software_rasterizer = false;
};

// ---------------------------------------------------------------- 平台 surface(D3)

// NativeSurface 值表(公共头零平台宏,平台语义只在 backend 内部解释):
//   Android : a = ANativeWindow*, b = 0
//   Win32   : a = HWND,           b = HINSTANCE
//   Xcb     : a = xcb_connection_t*, b = xcb_window_t
//   Wayland : a = wl_display*,    b = wl_surface*
//   Darwin  : a = CAMetalLayer*   (iOS 抽象位,只留枚举,本轮不实现)
//   Headless: a = 0, b = 0        (无窗口,创建 swapchain 返回 Unsupported)
struct NativeSurface {
  enum class Kind : std::uint8_t {
    Android = 0,
    Win32 = 1,
    Xcb = 2,
    Wayland = 3,
    Darwin = 4,
    Headless = 5,
  };
  Kind kind = Kind::Headless;
  void* a = nullptr;             // 主窗口句柄
  std::uintptr_t b = 0;          // 次句柄(实例/连接/展示等)
};

// ---------------------------------------------------------------- 渲染目标(D4)

// WindowTarget 与 OffscreenTarget 是同一种对象(spec D4)。
struct TargetDesc {
  enum class Kind : std::uint8_t { Window = 0, Offscreen = 1 };
  Kind kind = Kind::Offscreen;
  Extent2D extent{};
  u32 samples = 1;               // MSAA 采样数,1=不开启
  bool with_depth = true;
  bool with_stencil = false;
  Format format = Format::RGBA8_UNORM;
  NativeSurface surface{};       // kind==Window 时有效
};

}  // namespace magicblocks::device
