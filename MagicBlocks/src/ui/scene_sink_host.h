// MagicBlocks UI 消费层 —— 壳层 surface 宿主（src/ui/ 内部使用）
//
// 公共面（magicblocks/ui/frame_renderer.h）只暴露 `ISceneFrameSink` 抽象；
// 这里给出一个**桌面 / 离屏场景下的落地实现**：持有 SDK 交换链，把
// acquire / submit / present 三个动作接到 `IRenderDevice` 上。
//
// 移动端（Android SurfaceView / iOS CAMetalLayer）由壳层自己实现
// `ISceneFrameSink`，**不需要**这个类 —— 它只是给桌面调试与 CLI 用的参考实现。
//
// 为什么放在 `src/ui/` 而不是 `include/`：这是壳层适配器，不是 SDK 的对外契约。
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "mb_internal/magicblocks/device/render_device.h"
#include "mb_internal/magicblocks/device/swapchain.h"
#include "mb_internal/magicblocks/ui/frame_renderer.h"
#include "mb_internal/magicblocks/ui/types.h"

namespace magicblocks::ui {

class ShellSurfaceSink final : public ISceneFrameSink {
 public:
  ShellSurfaceSink();
  ~ShellSurfaceSink() override;

  ShellSurfaceSink(const ShellSurfaceSink&) = delete;
  ShellSurfaceSink& operator=(const ShellSurfaceSink&) = delete;

  // 建交换链。失败时填 error 并返回 false（**不静默降级到离屏**）。
  bool attach(device::IRenderDevice* device, const device::NativeSurface& surface,
              device::Extent2D extent, std::string* error);
  void detach();

  bool attached() const { return swapchain_ != nullptr; }

  // 旋转 / 窗口尺寸变化：走 ISwapchain::rebuild（不是每帧重建）。
  bool rebuild(device::Extent2D extent, std::string* error);

  // 当前 swapchain image 的不透明句柄值（0 = 没有）。
  std::uint64_t current_image_handle() const;

  // ISceneFrameSink
  device::Extent2D extent() const override;
  device::Format format() const override;
  bool ready() const override;
  bool acquire_image(SinkImage* out_image) override;
  bool submit_frame(resources::ICommandList& list) override;
  bool present_image(const SinkImage& image) override;
  SinkStatus last_status() const override;

 private:
  device::IRenderDevice* device_ = nullptr;
  std::unique_ptr<device::ISwapchain> swapchain_;
  SinkStatus last_status_ = SinkStatus::Ok;
};

}  // namespace magicblocks::ui
