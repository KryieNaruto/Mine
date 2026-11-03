// MagicBlocks UI 消费层 —— 主路径呈现宿主的私有声明(只给 src/ui/ 内部用)
//
// 公共面(`magicblocks/ui/presenter.h`)只暴露 `IPresentSurface` 抽象;
// 这里给出它的**壳层落地**:持有 SDK 交换链,并把 acquire/submit/present
// 三个动作接到 SDK 上。SDK 只到 L0 时,submit/present 明确失败(见 .cpp 说明)。
//
// 为什么放在 `src/ui/` 而不是 `include/`:这是壳层适配器,不是 SDK 的对外契约;
// 上层要的是 `IPresentSurface*`,不需要知道它怎么接的。
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "magicblocks/device/render_device.h"
#include "magicblocks/device/swapchain.h"
#include "magicblocks/ui/presenter.h"
#include "magicblocks/ui/types.h"

namespace magicblocks::ui {

class PresenterHost final : public IPresentSurface {
 public:
  PresenterHost();
  ~PresenterHost() override;

  PresenterHost(const PresenterHost&) = delete;
  PresenterHost& operator=(const PresenterHost&) = delete;

  // 建交换链。失败时填 error 并返回 false(**不静默降级到离屏**)。
  bool attach(device::IRenderDevice* device, const device::NativeSurface& surface,
              device::Extent2D extent, std::string* error);
  void detach();

  bool attached() const { return swapchain_ != nullptr; }

  // 旋转/窗口尺寸变化:走 ISwapchain::rebuild(不是每帧重建,spec §5.4)。
  bool rebuild(device::Extent2D extent, std::string* error);

  // 当前 swapchain image 的**不透明句柄值**(0 = 没有)。给后端录制用。
  std::uint64_t current_image_handle() const;

  // IPresentSurface
  bool acquire(FrameImage* out_image) override;
  bool submit(std::uint64_t cmd, std::uint64_t signal_semaphore) override;
  bool present(const FrameImage& image, std::uint64_t wait_semaphore) override;
  bool ready() const override;
  PresenterStatus last_failure() const override;

 private:
  device::IRenderDevice* device_ = nullptr;
  std::unique_ptr<device::ISwapchain> swapchain_;
  PresenterStatus last_failure_ = PresenterStatus::Ok;
};

}  // namespace magicblocks::ui
