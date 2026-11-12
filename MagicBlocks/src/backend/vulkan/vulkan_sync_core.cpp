// MagicBlocks —— Vulkan 后端:L0 多帧并行同步核心(实现)
//
// 设计说明见 vulkan_sync_core.h 的文件头。本文件的硬约束:
//   1. 渲染路径上**零** vkDeviceWaitIdle / vkQueueWaitIdle
//      (仅剩的两处在设备析构与交换链析构,vkDestroySwapchainKHR 的 VUID-01282
//       硬性要求有专门注释)。
//   2. 同步对象只创建一次、整场复用;命令缓冲同理(vkResetCommandBuffer)。
//   3. 每帧只等**当前槽位**的 fence,而不是设备/队列级全停。
#include "backend/vulkan/vulkan_sync_core.h"

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <utility>
#include <vector>

#include "backend/vulkan/vulkan_target.h"  // VulkanContext

namespace magicblocks::backend {

using namespace magicblocks::device;

namespace {
// 有界超时。无界等待的教训(vulkan_device_frame.cpp 旧注释):1fps 事故就是
// 无界等待时进程静默空转,CPU 0%、没有任何报错。有界超时把它变成一条能查的错误。
constexpr std::uint64_t kDefaultFrameTimeoutNs = 1'000'000'000ull;  // 1s

struct RegistryEntry {
  VkDevice device = VK_NULL_HANDLE;
  VulkanSyncCore* core = nullptr;
};

// 进程级登记表。加锁:OnSurfaceCreated/渲染/析构可能在不同线程。
// ⚠️ 只存指针,不拥有对象;对象析构时**按指针身份**反注册。
std::mutex g_registry_mutex;
std::vector<RegistryEntry> g_registry;

const char* result_text(VkResult r) { return vk_result_str(r).c_str(); }

}  // namespace

// ---------------------------------------------------------------- FrameRetireList

void FrameRetireList::destroy_all(VkDevice device) {
  if (device == VK_NULL_HANDLE) {
    render_passes_.clear();
    image_views_.clear();
    framebuffers_.clear();
    buffers_.clear();
    shared_buffers_.clear();
    return;
  }
  for (VkFramebuffer fb : framebuffers_) {
    if (fb != VK_NULL_HANDLE) vkDestroyFramebuffer(device, fb, nullptr);
  }
  for (VkImageView v : image_views_) {
    if (v != VK_NULL_HANDLE) vkDestroyImageView(device, v, nullptr);
  }
  for (VkRenderPass rp : render_passes_) {
    if (rp != VK_NULL_HANDLE) vkDestroyRenderPass(device, rp, nullptr);
  }
  // 缓冲最后销毁(与上面几种没有依赖关系,但沿用"逆序"的一致性)。
  // 到这里该槽的 fence 已经信号 —— 引用这些 VkBuffer 的 GPU 工作确实跑完了,
  // 此时 vkDestroyBuffer 才是安全的(VUID-vkDestroyBuffer-buffer-00922 不触发)。
  for (VkBuffer b : buffers_) {
    if (b != VK_NULL_HANDLE) vkDestroyBuffer(device, b, nullptr);
  }
  // 共享托管:每释放一份引用就把计数 -1,只有**最后一份**(所有引用过它的槽都
  // 排空了)才真正销毁。这就是"VkBuffer 跨帧复用 / 用完即弃"两种情况统一的生命
  // 周期表达 —— 全程零等待,纯 CPU 记账。
  for (const std::shared_ptr<BufferRef>& ref : shared_buffers_) {
    if (ref == nullptr) continue;
    if (ref.use_count() > 1) continue;  // 还有别的槽托管着它:继续留着
    if (ref->handle != VK_NULL_HANDLE) {
      vkDestroyBuffer(device, ref->handle, nullptr);
      ref->handle = VK_NULL_HANDLE;
    }
  }
  framebuffers_.clear();
  image_views_.clear();
  render_passes_.clear();
  buffers_.clear();
  shared_buffers_.clear();
}

// ---------------------------------------------------------------- VulkanSyncCore

VulkanSyncCore::~VulkanSyncCore() {
  // 设备若已析构且没人调 shutdown(),这里按记录下的 device 兜底销毁 ——
  // 但那种顺序本身是错的(VkDevice 已经没了),所以只在 device_ 还活着时做。
  // 正常路径:VulkanRenderDevice::~VulkanRenderDevice 先 shutdown() 再销毁 VkDevice。
  if (!slots_.empty() || device_ != VK_NULL_HANDLE) shutdown();
}

Status VulkanSyncCore::init(VkDevice device, VkCommandPool pool, std::uint32_t in_flight) {
  if (device == VK_NULL_HANDLE) return Status::InitFailed("sync_core: VkDevice 为空");
  if (pool == VK_NULL_HANDLE) return Status::InitFailed("sync_core: VkCommandPool 为空");
  if (in_flight < 2 || in_flight > 3) {
    return Status::Invalid("sync_core: in_flight 必须是 2 或 3,得到 " + std::to_string(in_flight) +
                           "(单帧在飞 = 旧设计的串行路径,不再允许)");
  }
  device_ = device;
  pool_ = pool;
  in_flight_ = in_flight;
  frame_index_ = 0;
  slots_.assign(in_flight, FrameSlot{});

  // ---- 每槽:2 个信号量 + 1 个 fence ----
  for (std::uint32_t i = 0; i < in_flight; ++i) {
    FrameSlot& s = slots_[i];
    VkSemaphoreCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkResult r = vkCreateSemaphore(device_, &sci, nullptr, &s.image_available);
    if (r != VK_SUCCESS) return Status::InitFailed(fmt_err("sync_core: vkCreateSemaphore(image_available)", r));
    r = vkCreateSemaphore(device_, &sci, nullptr, &s.render_finished);
    if (r != VK_SUCCESS) return Status::InitFailed(fmt_err("sync_core: vkCreateSemaphore(render_finished)", r));

    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    // 首轮即"已信号":第一帧的 begin_frame 不该等一个永远不会被信号的 fence。
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    r = vkCreateFence(device_, &fci, nullptr, &s.in_flight);
    if (r != VK_SUCCESS) return Status::InitFailed(fmt_err("sync_core: vkCreateFence", r));
  }

  // ---- 每槽:一条持久命令缓冲(vkAllocateCommandBuffers 只在这里做一次)----
  //
  // 改造前每帧 vkAllocateCommandBuffers + vkFreeCommandBuffers:
  // 不仅多一次驱动往返,更致命的是"命令缓冲只活一帧",于是它引用的
  // framebuffer / imageView / renderPass 也只能活一帧 —— 那正是
  // submit_device_frame() 必须在返回前同步等 fence 的根因。
  // 命令池已带 VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,复用只需
  // vkResetCommandBuffer(RESET 或 ONE_TIME_SUBMIT 之外不得在录制态重置)。
  std::vector<VkCommandBuffer> cmds(in_flight, VK_NULL_HANDLE);
  VkCommandBufferAllocateInfo cai{};
  cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cai.commandPool = pool_;
  cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cai.commandBufferCount = in_flight;
  VkResult r = vkAllocateCommandBuffers(device_, &cai, cmds.data());
  if (r != VK_SUCCESS) return Status::InitFailed(fmt_err("sync_core: vkAllocateCommandBuffers", r));
  for (std::uint32_t i = 0; i < in_flight; ++i) slots_[i].cmd = cmds[i];

  // ---- 辅助提交专用 fence(不参与帧槽轮转)----
  // 首轮即 signaled:第一次 submit_aux_and_wait 的"等上一次"不该真等。
  VkFenceCreateInfo aux_ci{};
  aux_ci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  aux_ci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
  r = vkCreateFence(device_, &aux_ci, nullptr, &aux_fence_);
  if (r != VK_SUCCESS) return Status::InitFailed(fmt_err("sync_core: vkCreateFence(aux)", r));

  bind(this);
  return Status::Ok_();
}

Status VulkanSyncCore::submit_aux_and_wait(VkQueue queue, VkCommandBuffer cmd,
                                           std::uint64_t timeout_ns) {
  if (device_ == VK_NULL_HANDLE || queue == VK_NULL_HANDLE || aux_fence_ == VK_NULL_HANDLE) {
    return Status::InitFailed("sync_core: submit_aux_and_wait 时未初始化");
  }
  if (cmd == VK_NULL_HANDLE) return Status::Invalid("sync_core: 辅助提交的命令缓冲为空");

  // 1) 先等"上一次辅助提交"完成(创建时就是 signaled,首轮不等待)。
  //    只有确认它 signal 过、可以复位,才允许复位 —— 否则 vkResetFences 一个
  //    尚未 signal 的 fence 是未定义行为。
  VkResult r = vkWaitForFences(device_, 1, &aux_fence_, VK_TRUE, timeout_ns);
  if (r == VK_TIMEOUT) return Status::DeviceLost("sync_core: 等上一次辅助提交超时");
  if (r != VK_SUCCESS) return Status::DeviceLost(fmt_err("sync_core: vkWaitForFences(aux)", r));
  r = vkResetFences(device_, 1, &aux_fence_);
  if (r != VK_SUCCESS) return Status::InitFailed(fmt_err("sync_core: vkResetFences(aux)", r));

  // 2) 提交到辅助 fence。**不碰** slots_ 里的任何 fence/semaphore:
  //    UI 通道 / 读回路径因此与 SDK 帧环完全解耦(架构不变量)。
  VkSubmitInfo si{};
  si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd;
  r = vkQueueSubmit(queue, 1, &si, aux_fence_);
  if (r != VK_SUCCESS) return Status::DeviceLost(fmt_err("sync_core: vkQueueSubmit(aux)", r));

  // 3) 定向等这一次提交完成。
  r = vkWaitForFences(device_, 1, &aux_fence_, VK_TRUE, timeout_ns);
  if (r == VK_TIMEOUT) return Status::DeviceLost("sync_core: 辅助提交超时(有界,避免静默空转)");
  if (r != VK_SUCCESS) return Status::DeviceLost(fmt_err("sync_core: 等辅助提交完成", r));
  return Status::Ok_();
}

void VulkanSyncCore::shutdown() {
  // 先把"我是谁的同步核心"这条登记摘掉:之后任何按 VkDevice 反查的路径都会
  // 明确报"未登记",而不是拿到一个正在被拆的对象。
  unbind();
  if (device_ == VK_NULL_HANDLE) {
    slots_.clear();
    return;
  }
  if (aux_fence_ != VK_NULL_HANDLE) {
    vkDestroyFence(device_, aux_fence_, nullptr);
    aux_fence_ = VK_NULL_HANDLE;
  }
  for (FrameSlot& s : slots_) {
    // 本槽延迟销毁栈:此时(设备层已排空在飞帧)没人再引用它们,可以安全销毁。
    s.retire.destroy_all(device_);
    if (s.image_available != VK_NULL_HANDLE) {
      vkDestroySemaphore(device_, s.image_available, nullptr);
      s.image_available = VK_NULL_HANDLE;
    }
    if (s.render_finished != VK_NULL_HANDLE) {
      vkDestroySemaphore(device_, s.render_finished, nullptr);
      s.render_finished = VK_NULL_HANDLE;
    }
    if (s.in_flight != VK_NULL_HANDLE) {
      vkDestroyFence(device_, s.in_flight, nullptr);
      s.in_flight = VK_NULL_HANDLE;
    }
  }
  std::vector<VkCommandBuffer> cmds;
  cmds.reserve(slots_.size());
  for (FrameSlot& s : slots_) {
    if (s.cmd != VK_NULL_HANDLE) {
      cmds.push_back(s.cmd);
      s.cmd = VK_NULL_HANDLE;
    }
  }
  if (!cmds.empty() && pool_ != VK_NULL_HANDLE) {
    vkFreeCommandBuffers(device_, pool_, static_cast<std::uint32_t>(cmds.size()), cmds.data());
  }
  slots_.clear();
  in_flight_ = 0;
  device_ = VK_NULL_HANDLE;
  pool_ = VK_NULL_HANDLE;
  frame_index_ = 0;
}

Status VulkanSyncCore::begin_frame(std::uint64_t timeout_ns) {
  if (!ready()) return Status::InitFailed("sync_core: begin_frame 时未初始化");
  FrameSlot& s = slots_[current_slot()];
  if (s.in_flight == VK_NULL_HANDLE) return Status::InitFailed("sync_core: 槽位 fence 为空");

  // 【多帧并行的关键点】只等**本槽**的 fence。其余 N-1 个槽可能还在 GPU 上跑,
  // 这正是与"每帧 vkDeviceWaitIdle"的本质区别:CPU 与 GPU 的重叠发生了。
  const VkResult r = vkWaitForFences(device_, 1, &s.in_flight, VK_TRUE, timeout_ns);
  if (r == VK_TIMEOUT) {
    return Status::DeviceLost("sync_core: 等待槽位 " + std::to_string(current_slot()) +
                              " 的 fence 超时 —— GPU 可能卡死(有界超时,避免静默空转)");
  }
  if (r != VK_SUCCESS) return Status::InitFailed(fmt_err("sync_core: vkWaitForFences", r));

  // 这个槽上一轮(第 i-N 帧)建的 pass 对象现在确实没人引用了 —— 真正销毁。
  // 这就是 submit_device_frame() 能去掉同步等待的全部理由。
  s.retire.destroy_all(device_);

  // 帧边界回调:此刻"第 i-N 帧确实跑完了"已被 fence 证明,于是
  //   - L1 分配器可以推进帧号 / 回收该帧引用的页(trim + in_flight 防护),
  //   - 设备可以按槽解除在途标记。
  // 传 frame_index_(单调帧号,**不是** current_slot() —— 槽号是 0..N-1 循环的,
  // 分配器需要单调递增的帧号来判定"某一帧已经彻底出清")**以及**刚就绪的槽号
  // (解除在途保护只能针对这一个槽,其余槽可能还在 GPU 上跑)。
  if (frame_tick_) frame_tick_(frame_index_, current_slot());

  // 复位命令缓冲,供本帧重新录制(池带 RESET_COMMAND_BUFFER_BIT)。
  const VkResult rr = vkResetCommandBuffer(s.cmd, 0);
  if (rr != VK_SUCCESS) {
    return Status::InitFailed(fmt_err("sync_core: vkResetCommandBuffer", rr));
  }
  return Status::Ok_();
}

Status VulkanSyncCore::end_frame(VkFence submitted) {
  if (!ready()) return Status::InitFailed("sync_core: end_frame 时未初始化");
  if (submitted != VK_NULL_HANDLE) {
    // 提交前复位 fence:否则 vkWaitForFences 会立刻返回,等于没等(旧实现在这里
    // 踩过 1fps:acquire 的 fence 一直处于 signaled 状态,每轮都等满超时)。
    const VkResult r = vkResetFences(device_, 1, &submitted);
    if (r != VK_SUCCESS) return Status::InitFailed(fmt_err("sync_core: vkResetFences", r));
  }
  ++frame_index_;
  return Status::Ok_();
}

Status VulkanSyncCore::drain(std::uint64_t timeout_ns) {
  if (!ready()) return Status::Ok_();
  std::vector<VkFence> fences;
  fences.reserve(slots_.size());
  for (const FrameSlot& s : slots_) {
    if (s.in_flight != VK_NULL_HANDLE) fences.push_back(s.in_flight);
  }
  if (fences.empty()) return Status::Ok_();
  const VkResult r =
      vkWaitForFences(device_, static_cast<std::uint32_t>(fences.size()), fences.data(), VK_TRUE,
                      timeout_ns);
  if (r == VK_TIMEOUT) return Status::DeviceLost("sync_core: 排空在飞帧超时");
  if (r != VK_SUCCESS) return Status::DeviceLost(fmt_err("sync_core: vkWaitForFences(排空)", r));
  // 全部槽位都停了:延迟销毁栈现在可以安全清空(对象确实没人引用)。
  for (FrameSlot& s : slots_) s.retire.destroy_all(device_);
  // 排空同样是一次帧边界。这里**所有**槽都已停下,所以逐槽回调,
  // 让每一份"按槽记账"的在途标记都被解除(只传一个槽号会漏掉其余槽)。
  if (frame_tick_) {
    for (std::uint32_t i = 0; i < slots_.size(); ++i) {
      frame_tick_(frame_index_, i);
    }
  }
  return Status::Ok_();
}

Status VulkanSyncCore::wait_slot_fence(std::uint32_t slot_index, const std::string& context,
                                       std::uint64_t timeout_ns) {
  if (!ready()) return Status::InitFailed("sync_core: wait_slot_fence 时未初始化");
  if (slot_index >= slots_.size()) {
    return Status::Invalid("sync_core: 槽 " + std::to_string(slot_index) + " 越界(槽数 " +
                           std::to_string(slots_.size()) + ")");
  }
  const VkFence f = slots_[slot_index].in_flight;
  if (f == VK_NULL_HANDLE) return Status::Ok_();  // 该槽没有 fence:没有可等的工作
  // 超时取默认有界值,kDefaultFrameTimeoutNs(1s 语义)。
  // 【不可以传 UINT64_MAX】无限等待会把"GPU 卡死"变成"进程永久挂死",
  // 在回归里表现为 ctest 超时而不是断言失败 —— 比红更难查。
  const std::uint64_t t = (timeout_ns == 0) ? kDefaultFrameTimeoutNs : timeout_ns;
  const VkResult r = vkWaitForFences(device_, 1, &f, VK_TRUE, t);
  if (r == VK_TIMEOUT) {
    return Status::DeviceLost(context.empty() ? "sync_core: 槽位 fence 等待超时" : context);
  }
  if (r != VK_SUCCESS) return Status::DeviceLost(fmt_err("sync_core: wait_slot_fence", r));
  return Status::Ok_();
}

void VulkanSyncCore::bind(VulkanSyncCore* core) {
  VulkanSyncRegistry::register_core(device_, core == nullptr ? this : core);
}

void VulkanSyncCore::unbind() { VulkanSyncRegistry::unregister_core(this); }

// ---------------------------------------------------------------- VulkanSyncRegistry

void VulkanSyncRegistry::register_core(VkDevice device, VulkanSyncCore* core) {
  if (device == VK_NULL_HANDLE || core == nullptr) return;
  std::lock_guard<std::mutex> lk(g_registry_mutex);
  for (RegistryEntry& e : g_registry) {
    if (e.core == core) {
      e.device = device;  // 同一核心重新登记:只更新设备句柄
      return;
    }
  }
  g_registry.push_back(RegistryEntry{device, core});
}

void VulkanSyncRegistry::unregister_core(VulkanSyncCore* core) {
  if (core == nullptr) return;
  std::lock_guard<std::mutex> lk(g_registry_mutex);
  // **按指针身份**删:即使 VkDevice 值已被驱动复用给新设备,也绝不会误删别人的登记。
  g_registry.erase(std::remove_if(g_registry.begin(), g_registry.end(),
                                  [core](const RegistryEntry& e) { return e.core == core; }),
                   g_registry.end());
}

VulkanSyncCore* VulkanSyncRegistry::find(VkDevice device) {
  if (device == VK_NULL_HANDLE) return nullptr;
  std::lock_guard<std::mutex> lk(g_registry_mutex);
  for (const RegistryEntry& e : g_registry) {
    if (e.device == device) return e.core;
  }
  return nullptr;
}

// ---------------------------------------------------------------- 帧同步辅助

Status submit_and_wait_directed(const VulkanContext& ctx, VkCommandBuffer cmd,
                                std::uint64_t timeout_ns) {
  if (ctx.device == VK_NULL_HANDLE || ctx.queue == VK_NULL_HANDLE) {
    return Status::InitFailed("submit_and_wait_directed: 设备未初始化");
  }
  if (cmd == VK_NULL_HANDLE) return Status::Invalid("submit_and_wait_directed: 命令缓冲为空");

  VkSubmitInfo si{};
  si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd;

  // 有登记的设备同步核心:走它的**辅助 fence**(aux_fence_)。
  // 这条 fence **不属于**帧环的任何一个槽位 —— UI 通道的提交既不占场景通道的
  // 槽位,也不去复位/等待 SDK 那一帧的 fence。架构不变量:
  // 「SDK 渲染通道与 UI 渲染通道相互独立」在同步层面就落在这里。
  VulkanSyncCore* core = VulkanSyncRegistry::find(ctx.device);
  if (core != nullptr && core->ready()) {
    return core->submit_aux_and_wait(ctx.queue, cmd, timeout_ns);
  }

  // 没有登记(手工拼 VulkanContext 的旁路,例如测试直接构造 ctx):
  // 建一颗**只属于这一次提交**的临时 fence,提交、定向等它、立刻销毁。
  // 注意:这仍然不是 queue/device 级全停 —— 只等这一次提交。
  VkFenceCreateInfo fci{};
  fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  VkFence fence = VK_NULL_HANDLE;
  VkResult r = vkCreateFence(ctx.device, &fci, nullptr, &fence);
  if (r != VK_SUCCESS) return Status::InitFailed(fmt_err("submit_and_wait_directed: vkCreateFence", r));
  r = vkQueueSubmit(ctx.queue, 1, &si, fence);
  Status out = Status::Ok_();
  if (r != VK_SUCCESS) {
    out = Status::DeviceLost(fmt_err("submit_and_wait_directed: vkQueueSubmit", r));
  } else {
    r = vkWaitForFences(ctx.device, 1, &fence, VK_TRUE, timeout_ns);
    if (r == VK_TIMEOUT) {
      out = Status::DeviceLost("submit_and_wait_directed: 等待本次提交超时");
    } else if (r != VK_SUCCESS) {
      out = Status::DeviceLost(fmt_err("submit_and_wait_directed: 等待本次提交", r));
    }
  }
  vkDestroyFence(ctx.device, fence, nullptr);
  return out;
}

Status wait_latest_frame(const VulkanContext& ctx) {
  if (ctx.device == VK_NULL_HANDLE) return Status::Ok_();
  VulkanSyncCore* core = VulkanSyncRegistry::find(ctx.device);
  if (core == nullptr || !core->ready()) {
    // 【不回落 vkDeviceWaitIdle】渲染路径上不允许出现 device 级全停。
    // 这条分支只会在"有人手工拼了一个 VulkanContext 却没建同步核心"时命中;
    // 那种情况下我们**没有任何可定向等待的对象**,与其偷偷全停设备,不如明确报错。
    return Status::InitFailed(
        "wait_latest_frame: 该 VkDevice 未登记同步核心,无法定向等待最近一帧"
        "(生产路径由 create_render_device() 建;手工构造 VulkanContext 的旁路请自行同步)");
  }
  // 【必须是 last_submitted_slot(),不是 current_slot()】2026-11-11 修。
  //
  // 调用链:submit_device_frame() → end_frame() → 内部 ++frame_index_ → 返回 →
  // 上层 readback_rgba8() → 这里。此刻 current_slot() 已被 end_frame 推到
  // **下一帧**的槽,拿它去等就等于等了一个和刚提交的那批无关的 fence:
  // 本帧的 fence 从没被等过。N=2 时表现为"读回可能读到还没写完的像素",
  // 且 13 条 CLI 出图路径全是 submit → readback 这个模式,全部受影响。
  FrameSlot& s = core->slot(core->last_submitted_slot());
  if (s.in_flight == VK_NULL_HANDLE) return Status::Ok_();
  const VkResult r = vkWaitForFences(ctx.device, 1, &s.in_flight, VK_TRUE, kDefaultFrameTimeoutNs);
  if (r == VK_TIMEOUT) {
    return Status::DeviceLost("wait_latest_frame: 刚提交那一帧(槽 " +
                              std::to_string(core->last_submitted_slot()) + ")fence 超时");
  }
  if (r != VK_SUCCESS) return Status::DeviceLost(fmt_err("wait_latest_frame: vkWaitForFences", r));
  return Status::Ok_();
}

Status wait_device_frames_idle(const VulkanContext& ctx) {
  if (ctx.device == VK_NULL_HANDLE) return Status::Ok_();
  VulkanSyncCore* core = VulkanSyncRegistry::find(ctx.device);
  if (core != nullptr && core->ready()) {
    // N 槽 fence 全等 = "设备在飞帧已排空",语义上等价于 vkDeviceWaitIdle,
    // 但走的是我们自己持有的 fence,不需要 queue 级全停。
    return core->drain(kDefaultFrameTimeoutNs);
  }
  // 【不回落 vkDeviceWaitIdle】理由同 wait_latest_frame。
  return Status::InitFailed(
      "wait_device_frames_idle: 该 VkDevice 未登记同步核心,无法按槽位排空在飞帧");
}

}  // namespace magicblocks::backend
