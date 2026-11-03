// MagicBlocks SDK —— L2 S5 `IRecorder`:三态状态机 + 双写 + 回放/复现
//
// 设计约束(spec §4 L2 S5 / plans-l2 §4.6 / §4.6.1 / §4.6.2):
//
//   **三态**(非法转移一律 Invalid,IllegalTransitionIsRejected):
//     Idle ──begin_frame()──► Recording ──capture()──► Idle
//     Idle ──replay()──────► Replaying ──────────────► Idle
//     Idle ──reproduce()───► Reproduce ──────────────► Idle
//
//   **双写(W1-W4)**:Recording 态下**每一条命令都由 L2 发起** —— L2 调
//   `ICommandList` 的 11 个方法之一,并在 L1 返回 Ok 之后**才**把同一条命令追加进
//   `CommandStream`。这样 L1 的公共接口一行都不用改(A1 裁决)。
//     W1 顺序固定:先校验 → 调 L1 → 成功才 append。**L1 是唯一权威**。
//     W2 `packed_ok` 预检在调 L1 **之前**(Q1):否则会留下"L1 记了、流里没记"的半态。
//     W3 帧内任一失败 → 整帧作废:状态回 Idle、流丢弃、**禁止**再 submit。
//     W4 `commands[]` 含 begin_pass/end_pass → `commands.size()` 必须等于
//        `target->command_count()`(Recorder.StreamMatchesCommandListCount)。
//
//   **注意 U1**:L1 的 `draw*` 在设备态**不发 `vkCmdDraw`**(只记录)。因此本层的
//   "回放"含义是**命令流还原 + 哈希比对**,不是"重放出图"。golden 的像素来自
//   **录制那一帧的真实提交**,不是回放帧(plans-l2 §4.6.1.1)。
//
//   **不读时钟**(D13):时间戳只从流里取,由 `check_scene_no_clock.py` 机械保证。
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "magicblocks/device/render_device.h"
#include "magicblocks/device/result.h"
#include "magicblocks/resources/command.h"
#include "magicblocks/resources/pipeline.h"
#include "magicblocks/scene/command_stream.h"
#include "magicblocks/scene/feature.h"
#include "magicblocks/scene/render_graph.h"
#include "magicblocks/scene/scene.h"
#include "magicblocks/scene/submit_queue.h"
#include "magicblocks/scene/types.h"

namespace magicblocks::scene {

using device::IRenderDevice;
using device::Status;
using resources::ICommandList;
using resources::IPipeline;

// 录制器的三种状态。Idle 之外的两态都**不允许**再开新帧。
enum class RecorderState : std::uint8_t {
  Idle = 0,
  Recording = 1,
  Replaying = 2,
  Reproduce = 3,
};

const char* recorder_state_name(RecorderState state);

// ---------------------------------------------------------------- PipelineTable(§5.2)

// 跨进程回放的前提:流里只带 `pipeline_hash`,要还原成 `IPipeline*` 就得有一张表。
// 键是 `IPipeline::hash()` —— 那是**纯 desc 派生**的哈希,不含任何 Vk 句柄,
// 因此跨进程稳定。
class PipelineTable {
 public:
  PipelineTable() = default;

  // 登记一条管线。重复 hash → 保留先登记的(返回 false)。
  bool add(IPipeline* pipeline);
  IPipeline* find(std::uint64_t hash) const;
  std::size_t size() const;
  void clear();
  // 已登记的全部 hash(调试/报错用)。
  const std::vector<std::uint64_t>& hashes() const { return hashes_; }

  static constexpr std::uint32_t kMaxListedOnMiss = 8;

 private:
  std::vector<IPipeline*> pipelines_;
  std::vector<std::uint64_t> hashes_;
};

// ---------------------------------------------------------------- 附加资源(§4.6.2)

// `on_attach()` 要建的东西。管线与顶点/索引缓冲由调用方给描述,
// 录制器负责经 `IRenderDevice` 真正创建并登记进 `PipelineTable`。
struct RecorderAttachments {
  // 至少一条图形管线(场景 pass 用)。**必须非空**,否则 on_attach 失败。
  std::vector<resources::PipelineDesc> pipelines;
};

// 复现函数:给定重建出来的场景,再录一遍命令流。
// 返回 Ok 表示"录完了",失败即复现失败。
using ReproduceFn = std::function<Status(IScene& scene, ICommandList& target,
                                         CommandStream& out)>;

class IRecorder {
 public:
  IRecorder(const IRecorder&) = delete;
  IRecorder& operator=(const IRecorder&) = delete;
  virtual ~IRecorder() = default;

  // ------------------------------------------------------------ 生命周期(N5)

  // 建管线并登记进 `PipelineTable`。**只调一次**;再次调用 → Invalid。
  // 这是 `PipelineTable` 唯一的填充者 —— 没有它,回放必然缺管线。
  virtual Status on_attach(IRenderDevice& device, const RecorderAttachments& attachments) = 0;

  virtual bool attached() const = 0;
  virtual const PipelineTable& pipelines() const = 0;

  // ------------------------------------------------------------ 状态

  virtual RecorderState state() const = 0;

  // 本帧已提交次数(每帧增量必须恰好 1,G5 / Recorder.OneSubmitPerFrame)。
  virtual std::uint64_t submit_count() const = 0;

  // ------------------------------------------------------------ Recording

  // 开始录制一帧。非 Idle → Invalid。
  // `extent` 必须与 target 一致;`scene` 会被**完整快照**进流(B7)。
  virtual Status begin_frame(std::uint64_t frame_index, const Extent2D& extent,
                             const IScene& scene) = 0;

  // 往当前帧里录一条输入事件(时间戳由调用方给)。
  virtual Status record_event(const EventRecord& event) = 0;

  // 往当前帧里录一个 pass 的元信息(供 dump/JSON;命令本体仍走 begin_pass)。
  virtual Status record_pass(const PassRecord& pass) = 0;

  // 录制入口。以下 11 个方法**一一对应** `ICommandList` 的 11 个命令,
  // 全部遵守 W1/W2/W3/W4。
  virtual Status record_bind_pipeline(ICommandList& target, IPipeline* pipeline) = 0;
  virtual Status record_bind_vertex_buffer(ICommandList& target, const resources::ResourceId& buffer,
                                           std::uint32_t binding, std::uint64_t offset) = 0;
  virtual Status record_bind_index_buffer(ICommandList& target, const resources::ResourceId& buffer,
                                          resources::IndexFormat format, std::uint64_t offset) = 0;
  virtual Status record_draw(ICommandList& target, const resources::DrawArgs& args) = 0;
  virtual Status record_draw_indexed(ICommandList& target, const resources::DrawArgs& args) = 0;
  virtual Status record_draw_instanced(ICommandList& target, const resources::DrawArgs& args) = 0;
  virtual Status record_push_constants(ICommandList& target, std::uint32_t stage_mask,
                                       std::uint32_t offset, std::uint32_t size_bytes,
                                       const void* data) = 0;
  virtual Status record_set_viewport(ICommandList& target, const resources::Viewport& viewport) = 0;
  virtual Status record_set_scissor(ICommandList& target, const resources::ScissorRect& rect) = 0;
  virtual Status record_begin_pass(ICommandList& target, const PassRecord& pass,
                                   IRenderTarget* target_image,
                                   const resources::RenderPassDesc& desc) = 0;
  virtual Status record_end_pass(ICommandList& target) = 0;

  // 结束录制并取出命令流。非 Recording → Invalid。
  // 本方法**不提交**;提交由调用方在流拿到后自己做(§4.6 提交时机)。
  virtual Result<CommandStream> capture() = 0;

  // ------------------------------------------------------------ Replaying

  // 把流里的命令**逐条重录**进一张新列表,并与流哈希比对。
  // 非 Idle → Invalid。缺管线(流里的 hash 不在 PipelineTable)→ Unsupported,
  // message 带缺失 hash 的十六进制与最多 8 个已登记 hash。
  //
  // `pass_target`:**必填**。命令流不携带 `IRenderTarget*`(它没有稳定身份,
  // 见侦察事实 4),所以流里每条 `begin_pass` 都要靠调用方给的目标来重录。
  // 为空且流里有 begin_pass → Invalid(不静默跳过,那不叫回放)。
  virtual Status replay(const CommandStream& stream, ICommandList& target,
                        IRenderTarget* pass_target) = 0;

  // ------------------------------------------------------------ Reproduce

  // 从流里读出场景快照重建,再跑 `fn` 重录一遍,与流哈希比对。
  // 非 Idle → Invalid。
  virtual Status reproduce(const CommandStream& stream, const ReproduceFn& fn) = 0;

 protected:
  IRecorder() = default;
};

// 工厂。返回的对象由调用方持有;`on_attach()` 之前不可用于录制。
std::unique_ptr<IRecorder> create_recorder();

}  // namespace magicblocks::scene
