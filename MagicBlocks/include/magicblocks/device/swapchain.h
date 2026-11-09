// MagicBlocks SDK —— 交换链(D3)
//
// 平台差异只体现在 NativeSurface::kind 与 backend 的 TU 划分上,
// 本头不出现任何平台宏或平台类型(spec §9:iOS 走同一抽象)。
#pragma once

#include <memory>
#include <string>
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

  // ---- 上屏三件事(2026-11 补:此前 acquire/present 在后端根本没有实现)----
  //
  // acquire 从交换链取下一张可写 image;present 把已渲染的那张交给显示端。
  // 这两个动作**只做取图/呈现**,不碰像素 —— 因此上屏路径零 CPU 回读。
  //
  // 失败语义必须明确:
  //   AcquireOutOfDate / PresentOutOfDate → 调用方 rebuild 后重试这一帧
  //   SurfaceLost                        → 壳层 surface 已销毁,停下等重建

  enum class ImageStatus : std::uint8_t {
    Ok = 0,
    OutOfDate = 1,  // 交换链与 surface 不匹配,需要 rebuild
    SurfaceLost = 2,  // surface 已失效(Android 上 SurfaceHolder.surfaceDestroyed)
    Unsupported = 3,  // 该平台不支持(silently 沿旧图是禁止的)
    Failed = 4,
  };

  struct ImageResult {
    ImageStatus status = ImageStatus::Failed;
    IRenderTarget* target = nullptr;  // 仅 status==Ok 时有效,由 swapchain 拥有
    std::string message;
  };

  // 取下一张可写 image。超时由实现内部决定(不允许永久阻塞 UI 线程)。
  virtual ImageResult acquire_next_image() = 0;

  // 把当前 image 呈现到显示端。
  virtual ImageStatus present_current() = 0;
};

}  // namespace magicblocks::device
