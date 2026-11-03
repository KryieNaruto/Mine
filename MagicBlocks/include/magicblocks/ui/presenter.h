// MagicBlocks UI 消费层 —— 上屏链路(spec §5,本文档核心)
//
// 主路径(spec §3 方案 C,**方案 A 硬禁止**):
//
//   Frame n:
//     1. Wait Fence(n)               ← 复用两帧前资源,不阻塞当前
//     2. Acquire swapchain image
//     3. Record into cmd(n):
//          [Pass 1 Scene]  渲染各玩法内容(深度测试开,清深度)
//          [Pass 2 UI]     HUD + 覆盖层,深度测试关,LoadOp::Load 叠加
//          (End rendering → image layout PRESENT)
//     4. Submit(cmd) + signal semaphore        ← **每帧恰好 1 次**
//     5. Present(image, semaphore)
//     6. Advance 帧索引(N=2 ring)
//
// 本模块只做三件事,全部是**可单测的纯状态机**:
//   - 帧阶段推进与合法性检查(乱序调用立即报错,不吞);
//   - 每帧恰一次 submit 的计数与断言(spec §10 验收门 2);
//   - 输入→呈现延迟的帧差统计(spec §6 G6,目标 ≤ 2 帧)。
//
// **本模块不做任何 GPU 调用**。真正的录制/提交/present 由 `IFrameRenderer`
// 与 `IPresentSurface` 实现完成(它们由 SDK 落地后替换,见 frame_renderer.h);
// Presenter 只持有它们并调度顺序 —— 这样"禁止事项"(spec §5.4)集中在两个
// 极小接口上,静态门控脚本能一眼看全。
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "magicblocks/device/render_target.h"
#include "magicblocks/device/swapchain.h"
#include "magicblocks/ui/frame_renderer.h"
#include "magicblocks/ui/types.h"

namespace magicblocks::ui {

// 一帧内部阶段(用于驱动与调试;顺序即 spec §5.2 的顺序)。
enum class FramePhase : std::uint8_t {
  Idle = 0,
  WaitFence,     // 1
  Acquire,       // 2
  Record,        // 3
  Submit,        // 4
  Presented,     // 5
  Skipped,       // 后台/无 surface:本帧不渲染不 present(G7)
};

const char* to_string(FramePhase phase);

enum class PresenterStatus : std::uint8_t {
  Ok = 0,
  NoSurface,          // 后台/尚未拿到 surface → 跳过(G7),不是错误
  NeedRebuild,        // OUT_OF_DATE / SUBOPTIMAL → 调 rebuild()
  SurfaceLost,        // VK_ERROR_SURFACE_LOST / DeviceLost
  RecordFailed,
  SubmitFailed,
  PresentFailed,
  InvalidState,       // 阶段乱序(编程错误,立即报)
};

const char* to_string(PresenterStatus status);

// 主路径上 SDK 的"取图 / 提交 / 呈现"三个动作(spec §5.2 的 2/4/5)。
// SDK 目前只有 L0(ISwapchain 连 acquire/present 都没有),所以这里以桩声明,
// **签名按 Vulkan 语义定型**:out_semaphore / in_semaphore 是"present 前等哪个信号量"。
class IPresentSurface {
 public:
  virtual ~IPresentSurface() = default;

  // 2. Acquire:取下一张可用的 swapchain image。false → 见 status 语义(上层转 NeedRebuild)。
  virtual bool acquire(FrameImage* out_image) = 0;

  // 4. Submit:提交 **一整帧** 的命令缓冲,并在完成时 signal `signal_semaphore`。
  // 每帧只允许调用一次(spec §5.4「UI 单独一次 submit」= 禁止)。
  virtual bool submit(std::uint64_t cmd, std::uint64_t signal_semaphore) = 0;

  // 5. Present:呈现,并等待 `wait_semaphore`。
  virtual bool present(const FrameImage& image, std::uint64_t wait_semaphore) = 0;

  // 前台的 acquire 是否可用(surface 存在且未暂停)。
  virtual bool ready() const = 0;

  // 前一次失败的原因(acquire 返回 false 时由实现给出)。Presenter 据此把
  // "OUT_OF_DATE / SUBOPTIMAL" 映射成 NeedRebuild、"SURFACE_LOST" 映射成 SurfaceLost,
  // 而不是把所有 acquire 失败都当成同一种错误。
  virtual PresenterStatus last_failure() const = 0;
};

struct PresenterConfig {
  // spec §5.1 P5:UI 线程不做 GPU 同步等待,帧栅栏最多滞后 N=2 帧。
  u32 in_flight_frames = 2;
  // spec §5.1 P4:默认 FIFO 保稳,可切 MAILBOX(无垂直同步等待)。
  device::PresentMode present_mode = device::PresentMode::Fifo;
  // spec §5.5:离屏路径的最小间隔(0 = 每帧都出图,仅测试用)。
  TargetMode mode = TargetMode::Swapchain;
  // 每帧给 SDK 的命令缓冲句柄(0 = 由 IFrameRenderer 自建)。测试可注入固定值。
  std::uint64_t external_cmd = 0;
};

// 单帧执行结果。
struct FrameResult {
  PresenterStatus status = PresenterStatus::Ok;
  FramePhase phase = FramePhase::Idle;
  u64 frame_index = 0;
  u32 slot = 0;
  u32 submits_this_frame = 0;
  u32 input_latency_frames = 0;
  FrameImage image{};

  bool ok() const {
    return status == PresenterStatus::Ok || status == PresenterStatus::NoSurface;
  }
};

// 上屏链路状态机(spec §5)。**单线程**:UI 线程调用,不做等待、不做重试循环。
class Presenter {
 public:
  Presenter();
  explicit Presenter(const PresenterConfig& cfg);
  ~Presenter();

  Presenter(const Presenter&) = delete;
  Presenter& operator=(const Presenter&) = delete;

  const PresenterConfig& config() const;

  // swapchain 与渲染器都是**外部持有**(Presenter 不拥有、不销毁 —— spec §5.4
  // 「每帧创建/销毁 swapchain 或 render pass」禁止)。两者都可为 null(仅测试状态机)。
  void set_surface(device::ISwapchain* swapchain, IPresentSurface* surface);
  void set_renderer(IFrameRenderer* renderer);
  void set_offscreen_target(device::IRenderTarget* target);  // Offscreen 模式用

  // 前台/后台(spec §6 G7):surface 销毁时暂停 present,保留 CPU 侧状态,
  // 避免唤醒时黑屏。暂停期间 `run_frame()` 返回阶段 Skipped,**不消耗帧号**。
  void set_active(bool active);
  bool active() const;

  // 走到 spec §5.2 的哪一步就停(默认 Present:完整走完 1→5)。
  // 供阶段测试逐步驱动:传入的 phase 表示"执行到并停在哪个阶段之后"。
  void set_stop_after(FramePhase phase);

  // 跑一帧。任何失败都返回明确的 PresenterStatus,不重试、不静默。
  FrameResult run_frame(f32 scene_ms = 0.f, f32 ui_ms = 0.f);

  // 输入 → 帧号登记。用于 G6「input→present 帧数」。
  // 参数是该输入被交到 UI 层时**正在运行的帧号**(UiApp 传当前帧号):
  // 之后每次 run_frame 结束都会算出"距最近一次输入过了一帧"的帧差。
  void mark_input(std::uint64_t frame_index);

  const std::vector<FrameResult>& history() const;
  const FrameResult& last() const;

  // 帧栅栏最多滞后 in_flight_frames(断言用)。
  u32 frames_in_flight() const;

  void reset();

 private:
  FrameResult finish_frame_early(FrameResult r, PresenterStatus status);
  FrameResult finish_failure(FrameResult r, PresenterStatus status);

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace magicblocks::ui
