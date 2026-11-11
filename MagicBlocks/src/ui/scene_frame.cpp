// MagicBlocks UI 消费层 —— 场景帧驱动实现
//
// **本文件不做任何 GPU 调用、不做任何 UI 合成**。它只按固定顺序调度壳层提供的
// `ISceneFrameSink`，并维护帧阶段 / 提交计数 / 输入延迟这几个可单测的状态。
//
// 与旧 Presenter 的关键差别：
//   - 旧版在 Record 阶段把「场景 pass + UI pass」录进同一 command list；
//   - 新版**只提交场景**，UI 由壳层（Compose / CMP）自己渲染与合成。
#include "mb_internal/magicblocks/ui/scene_frame.h"

#include <algorithm>
#include <string>

namespace magicblocks::ui {

const char* to_string(FramePhase phase) {
  switch (phase) {
    case FramePhase::Idle: return "Idle";
    case FramePhase::Acquire: return "Acquire";
    case FramePhase::Record: return "Record";
    case FramePhase::Submit: return "Submit";
    case FramePhase::Presented: return "Presented";
    case FramePhase::Skipped: return "Skipped";
  }
  return "Unknown";
}

const char* to_string(FrameStatus status) {
  switch (status) {
    case FrameStatus::Ok: return "Ok";
    case FrameStatus::NoSurface: return "NoSurface";
    case FrameStatus::NeedRebuild: return "NeedRebuild";
    case FrameStatus::SurfaceLost: return "SurfaceLost";
    case FrameStatus::RecordFailed: return "RecordFailed";
    case FrameStatus::SubmitFailed: return "SubmitFailed";
    case FrameStatus::PresentFailed: return "PresentFailed";
    case FrameStatus::InvalidState: return "InvalidState";
  }
  return "Unknown";
}

struct SceneFrameDriver::Impl {
  SceneFrameConfig cfg{};
  ISceneFrameSink* sink = nullptr;
  resources::ICommandList* command_list = nullptr;

  bool active = true;
  u64 frame_index = 0;
  u32 in_flight = 0;
  // 最近一次输入的帧号 + 是否已登记过。
  u64 last_input_frame = 0;
  bool has_input = false;
  std::vector<SceneFrameResult> history;
};

SceneFrameDriver::SceneFrameDriver() : impl_(std::make_unique<Impl>()) {}
SceneFrameDriver::SceneFrameDriver(const SceneFrameConfig& cfg) : impl_(std::make_unique<Impl>()) {
  impl_->cfg = cfg;
}
SceneFrameDriver::~SceneFrameDriver() = default;

const SceneFrameConfig& SceneFrameDriver::config() const { return impl_->cfg; }

void SceneFrameDriver::set_sink(ISceneFrameSink* sink) { impl_->sink = sink; }

void SceneFrameDriver::set_command_list(resources::ICommandList* list) {
  impl_->command_list = list;
}

void SceneFrameDriver::set_active(bool active) { impl_->active = active; }
bool SceneFrameDriver::active() const { return impl_->active; }

void SceneFrameDriver::on_lifecycle(const device::AppLifecycleEvent& ev) {
  // 壳层事件只影响"是否还在前台"，具体 surface 的创建/重建由壳层自己管
  // （它实现了 ISceneFrameSink）。这里只跟着暂停/恢复，不清任何 GPU 资源。
  switch (ev.kind) {
    case device::LifecycleKind::Paused:
    case device::LifecycleKind::SurfaceDestroyed:
      impl_->active = false;
      break;
    case device::LifecycleKind::Resumed:
    case device::LifecycleKind::SurfaceCreated:
    case device::LifecycleKind::SurfaceChanged:
      impl_->active = true;
      break;
  }
}

SceneFrameResult SceneFrameDriver::finish_early(SceneFrameResult r, FrameStatus status) {
  r.status = status;
  r.phase = FramePhase::Skipped;
  // 跳过的帧不消耗帧号（G7：唤醒后不应出现"跳了一堆帧"的观感）。
  impl_->history.push_back(r);
  return r;
}

SceneFrameResult SceneFrameDriver::run_frame() {
  Impl& d = *impl_;
  SceneFrameResult r;
  r.frame_index = d.frame_index;

  // ---- 阶段 0：后台 / 无 sink / 无 surface → 跳过 ----
  if (!d.active) return finish_early(r, FrameStatus::NoSurface);
  if (d.sink == nullptr) {
    // 没有壳层 surface:离屏模式(CLI/CI/golden)就是这种情形 —— 场景帧跳过是
    // **正常**的,不是编程错误。UI 侧出图由离屏通道单独完成。
    return finish_early(r, FrameStatus::NoSurface);
  }
  if (!d.sink->ready()) {
    const SinkStatus ss = d.sink->last_status();
    // 壳层明确说"没有 surface"才跳过；其他失败原因原样上报，不吞。
    if (ss == SinkStatus::NoSurface || ss == SinkStatus::Ok) {
      return finish_early(r, FrameStatus::NoSurface);
    }
    if (ss == SinkStatus::SurfaceLost) return finish_early(r, FrameStatus::SurfaceLost);
    if (ss == SinkStatus::NeedRebuild) return finish_early(r, FrameStatus::NeedRebuild);
    return finish_early(r, FrameStatus::NoSurface);
  }

  // ---- 阶段 1：Acquire ----
  r.phase = FramePhase::Acquire;
  SinkImage image{};
  if (!d.sink->acquire_image(&image)) {
    const SinkStatus ss = d.sink->last_status();
    FrameStatus st = FrameStatus::SubmitFailed;
    switch (ss) {
      case SinkStatus::NoSurface: st = FrameStatus::NoSurface; break;
      case SinkStatus::NeedRebuild: st = FrameStatus::NeedRebuild; break;
      case SinkStatus::SurfaceLost: st = FrameStatus::SurfaceLost; break;
      default: st = FrameStatus::SubmitFailed; break;
    }
    return finish_early(r, st);
  }
  r.image = image;

  // ---- 阶段 2：Record（只录场景；UI 由壳层自己画）----
  r.phase = FramePhase::Record;
  // 命令列表由 SDK 侧录制完成，这里不改动它。
  // 空帧（无命令列表 / 列表为空）是合法的 —— 壳层照样 acquire / present，
  // 只是这一帧没有几何。这与旧 Presenter "空命令列表不报错" 同口径。

  // ---- 阶段 3：Submit（每帧**恰好一次**，验收口径 UI-P8）----
  r.phase = FramePhase::Submit;
  r.submits_this_frame = 0;
  if (d.command_list != nullptr) {
    // 有命令列表就提交一次 —— 哪怕它是空的（空帧也走一次 submit）。
    if (!d.sink->submit_frame(*d.command_list)) {
      const SinkStatus ss = d.sink->last_status();
      FrameStatus st = ss == SinkStatus::NeedRebuild ? FrameStatus::NeedRebuild
                                                      : FrameStatus::SubmitFailed;
      return finish_early(r, st);
    }
    r.submits_this_frame = 1;
  }
  // 没有命令列表 = 纯空帧：acquire + present，但不属于"一次渲染提交"。

  // ---- 阶段 4：Present ----
  r.phase = FramePhase::Presented;
  if (!d.sink->present_image(image)) {
    const SinkStatus ss = d.sink->last_status();
    FrameStatus st = ss == SinkStatus::SurfaceLost ? FrameStatus::SurfaceLost
                                                    : FrameStatus::PresentFailed;
    return finish_early(r, st);
  }

  // ---- 收尾：帧号推进、在途计数、输入延迟 ----
  r.status = FrameStatus::Ok;
  const u32 in_flight_max = d.cfg.in_flight_frames ? d.cfg.in_flight_frames : 2;
  d.in_flight = std::min(d.in_flight + 1, in_flight_max);
  if (d.in_flight > 0) --d.in_flight;  // 本帧已 present，栅栏释放

  if (d.has_input && d.frame_index >= d.last_input_frame) {
    r.input_latency_frames = static_cast<u32>(d.frame_index - d.last_input_frame);
  }

  ++d.frame_index;
  d.history.push_back(r);
  return r;
}

void SceneFrameDriver::mark_input(std::uint64_t frame_index) {
  impl_->last_input_frame = frame_index;
  impl_->has_input = true;
}

const std::vector<SceneFrameResult>& SceneFrameDriver::history() const { return impl_->history; }

const SceneFrameResult& SceneFrameDriver::last() const {
  static const SceneFrameResult kEmpty{};
  return impl_->history.empty() ? kEmpty : impl_->history.back();
}

u32 SceneFrameDriver::frames_in_flight() const { return impl_->in_flight; }

void SceneFrameDriver::reset() {
  Impl& d = *impl_;
  d.frame_index = 0;
  d.in_flight = 0;
  d.has_input = false;
  d.last_input_frame = 0;
  d.history.clear();
  d.active = true;
}

}  // namespace magicblocks::ui
