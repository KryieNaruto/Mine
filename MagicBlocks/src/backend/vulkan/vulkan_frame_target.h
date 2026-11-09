// MagicBlocks —— 后端内部：设备帧提交所需的最小图像接口
//
// 为什么需要它：
//   `IRenderTarget` 是**公共**接口（include/），只暴露尺寸/格式/读回，
//   刻意不暴露任何 Vk* 句柄（G3：Vk* 不许出现在 src/backend/ 之外）。
//   而 `submit_device_frame()` 要把 render pass 建在某张具体图像上，
//   所以它需要的恰恰是「颜色/深度 VkImage」这对后端句柄。
//
//   历史上这段代码用 `dynamic_cast<OffscreenTarget*>` 直接强转，
//   结果交换链图像（SwapchainImageView）永远进不来 —— pad 上的表现是
//   "交换链建好了，但每帧都报『目标不是后端 OffscreenTarget』"。
//
//   这里抽出窄接口后：
//     - OffscreenTarget    : 图像自己分配自己持有
//     - SwapchainImageView : 图像由 VkSwapchainKHR 持有，这里只借用
//   两者对 submit_device_frame 完全等价，不需要任何 if/强转分支。
#pragma once

#include <vulkan/vulkan.h>

namespace magicblocks::backend {

class IFrameTargetImage {
 public:
  virtual ~IFrameTargetImage() = default;

  // 颜色附件图像（必然有效；无效时应返回 VK_NULL_HANDLE，调用方据此报错）。
  // MSAA 时这是**多重采样**那张（渲染写它）。
  virtual VkImage frame_color_image() const = 0;

  // MSAA 的单采样 resolve 图像。samples == 1 时返回与 frame_color_image()
  // 相同的图像（此时 render pass 不需要 resolve 附件）。
  //
  // 默认实现回落到 frame_color_image()：交换链图像在 MSAA 接入前一律 1 采样，
  // 这样它不必为了一个用不到的方法改代码；真要做交换链 MSAA 时在这里覆盖即可。
  virtual VkImage frame_resolve_image() const { return frame_color_image(); }

  // 深度附件图像。没有深度附件时返回 VK_NULL_HANDLE。
  virtual VkImage frame_depth_image() const = 0;

  // 图像来自交换链时，render pass 的 load/store 与初始/最终布局要按
  // "present 之前"的语义来写（初始布局 PRESENT_SRC_KHR，最终布局 PRESENT_SRC_KHR）。
  // 离屏目标返回 false。
  virtual bool frame_from_swapchain() const = 0;
};

}  // namespace magicblocks::backend
