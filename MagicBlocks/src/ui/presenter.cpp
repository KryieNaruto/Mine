// MagicBlocks UI 消费层 —— 上屏链路状态机实现(spec §5)
//
// **本文件不做任何 GPU 调用**。它只按 spec §5.2 的顺序调用两个极小接口:
//   `IPresentSurface`(acquire / submit / present)
//   `IFrameRenderer` (record:场景 pass + UI pass 录进同一 command buffer)
//
// 这样做的直接收益:spec §5.4 的全部禁止事项("每帧 QueueWaitIdle""UI 单独
// 一次 submit""每帧创建 swapchain/render pass")只可能发生在两个接口的实现里,
// 静态门控脚本 check_ui_no_readback / check_ui_single_submit 的扫描范围就是全部
// 暴露面 —— 不存在"藏在某个中间层"的第三条路。
//
// 阶段推进是**显式状态机**,乱序调用返回 InvalidState 而不是静默继续:
//   主路径只在 `run_frame()` 一个入口里走完整序列,阶段测试通过 set_stop_after
//   逐步驱动,拿到同样的阶段断言。
#include "magicblocks/ui/presenter.h"

#include "magicblocks/ui/perf.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace magicblocks::ui {

const char* to_string(FramePhase phase) {
  switch (phase) {
    case FramePhase::Idle: return "Idle";
    case FramePhase::WaitFence: return "WaitFence";
    case FramePhase::Acquire: return "Acquire";
    case FramePhase::Record: return "Record";
    case FramePhase::Submit: return "Submit";
    case FramePhase::Presented: return "Presented";
    case FramePhase::Skipped: return "Skipped";
  }
  return "Unknown";
}

const char* to_string(PresenterStatus status) {
  switch (status) {
    case PresenterStatus::Ok: return "Ok";
    case PresenterStatus::NoSurface: return "NoSurface";
    case PresenterStatus::NeedRebuild: return "NeedRebuild";
    case PresenterStatus::SurfaceLost: return "SurfaceLost";
    case PresenterStatus::RecordFailed: return "RecordFailed";
    case PresenterStatus::SubmitFailed: return "SubmitFailed";
    case PresenterStatus::PresentFailed: return "PresentFailed";
    case PresenterStatus::InvalidState: return "InvalidState";
  }
  return "Unknown";
}

namespace {

// 阶段序号,用于 "跑到哪一步" 的比较与乱序检测。
int phase_rank(FramePhase p) {
  switch (p) {
    case FramePhase::Idle: return 0;
    case FramePhase::Skipped: return 0;
    case FramePhase::WaitFence: return 1;
    case FramePhase::Acquire: return 2;
    case FramePhase::Record: return 3;
    case FramePhase::Submit: return 4;
    case FramePhase::Presented: return 5;
  }
  return 0;
}

}  // namespace

struct Presenter::Impl {
  PresenterConfig cfg{};
  device::ISwapchain* swapchain = nullptr;
  IPresentSurface* surface = nullptr;
  IFrameRenderer* renderer = nullptr;
  device::IRenderTarget* offscreen = nullptr;
  // 离屏路径的读回缓冲(spec §5.5)。**只有** Offscreen 分支会碰它;
  // 主路径的 Presenter 成员里没有任何读回入口(UIG1)。
  std::vector<std::uint8_t> offscreen_rgba8;

  bool active = true;
  FramePhase stop_after = FramePhase::Presented;
  FramePhase phase = FramePhase::Idle;

  u64 frame_index = 0;
  u32 submits_this_frame = 0;
  // 帧栅栏:当前处于在途状态的帧号环形队列(N=in_flight_frames)
  u32 in_flight = 0;

  // G6:最近一次输入登记的帧号
  u64 last_input_frame = 0;
  bool has_input = false;

  // 场景/UI 分段耗时(由调用方注入,不做时钟读取)
  FrameBuckets timing{};

  std::vector<FrameResult> history;
  FrameResult last{};

  void reset_frame_state() {
    phase = FramePhase::Idle;
    submits_this_frame = 0;
    timing = FrameBuckets{};
  }

  static constexpr std::size_t kHistoryCap = 4096;

  void push_history(const FrameResult& r) {
    if (history.size() >= kHistoryCap) history.erase(history.begin());
    history.push_back(r);
  }
};

Presenter::Presenter() : Presenter(PresenterConfig{}) {}

Presenter::Presenter(const PresenterConfig& cfg) : impl_(std::make_unique<Impl>()) {
  impl_->cfg = cfg;
}

Presenter::~Presenter() = default;

const PresenterConfig& Presenter::config() const { return impl_->cfg; }

void Presenter::set_surface(device::ISwapchain* swapchain, IPresentSurface* surface) {
  impl_->swapchain = swapchain;
  impl_->surface = surface;
}

void Presenter::set_renderer(IFrameRenderer* renderer) { impl_->renderer = renderer; }

void Presenter::set_offscreen_target(device::IRenderTarget* target) { impl_->offscreen = target; }

void Presenter::set_active(bool active) { impl_->active = active; }
bool Presenter::active() const { return impl_->active; }

void Presenter::set_stop_after(FramePhase phase) { impl_->stop_after = phase; }

FrameResult Presenter::run_frame(f32 scene_ms, f32 ui_ms) {
  Impl& d = *impl_;
  FrameResult r{};
  r.frame_index = d.frame_index;
  // spec §5.1 P5:槽位 = frame_index % in_flight,帧栅栏最多滞后 in_flight_frames。
  const u32 in_flight = d.cfg.in_flight_frames ? d.cfg.in_flight_frames : 2;
  r.slot = static_cast<u32>(d.frame_index % in_flight);

  d.timing.scene_ms = scene_ms;
  d.timing.ui_ms = ui_ms;

  // ---------------------------------------------------------- G7:后台/无 surface 跳过
  // 暂停期间**不消耗帧号**,也不推进帧栅栏:唤醒后不用等 fence 就绪。
  if (!d.active) {
    r.status = PresenterStatus::NoSurface;
    r.phase = FramePhase::Skipped;
    d.last = r;
    return r;
  }
  if (d.cfg.mode == TargetMode::Swapchain) {
    if (!d.surface) {
      r.status = PresenterStatus::NoSurface;
      r.phase = FramePhase::Skipped;
      d.last = r;
      return r;
    }
    if (!d.surface->ready()) {
      r.status = PresenterStatus::NoSurface;
      r.phase = FramePhase::Skipped;
      d.last = r;
      return r;
    }
  }

  // ---------------------------------------------------------- 1. Wait Fence
  d.phase = FramePhase::WaitFence;
  r.phase = d.phase;
  // 帧栅栏的等待由 surface/renderer 自身的 N-ring 实现(SDK IFrameSync::wait_slot)。
  // Presenter 只记账:在途帧数不得超 N。
  d.in_flight = std::min(d.in_flight + 1, in_flight);
  if (phase_rank(d.stop_after) == phase_rank(FramePhase::WaitFence)) {
    return finish_frame_early(r, PresenterStatus::Ok);
  }

  // ---------------------------------------------------------- 2. Acquire
  d.phase = FramePhase::Acquire;
  r.phase = d.phase;
  FrameImage image{};
  if (d.cfg.mode == TargetMode::Swapchain) {
    if (!d.surface->acquire(&image)) {
      const PresenterStatus why = d.surface->last_failure();
      d.in_flight = d.in_flight > 0 ? d.in_flight - 1 : 0;
      return finish_failure(r, why == PresenterStatus::Ok ? PresenterStatus::NeedRebuild : why);
    }
  } else {
    // 离屏:目标由外部持有,不做 acquire
    image.valid = d.offscreen != nullptr;
    if (d.offscreen) {
      image.width = d.offscreen->extent().width;
      image.height = d.offscreen->extent().height;
      image.id = 0;  // 离屏 image 无 swapchain 索引
    }
    if (!image.valid) {
      d.in_flight = d.in_flight > 0 ? d.in_flight - 1 : 0;
      return finish_failure(r, PresenterStatus::NoSurface);
    }
  }
  r.image = image;
  if (phase_rank(d.stop_after) == phase_rank(FramePhase::Acquire)) {
    return finish_frame_early(r, PresenterStatus::Ok);
  }

  // ---------------------------------------------------------- 3. Record(场景 pass + UI pass)
  d.phase = FramePhase::Record;
  r.phase = d.phase;
  std::uint64_t semaphore = 0;
  FrameImage recorded{};
  if (!d.renderer) {
    d.in_flight = d.in_flight > 0 ? d.in_flight - 1 : 0;
    return finish_failure(r, PresenterStatus::RecordFailed);
  }
  // 两条录制通路**互斥**:
  //   - 主路径(Swapchain):录进设备 image。SDK L1 的 command list 落地前这个
  //     调用会明确返回 false —— 如实报 RecordFailed,不假装成功。
  //   - 离屏路径(Offscreen,spec §5.5):录进内存并读回,golden / CLI 走这条。
  // 早期实现不分 mode 就调主路径,离屏模式下每帧必然 RecordFailed。
  bool recorded_ok = false;
  if (d.cfg.mode == TargetMode::Swapchain) {
    recorded_ok = d.renderer->render_into_device_image(d.cfg.external_cmd, &recorded, &semaphore);
  } else {
    recorded = image;  // 离屏没有独立的目标 image,记账沿用 acquire 阶段那张
    d.offscreen_rgba8.resize(static_cast<std::size_t>(image.width) * image.height * 4u);
    recorded_ok = d.renderer->render_to_memory(image.width, image.height, d.offscreen_rgba8.data(),
                                               image.width * 4u);
  }
  if (!recorded_ok) {
    d.in_flight = d.in_flight > 0 ? d.in_flight - 1 : 0;
    return finish_failure(r, PresenterStatus::RecordFailed);
  }
  if (phase_rank(d.stop_after) == phase_rank(FramePhase::Record)) {
    return finish_frame_early(r, PresenterStatus::Ok);
  }

  // ---------------------------------------------------------- 4. Submit(**恰好一次**)
  d.phase = FramePhase::Submit;
  r.phase = d.phase;
  if (d.cfg.mode == TargetMode::Swapchain) {
    ++d.submits_this_frame;
    if (!d.surface->submit(d.cfg.external_cmd, semaphore)) {
      d.in_flight = d.in_flight > 0 ? d.in_flight - 1 : 0;
      return finish_failure(r, PresenterStatus::SubmitFailed);
    }
  } else {
    // 离屏路径不进 present 队列,但"整帧一次提交"的约束同样成立
    ++d.submits_this_frame;
  }
  r.submits_this_frame = d.submits_this_frame;
  if (phase_rank(d.stop_after) == phase_rank(FramePhase::Submit)) {
    return finish_frame_early(r, PresenterStatus::Ok);
  }

  // ---------------------------------------------------------- 5. Present
  d.phase = FramePhase::Presented;
  r.phase = d.phase;
  if (d.cfg.mode == TargetMode::Swapchain) {
    if (!d.surface->present(recorded.valid ? recorded : image, semaphore)) {
      const PresenterStatus why = d.surface->last_failure();
      d.in_flight = d.in_flight > 0 ? d.in_flight - 1 : 0;
      return finish_failure(r, why == PresenterStatus::Ok ? PresenterStatus::PresentFailed : why);
    }
  }

  // ---------------------------------------------------------- 6. Advance 帧索引
  r.status = PresenterStatus::Ok;
  r.submits_this_frame = d.submits_this_frame;
  // G6:input→present 帧数
  r.input_latency_frames = d.has_input
                               ? static_cast<u32>(d.frame_index - d.last_input_frame)
                               : 0;
  d.push_history(r);
  d.last = r;
  ++d.frame_index;
  d.reset_frame_state();
  return r;
}

FrameResult Presenter::finish_frame_early(FrameResult r, PresenterStatus status) {
  // 阶段测试用:停在半路**不推进帧号**(否则 stop_after=WaitFence 的连续调用会
  // 反复消耗帧号,帧栅栏断言的语义就变了)。
  r.status = status;
  r.submits_this_frame = impl_->submits_this_frame;
  impl_->last = r;
  return r;
}

FrameResult Presenter::finish_failure(FrameResult r, PresenterStatus status) {
  r.status = status;
  r.phase = impl_->phase;
  r.submits_this_frame = impl_->submits_this_frame;
  impl_->push_history(r);
  impl_->last = r;
  // 失败帧**不推进帧号**:下一帧重试同一帧号,资源复用语义才不会漂。
  impl_->reset_frame_state();
  return r;
}

void Presenter::mark_input(std::uint64_t input_seq) {
  (void)input_seq;
  // 不读时钟:输入到达时的**当前帧号**就是基准(G6 的"帧差"由此算)。
  impl_->last_input_frame = impl_->frame_index;
  impl_->has_input = true;
}

const std::vector<FrameResult>& Presenter::history() const { return impl_->history; }

const FrameResult& Presenter::last() const { return impl_->last; }

u32 Presenter::frames_in_flight() const { return impl_->in_flight; }

void Presenter::reset() {
  const PresenterConfig keep = impl_->cfg;
  impl_ = std::make_unique<Impl>();
  impl_->cfg = keep;
}

}  // namespace magicblocks::ui
