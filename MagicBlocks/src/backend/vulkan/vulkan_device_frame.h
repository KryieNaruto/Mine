// MagicBlocks —— Vulkan 后端:设备帧提交器(L3 的"命令流真正出图"通路)
//
// 背景(为什么需要这个文件):
//   L1 的 `ICommandList` 里 `draw*` **只写入内部记录**,不调 vkCmdDraw
//   (plans-l1 的 U1 裁定);`bind_vertex_buffer` 也只是记录句柄,不调
//   vkCmdBindVertexBuffers。于是 L2 录制的命令流在设备态上"录了但没画",
//   L0/L2 的 golden 只能靠 `OffscreenTarget::draw_selftest()` 这条旁路出图。
//
//   L3 的要求不同:**每一次 L3 绘制都必须产生像素**,而且必须与命令流一一对应
//   (spec §5「录制 vs 回放」的逐像素校验)。因此这里新增一条 backend 内的
//   "把已录制的 11 类命令真正编码成 vkCmd* 并提交"的通路:
//
//     ICommandList(设备态录制) ──recorded()──► submit_device_frame()
//                                                     │ 逐条翻译
//                                                     ▼
//                                          VkCommandBuffer → vkQueueSubmit → fence
//
// 设计约束:
//   - **只在 backend 内**:本头出现大量 Vk* 类型,G3 门控要求它们只出现在
//     `src/backend/**`。L3(primitives)只通过返回 `IRenderTarget*` 的
//     `create_render_pass()` 与转成 `uintptr_t` 的句柄间接使用,不 include 本头。
//   - **单队列同步(G5)**:一次提交 = 一个 VkCommandBuffer + 一个 fence 等待
//     (与 L1 submit_and_wait 同口径,确保离屏读回前 GPU 已完成)。
//   - **零裸分配(G4)**:本文件不调用任何 vkAllocateMemory / vmaAllocateMemory;
//     framebuffer 视图等临时 Vk 对象是普通 vkCreate* (非内存分配)。
//   - **不复制 L1 的命令解释**:draw 参数、viewport、scissor 全部从
//     `RecordedCommand` 取值,与命令流逐字段同源。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "backend/vulkan/vk_common.h"
#include "backend/vulkan/vulkan_sync_core.h"
#include "backend/vulkan/vulkan_target.h"
#include "magicblocks/device/result.h"
#include "magicblocks/resources/command.h"

namespace magicblocks::backend {

class VulkanCommandList;
class VulkanPipeline;
class VulkanBuffer;
class VulkanTexture;

// 一次设备帧提交的结果统计(L3 的 CLI / 测试用来断言"确实画了")。
struct DeviceFrameStats {
  std::uint32_t draw_calls = 0;          // 实际下发 vkCmdDraw* 的次数
  std::uint32_t instances = 0;           // 实例总数
  std::uint32_t indices = 0;             // 索引总数
  std::uint32_t pass_count = 0;          // begin_pass/end_pass 对
  std::uint32_t pipeline_binds = 0;
  std::uint32_t skipped_commands = 0;    // 认不出内容的命令(理论上恒 0)
  // shader 采样器绑定用的是纹理视图:本后端无描述符集(R4),恒 0,保留以便观测。
  std::uint32_t sampler_binds = 0;
  // 本帧在飞帧防护需要立标记/托管的资源页(page_id,去重)。
  //
  // 【为什么由 submit_device_frame 填、而不是 submit_command_list 自己扫一遍】
  // 因为**翻译成功的命令才是真的提交了**:命令流里记了句柄、但设备帧提交因为
  // 缺句柄/类型不符而跳过的那条(pipeline 缺失等)并没有下发。这里只收
  // "真的进入了 vkCmdBind* / vkCmdDraw*"的 page_id,才不会有"绑了名字却
  // 没被 GPU 读"的假在途页把 L1 的页淘汰永久卡住。
  std::vector<std::uint32_t> pages;
  // 同上,去重后的 VkBuffer 句柄(供退休托管的反查;只含本帧真实绑定的)。
  std::vector<VkBuffer> buffers;
};

// 按 `list.recorded()` 的顺序把全部命令**真正编码**到一张 VkCommandBuffer 并提交。
//
// 前置条件:
//   - list 必须是设备态创建的(`VulkanCommandList(ctx)`),且已经录制完成
//     (`in_pass() == false`)。
//   - 参与绘制的 pipeline / vertex buffer / index buffer 都必须是本设备创建的
//     backend 实现类型(否则返回 Unsupported,并指出是哪条命令)。
//   - core 是设备的 N 槽同步核心(VulkanRenderDevice::sync_core_impl())。
//     调用方必须已经对该设备调用过 begin_frame()(acquire 之前),否则本帧的
//     image_available 语义量不会有对应的 acquire 去信号它。
//
// 失败语义(D8 不静默降级):任何一条命令无法翻译都返回非 Ok,不提交半帧。
//
// 【2026-11-11 重构】本函数**返回时 GPU 可能还在跑**(不再同步 vkWaitForFences)。
// 帧内建的 framebuffer / imageView / renderPass 被压到该槽位的延迟销毁栈,
// 由 VulkanSyncCore::begin_frame 在 N 帧后该槽位复用时销毁。
// 需要"像素已经落地"的调用方(读回 / present)必须自己等:
//   - present:present_current() 等 render_finished 信号量(swapchain 侧负责);
//   - 读回:readback_rgba8() 内部先 wait_latest_frame()。
device::Status submit_device_frame(const VulkanContext& ctx, const VulkanCommandList& list,
                                   DeviceFrameStats* out_stats, VulkanSyncCore* core);

// ---------------------------------------------------------------- 目标缓存
//
// VkPipeline 与 VkRenderPass 必须**兼容**(附件格式/采样数/有无深度一致),
// 而 L1 的 create_pipeline(desc) 用的是固定形状 {RGBA8, 1 采样, 无深度}。
// L3 需要按目标真实形状建管线,因此由 L3 提供一个 registry:
//
//   interface IPipelineRegistry {
//     virtual resources::IPipeline* create(const PipelineDesc&, const TargetShape&) = 0;
//   };
// 但那样 L3 就要 include backend 头(违反分层)。折中:本文件提供一个
// **不透明**的 C 风格入口,句柄用 uintptr_t,由 L3 通过 `primitives/backend_hooks.h`
// 的弱符号接口取用(见该头)。这里只实现,不暴露 Vk 类型。
}  // namespace magicblocks::backend
