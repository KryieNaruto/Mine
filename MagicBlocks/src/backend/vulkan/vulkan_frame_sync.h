// MagicBlocks —— Vulkan 后端:帧同步(D5)
//
// IFrameSync 只负责 fence/槽位/帧号 —— **不做 present**(present 属 ISwapchain,D3)。
#pragma once

#include <vector>

#include "backend/vulkan/vk_common.h"
#include "magicblocks/device/frame_sync.h"

namespace magicblocks::backend {

class VulkanFrameSync final : public device::IFrameSync {
 public:
  VulkanFrameSync(VkDevice device, std::uint32_t in_flight);
  ~VulkanFrameSync() override;

  device::u32 in_flight_count() const override { return in_flight_; }
  device::u32 slot_for_frame(device::u64 frame) const override {
    return static_cast<device::u32>(frame % in_flight_);
  }
  device::Status wait_slot(device::u32 slot) override;
  void advance_frame_index() override { ++frame_index_; }
  device::u64 frame_index() const override { return frame_index_; }

  // L0 内部:拿某槽的 fence(给上层渲染通路用)
  VkFence fence(device::u32 slot) const { return fences_[slot]; }
  // 真提交一次空工作并使该槽 fence 进入"未 signal → 可被 wait_slot 复位"状态,
  // 用于验证槽位复用真的靠 fence 而不是靠算术。
  device::Status submit_empty(VkQueue queue, device::u32 slot);

 private:
  VkDevice device_ = VK_NULL_HANDLE;
  std::uint32_t in_flight_ = 0;
  std::vector<VkFence> fences_;
  device::u64 frame_index_ = 0;
};

}  // namespace magicblocks::backend
