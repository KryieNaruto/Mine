// MagicBlocks —— Vulkan 后端:L1 命令列表(R3 / M3 / G5)
//
// spec R3 / §5.2 / §6.3 / G5 的落地(plans-l1 §3,rulings 裁定 1:命令条数 = 11):
//   - **恰好 11 类命令**:bind_pipeline / bind_vertex_buffer / bind_index_buffer /
//     draw / draw_indexed / draw_instanced / push_constants / set_viewport /
//     set_scissor / begin_pass / end_pass。每成功录制一次 command_count() +1,
//     失败不计数 → Command.RecordsAllOps 断言 == 11。
//   - **命令流记录偏移而不是指针(M3)**:内部每条命令存 ResourceId(值语义),
//     并把可打包的句柄 pack 成 u64(packed_resource)—— 回放时同一 page_id/offset
//     指向同一片内存,且命令流可以整块序列化(L2)。
//   - **单队列单线程(G5)**:录制走 ctx.cmd_pool 下的一张真实 VkCommandBuffer;
//     submit_and_wait() 提交一次并等 fence,不碰别的队列。
//   - **录制期校验**(command.h 明文):未绑定管线就 draw → InvalidArgument;
//     未开 pass 就 draw → InvalidArgument;未 end_pass 就 submit → InvalidArgument;
//     render area 越界 → InvalidArgument。
//
// 纯软件态(不给 ctx):全部校验与命令流记录照常工作,只是不建 VkCommandBuffer ——
// 命令计数与句柄记录因此能在无 Vulkan 设备的 ctest 里被断言。
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "backend/vulkan/vk_common.h"
#include "magicblocks/device/result.h"
#include "magicblocks/resources/command.h"
#include "magicblocks/resources/handles.h"
// VulkanContext / backend::Status 别名(见 vulkan_buffer.h 的同一条注释)。
#include "backend/vulkan/vulkan_target.h"

namespace magicblocks::backend {

using resources::DrawArgs;
using resources::ICommandList;
using resources::IndexFormat;
using resources::IPipeline;
using resources::RenderPassDesc;
using resources::ResourceId;
using resources::ScissorRect;
using resources::Viewport;
using resources::u32;
using resources::u64;

// 命令流里的一个操作码(L2 的命令流格式就按这张表发号;本轮只用于内部记录)。
enum class CommandOp : std::uint8_t {
  BindPipeline = 0,
  BindVertexBuffer = 1,
  BindIndexBuffer = 2,
  Draw = 3,
  DrawIndexed = 4,
  DrawInstanced = 5,
  PushConstants = 6,
  SetViewport = 7,
  SetScissor = 8,
  BeginPass = 9,
  EndPass = 10,
};

const char* command_op_name(CommandOp op);

// 一条已录制的命令(值语义、可序列化:不含任何指针)。
struct RecordedCommand {
  CommandOp op = CommandOp::BindPipeline;
  ResourceId resource{};       // 涉及的资源句柄(M3:偏移而非指针)
  u64 packed_resource = 0;     // pack(resource) 的结果;0 表示不适用/不可打包
  bool packed_ok = false;      // resource.pack() 是否成功
  u64 pipeline_hash = 0;       // BindPipeline 用
  u32 a = 0;                   // 通用整数参数(绑定槽位 / stage_mask / offset / vertex_count...)
  u32 b = 0;                   // 通用整数参数(index_count / instance_count / size_bytes...)
  u32 c = 0;                   // 通用整数参数(first_vertex / first_instance / format...)
  int32_t i = 0;               // 有符号参数(vertex_offset)
  float f[7] = {};             // 浮点参数(viewport 六元组 / clear_color_rgba 之类)
  u32 pass_area[4] = {};       // begin_pass 的 area(x,y,w,h)
  u32 width = 0;               // set_scissor 的宽
  u32 height = 0;              // set_scissor 的高
  std::vector<std::uint8_t> payload;  // push_constants 的字节
};

class VulkanCommandList final : public ICommandList {
 public:
  // 纯软件态:不建 VkCommandBuffer,只做录制与校验。
  VulkanCommandList();
  // 设备态:录制进 ctx.cmd_pool 下的一张真实 VkCommandBuffer。
  explicit VulkanCommandList(const VulkanContext& ctx);

  ~VulkanCommandList() override;

  VulkanCommandList(const VulkanCommandList&) = delete;
  VulkanCommandList& operator=(const VulkanCommandList&) = delete;

  device::Status init();

  // ------------------------------------------------------------ ICommandList(R3)
  device::Status bind_pipeline(IPipeline* pipeline, u64 pipeline_hash) override;
  device::Status bind_vertex_buffer(const ResourceId& buffer, u32 binding = 0,
                                    u64 offset = 0) override;
  device::Status bind_index_buffer(const ResourceId& buffer, IndexFormat format,
                                   u64 offset = 0) override;
  device::Status draw(const DrawArgs& args) override;
  device::Status draw_indexed(const DrawArgs& args) override;
  device::Status draw_instanced(const DrawArgs& args) override;
  device::Status push_constants(u32 stage_mask, u32 offset, u32 size_bytes,
                                const void* data) override;
  device::Status set_viewport(const Viewport& viewport) override;
  device::Status set_scissor(const ScissorRect& rect) override;
  device::Status begin_pass(const RenderPassDesc& pass) override;
  device::Status end_pass() override;

  bool recording() const override { return recording_; }
  bool in_pass() const override { return in_pass_; }
  u32 command_count() const override { return command_count_; }

  void reset() override;

  device::Status submit_and_wait() override;

  // ------------------------------------------------------------ 后端查询(gtest 用)
  bool has_vk_command_buffer() const { return vk_cmd_ != VK_NULL_HANDLE; }
  VkCommandBuffer vk_command_buffer() const { return vk_cmd_; }
  // 命令流快照(按录制顺序;值语义,不含指针)。
  const std::vector<RecordedCommand>& recorded() const { return recorded_; }
  // 已录制命令里 op 出现的次数。
  u32 op_count(CommandOp op) const;
  // 已录制命令流是否用了"记录偏移而不是指针"的手法(bind_* 的句柄都能 pack 成功)。
  bool all_resource_handles_packed() const;
  // 各 op 的句柄打包结果(单测直接查时用)。
  bool resource_packed(CommandOp op) const;
  // 已成功提交的次数与最近一次提交失败原因。
  u32 submit_count() const { return submit_count_; }
  const std::string& last_submit_error() const { return last_submit_error_; }
  // 当前绑定的管线哈希(0 = 未绑定)。
  u64 bound_pipeline_hash() const { return bound_.hash; }
  // 最近一次 begin_pass 的 pass 描述(录制期快照)。
  const RenderPassDesc& last_pass_desc() const { return last_pass_; }

 private:
  struct Bound {
    IPipeline* pipeline = nullptr;
    u64 hash = 0;
    u32 push_constant_bytes = 0;
  };

  device::Status begin_recording();
  // 追加一条命令:成功计数 +1(唯一的计数入口,防止漏计/重复计)。
  void append(const RecordedCommand& cmd);
  // 尝试把句柄打包进命令流(M3);失败不阻塞录制,但记录 packed_ok=false。
  void set_resource(RecordedCommand* cmd, const ResourceId& id);
  device::Status check_draw_state(const char* what) const;
  device::Status ensure_vk_command_buffer();
  void destroy_vk_command_buffer();

  VulkanContext ctx_{};
  bool has_ctx_ = false;

  bool recording_ = false;
  bool in_pass_ = false;
  u32 command_count_ = 0;
  u32 submit_count_ = 0;
  std::string last_submit_error_;

  Bound bound_{};
  RenderPassDesc last_pass_{};
  bool has_vk_pass_target_ = false;  // 设备态 begin_pass 成功开了真实 render pass
  // end_pass 之后,render pass / framebuffer / image view 仍被命令缓冲引用;
  // 必须等 submit_and_wait 的 fence 等到后再销毁,否则 vkQueueSubmit 会读到
  // 已释放的句柄(随机访问冲突)。
  bool pass_targets_pending_cleanup_ = false;

  std::vector<RecordedCommand> recorded_;

  VkCommandBuffer vk_cmd_ = VK_NULL_HANDLE;
  bool vk_recording_ = false;  // 真实 VkCommandBuffer 已 vkBeginCommandBuffer
  // pass 里的 framebuffer 视图(每次 begin_pass 建、end_pass 销毁)。
  VkRenderPass vk_render_pass_ = VK_NULL_HANDLE;
  VkFramebuffer vk_framebuffer_ = VK_NULL_HANDLE;
  VkImageView vk_color_view_ = VK_NULL_HANDLE;
  VkImageView vk_depth_view_ = VK_NULL_HANDLE;
};

}  // namespace magicblocks::backend
