// MagicBlocks UI 消费层 —— 场景帧驱动（取代旧 Presenter）
//
// ============================================================================
// 与旧设计的关系（必读）
// ============================================================================
//
// 旧 `Presenter`（include/magicblocks/ui/presenter.h）实现的是
// 「场景 pass + UI pass 录进同一 command list → 每帧一次 submit → present」。
// 调研 v2 §7 已判定该路线**作废**，故整体移除：
//
//   - 删除 `IPresentSurface`（它要求 UI 与场景共用同一次 submit）；
//   - 删除「Record 阶段同时录 UI pass」的概念；
//   - 新增 `ISceneFrameSink`（见 frame_renderer.h），由**壳层**实现。
//
// 本模块保留旧 Presenter 里**与合成无关**的那部分价值：
//   - 帧阶段推进与合法性检查（乱序调用立即报错，不吞）；
//   - 每帧**恰好一次** `submit_frame` 的计数与断言（验收口径 UI-P8）；
//   - 输入 → 呈现延迟的帧差统计（G6，目标 ≤ 2 帧）；
//   - 后台 / surface 丢失 / 需要重建 的分支处理。
//
// **本模块不做任何 GPU 调用**，也不做 UI 合成 —— 它只调度壳层提供的 sink。
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "mb_internal/magicblocks/device/lifecycle.h"
#include "mb_internal/magicblocks/resources/command.h"
#include "mb_internal/magicblocks/ui/frame_renderer.h"
#include "mb_internal/magicblocks/ui/types.h"

namespace magicblocks::ui {

// 一帧内部的阶段（顺序即执行顺序）。
enum class FramePhase : std::uint8_t {
  Idle = 0,
  Acquire,    // 1. 从壳层取一张可写图像
  Record,     // 2. 把**场景**录进命令列表（不含 UI 合成）
  Submit,     // 3. 交给壳层提交（每帧恰好一次）
  Presented,  // 4. 壳层呈现
  Skipped,    // 后台 / 无 surface：本帧不渲染不提交（G7）
};

const char* to_string(FramePhase phase);

// 一帧的最终状态。
enum class FrameStatus : std::uint8_t {
  Ok = 0,
  NoSurface,     // 没有可用 surface → 跳过（G7），不是错误
  NeedRebuild,   // 壳层要求重建目标
  SurfaceLost,   // surface 不可恢复地丢失
  RecordFailed,  // SDK 侧录制失败
  SubmitFailed,  // 壳层提交失败
  PresentFailed, // 壳层呈现失败
  InvalidState,  // 阶段乱序（编程错误，立即报）
};

const char* to_string(FrameStatus status);

struct SceneFrameConfig {
  // 帧栅栏最多滞后 N 帧（UI 线程不做 GPU 同步等待）。
  u32 in_flight_frames = 2;
  // 离屏模式的目标（CLI / CI / golden 用）。nullptr = 走壳层主路径。
  TargetMode mode = TargetMode::Swapchain;
};

// 单帧执行结果。
struct SceneFrameResult {
  FrameStatus status = FrameStatus::Ok;
  FramePhase phase = FramePhase::Idle;
  u64 frame_index = 0;
  u32 submits_this_frame = 0;
  u32 input_latency_frames = 0;
  SinkImage image{};

  bool ok() const { return status == FrameStatus::Ok || status == FrameStatus::NoSurface; }
};

// 场景帧驱动。**单线程**：UI 线程调用，不做等待、不做重试循环。
//
// 用法：
//   SceneFrameDriver driver(cfg);
//   driver.set_sink(&my_shell_sink);        // 壳层实现
//   driver.set_command_list(&cmd);          // SDK 侧录好的一帧场景命令
//   auto r = driver.run_frame();
class SceneFrameDriver {
 public:
  SceneFrameDriver();
  explicit SceneFrameDriver(const SceneFrameConfig& cfg);
  ~SceneFrameDriver();

  SceneFrameDriver(const SceneFrameDriver&) = delete;
  SceneFrameDriver& operator=(const SceneFrameDriver&) = delete;

  const SceneFrameConfig& config() const;

  // sink 由壳层持有，Driver 不拥有、不销毁。
  void set_sink(ISceneFrameSink* sink);
  // 每帧要提交的命令列表（SDK 侧已录制完成）。nullptr = 空帧。
  void set_command_list(resources::ICommandList* list);

  // 前台 / 后台（G7）：后台时 run_frame 返回 Skipped，**不消耗帧号**。
  void set_active(bool active);
  bool active() const;

  // 壳层生命周期事件（SurfaceCreated / SurfaceChanged / SurfaceDestroyed / Paused）。
  void on_lifecycle(const device::AppLifecycleEvent& ev);

  // 跑一帧。
  SceneFrameResult run_frame();

  // 输入 → 帧号登记，用于 G6「input→present 帧数」。
  void mark_input(std::uint64_t frame_index);

  const std::vector<SceneFrameResult>& history() const;
  const SceneFrameResult& last() const;

  // 帧栅栏最多滞后 in_flight_frames（断言用）。
  u32 frames_in_flight() const;

  void reset();

 private:
  SceneFrameResult finish_early(SceneFrameResult r, FrameStatus status);

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace magicblocks::ui
