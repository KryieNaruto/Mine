// MagicBlocks —— Vulkan 后端:帧同步(D5)
//
// IFrameSync 只负责 fence/槽位/帧号 —— **不做 present**(present 属 ISwapchain,D3)。
//
// 【2026-11-11 重构:本类是 VulkanSyncCore 的一层薄委托,不再自己持有 fence】
//
// 改造前本类自己建了一套 fence(fences_),由 submit_empty() 去 signal。
// 而 t19 把真实提交改走 VulkanSyncCore 后,GPU 的完成凭据变成了
// core 的 slot.in_flight(见 vulkan_device_frame.cpp 的 vkQueueSubmit),
// 于是 submit_empty() 的调用方归零 → 那套 fence 创建时 signaled 一次之后再无
// 人 signal → wait_slot() 对着一个**永不再 signal 的 fence** 无界等待(UINT64_MAX)
// → 谁调谁永久挂死(表现为 ctest 超时,不是断言失败,比红更难查)。
//
// 现在:全部语义转发给 VulkanSyncCore 的槽 fence,**全仓只有一套 fence**。
// 两套 fence 并存这个裂缝是"缝合后消失",而不是留一份永不 signal 的死设施。
#pragma once

#include <cstdint>

#include "backend/vulkan/vk_common.h"
#include "magicblocks/device/frame_sync.h"

namespace magicblocks::backend {

class VulkanSyncCore;

class VulkanFrameSync final : public device::IFrameSync {
 public:
  // core 必须是**已 init 且整场存活**的同步核心:本类不拥有它,只转发。
  // 生命周期由 VulkanRenderDevice 保证(core 先建、后拆;frame_sync 在 core
  // 之前 reset,见 ~VulkanRenderDevice 的析构顺序注释)。
  explicit VulkanFrameSync(VulkanSyncCore* core);
  ~VulkanFrameSync() override;

  VulkanFrameSync(const VulkanFrameSync&) = delete;
  VulkanFrameSync& operator=(const VulkanFrameSync&) = delete;

  // D3:帧环大小与 VulkanSyncCore::in_flight_ **同值**(同一个真源)。
  // core 未就绪时返回 0 —— L4 见到 0 会直接拒绝建渲染器(明确失败,不静默降级)。
  device::u32 in_flight_count() const override;
  // D2:帧序号 → 槽位。与 core 的 current_slot() 同一个公式、同一个 N。
  device::u32 slot_for_frame(std::uint64_t frame_index) const override;

  // D1 + D4:等该槽**上一轮的提交**在 GPU 侧完成。
  // 转发到 core 的 slot.in_flight,**有界超时**(core 的 kDefaultFrameTimeoutNs,
  // 1 秒语义),超时返回 Status::DeviceLost 并带槽号 —— 绝不无限阻塞。
  device::Status wait_slot(device::u32 slot) override;

  // D2:帧号单一真源 —— 直接代理 core->frame_index(),不另设计数器。
  void advance_frame_index() override;
  std::uint64_t frame_index() const override;

 private:
  VulkanSyncCore* core_ = nullptr;  // 非拥有者
};

}  // namespace magicblocks::backend
