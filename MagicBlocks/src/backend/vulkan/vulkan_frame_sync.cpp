// MagicBlocks —— Vulkan 后端:帧同步实现(D5)
//
// 本文件**不再创建任何 VkFence**。全部语义转发给 VulkanSyncCore 的槽 fence,
// 因此全仓只有一套 GPU 完成凭据。设计说明见 vulkan_frame_sync.h 的文件头。
#include "backend/vulkan/vulkan_frame_sync.h"

#include <string>

#include "backend/vulkan/vulkan_sync_core.h"

namespace magicblocks::backend {

using namespace magicblocks::device;

VulkanFrameSync::VulkanFrameSync(VulkanSyncCore* core) : core_(core) {}

// 不拥有 core,也不销毁任何 Vk 对象 —— 槽 fence 的生命周期归 VulkanSyncCore
// (它在 init() 里创建、shutdown() 里销毁)。这里空实现是正确的,
// 而不是"忘了写"。
VulkanFrameSync::~VulkanFrameSync() = default;

u32 VulkanFrameSync::in_flight_count() const {
  if (core_ == nullptr || !core_->ready()) return 0;
  // 直接用槽数(= core 实际持有的 FrameSlot 数量),而不是 core_->in_flight():
  // 两个数在 init 里相等,但槽数组才是 wait_slot 真正索引的东西。取真正被索引的
  // 那个,才能保证"能等的槽"与"报告出来的槽数"永远一致。
  return core_->slot_count();
}

u32 VulkanFrameSync::slot_for_frame(std::uint64_t frame_index) const {
  const u32 n = in_flight_count();
  if (n == 0) return 0;
  return static_cast<u32>(frame_index % n);
}

std::uint64_t VulkanFrameSync::frame_index() const {
  return core_ == nullptr ? 0ull : core_->frame_index();
}

void VulkanFrameSync::advance_frame_index() {
  if (core_ == nullptr || !core_->ready()) return;
  // 【帧号单一真源】这里不去 ++ 任何本类成员(本类已经没有帧号成员)。
  // 走 core->end_frame(VK_NULL_HANDLE):语义就是"本帧没有提交,只推进序号",
  // 正好与"只推进帧号"的 IFrameSync 契约一致,而且不会复位任何 fence
  // (传的是 VK_NULL_HANDLE,end_frame 内部跳过 vkResetFences)。
  //
  // 不用 core 的私有 ++:frame_index_ 只有一个写入点(end_frame),留给
  // 真正提交的路径去推,这里借用同一个入口,避免出现第二个自增点。
  (void)core_->end_frame(VK_NULL_HANDLE);
}

Status VulkanFrameSync::wait_slot(u32 slot) {
  if (core_ == nullptr || !core_->ready()) {
    return Status::InitFailed("wait_slot: 同步核心未就绪(设备未初始化?)");
  }
  if (slot >= core_->slot_count()) {
    return Status::Invalid("wait_slot: slot " + std::to_string(slot) + " 越界(槽数 " +
                           std::to_string(core_->slot_count()) + ")");
  }
  // 【D1】等的就是真实提交挂的那颗 fence(vulkan_device_frame.cpp 的
  //   vkQueueSubmit(..., slot.in_flight)),不是另建的一套。
  // 【D4】有界超时由 core 内部保证(kDefaultFrameTimeoutNs = 1s 语义),
  //   超时返回 DeviceLost 而不是永久阻塞。挂死比红更难查,而且会污染
  //   整条回归流水线(表现为 ctest 超时而不是断言失败)。
  //   这里刻意**不**把 UINT64_MAX 传下去。
  const std::string ctx = "wait_slot: 槽 " + std::to_string(slot) + " 等待超时";
  return core_->wait_slot_fence(slot, ctx);
}

}  // namespace magicblocks::backend
