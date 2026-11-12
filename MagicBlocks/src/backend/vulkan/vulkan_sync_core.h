// MagicBlocks —— Vulkan 后端:L0 多帧并行同步核心
//
// 为什么需要这个文件(2026-11-11,用户硬约束「最开始的设计居然没有多帧并行!」):
//
//   改造前,渲染路径上是**三处 device/queue 级全停** + 一处"提交后同步等 fence":
//     vulkan_device_frame.cpp  submit 后 vkWaitForFences(1s) + 紧跟 cleanup()
//     vulkan_ui_overlay.cpp    提交后 vkQueueWaitIdle
//     vulkan_target.cpp        读回时 vkQueueWaitIdle
//     vulkan_draw.cpp          CLI 旁路 vkQueueWaitIdle
//     vulkan_swapchain.cpp     destroy_swapchain()(OUT_OF_DATE 重建路径)vkDeviceWaitIdle
//     vulkan_render_device.cpp wait_idle() 公开 API 直接 vkDeviceWaitIdle
//   于是每帧严格串行:CPU 录完就干等 GPU,GPU 完整跑完一帧 CPU 才开始下一帧。
//
//   本文件把同步从"每帧临时造、造完就炸掉"改成**设备持有的 N 槽环形资源池**,
//   每个槽一套完整、互不干扰的同步与命令对象:
//
//     槽 i: image_available(i)  render_finished(i)  in_flight(i)[fence]
//            cmd(i)[持久复用]  retire(i)[延迟销毁栈]
//
//   一帧的生命周期(见 FrameRing::begin_frame / end_frame):
//     begin_frame  → 等 in_flight(i) 信号(只等这一个槽)→ 复位 → 执行该槽的退休栈
//     acquire      → vkAcquireNextImageKHR(…, image_available(i), VK_NULL_HANDLE, …)
//     submit       → vkQueueSubmit(… wait=image_available(i) → signal=render_finished(i),
//                                     fence=in_flight(i))
//     present      → vkQueuePresentKHR(waitSemaphores = [render_finished(i)])
//
//   注意 image_available / render_finished **必须分开**:
//     image_available 的对应方是 vkAcquireNextImageKHR,render_finished 的对应方是
//     vkQueuePresentKHR。用同一个信号量时,驱动侧的等待/信号配对会被打乱
//     (首帧 image_available 尚未被信号化就被 present 等 —— 死锁或校验层报错)。
//
// 为什么挂在 VulkanRenderDevice 上,而不是别处(两条都是踩过的事故):
//   1. **不是 thread_local**:VkDevice 句柄值会被驱动复用 —— 销毁旧设备再建新设备,
//      新设备完全可能拿到同一个句柄值,"值相同"被误判成"同一个设备",于是拿
//      已销毁的 VkFence 去 vkResetFences → 无效对象 + 崩溃。曾一次性炸掉 106 条用例。
//   2. **不放进 VulkanContext**:VulkanContext 是纯句柄容器,被 backend 各层**按值**
//      到处传递(VulkanSwapchain、OffscreenTarget、VulkanCommandList 都存了一份)。
//      把需要 vkCreateSemaphore/vkDestroySemaphore 的拥有型资源塞进去,
//      "谁负责销毁"立刻含糊,而且是 N 份拷贝各自持有同一组句柄。
//   非拥有者只拿**裸指针**登记(见 VulkanSyncRegistry),生命周期由设备严格包住。
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "backend/vulkan/vk_common.h"
#include "magicblocks/device/result.h"

namespace magicblocks::backend {

class VulkanContext;
class VulkanSyncCore;

// 帧内"延迟销毁"栈:本帧 submit_device_frame() 建的 framebuffer / imageView /
// renderPass 全部压到**本帧槽位**的退休栈上,该槽位的 fence 信号之后才真正销毁。
//
// 这是 submit 去同步化的**前置条件**:改造前 submit_device_frame() 返回前就把这些
// 对象销毁了,所以它必须先 vkWaitForFences 等 GPU 跑完 —— 去掉等待就是 use-after-free。
// 现在销毁时机改为"N 帧之后该槽位复用时",等待自然只剩 begin_frame 里那一次
// 定向的 vkWaitForFences(槽位级),不再有"提交即阻塞"。
class FrameRetireList {
 public:
  // 一个 VkBuffer 的共享托管票据。
  //
  // VkBuffer 会**跨越**多个帧/槽被引用(同一块实例缓冲连续多帧在用),所以
  // "某个槽排空"并不代表"这个 VkBuffer 没人读了"。这里用 shared_ptr 的引用计数
  // 表达:每托管一次 +1,每销毁一个槽的那份 -1,减到 0 才真正 vkDestroyBuffer。
  //   - 引用计数归零 ⇒ 所有引用过它的槽都已经排空 ⇒ GPU 确实不读了;
  //   - 不需要等待、不需要任何帧级阻塞,纯 CPU 记账。
  struct BufferRef {
    VkBuffer handle = VK_NULL_HANDLE;
  };

  // 把这一帧建的对象**交出去**:之后由 FrameRing 在合适时机销毁。
  void push_owned_render_pass(VkRenderPass rp) { render_passes_.push_back(rp); }
  void push_owned_image_view(VkImageView view) { image_views_.push_back(view); }
  void push_framebuffer(VkFramebuffer fb) { framebuffers_.push_back(fb); }
  // VkBuffer 的移交入口(2026-11-11,提交异步化)。
  //
  // 为什么 buffer 也要走这里:改造前 submit 返回即 GPU 完成,"缓冲随手扔"成立;
  // 现在提交是异步的,调用方在 submit_command_list() 返回后立刻销毁网格/实例
  // 缓冲是合法用法(gtest 的 FrameRig、F-VX 的 slot->reset() 都是),
  // 直接 vkDestroyBuffer 就是 VUID-vkDestroyBuffer-buffer-00922 + UAF。
  // 而**不能**用"析构里排空全部槽位"来兜:那等于每销毁一个缓冲就把多帧并行
  // 清零一次。所以按槽托管 —— 只有真正用过它的那个槽,才在复用时销毁它。
  void push_owned_buffer(VkBuffer b) { buffers_.push_back(b); }
  // 带计数的托管:同一个 VkBuffer 可能被多帧/多槽同时引用,必须等**所有**托管
  // 它的槽都排空后才销毁。计数放在共享的 box 里,各槽各持一份引用。
  void push_shared_buffer(std::shared_ptr<BufferRef> ref) { shared_buffers_.push_back(std::move(ref)); }

  bool empty() const {
    return render_passes_.empty() && image_views_.empty() && framebuffers_.empty() &&
           buffers_.empty() && shared_buffers_.empty();
  }
  // 销毁顺序与创建顺序相反:framebuffer → imageView → renderPass → buffer
  // (framebuffer 引用 imageView 与其 renderPass 的兼容性,view 引用 image;
  //  buffer 彼此独立,放在最后即可)。
  void destroy_all(VkDevice device);

 private:
  std::vector<VkRenderPass> render_passes_;
  std::vector<VkImageView> image_views_;
  std::vector<VkFramebuffer> framebuffers_;
  std::vector<VkBuffer> buffers_;
  std::vector<std::shared_ptr<BufferRef>> shared_buffers_;
};

// 一个 in-flight 槽位。**句柄只创建一次**,之后整场复用。
struct FrameSlot {
  VkSemaphore image_available = VK_NULL_HANDLE;   // acquire → submit
  VkSemaphore render_finished = VK_NULL_HANDLE;   // submit → present
  VkFence in_flight = VK_NULL_HANDLE;             // submit → 槽位复用
  VkCommandBuffer cmd = VK_NULL_HANDLE;           // 持久复用(vkResetCommandBuffer)
  FrameRetireList retire;                         // 本槽位延迟销毁栈
};

// N 槽环形帧同步。N = DeviceDesc::in_flight_frames ∈ {2,3},由设备校验。
class VulkanSyncCore {
 public:
  // 帧边界回调:槽位 fence 确认信号后触发(见 begin_frame)。
  //
  // 【为什么是回调而不是让 sync_core 自己去找分配器】
  // sync_core 在 backend 里、L1 分配器在 resources 面,两边直接互相 include 会把
  // "同步核心"和"资源分配"耦成一个循环。这里只暴露一个 std::function:
  // 设备在 init() 里把「推分配器帧号 + 回收退役缓冲」挂上来,core 只负责在正确的
  // 时机(槽位 fence 之后)叫一声。
  //
  // 参数语义(两个都必要,不能省成一个):
  //   frame_index = 单调帧号(分配器要它判"哪一帧已彻底出清");
  //   slot        = 刚刚就绪的那个槽(**只有这个槽**可以让出在途资源,
  //                 其余 N-1 个槽可能还在 GPU 上跑)。
  // 曾经只传 frame_index,于是回调里只能"全清在途标记" —— 那会在第 f+1 帧的
  // 边界就把第 f 帧仍被 GPU 读着的页解开保护。按槽记账必须知道是哪个槽。
  using FrameTick = std::function<void(std::uint64_t frame_index, std::uint32_t slot)>;
  void set_frame_tick(FrameTick fn) { frame_tick_ = std::move(fn); }

  VulkanSyncCore() = default;
  VulkanSyncCore(const VulkanSyncCore&) = delete;
  VulkanSyncCore& operator=(const VulkanSyncCore&) = delete;
  ~VulkanSyncCore();
  // 建全部同步对象与**持久命令缓冲**。命令池必须带
  // VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT(本仓库 cmd_pool 已带)。
  // 失败时已建的对象由析构函数兜底销毁(半成品不会泄漏)。
  device::Status init(VkDevice device, VkCommandPool pool, std::uint32_t in_flight);
  // 销毁全部同步对象与命令缓冲。调用方必须已经排空在飞帧
  // (见 VulkanRenderDevice::drain_submissions)。
  void shutdown();

  std::uint32_t in_flight() const { return in_flight_; }
  bool ready() const { return in_flight_ > 0 && slots_.size() == in_flight_; }
  std::uint64_t frame_index() const { return frame_index_; }
  std::uint32_t slot_count() const { return static_cast<std::uint32_t>(slots_.size()); }

  // **下一帧**将要使用的槽位(按 FIFO 轮转:第 i 帧用槽 i % N)。
  // 语义限制:只在 begin_frame 之前 / 之内有意义。begin_frame 之后到 end_frame
  // 之前它仍是"本帧正在录制的槽";**end_frame 一执行(内部 ++frame_index_),
  // 它立刻改为指向下一帧** —— 所以"刚提交完的那一帧是哪个槽"不能问它。
  std::uint32_t current_slot() const {
    return in_flight_ == 0 ? 0u : static_cast<std::uint32_t>(frame_index_ % in_flight_);
  }

  // **刚提交完的那一帧**用的槽位(submit 返回到 end_frame 之后都正确)。
  //
  // 为什么必须单独有这个访问器(2026-11-11 修,由 syncl13 在 t20 复核时发现):
  //   end_frame() 内部执行 ++frame_index_,于是 submit 成功后 current_slot()
  //   已经指向**下一帧**的槽。凡是"提交之后"还要认领本帧槽位的地方,用
  //   current_slot() 都会偏一槽:
  //     - wait_latest_frame():等到了另一个在飞帧的 fence,本帧从没被等过
  //       → N=2 时读回可能读到没写完的像素(13 条 CLI 出图路径全中);
  //     - 设备侧在飞页记账 / 缓冲退休托管:记到下一帧的槽上,而下一帧
  //       begin_frame() 一进来就 destroy_all() 该槽退休栈 → use-after-free。
  //   公式 (frame_index_ + N - 1) % N 即"上一帧的槽";N-1 而不是 N 是为了避开
  //   in_flight_==0 时的下溢。in_flight_ 为 0 时返回 0(与 current_slot 一致)。
  std::uint32_t last_submitted_slot() const {
    return in_flight_ == 0
               ? 0u
               : static_cast<std::uint32_t>((frame_index_ + in_flight_ - 1) % in_flight_);
  }

  FrameSlot& slot(std::uint32_t i) { return slots_[i]; }
  const FrameSlot& slot(std::uint32_t i) const { return slots_[i]; }

  // ---- 一帧的开头 ----
  // 等**当前槽位**的 fence(只等这一个槽,不是 vkDeviceWaitIdle),复位它,
  // 执行该槽延迟销毁栈(上一轮用这张命令缓冲的对象现在确实没人用了)。
  device::Status begin_frame(std::uint64_t timeout_ns);
  // ---- 一帧的结尾 ----
  // 把 fence 复位(必须在 vkQueueSubmit 之前:否则 wait 会立刻返回)、推进帧序号。
  // fence 为 VK_NULL_HANDLE 时只推进序号(本帧没有提交)。
  device::Status end_frame(VkFence submitted);

  // 等全部槽位 fence。用于:设备析构前、swapchain 重建前、公开 wait_idle()、
  // 读回前的"排空"。**不调用 vkDeviceWaitIdle / vkQueueWaitIdle**。
  device::Status drain(std::uint64_t timeout_ns);

  // ---- 单个槽位的定向等待(给 IFrameSync 用)----
  //
  // 等 slot.in_flight —— 也就是**该槽上一轮的提交**是否已在 GPU 侧完成。
  // 这正是 VulkanFrameSync::wait_slot() 的语义,这里做成 core 的成员是为了:
  //   1. 有界超时只有一份实现(默认 kDefaultFrameTimeoutNs),不会某条路径
  //      手滑传个 UINT64_MAX 就把上层永久挂死;
  //   2. 超时时能报出**槽号**与调用方给的上下文串,便于定位是哪一帧卡住。
  // 不复位 fence、不推进帧号 —— 纯只读等待,可以安全地重复调用。
  // timeout_ns 传 0 表示用默认有界超时;显式给值则用该值。
  device::Status wait_slot_fence(std::uint32_t slot_index, const std::string& context,
                                 std::uint64_t timeout_ns = 0);

  // ---- 辅助提交(UI pass / 读回 / CLI 自检出图)----
  //
  // 提交一条"不属于帧环"的一次性命令缓冲,并**定向等待它自己完成**。
  //
  // 为什么不复用帧环槽位的 fence:槽位 fence 属于 SDK 场景通道。
  // UI 通道的提交如果去借场景通道当前槽位的 fence,就变成
  //   「UI 提交必须等 SDK 那一批完成 / 甚至复位一个尚未 signal 的 fence」——
  // 那直接破坏「SDK 渲染通道与 UI 渲染通道相互独立」这条架构不变量。
  //
  // 所以这里有一条**独立的、不参与帧槽轮转**的 fence(aux_fence_):
  //   创建一次、整场复用;每次提交前先等它(上一次辅助提交肯定早已完成)、
  //   再复位、再提交 —— 于是"上一次"与"这一次"之间不会互相踩,
  //   也永远不会碰到 slots_ 里的任何 fence。
  // 代价:辅助提交之间彼此串行。这是正确的取舍 —— 它们本来就是
  // "CPU 必须拿到结果"的同步点(读回 / 出图),而且与帧环完全解耦。
  device::Status submit_aux_and_wait(VkQueue queue, VkCommandBuffer cmd, std::uint64_t timeout_ns);

 private:
  // 把(或摘掉)自己写进 VulkanSyncRegistry 的登记表。init/shutdown 内部调用。
  void bind(VulkanSyncCore* core);
  void unbind();

  VkDevice device_ = VK_NULL_HANDLE;
  VkCommandPool pool_ = VK_NULL_HANDLE;
  std::uint32_t in_flight_ = 0;
  std::uint64_t frame_index_ = 0;
  std::vector<FrameSlot> slots_;
  // 辅助提交专用 fence(不参与帧槽轮转,见 submit_aux_and_wait)。
  VkFence aux_fence_ = VK_NULL_HANDLE;
  // 帧边界回调(见 set_frame_tick)。
  FrameTick frame_tick_;
};

// 设备级的"在飞帧排空"登记表(非拥有者视角)。
//
// 为什么需要全局表:swapchain 只持有 VulkanContext(纯句柄),而 VulkanContext 到处
// 按值传,不能塞拥有型资源。于是设备在 init() 成功后把自己的同步核心**登记**到
// 这张表里(以 VkDevice 为键),swapchain / UI / 读回路径按 ctx.device 反查:
//   - 找得到 → 走槽位 fence 定向等待(多帧并行的正确路径);
//   - 找不到 → **不回落 device 级等待**:返回"未登记"让调用方明确失败
//     (生产路径上设备一定登记过;手工拼 VulkanContext 的测试旁路自己负责同步)。
//
// 历史事故(不要改回"按 VkDevice 值做静态缓存"):VkDevice 句柄值会被驱动复用,
// 旧设备的登记在线程缓存里没清,新设备拿同一个值 → 命中已销毁的同步对象 → 崩溃。
// 所以这里:
//   1. 不用 thread_local(跨线程/跨设备都会串);
//   2. 设备析构时**按指针身份反注册**(unregister 时比对指针,不比对值),
//      即使句柄值被复用也不会命中悬垂项。
class VulkanSyncRegistry {
 public:
  static void register_core(VkDevice device, VulkanSyncCore* core);
  static void unregister_core(VulkanSyncCore* core);
  // 没有登记(或已注销)时返回 nullptr。
  static VulkanSyncCore* find(VkDevice device);
};

// ---- 帧同步辅助(全 backend 共用,避免各处各造一套)----

// 一次性提交并**定向等待这一次提交**(不是 queue/device 级全停)。
// 用于:离屏读回(CPU 必须等像素落地)、CLI 自检出图旁路、UI 离屏 pass。
//
//   - 有设备同步核心:走 core 的**辅助 fence**(aux_fence_,见 VulkanSyncCore::
//     submit_aux_and_wait)—— 与帧环槽位完全无关,不抢场景通道的 fence,
//     UI 通道与 SDK 通道互不等待;
//   - 没有核心(测试手工拼 VulkanContext 的旁路):建临时 fence,提交,等它,销毁它。
//   两条路径都不调用 vkDeviceWaitIdle / vkQueueWaitIdle。
//
// 返回值:Status::Ok_() 表示这一次提交已完成;否则带 VkResult 细节。
device::Status submit_and_wait_directed(const VulkanContext& ctx, VkCommandBuffer cmd,
                                        std::uint64_t timeout_ns);

// 等待**设备全部在飞帧**完成(帧环槽位 fence 全等)。
// 调用点:公开 wait_idle()、swapchain 重建前。
// 没有登记同步核心时返回 InitFailed("未登记同步核心,无法定向等待") ——
// **不回落到 vkDeviceWaitIdle**(渲染路径上不允许出现 device 级全停)。
device::Status wait_device_frames_idle(const VulkanContext& ctx);

// 等待最近一次 acquire/提交的那一帧完成(读回前用)。
// 有核心时只等**当前槽位**的 fence(不波及其他在飞帧);
// 没核心时同样返回 InitFailed 而不是 device 级全停。
device::Status wait_latest_frame(const VulkanContext& ctx);

}  // namespace magicblocks::backend
