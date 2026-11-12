// MagicBlocks —— Vulkan 后端:L1 命令列表(R3 / M3 / G5)
//
// spec R3 / §5.2 / §6.3 / G5 的落地(plans-l1 §3,rulings 裁定 1:命令条数 = 11):
//   - **恰好 11 类命令**:bind_pipeline / bind_vertex_buffer / bind_index_buffer /
//     draw / draw_indexed / draw_instanced / push_constants / set_viewport /
//     set_scissor / begin_pass / end_pass。每成功录制一次 command_count() +1,
//     失败不计数 → Command.RecordsAllOps 断言 == 11。
//     2026-11-11 勘误:P0(X1)又追加了第 12 类 **bind_sampled_texture**(取值 11,
//     追加在末尾、不改既有取值)。上面「11 类」是裁定 1 的**原始**口径,仍由
//     Command.RecordsAllOps 守着;新 op 走独立用例。枚举现共 12 个值。
//     历史上这句注释停留在 11、而枚举已是 12 —— 正是回放侧漏实现
//     BindSampledTexture 的诱因(见 vulkan_device_frame.cpp 的 default 分支)。
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
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "backend/vulkan/vk_common.h"
#include "magicblocks/device/result.h"
#include "magicblocks/resources/command.h"
#include "magicblocks/resources/handles.h"
// VulkanContext / backend::Status 别名(见 vulkan_buffer.h 的同一条注释)。
#include "backend/vulkan/vulkan_target.h"
#include "magicblocks/resources/buffer.h"
#include "magicblocks/resources/texture.h"

namespace magicblocks::backend {

using resources::DrawArgs;
using resources::IBuffer;
using resources::ICommandList;
using resources::IndexFormat;
using resources::IPipeline;
using resources::ISampler;
using resources::ICubeMap;
using resources::ITexture;
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
  // P0(X1)新增第 12 类。**追加在末尾、不改既有取值** ——
  // L2 已按 0..10 落盘的命令流保持兼容,新 op 只是多一个未知编号。
  BindSampledTexture = 11,
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
  // **64 位绑定偏移**(L3 需要):bind_vertex/index_buffer 的 offset 可以是 64 位
  // (M3「句柄 = 偏移」的 page 内偏移可能超过 4 GiB)。原先 b 用 u32 截断,
  // 大缓冲上的子区间绑定会让设备态 vkCmdBind*Buffers 拿到错误的偏移。
  // 规则:b 在 offset <= 0xFFFFFFFF 时仍写同一数值(命令流口径不变),
  //       超出的部分写进 off64;设备态读取一律优先用 off64(见 effective_bind_offset)。
  u64 off64 = 0;
  int32_t i = 0;               // 有符号参数(vertex_offset)
  float f[7] = {};             // 浮点参数(viewport 六元组 / clear_color_rgba 之类)
  u32 pass_area[4] = {};       // begin_pass 的 area(x,y,w,h)
  u32 width = 0;               // set_scissor 的宽
  u32 height = 0;              // set_scissor 的高
  // P0(X1):BindSampledTexture 用。resource = 纹理句柄;slot 写在 a;
  // 采样器句柄放这里(resource 槽已被纹理占用),以及它的打包值。
  // 打包失败(packed_ok=false)不阻塞录制,但设备态下发会明确报错,不静默跳过。
  ResourceId sampler{};
  u64 packed_sampler = 0;
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
  device::Status bind_sampled_texture(const ResourceId& texture, const ResourceId& sampler,
                                      u32 slot) override;
  // R1 扩展:从设备刷新"句柄 → 对象"反查表(设备态自动在 begin_pass 调用)。
  device::Status sync_resources() override;
  // 设备在创建命令列表时挂上刷新回调(资源可能晚于命令列表创建)。
  void set_sync_resources_callback(std::function<void(VulkanCommandList&)> cb) {
    on_sync_resources_ = std::move(cb);
  }

  bool recording() const override { return recording_; }
  bool in_pass() const override { return in_pass_; }
  u32 command_count() const override { return command_count_; }

  // 【设备帧提交路径专用】丢弃本列表在 begin_pass/end_pass 期间"顺带"建出来的
  // VkCommandBuffer / VkFramebuffer / VkRenderPass / VkImageView。
  //
  // 背景:begin_pass() 为了让 L1 的录制在设备态也能自洽,会无条件
  // vkBeginCommandBuffer 并建一套 pass 对象;但 submit_command_list() 走的是
  // submit_device_frame(),**另建**命令缓冲重放 recorded_,压根不用这一套。
  // 于是这一套变成"僵尸":命令缓冲一直在录制态、却引用着调用方 target 的 VkImage。
  // 调用方(如 Sky 测试)的 target 先于命令列表析构 → 析构时 vkEndCommandBuffer
  // 发现引用的 image 已销毁,校验层报
  //   VUID-vkEndCommandBuffer-commandBuffer-00059 (invalid state)
  // 这就是全仓 00059 剩余的来源。形态:提交完立刻丢弃,趁 image 还活着。
  void discard_vk_side_channel() { destroy_vk_command_buffer(); }

  void reset() override;

  device::Status submit_and_wait() override;

  // ------------------------------------------------------------ 后端查询(gtest 用)
  bool has_vk_command_buffer() const { return vk_cmd_ != VK_NULL_HANDLE; }
  VkCommandBuffer vk_command_buffer() const { return vk_cmd_; }
  // 命令流快照(按录制顺序;值语义,不含指针)。
  const std::vector<RecordedCommand>& recorded() const { return recorded_; }

  // 设备态旁路 render pass 是否已录进 vk_cmd_(见 begin_pass 的 simple_shape 分支)。
  //
  // 存在的理由:submit_command_list() 走的是 submit_device_frame(),它**另建**
  // 命令缓冲重放 recorded_,压根不执行 vk_cmd_。于是 vk_cmd_ 里那条真实
  // vkCmdBeginRenderPass 永远不会被执行,却把调用方 target 的 VkImage 绑进了
  // framebuffer。该 render pass 的 attachment 是 initialLayout=UNDEFINED /
  // finalLayout=GENERAL,与实际执行的那条 pass 的布局链互不相干 —— 两条链
  // 对同一张图给出矛盾的布局声明,属于未定义行为。
  //
  // 该 gotcha 由本任务(t23)的 SyncModel.UiAndSdkChannelsStayIndependent 暴露。
  bool has_vk_pass_target() const { return has_vk_pass_target_; }
  // 丢弃"只录不执行"的旁路。submit_device_frame() 用自己的命令缓冲重放
  // recorded_,submit_and_wait() 才需要 vk_cmd_ —— 走设备帧提交这条路上,
  // vk_cmd_ 里的一切都是死代码。销毁 target 的 pass 对象、结束并复位命令
  // 缓冲,避免两条矛盾的布局链同时存在。
  void abandon_vk_side_channel();

  // 【帧环不变量探针,只读】本列表自本帧 begin_frame 起 `abandon_vk_side_channel()`
  // 被调用了几次。
  //
  // 为什么需要一个探针,而不是"看代码就知道":这个丢弃动作必须发生在
  // submit_device_frame() **之前**(见 vulkan_render_device.cpp 的注释),而
  // `SyncCore.FrameRingAdvancesPerSubmit` 要在**外部调用方**跑完之后复核这条
  // 不变量 —— 那条用例只能拿到 ICommandList&,拿不到后端内部状态。
  //
  // 计数**不**在 abandon 里清零:清零发生在 `reset()`(帧环每次换槽都会调它,
  // 见 vulkan_render_device.cpp 提交前的那次 reset),所以"本帧提交前丢弃过 1 次"
  // 与"提交后又丢弃了 1 次"(那会把这一次绘制吃掉)在计数上可区分。
  //
  // 它不是性能计数器,也不进命令流、不参与哈希。
  u32 side_channel_abandon_count() const { return side_channel_abandon_count_; }

  // ------------------------------------------------------------ 设备帧提交用的指针视图
  //
  // RecordedCommand 刻意只存值(可序列化),但设备帧提交
  // (vulkan_device_frame.cpp)要把命令**真正编码**成 vkCmd*,必须拿到
  // IPipeline* / IBuffer* 才能取到 Vk 句柄。这两张旁路表与 recorded_ 等长、
  // 同下标对齐,只在本 TU 与后端内部使用 —— 不进入命令流、不参与哈希。
  //
  // 指针在 submit_and_wait()/reset() 之后失效(随 recorded_ 一起清空)。
  IPipeline* pipeline_at(std::size_t index) const {
    return index < command_pipeline_.size() ? command_pipeline_[index] : nullptr;
  }
  // 按 ResourceId 反查录制时用的缓冲指针(没绑过 → nullptr)。
  IBuffer* buffer_at(const ResourceId& id) const;
  // P0(X1):按 ResourceId 反查纹理/采样器指针(设备帧提交要取 VkImageView/VkSampler)。
  // 没绑过 → nullptr,设备态下发时明确报错(bind_sampled_texture 的 Unsupported 分支)。
  ITexture* texture_at(const ResourceId& id) const;
  ISampler* sampler_at(const ResourceId& id) const;
  // R1 扩展:立方体贴图**(不是 ITexture 的子类)**的同款反查表。
  ICubeMap* cube_map_at(const ResourceId& id) const;
  // 当前绑定的管线(供 push_constants 取 pipeline layout)。
  IPipeline* bound_pipeline() const { return bound_.pipeline; }

  // L1 的公共 bind_vertex_buffer / bind_index_buffer 只收 ResourceId(值语义,M3),
  // 不含指针 —— 这是正确的公共面,但设备帧提交必须拿到 IBuffer* 才能取 VkBuffer。
  // 调用方(backend 内的录制辅助 / L3 的 backend hook)在 bind 之后调这个把
  // 句柄与指针挂上;未挂的句柄在设备帧提交时按 Unsupported 报错(不静默跳过)。
  Status remember_buffer_binding(const ResourceId& id, IBuffer* buffer);
  // P0(X1):同款手法 —— 把纹理/采样器句柄挂到真实对象上,供设备态下发取
  // VkImageView/VkSampler。未挂的句柄在设备态下发时按 Unsupported 报错。
  Status remember_texture_sampler(const ResourceId& texture, ITexture* tex,
                                  const ResourceId& sampler, ISampler* smp);
  // R1 扩展:登记立方体贴图句柄 → 对象(采样绑定与设备帧提交都要取它的 cube 视图)。
  Status remember_cube_map(const ResourceId& cube, ICubeMap* cube_obj);
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
  // 旁路表:登记"这个句柄对应哪个 IBuffer*"(设备帧提交要取 VkBuffer)。
  void remember_buffer(const ResourceId& id, IBuffer* buffer);
  // 设备在创建命令列表时挂上"刷新反查表"的回调:资源可以晚于命令列表创建,
  // 所以 begin_pass 里再拉一次,保证"同一设备上任何命令列表都能用任何资源"。
  std::function<void(VulkanCommandList&)> on_sync_resources_;
  device::Status check_draw_state(const char* what) const;
  device::Status ensure_vk_command_buffer();
  // 结束并销毁旁路命令缓冲。**不**销毁 pass 对象(vk_render_pass_ / vk_framebuffer_ /
  // 视图):submit_and_wait() 在 fence 等到之后才需要它们,见
  // pass_targets_pending_cleanup_ 的注释。
  void destroy_vk_command_buffer();
  // 真正销毁 pass / framebuffer / 视图并把 pass 相关标志复位。
  void release_pass_targets();

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
  // 本帧内 abandon_vk_side_channel() 的调用次数(只读探针,见该访问器的注释)。
  // **只在 reset() 里清零** —— 帧环每次换槽前都会 reset 一次,所以它天然是
  // "本帧"的计数;提交后再清零就等于把"提交之后又丢弃一次"抹掉,那恰恰是要抓的。
  u32 side_channel_abandon_count_ = 0;

  std::vector<RecordedCommand> recorded_;
  // 与 recorded_ 等长的旁路表:每条命令对应的 IPipeline*(仅 bind_pipeline 非空)。
  // 见 pipeline_at() 的说明;不参与命令流,不参与哈希。
  std::vector<IPipeline*> command_pipeline_;
  // 录制期见过的缓冲区句柄 → IBuffer*(M3 的句柄是值,这里是反查表)。
  std::vector<std::pair<ResourceId, IBuffer*>> buffers_;
  // P0(X1):纹理/采样器同款反查表(只在设备帧提交取 Vk 句柄时用)。
  std::vector<std::pair<ResourceId, ITexture*>> textures_;
  std::vector<std::pair<ResourceId, ISampler*>> samplers_;
  // R1 扩展:立方体贴图走独立反查表(ICubeMap 不是 ITexture 的子类)。
  std::vector<std::pair<ResourceId, ICubeMap*>> cube_maps_;

  VkCommandBuffer vk_cmd_ = VK_NULL_HANDLE;
  bool vk_recording_ = false;  // 真实 VkCommandBuffer 已 vkBeginCommandBuffer
  // pass 里的 framebuffer 视图(每次 begin_pass 建、end_pass 销毁)。
  VkRenderPass vk_render_pass_ = VK_NULL_HANDLE;
  VkFramebuffer vk_framebuffer_ = VK_NULL_HANDLE;
  VkImageView vk_color_view_ = VK_NULL_HANDLE;
  VkImageView vk_depth_view_ = VK_NULL_HANDLE;
};

}  // namespace magicblocks::backend
