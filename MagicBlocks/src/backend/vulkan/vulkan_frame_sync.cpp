// MagicBlocks —— Vulkan 后端:帧同步实现
#include "backend/vulkan/vulkan_frame_sync.h"

namespace magicblocks::backend {

using namespace magicblocks::device;

VulkanFrameSync::VulkanFrameSync(VkDevice device, std::uint32_t in_flight)
    : device_(device), in_flight_(in_flight) {
  fences_.assign(in_flight_, VK_NULL_HANDLE);
  for (std::uint32_t i = 0; i < in_flight_; ++i) {
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;  // 首轮即可用
    if (vkCreateFence(device_, &fci, nullptr, &fences_[i]) != VK_SUCCESS) {
      fences_[i] = VK_NULL_HANDLE;
    }
  }
}

VulkanFrameSync::~VulkanFrameSync() {
  for (auto f : fences_) {
    if (f) vkDestroyFence(device_, f, nullptr);
  }
}

Status VulkanFrameSync::wait_slot(u32 slot) {
  if (slot >= in_flight_) return Status::Invalid("wait_slot: slot 越界");
  VkFence f = fences_[slot];
  if (!f) return Status::InitFailed("wait_slot: fence 未创建");
  VkResult r = vkWaitForFences(device_, 1, &f, VK_TRUE, UINT64_MAX);
  if (r != VK_SUCCESS) return Status::DeviceLost(fmt_err("vkWaitForFences", r));
  return Status::Ok_();
}

Status VulkanFrameSync::submit_empty(VkQueue queue, u32 slot) {
  if (slot >= in_flight_) return Status::Invalid("submit_empty: slot 越界");
  VkFence f = fences_[slot];
  if (!f) return Status::InitFailed("submit_empty: fence 未创建");
  // 真提交空批:fence 由 signaled → 未 signaled,随后靠 GPU 完成再 signal。
  VkResult r = vkResetFences(device_, 1, &f);
  if (r != VK_SUCCESS) return Status::InitFailed(fmt_err("vkResetFences", r));
  VkSubmitInfo si{};
  si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  r = vkQueueSubmit(queue, 1, &si, f);
  if (r != VK_SUCCESS) return Status::DeviceLost(fmt_err("vkQueueSubmit", r));
  return Status::Ok_();
}

}  // namespace magicblocks::backend
