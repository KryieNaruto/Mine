// MagicBlocks UI 消费层 —— 主路径呈现宿主(spec §5.2 的 2/4/5)
//
// `IPresentSurface` 的落地:acquire / submit / present。
// SDK 目前只有 L0 `ISwapchain`,既没有 fence 也没有 semaphore(见
// `magicblocks/device/frame_sync.h`:IFrameSync 只做槽位,不做 present),
// 所以真上屏的采集/提交/呈现三家还没法接上。本宿主因此分成两种状态:
//
//   attached() == true  —— 壳层给了原生 surface(Android ANativeWindow / Win32 HWND),
//                          swapchain 已建成。此时 acquire 走 L0 的 current_target()
//                          (L0 的交换链没有真 acquire,返回"当前目标视图"),事件循环
//                          由壳层驱动。**submit/present 明确返回 false**:没有可用的
//                          fence/semaphore 通路,假装成功等于把上屏问题藏起来。
//   attached() == false —— 后台/无 surface:Present 阶段返回 NoSurface(G7 跳过),
//                          不消耗帧号、不留黑屏状态。
//
// 替换路径(等 SDK L1/L2 落地):把 `acquire/submit/present` 换成真
// vkAcquireNextImageKHR / vkQueueSubmit(fence+semaphore) / vkQueuePresentKHR,
// Presenter 的状态机与全部断言**完全不用改**。
#include "presenter_host.h"

#include <algorithm>
#include <cstring>

#include "backend/vulkan/vulkan_render_device.h"
#include "backend/vulkan/vulkan_swapchain.h"
#include "backend/vulkan/vulkan_ui_overlay.h"

namespace magicblocks::ui {

PresenterHost::PresenterHost() = default;
PresenterHost::~PresenterHost() = default;

bool PresenterHost::attach(device::IRenderDevice* device, const device::NativeSurface& surface,
                           device::Extent2D extent, std::string* error) {  detach();
  if (!device) {
    if (error) *error = "PresenterHost::attach: device 为空";
    return false;
  }
  if (surface.kind == device::NativeSurface::Kind::Headless) {
    if (error) *error = "PresenterHost::attach: Headless 不建交换链(请用离屏模式)";
    return false;
  }
  auto sc = device->create_swapchain(surface, extent);
  if (!sc.ok()) {
    if (error) *error = sc.message();
    return false;
  }
  device_ = device;
  swapchain_ = std::move(sc.value());
  last_failure_ = PresenterStatus::Ok;
  return true;
}

void PresenterHost::detach() {
  if (swapchain_) {
    // 交换链悬挂在 device 上,必须**先于** device 析构(壳层负责顺序)。
    swapchain_.reset();
  }
  device_ = nullptr;
}

bool PresenterHost::acquire(FrameImage* out_image) {
  last_failure_ = PresenterStatus::Ok;
  if (!swapchain_) {
    last_failure_ = PresenterStatus::NoSurface;
    return false;
  }
  device::IRenderTarget* target = swapchain_->current_target();
  if (!target) {
    last_failure_ = PresenterStatus::NeedRebuild;
    return false;
  }
  if (out_image) {
    out_image->id = 0;
    out_image->width = target->extent().width;
    out_image->height = target->extent().height;
    out_image->valid = true;
  }
  return true;
}

bool PresenterHost::submit(std::uint64_t, std::uint64_t) {
  // L0 没有 fence/semaphore 通路(spec §5.2 第 4 步所需的对象尚未由 SDK 提供)。
  // 明确失败,不 pretend。
  last_failure_ = PresenterStatus::SubmitFailed;
  return false;
}

bool PresenterHost::present(const FrameImage&, std::uint64_t) {
  last_failure_ = PresenterStatus::PresentFailed;
  return false;
}

// src/ui 是 UI 逻辑,G3 禁止出现 Vk*；这里只做**不透明句柄透传**：
// 通过 device 公共面拿到当前 swapchain 目标,再用后端私有接口取句柄值。
// 句柄值只被当成不透明整数传给 backend 内部,src/ui 不解引用、不比较语义。
bool PresenterHost::ready() const {
  if (!swapchain_) return false;
  return swapchain_->current_target() != nullptr;
}

PresenterStatus PresenterHost::last_failure() const { return last_failure_; }

std::uint64_t PresenterHost::current_image_handle() const {
  if (!swapchain_) return 0;
  device::IRenderTarget* target = swapchain_->current_target();
  if (!target) return 0;
  // 后端私有钩子:swapchain 实现知道自己的当前 image 是哪一张。
  auto* probe = dynamic_cast<backend::SwapchainImageProbe*>(swapchain_.get());
  return probe ? probe->current_image_value() : 0;
}

bool PresenterHost::rebuild(device::Extent2D extent, std::string* error) {
  if (!swapchain_) {
    if (error) *error = "PresenterHost::rebuild: 未 attach";
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
