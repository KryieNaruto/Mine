// MagicBlocks SDK —— L2 S5 录制器实现(三态 + 双写 W1-W4 + PipelineTable + 回放/复现)
//
// 见 include/magicblocks/scene/recorder.h 的设计说明。要点复述:
//   W1 顺序:先校验 → 调 L1 → **成功才** append 进 CommandStream;L1 是唯一权威。
//   W2 `packed_ok` 预检在调 L1 之前(Q1):否则败在 L1 里就已经写了一半状态。
//   W3 帧内任一失败 → 整帧作废:回 Idle、丢流、禁止再 submit。
//   W4 `commands[]` 含 begin_pass/end_pass → `commands.size() == target.command_count()`。
//
// **本文件不读时钟**(D13)、**不含 Vk\***(G3)、**不做裸分配**(G4)。
#include <cstdio>
#include <bit>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "scene/scene_internal.h"

namespace magicblocks::scene {
namespace {

// 浮点按位相等(spec §6.3 / R7):-0.0f 与 0.0f 视为不同,NaN 视为相同。
bool bits_equal(float a, float b) { return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b); }

using resources::u32;
using resources::u64;

std::string hex16(std::uint64_t v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "0x%016llx", static_cast<unsigned long long>(v));
  return std::string(buf);
}

class RecorderImpl final : public IRecorder {
 public:
  // ------------------------------------------------------------ 生命周期

  Status on_attach(IRenderDevice& device, const RecorderAttachments& attachments) override {
    if (attached_) {
      return Status::Invalid("IRecorder::on_attach: 已经 attach 过(只允许一次)");
    }
    if (attachments.pipelines.empty()) {
      return Status::Invalid("IRecorder::on_attach: attachments.pipelines 为空 —— "
                             "回放必须有至少一条可登记的管线");
    }
    std::vector<std::unique_ptr<IPipeline>> created;
    created.reserve(attachments.pipelines.size());
    for (std::size_t k = 0; k < attachments.pipelines.size(); ++k) {
      auto result = device.create_pipeline(attachments.pipelines[k]);
      if (!result.ok()) {
        return Status(result.status().code,
                      "IRecorder::on_attach: 第 " + std::to_string(k) + " 条管线创建失败: " +
                          result.status().message);
      }
      created.push_back(std::move(result.value()));
    }
    for (auto& pipeline : created) {
      pipelines_.add(pipeline.get());
      owned_pipelines_.push_back(std::move(pipeline));
    }
    attached_ = true;
    return Status::Ok_();
  }

  bool attached() const override { return attached_; }

  const PipelineTable& pipelines() const override { return pipelines_; }

  // ------------------------------------------------------------ 状态

  RecorderState state() const override { return state_; }

  std::uint64_t submit_count() const override { return submit_count_; }

  // ------------------------------------------------------------ Recording

  Status begin_frame(std::uint64_t frame_index, const Extent2D& extent,
                     const IScene& scene) override {
    if (state_ != RecorderState::Idle) {
      return Status::Invalid(std::string("IRecorder::begin_frame: 当前状态 ") +
                             recorder_state_name(state_) + ",只能从 Idle 开新帧");
    }
    if (!attached_) {
      return Status::Invalid("IRecorder::begin_frame: 还没 on_attach —— 没有管线就无法回放本流");
    }
    if (extent.width == 0 || extent.height == 0) {
      return Status::Invalid("IRecorder::begin_frame: extent 宽高必须 > 0");
    }
    stream_ = CommandStream{};
    stream_.backend = "vulkan";
    stream_.engine_major = 1;
    stream_.frame_index = frame_index;
    stream_.extent = extent;
    // B7:整份场景快照内嵌进流 —— 这是"自包含"的核心。
    stream_.scene = scene.snapshot();
    state_ = RecorderState::Recording;
    frame_failed_ = false;
    return Status::Ok_();
  }

  Status record_event(const EventRecord& event) override {
    Status s = require_recording("record_event");
    if (!s.ok()) return s;
    stream_.events.push_back(event);
    return Status::Ok_();
  }

  Status record_pass(const PassRecord& pass) override {
    Status s = require_recording("record_pass");
    if (!s.ok()) return s;
    if (pass.name.empty()) return fail_frame("record_pass: pass 名不能为空");
    // 幂等:同一 pass 的元信息只登记一次。调用方可能先 record_pass 再
    // record_begin_pass(推荐顺序),也可能只调 record_begin_pass;
    // 两种情况流的 passes 都必须只有一条,否则回放时会重复建 pass
    // (plans-l2 §4.6 record_pass 幂等性)。
    if (!has_pass(pass)) stream_.passes.push_back(pass);
    return Status::Ok_();
  }

  // ------------------------------------------------------------ 双写(11 条)

  Status record_bind_pipeline(ICommandList& target, IPipeline* pipeline) override {
    Status guard = require_recording("record_bind_pipeline");
    if (!guard.ok()) return guard;
    if (pipeline == nullptr) return fail_frame("record_bind_pipeline: pipeline 为空");

    const std::uint64_t hash = pipeline->hash();
    // W1:先调 L1(L1 做真正的合法性判定)。
    Status s = target.bind_pipeline(pipeline, hash);
    if (!s.ok()) return fail_frame("bind_pipeline 被 L1 拒绝: " + s.message);

    CommandRecord c;
    c.op = CommandOp::BindPipeline;
    c.pipeline_hash = hash;
    append(c);
    return Status::Ok_();
  }

  Status record_bind_vertex_buffer(ICommandList& target, const resources::ResourceId& buffer,
                                   std::uint32_t binding, std::uint64_t offset) override {
    Status guard = require_recording("record_bind_vertex_buffer");
    if (!guard.ok()) return guard;

    // W2:pack() 是 const 纯函数,**在调 L1 之前**预检(Q1)。
    // 失败就一条都不录 —— 绝不会出现"L1 记了、流里没记"的半态。
    std::uint64_t packed = 0;
    if (!buffer.pack(&packed)) {
      return fail_frame("record_bind_vertex_buffer: ResourceId 无法打包(page_id=" +
                        std::to_string(buffer.page_id) + ", offset=" + std::to_string(buffer.offset) +
                        ", size=" + std::to_string(buffer.size) + ")");
    }
    Status s = target.bind_vertex_buffer(buffer, binding, offset);
    if (!s.ok()) return fail_frame("bind_vertex_buffer 被 L1 拒绝: " + s.message);

    CommandRecord c;
    c.op = CommandOp::BindVertexBuffer;
    c.packed_resource = packed;
    c.a = binding;
    c.b = static_cast<std::uint32_t>(offset & 0xFFFFFFFFull);
    c.c = static_cast<std::uint32_t>((offset >> 32) & 0xFFFFFFFFull);
    append(c);
    return Status::Ok_();
  }

  Status record_bind_index_buffer(ICommandList& target, const resources::ResourceId& buffer,
                                  resources::IndexFormat format, std::uint64_t offset) override {
    Status guard = require_recording("record_bind_index_buffer");
    if (!guard.ok()) return guard;

    std::uint64_t packed = 0;
    if (!buffer.pack(&packed)) {
      return fail_frame("record_bind_index_buffer: ResourceId 无法打包(page_id=" +
                        std::to_string(buffer.page_id) + ", offset=" + std::to_string(buffer.offset) +
                        ", size=" + std::to_string(buffer.size) + ")");
    }
    Status s = target.bind_index_buffer(buffer, format, offset);
    if (!s.ok()) return fail_frame("bind_index_buffer 被 L1 拒绝: " + s.message);

    CommandRecord c;
    c.op = CommandOp::BindIndexBuffer;
    c.packed_resource = packed;
    c.a = static_cast<std::uint32_t>(format);
    c.b = static_cast<std::uint32_t>(offset & 0xFFFFFFFFull);
    c.c = static_cast<std::uint32_t>((offset >> 32) & 0xFFFFFFFFull);
    append(c);
    return Status::Ok_();
  }

  Status record_draw(ICommandList& target, const resources::DrawArgs& args) override {
    Status guard = require_recording("record_draw");
    if (!guard.ok()) return guard;
    Status s = target.draw(args);
    if (!s.ok()) return fail_frame("draw 被 L1 拒绝: " + s.message);
    CommandRecord c;
    c.op = CommandOp::Draw;
    c.a = args.vertex_count;
    c.b = args.instance_count;
    c.c = args.first_vertex;
    c.i = args.vertex_offset;
    c.f[0] = static_cast<float>(args.first_instance);
    append(c);
    return Status::Ok_();
  }

  Status record_draw_indexed(ICommandList& target, const resources::DrawArgs& args) override {
    Status guard = require_recording("record_draw_indexed");
    if (!guard.ok()) return guard;
    Status s = target.draw_indexed(args);
    if (!s.ok()) return fail_frame("draw_indexed 被 L1 拒绝: " + s.message);
    CommandRecord c;
    c.op = CommandOp::DrawIndexed;
    c.a = args.index_count;
    c.b = args.instance_count;
    c.c = args.first_index;
    c.i = args.vertex_offset;
    c.f[0] = static_cast<float>(args.first_instance);
    append(c);
    return Status::Ok_();
  }

  Status record_draw_instanced(ICommandList& target, const resources::DrawArgs& args) override {
    Status guard = require_recording("record_draw_instanced");
    if (!guard.ok()) return guard;
    Status s = target.draw_instanced(args);
    if (!s.ok()) return fail_frame("draw_instanced 被 L1 拒绝: " + s.message);
    CommandRecord c;
    c.op = CommandOp::DrawInstanced;
    c.a = args.vertex_count;
    c.b = args.instance_count;
    c.c = args.first_vertex;
    c.i = args.vertex_offset;
    c.f[0] = static_cast<float>(args.first_instance);
    append(c);
    return Status::Ok_();
  }

  Status record_push_constants(ICommandList& target, std::uint32_t stage_mask,
                               std::uint32_t offset, std::uint32_t size_bytes,
                               const void* data) override {
    Status guard = require_recording("record_push_constants");
    if (!guard.ok()) return guard;
    Status s = target.push_constants(stage_mask, offset, size_bytes, data);
    if (!s.ok()) return fail_frame("push_constants 被 L1 拒绝: " + s.message);

    CommandRecord c;
    c.op = CommandOp::PushConstants;
    c.a = stage_mask;
    c.b = offset;
    c.c = size_bytes;
    if (data != nullptr && size_bytes > 0) {
      const auto* bytes = static_cast<const std::uint8_t*>(data);
      c.payload.assign(bytes, bytes + size_bytes);
    }
    append(c);
    return Status::Ok_();
  }

  Status record_set_viewport(ICommandList& target, const resources::Viewport& viewport) override {
    Status guard = require_recording("record_set_viewport");
    if (!guard.ok()) return guard;
    Status s = target.set_viewport(viewport);
    if (!s.ok()) return fail_frame("set_viewport 被 L1 拒绝: " + s.message);
    CommandRecord c;
    c.op = CommandOp::SetViewport;
    c.f[0] = viewport.x;
    c.f[1] = viewport.y;
    c.f[2] = viewport.width;
    c.f[3] = viewport.height;
    c.f[4] = viewport.min_depth;
    c.f[5] = viewport.max_depth;
    append(c);
    return Status::Ok_();
  }

  Status record_set_scissor(ICommandList& target, const resources::ScissorRect& rect) override {
    Status guard = require_recording("record_set_scissor");
    if (!guard.ok()) return guard;
    Status s = target.set_scissor(rect);
    if (!s.ok()) return fail_frame("set_scissor 被 L1 拒绝: " + s.message);
    CommandRecord c;
    c.op = CommandOp::SetScissor;
    c.a = rect.x;
    c.b = rect.y;
    c.width = rect.width;
    c.height = rect.height;
    append(c);
    return Status::Ok_();
  }

  Status record_begin_pass(ICommandList& target, const PassRecord& pass,
                           IRenderTarget* target_image,
                           const resources::RenderPassDesc& desc) override {
    Status guard = require_recording("record_begin_pass");
    if (!guard.ok()) return guard;
    if (target_image == nullptr) {
      return fail_frame("record_begin_pass: target_image 为空");
    }
    if (pass.name.empty()) return fail_frame("record_begin_pass: pass 名不能为空");

    // pass 元信息先进流:这样即便后续 L1 拒绝,失败帧整体作废,不会留下半条元信息
    // 被读到(整帧丢弃由 fail_frame 保证)。
    // 幂等:若调用方已经用 record_pass 登记过同一条,这里不重复追加。
    if (!has_pass(pass)) stream_.passes.push_back(pass);

    Status s = target.begin_pass(desc);
    if (!s.ok()) return fail_frame("begin_pass 被 L1 拒绝: " + s.message);

    CommandRecord c;
    c.op = CommandOp::BeginPass;
    // U2:**原始** area,不做 0→extent 的解析(L1 会解析,那是它的入流口径)。
    c.pass_area[0] = pass.area[0];
    c.pass_area[1] = pass.area[1];
    c.pass_area[2] = pass.area[2];
    c.pass_area[3] = pass.area[3];
    // 清屏参数也进流 —— 回放时重建 begin_pass 需要它们。
    c.f[0] = pass.clear_color ? 1.0f : 0.0f;
    c.f[1] = pass.clear_color_rgba[0];
    c.f[2] = pass.clear_color_rgba[1];
    c.f[3] = pass.clear_color_rgba[2];
    c.f[4] = pass.clear_color_rgba[3];
    c.f[5] = pass.clear_depth ? 1.0f : 0.0f;
    c.f[6] = pass.clear_depth_value;
    append(c);
    return Status::Ok_();
  }

  Status record_end_pass(ICommandList& target) override {
    Status guard = require_recording("record_end_pass");
    if (!guard.ok()) return guard;
    Status s = target.end_pass();
    if (!s.ok()) return fail_frame("end_pass 被 L1 拒绝: " + s.message);
    CommandRecord c;
    c.op = CommandOp::EndPass;
    append(c);
    return Status::Ok_();
  }

  // ------------------------------------------------------------ capture

  Result<CommandStream> capture() override {
    if (state_ != RecorderState::Recording) {
      return Status::Invalid(std::string("IRecorder::capture: 当前状态 ") +
                             recorder_state_name(state_) + ",只能在 Recording 取流");
    }
    if (frame_failed_) {
      return Status::Invalid("IRecorder::capture: 本帧已因录制失败作废(W3),不能取流");
    }
    // 算好哈希再交出去,调用方拿到就能直接落盘/比对。
    CommandStream out = stream_;
    out.stream_hash = compute_stream_hash(out);

    state_ = RecorderState::Idle;
    stream_ = CommandStream{};
    return out;
  }

  // ------------------------------------------------------------ Replaying

  Status replay(const CommandStream& stream, ICommandList& target,
                IRenderTarget* pass_target) override {
    if (state_ != RecorderState::Idle) {
      return Status::Invalid(std::string("IRecorder::replay: 当前状态 ") +
                             recorder_state_name(state_) + ",只能从 Idle 进入");
    }
    if (!attached_) {
      return Status::Invalid("IRecorder::replay: 还没 on_attach —— PipelineTable 为空");
    }
    state_ = RecorderState::Replaying;
    replay_target_ = pass_target;

    Status s = replay_into(stream, target);
    replay_target_ = nullptr;
    state_ = RecorderState::Idle;
    // 回放结果**留给调用方**:命令列表里必须是重录出来的整帧,调用方据此
    // submit_and_wait 或比对 command_count()。早先这里调了 target.reset(),
    // 会把刚重录好的命令全部丢掉(replay 后 command_count() 恒为 0)。
    return s;
  }

  // ------------------------------------------------------------ Reproduce

  Status reproduce(const CommandStream& stream, const ReproduceFn& fn) override {
    if (state_ != RecorderState::Idle) {
      return Status::Invalid(std::string("IRecorder::reproduce: 当前状态 ") +
                             recorder_state_name(state_) + ",只能从 Idle 进入");
    }
    if (!fn) return Status::Invalid("IRecorder::reproduce: 复现函数为空");
    state_ = RecorderState::Reproduce;

    // B7:场景完全从流里重建 —— 不依赖外部 IScene 实例。
    std::unique_ptr<IScene> rebuilt = create_scene();
    if (!rebuilt->restore(stream.scene)) {
      state_ = RecorderState::Idle;
      return Status::Invalid("IRecorder::reproduce: 流里的 scene_snapshot 无法重建"
                             "(快照非法:id 冲突/parent 悬空/成环)");
    }

    // 复现用的目标列表由调用方提供?不 —— L2 没有 command list 工厂,
    // 因此复现只重录命令流,**不重录进任何 L1 列表**(U1:回放不改 GPU 状态)。
    // 这条 11 方法的驱动由 fn 自己决定;这里只给场景与新流骨架。
    CommandStream fresh = stream;
    fresh.commands.clear();
    fresh.passes.clear();

    struct NullCommandList final : public ICommandList {
      Status bind_pipeline(IPipeline*, resources::u64) override { return Status::Ok_(); }
      Status bind_vertex_buffer(const resources::ResourceId&, resources::u32,
                                resources::u64) override {
        return Status::Ok_();
      }
      Status bind_index_buffer(const resources::ResourceId&, resources::IndexFormat,
                               resources::u64) override {
        return Status::Ok_();
      }
      Status draw(const resources::DrawArgs&) override { return Status::Ok_(); }
      Status draw_indexed(const resources::DrawArgs&) override { return Status::Ok_(); }
      Status draw_instanced(const resources::DrawArgs&) override { return Status::Ok_(); }
      Status push_constants(resources::u32, resources::u32, resources::u32,
                            const void*) override {
        return Status::Ok_();
      }
      Status set_viewport(const resources::Viewport&) override { return Status::Ok_(); }
      Status set_scissor(const resources::ScissorRect&) override { return Status::Ok_(); }
      Status begin_pass(const resources::RenderPassDesc&) override { return Status::Ok_(); }
      Status end_pass() override { return Status::Ok_(); }
      bool recording() const override { return true; }
      bool in_pass() const override { return false; }
      resources::u32 command_count() const override { return 0; }
      void reset() override {}
      Status submit_and_wait() override { return Status::Ok_(); }
    } sink;

    Status s = fn(*rebuilt, sink, fresh);
    if (!s.ok()) {
      state_ = RecorderState::Idle;
      return Status(s.code, "IRecorder::reproduce: 复现函数失败: " + s.message);
    }

    const std::uint64_t fresh_hash = compute_stream_hash(fresh);
    const std::uint64_t want = stream.stream_hash != 0 ? stream.stream_hash
                                                       : compute_stream_hash(stream);
    state_ = RecorderState::Idle;
    if (fresh_hash != want) {
      return Status::Invalid("IRecorder::reproduce: 复现哈希不符(期望 " + hex16(want) + ",得到 " +
                             hex16(fresh_hash) + ")");
    }
    return Status::Ok_();
  }

 private:
  // ------------------------------------------------------------ 内部工具

  Status require_recording(const char* what) const {
    if (state_ != RecorderState::Recording) {
      return Status::Invalid(std::string("IRecorder::") + what + ": 当前状态 " +
                             recorder_state_name(state_) + ",必须先 begin_frame");
    }
    if (frame_failed_) {
      return Status::Invalid(std::string("IRecorder::") + what +
                             ": 本帧已作废(W3),本帧不允许再录任何命令");
    }
    return Status::Ok_();
  }

  // W3:任一步失败 → 整帧作废。返回原始 Status(带上"整帧作废"的说明)。
  Status fail_frame(const std::string& why) {
    frame_failed_ = true;
    state_ = RecorderState::Idle;
    stream_ = CommandStream{};
    return Status::Invalid(why + "(W3:整帧作废,本次录制不得提交)");
  }

  void append(const CommandRecord& c) { stream_.commands.push_back(c); }

  // pass 元信息是否已登记(按全部字段比较;同名的不同 pass 允许并存,
  // 因为多 pass 渲染图里 pass 名可以不唯一,而逐字段相等才代表同一条)。
  bool has_pass(const PassRecord& pass) const {
    for (const PassRecord& p : stream_.passes) {
      if (p.name != pass.name) continue;
      if (p.layer != pass.layer || p.channel != pass.channel) continue;
      if (p.clear_color != pass.clear_color || p.clear_depth != pass.clear_depth) continue;
      if (p.color_format != pass.color_format || p.has_depth != pass.has_depth) continue;
      bool same = true;
      for (int k = 0; k < 4; ++k) {
        if (!bits_equal(p.clear_color_rgba[k], pass.clear_color_rgba[k])) same = false;
        if (p.area[k] != pass.area[k]) same = false;
      }
      if (!same) continue;
      if (!bits_equal(p.clear_depth_value, pass.clear_depth_value)) continue;
      return true;
    }
    return false;
  }

  // 把流里的命令逐条重录进 target,并比对哈希。
  Status replay_into(const CommandStream& stream, ICommandList& target) {
    // pass 元信息按命令流里的名字顺序重建查表(回放只需 RenderPassDesc 的形状)。
    for (std::size_t k = 0; k < stream.commands.size(); ++k) {
      const CommandRecord& c = stream.commands[k];
      const std::string where = "第 " + std::to_string(k) + " 条命令(" + command_op_name(c.op) + "): ";

      switch (c.op) {
        case CommandOp::BindPipeline: {
          IPipeline* pipeline = pipelines_.find(c.pipeline_hash);
          if (pipeline == nullptr) {
            return missing_pipeline(c.pipeline_hash, where);
          }
          Status s = target.bind_pipeline(pipeline, c.pipeline_hash);
          if (!s.ok()) return Status(s.code, "replay " + where + s.message);
          break;
        }
        case CommandOp::BindVertexBuffer: {
          const u64 offset = static_cast<u64>(c.b) | (static_cast<u64>(c.c) << 32);
          Status s = target.bind_vertex_buffer(resources::ResourceId::unpack(c.packed_resource),
                                               c.a, offset);
          if (!s.ok()) return Status(s.code, "replay " + where + s.message);
          break;
        }
        case CommandOp::BindIndexBuffer: {
          const u64 offset = static_cast<u64>(c.b) | (static_cast<u64>(c.c) << 32);
          Status s = target.bind_index_buffer(
              resources::ResourceId::unpack(c.packed_resource),
              static_cast<resources::IndexFormat>(c.a), offset);
          if (!s.ok()) return Status(s.code, "replay " + where + s.message);
          break;
        }
        case CommandOp::Draw: {
          resources::DrawArgs args;
          args.vertex_count = c.a;
          args.instance_count = c.b;
          args.first_vertex = c.c;
          args.vertex_offset = c.i;
          args.first_instance = static_cast<u32>(c.f[0]);
          Status s = target.draw(args);
          if (!s.ok()) return Status(s.code, "replay " + where + s.message);
          break;
        }
        case CommandOp::DrawIndexed: {
          resources::DrawArgs args;
          args.index_count = c.a;
          args.instance_count = c.b;
          args.first_index = c.c;
          args.vertex_offset = c.i;
          args.first_instance = static_cast<u32>(c.f[0]);
          Status s = target.draw_indexed(args);
          if (!s.ok()) return Status(s.code, "replay " + where + s.message);
          break;
        }
        case CommandOp::DrawInstanced: {
          resources::DrawArgs args;
          args.vertex_count = c.a;
          args.instance_count = c.b;
          args.first_vertex = c.c;
          args.vertex_offset = c.i;
          args.first_instance = static_cast<u32>(c.f[0]);
          Status s = target.draw_instanced(args);
          if (!s.ok()) return Status(s.code, "replay " + where + s.message);
          break;
        }
        case CommandOp::PushConstants: {
          Status s = target.push_constants(c.a, c.b, c.c,
                                           c.payload.empty() ? nullptr : c.payload.data());
          if (!s.ok()) return Status(s.code, "replay " + where + s.message);
          break;
        }
        case CommandOp::SetViewport: {
          resources::Viewport vp;
          vp.x = c.f[0];
          vp.y = c.f[1];
          vp.width = c.f[2];
          vp.height = c.f[3];
          vp.min_depth = c.f[4];
          vp.max_depth = c.f[5];
          Status s = target.set_viewport(vp);
          if (!s.ok()) return Status(s.code, "replay " + where + s.message);
          break;
        }
        case CommandOp::SetScissor: {
          resources::ScissorRect r;
          r.x = c.a;
          r.y = c.b;
          r.width = c.width;
          r.height = c.height;
          Status s = target.set_scissor(r);
          if (!s.ok()) return Status(s.code, "replay " + where + s.message);
          break;
        }
        case CommandOp::BeginPass: {
          // 回放时 target 由调用方通过 stream 的 pass 元信息与本帧目标列表对应;
          // 因为命令流**不含 IRenderTarget 指针**(它没有稳定身份,事实 4),
          // 这里用 replay 时登记的目标。
          if (replay_target_ == nullptr) {
            return Status::Invalid(
                "replay " + where +
                "流里有 begin_pass 但未提供回放目标(IRenderTarget 无稳定身份,必须由调用方给)");
          }
          resources::RenderPassDesc desc;
          desc.target = replay_target_;
          desc.clear_color = c.f[0] != 0.0f;
          desc.clear_color_rgba[0] = c.f[1];
          desc.clear_color_rgba[1] = c.f[2];
          desc.clear_color_rgba[2] = c.f[3];
          desc.clear_color_rgba[3] = c.f[4];
          desc.clear_depth = c.f[5] != 0.0f;
          desc.clear_depth_value = c.f[6];
          // U2:原始 area 原样回填(0 仍表示"整图",交给 L1 自己解释)。
          desc.area_x = c.pass_area[0];
          desc.area_y = c.pass_area[1];
          desc.area_width = c.pass_area[2];
          desc.area_height = c.pass_area[3];
          Status s = target.begin_pass(desc);
          if (!s.ok()) return Status(s.code, "replay " + where + s.message);
          break;
        }
        case CommandOp::EndPass: {
          Status s = target.end_pass();
          if (!s.ok()) return Status(s.code, "replay " + where + s.message);
          break;
        }
        default:
          return Status::Invalid("replay " + where + "未知操作码");
      }
    }

    // 强不变量:回放完的 L1 命令计数必须等于流里的命令条数(W4)。
    if (target.command_count() != stream.commands.size()) {
      return Status::Invalid("replay: 命令计数不符 —— L1 记了 " +
                             std::to_string(target.command_count()) + " 条,流里 " +
                             std::to_string(stream.commands.size()) + " 条(W4 不变量被破坏)");
    }
    return Status::Ok_();
  }

  Status missing_pipeline(std::uint64_t hash, const std::string& where) const {
    std::string msg = "replay " + where + "流引用的管线 " + hex16(hash) +
                      " 不在 PipelineTable 里(本进程已登记 " +
                      std::to_string(pipelines_.size()) + " 条";
    if (!pipelines_.hashes().empty()) {
      msg += ": ";
      const std::size_t limit =
          std::min<std::size_t>(PipelineTable::kMaxListedOnMiss, pipelines_.hashes().size());
      for (std::size_t k = 0; k < limit; ++k) {
        if (k != 0) msg += ", ";
        msg += hex16(pipelines_.hashes()[k]);
      }
      if (pipelines_.hashes().size() > limit) msg += ", ...";
    }
    msg += ")";
    return Status::Unsupported(msg);
  }

 public:
  RecorderImpl() = default;

 private:
  RecorderState state_ = RecorderState::Idle;
  std::uint64_t submit_count_ = 0;
  bool attached_ = false;
  bool frame_failed_ = false;

  CommandStream stream_{};
  PipelineTable pipelines_;
  std::vector<std::unique_ptr<IPipeline>> owned_pipelines_;
  IRenderTarget* replay_target_ = nullptr;
};

}  // namespace

const char* recorder_state_name(RecorderState state) {
  switch (state) {
    case RecorderState::Idle: return "Idle";
    case RecorderState::Recording: return "Recording";
    case RecorderState::Replaying: return "Replaying";
    case RecorderState::Reproduce: return "Reproduce";
  }
  return "Unknown";
}

bool PipelineTable::add(IPipeline* pipeline) {
  if (pipeline == nullptr) return false;
  const std::uint64_t hash = pipeline->hash();
  for (std::uint64_t existing : hashes_) {
    if (existing == hash) return false;
  }
  pipelines_.push_back(pipeline);
  hashes_.push_back(hash);
  return true;
}

IPipeline* PipelineTable::find(std::uint64_t hash) const {
  for (std::size_t k = 0; k < hashes_.size(); ++k) {
    if (hashes_[k] == hash) return pipelines_[k];
  }
  return nullptr;
}

std::size_t PipelineTable::size() const { return pipelines_.size(); }

void PipelineTable::clear() {
  pipelines_.clear();
  hashes_.clear();
}

std::unique_ptr<IRecorder> create_recorder() { return std::make_unique<RecorderImpl>(); }

}  // namespace magicblocks::scene
