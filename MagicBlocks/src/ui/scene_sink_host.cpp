// MagicBlocks UI 消费层 —— 壳层 surface 宿主实现
//
// `ISceneFrameSink` 的桌面/离屏参考落地：acquire / submit / present。
//
// 关键变化（本轮）：
//   旧 `ShellSurfaceSink::submit()` / `present()` **恒返回 false** —— 因为当时 SDK
//   没有任何公开的"提交一条命令列表"入口（`submit_device_frame()` 是死代码）。
//   现在 `IRenderDevice::submit_command_list()` 已打通并验证能真正出图，
//   所以这里可以给出**真实实现**，而不是"明确失败但不假装成功"。
//
// 上屏（2026-11 补齐）：`ISwapchain` 现在有真正的 `acquire_next_image()` /
// `present_current()`，后端对应实现 vkAcquireNextImageKHR / vkQueuePresentKHR。
// 因此 acquire 取的是**环形交换链里的下一张**，present 是**真的提交给显示端**。
//
// 全程零 CPU 回读：acquire 只取图，present 只交图，没有一个字节被读回 CPU。
// 这条路径同时被 Android SurfaceView 使用（见 src/android/scene_surface_sink.*）。
#include "scene_sink_host.h"

#include <algorithm>
#include <cstring>

#include "backend/vulkan/vulkan_render_device.h"
#include "backend/vulkan/vulkan_swapchain.h"
#include "backend/vulkan/vulkan_ui_overlay.h"

namespace magicblocks::ui {

ShellSurfaceSink::ShellSurfaceSink() = default;
ShellSurfaceSink::~ShellSurfaceSink() = default;

bool ShellSurfaceSink::attach(device::IRenderDevice* device, const device::NativeSurface& surface,
                              device::Extent2D extent, std::string* error) {
  detach();
  if (!device) {
    if (error) *error = "ShellSurfaceSink::attach: device 为空";
    return false;
  }
  if (surface.kind == device::NativeSurface::Kind::Headless) {
    if (error) *error = "ShellSurfaceSink::attach: Headless 不建交换链(请用离屏模式)";
    return false;
  }
  auto sc = device->create_swapchain(surface, extent);
  if (!sc.ok()) {
    if (error) *error = sc.message();
    return false;
  }
  device_ = device;
  swapchain_ = std::move(sc.value());
  last_status_ = SinkStatus::Ok;
  return true;
}

void ShellSurfaceSink::detach() {
  if (swapchain_) {
    // 交换链悬挂在 device 上，必须**先于** device 析构（壳层负责顺序）。
    swapchain_.reset();
  }
  device_ = nullptr;
}

device::Extent2D ShellSurfaceSink::extent() const {
  if (!swapchain_) return device::Extent2D{0, 0};
  return swapchain_->extent();
}

device::Format ShellSurfaceSink::format() const {
  if (!swapchain_) return device::Format::RGBA8_UNORM;
  return swapchain_->format();
}

bool ShellSurfaceSink::ready() const {
  // 只判断"壳层这边有没有可用的交换链"，不做 acquire —— acquire 必须每帧只做一次。
  return swapchain_ != nullptr;
}

bool ShellSurfaceSink::acquire_image(SinkImage* out_image) {
  last_status_ = SinkStatus::Ok;
  if (!swapchain_) {
    last_status_ = SinkStatus::NoSurface;
    return false;
  }
  // 真实环形取图：vkAcquireNextImageKHR。
  auto r = swapchain_->acquire_next_image();
  switch (r.status) {
    case device::ISwapchain::ImageStatus::Ok:
      break;
    case device::ISwapchain::ImageStatus::OutOfDate:
      last_status_ = SinkStatus::NeedRebuild;
      return false;
    case device::ISwapchain::ImageStatus::SurfaceLost:
      last_status_ = SinkStatus::SurfaceLost;
      return false;
    case device::ISwapchain::ImageStatus::Unsupported:
      last_status_ = SinkStatus::NoSurface;
      return false;
    case device::ISwapchain::ImageStatus::Failed:
    default:
      last_status_ = SinkStatus::SubmitFailed;
      return false;
  }
  device::IRenderTarget* target = r.target;
  if (!target) {
    last_status_ = SinkStatus::NeedRebuild;
    return false;
  }
  if (out_image) {
    out_image->id = current_image_handle();
    out_image->extent = target->extent();
    out_image->format = target->color_format();
    out_image->valid = true;
  }
  last_status_ = SinkStatus::Ok;
  return true;
}

bool ShellSurfaceSink::submit_frame(resources::ICommandList& list) {
  if (!device_) {
    last_status_ = SinkStatus::NoSurface;
    return false;
  }
  // 真正的提交：IRenderDevice::submit_command_list() 会把命令流翻译成 vkCmd*，
  // 然后 vkQueueSubmit + fence 等待。这就是本轮打通的路径。
  auto s = device_->submit_command_list(list);
  if (!s.ok()) {
    last_status_ = SinkStatus::SubmitFailed;
    return false;
  }
  last_status_ = SinkStatus::Ok;
  return true;
}

bool ShellSurfaceSink::present_image(const SinkImage&) {
  if (!swapchain_) {
    last_status_ = SinkStatus::NoSurface;
    return false;
  }
  // 真实呈现：vkQueuePresentKHR。
  switch (swapchain_->present_current()) {
    case device::ISwapchain::ImageStatus::Ok:
      last_status_ = SinkStatus::Ok;
      return true;
    case device::ISwapchain::ImageStatus::OutOfDate:
      last_status_ = SinkStatus::NeedRebuild;
      return false;
    case device::ISwapchain::ImageStatus::SurfaceLost:
      last_status_ = SinkStatus::SurfaceLost;
      return false;
    default:
      last_status_ = SinkStatus::PresentFailed;
      return false;
  }
}

SinkStatus ShellSurfaceSink::last_status() const { return last_status_; }

std::uint64_t ShellSurfaceSink::current_image_handle() const {
  if (!swapchain_) return 0;
  device::IRenderTarget* target = swapchain_->current_target();
  if (!target) return 0;
  // 后端私有钩子：swapchain 实现知道自己的当前 image 是哪一张。
  auto* probe = dynamic_cast<backend::SwapchainImageProbe*>(swapchain_.get());
  return probe ? probe->current_image_value() : 0;
}

bool ShellSurfaceSink::rebuild(device::Extent2D extent, std::string* error) {
  if (!swapchain_) {
    if (error) *error = "ShellSurfaceSink::rebuild: 未 attach";
    return false;
  }
  device::Status s = swapchain_->rebuild(extent);
  if (!s.ok()) {
    if (error) *error = s.message;
    return false;
  }
  return true;
}

}  // namespace magicblocks::ui
