// MagicBlocks —— 核心层(core/)对"后端私有能力"的唯一转发面
//
// ============================================================================
// 为什么需要这个文件
// ============================================================================
// 公共面(include/magicblocks/device|resources/*)是后端无关抽象,刻意不暴露
// "按采样数建交换链""按交换链形状建管线""交换链实际生效的采样数/旋转角"这些
// 只有具体后端才答得上来的问题。
//
// 而这些能力**必须**有人提供:Android 的 scene_renderer 要开 MSAA、要在
// 交换链格式/采样数变化后重建管线、要把屏幕旋转转成角度。此前的做法是让
// android/scene_renderer.cpp 自己 `#include "backend/vulkan/vulkan_*.h"` 并
// `dynamic_cast<backend::VulkanRenderDevice*>`,于是:
//
//   - UI 侧最近的那一层(android/)反向依赖了 backend/ 的实现类型;
//   - 换后端 = 改 UI 侧代码(与"SDK 内部重构不影响 UI"直接冲突);
//   - G3 分层纯度门控(tools/check_layer_purity.py)判红 —— 且这红是真红,
//     不该靠白名单绕过。
//
// 解法:把"需要后端知识的动作"收进 core/ 这**一个**文件。backend/ 的实现细节
// 只在这里出现;上面的调用方(android/、cli/、primitives/)只看这份纯值接口。
//
// ============================================================================
// 本文件的纪律
// ============================================================================
//   1. 本文件的头(device_ops.h)是**纯值**:只用公共头里的类型,零 VK_、零 backend::。
//   2. 所有 backend::/VK_ 只出现在 device_ops.cpp 里(它是 backend 与公共面之间
//      唯一被允许的"翻译层")。
//   3. 新增函数时先问:能不能进公共面接口?能就别加到这里 —— 这里每加一条,
//      就多一处"只有 Vulkan 后端才答得上来"的隐含假设。
#ifndef MAGICBLOCKS_CORE_DEVICE_OPS_H
#define MAGICBLOCKS_CORE_DEVICE_OPS_H

#include <cstdint>
#include <memory>

#include "mb_internal/magicblocks/device/render_device.h"
#include "mb_internal/magicblocks/device/render_target.h"
#include "mb_internal/magicblocks/device/swapchain.h"
#include "mb_internal/magicblocks/resources/pipeline.h"

namespace magicblocks::core {

// 创建设备。当前只有 Vulkan 后端;其它后端在设备 init() 里被拒为 Unsupported。
device::Result<std::unique_ptr<device::IRenderDevice>> create_render_device(
    const device::DeviceDesc& desc);

// 建交换链,并请求 MSAA 采样数(0/1 = 关闭)。
//
// 顺序敏感:"设采样数"必须在交换链 init() 之前,而公共面 create_swapchain() 不带
// 采样参数 —— 所以这组顺序被收进后端,调用方只传一个数字。
// 采样数不可用时**不失败**:后端退回 1x 并说明原因(MSAA 不可用不该挡住上屏),
// 实际生效值请用 swapchain_msaa_samples() 读。
device::Result<std::unique_ptr<device::ISwapchain>> create_swapchain_with_msaa(
    device::IRenderDevice* device, const device::NativeSurface& surface,
    device::Extent2D extent, std::uint32_t msaa_samples);

// 交换链**实际生效**的采样数(设备回退过就是回退值)。chain 为空时返回 1。
std::uint32_t swapchain_msaa_samples(const device::ISwapchain* chain);

// 屏幕旋转角(0/90/180/270,度)。平台旋转标志(raw 位)的解码在 backend 内完成,
// 调用方只拿这个平台无关的角度。chain 为空时返回 0。
std::uint32_t swapchain_rotation_degrees(const device::ISwapchain* chain);

// 按**交换链的真实形状**建管线(颜色格式/采样数/from_swapchain 全由后端决定)。
//
// 为什么必须用这条入口:VkPipeline 绑定的 VkRenderPass 必须与 begin_pass 用的
// render pass 逐项兼容(颜色格式/采样数/深度/finalLayout)。不兼容时 vkCmdDraw
// 被**静默丢弃** —— 画面只剩清屏色,且没有任何报错。
//   - 采样数取交换链实际生效值(不是 msaa_hint):否则管线和 pass 对不上;
//   - from_swapchain 置真:否则 present 的 layout 不对,vkQueuePresentKHR 报 E-01430。
// msaa_hint 仅在 chain 不是当前后端实现(例如测试桩)时作为兜底。
resources::Result<std::unique_ptr<resources::IPipeline>> create_swapchain_pipeline(
    device::IRenderDevice* device, const device::ISwapchain* chain,
    const resources::PipelineDesc& desc, std::uint32_t msaa_hint);

// ---------------------------------------------------------------------------
// 离屏目标的自检绘制(t9)
// ---------------------------------------------------------------------------
// 语义:`draw_selftest` 是 backend 内部自建 render pass + vkCmdDraw 的出图入口,
// 它是 L0/L2 golden 的来源(见 src/backend/vulkan/vulkan_target.h)。UI 层与 CLI
// 此前靠 `dynamic_cast<backend::OffscreenTarget*>` 才够得到它。
//
// 为什么收成"纯值枚举 + 一条函数":
//   `SelftestKind` 本身与 Vulkan 毫无关系(它选的是哪套 shader),所以它应当出现在
//   公共面;而"哪个后端、怎么画"留在 backend。调用方传一个枚举,拿一个 Status。
enum class SelftestKind : std::uint8_t {
  kL0Default = 0,  // fullscreen.vert/frag:纯红标定点 + 黑清屏(L0 golden)
  kL2Scene = 1,    // l2_scene.vert/frag:纯绿标定点 + 深蓝清屏(L2 golden)
};

// 在离屏目标上画一帧自检图案。
// 目标不是当前后端实现时返回 Unsupported(不假装画了)。
device::Status draw_offscreen_selftest(device::IRenderTarget* target, std::uint64_t frame_index,
                                       SelftestKind kind = SelftestKind::kL0Default);

// ---------------------------------------------------------------------------
// UI 离屏帧渲染器(不透明句柄,t9)
// ---------------------------------------------------------------------------
// 把 UI 图元(矩形/描边/线段)合批渲染进离屏目标并读回 RGBA8。这是 CI/golden
// 专用的离屏路径(spec §5.5),不是上屏路径。
//
// 为什么是"不透明句柄 + 自由函数"而不是暴露一个类:
//   `VulkanFrameRenderer` 的构造需要 `VulkanContext`(内含 VkInstance/VkDevice/
//   VkQueue),把它写进本头就等于把 Vk* 泄漏到公共面。今天调用方拿到的只是
//   "我可以往里灌图元、然后渲染到内存"这个**能力**,不需要知道它是谁。
class OffscreenFrameRenderer {
 public:
  virtual ~OffscreenFrameRenderer() = default;

  // 一帧:重置实例 → 调用方逐个 append → render_to_memory。
  virtual void reset_instances() = 0;
  virtual device::Status append_rect(float x0, float y0, float x1, float y1, float r, float g,
                                     float b, float a) = 0;
  virtual device::Status append_stroke(float x0, float y0, float x1, float y1, float r, float g,
                                       float b, float a) = 0;
  virtual device::Status append_line(float ax, float ay, float bx, float by, float width, float r,
                                     float g, float b, float a) = 0;

  // 渲染并读回 RGBA8(out 长度 >= width*height*4;row_bytes >= width*4)。
  virtual bool render_to_memory(std::uint32_t width, std::uint32_t height, std::uint8_t* out_rgba8,
                                std::uint32_t row_bytes) = 0;
};

// 为一个离屏目标创建 UI 帧渲染器。
//
// `target` 必须由 `device` 创建(当前实现里就是它的离屏目标)。任一为空、
// 或目标不是当前后端的离屏目标时返回 Invalid —— **不**返回一个"什么也画不出"
// 的对象,那会让调用方以为渲染成功了。
device::Result<std::unique_ptr<OffscreenFrameRenderer>> create_offscreen_frame_renderer(
    device::IRenderDevice* device, device::IRenderTarget* target);

// 交换链当前 image 的**不透明数值句柄**。
//
// 为什么是一个纯 uint64:这个值只被当成"这帧是哪张图"的标识符向上转发
// (见 SinkImage::id),任何持有者都不拿它去调 Vulkan。把"怎么取到它"留在
// backend,上层就永远不需要认识 VkImage。
// 非当前后端实现 / 空指针 → 0(0 表示"没有可用句柄",不是有效 image)。
std::uint64_t swapchain_current_image_value(const device::ISwapchain* chain);

}  // namespace magicblocks::core

#endif  // MAGICBLOCKS_CORE_DEVICE_OPS_H
