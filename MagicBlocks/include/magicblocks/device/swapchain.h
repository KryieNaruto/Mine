// MagicBlocks SDK —— 交换链(D3)
//
// 平台差异只体现在 NativeSurface::kind 与 backend 的 TU 划分上,
// 本头不出现任何平台宏或平台类型(spec §9:iOS 走同一抽象)。
#pragma once

#include <memory>
#include <vector>

#include "magicblocks/device/result.h"
#include "magicblocks/device/types.h"

namespace magicblocks::device {

class IRenderTarget;

class ISwapchain {
 public:
  virtual ~ISwapchain() = default;

  virtual Extent2D extent() const = 0;
  virtual Format format() const = 0;
  virtual PresentMode present_mode() const = 0;
  virtual std::vector<PresentMode> supported_present_modes() const = 0;

  // 旋转/窗口尺寸变化时重建。失败必须返回明确错误,不静默沿用旧尺寸。
  virtual Status rebuild(Extent2D new_extent) = 0;

  // 取下当前帧的渲染目标(由 swapchain 拥有,不可 Delete)。
  virtual IRenderTarget* current_target() = 0;
};

}  // namespace magicblocks::device
