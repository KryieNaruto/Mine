// MagicBlocks UI 消费层 —— 场景帧出口（壳层持有 surface）
//
// ============================================================================
// 路线变更说明（必读）
// ============================================================================
//
// 旧设计（docs/superpowers/specs/2026-11-03-magicblocks-ui-consumer-design.md §5）
// 要求 SDK 把「场景 pass + UI pass」录进**同一个 command list**，每帧**恰好一次**
// submit 后 present。调研 v2（2026-11-05-magicblocks-ui-rendering-route-research.md）
// §7 已明确判定这条路线**作废**，原因是物理层面的：
//
//   - Android 上 UI 由 Compose 画在**自己的 RenderNode / Surface** 上，3D 场景是
//     独立的 SurfaceView。两者由 **SurfaceFlinger 合成**，不需要、也不允许游戏
//     引擎替系统做合成。
//   - 若 SDK 硬要在同一张 swapchain image 上做「场景 pass + UI pass 叠加」，
//     就等于自研了一套合成器 —— 既拿不到 Compose 的控件，也和壳层的 Z 序 / 裁剪
//     规则直接冲突。
//
// 所以新边界是：**SDK 只负责「把一帧 3D 场景渲染到壳层给的 surface」，UI 合成
// 完全交给壳层。**
//
// ============================================================================
// 本接口的职责
// ============================================================================
//
// `ISceneFrameSink` 由**壳层实现**（Android NativeActivity / iOS / 桌面壳），
// 用来把「一次 submit + present」的能力交给 SDK：
//
//   壳层                                          SDK
//   ─────────────────────────────────────────────────────────────
//   创建 ANativeWindow / CAMetalLayer
//   实现 ISceneFrameSink ───────────────────────► set_scene_sink()
//                                                 渲染一帧场景
//   ◄───────────────────────────── submit_frame(list)
//   present_image()                    （壳层负责真正上屏）
//
// **两条硬约束**（与旧路线相同，仍然有效）：
//   1. 主路径上 SDK 侧 **零 CPU 回读**（门控 check_ui_no_readback.py）；
//   2. 每帧场景 **恰好一次 submit_frame**（门控 check_ui_single_submit.py）。
//
// 本接口**没有** readback / map 成员：主路径在类型层面就够不着 CPU 内存。
#pragma once

#include <cstdint>

#include "magicblocks/device/types.h"
#include "magicblocks/resources/command.h"
#include "magicblocks/ui/types.h"

namespace magicblocks::ui {

// 壳层 surface 的状态。SDK 据此决定「继续渲染 / 重建目标 / 放弃本帧」，
// 而不是把所有失败都当成同一种错误（fail-loud）。
enum class SinkStatus : std::uint8_t {
  Ok = 0,
  NoSurface,    // 壳层还没有可用的 surface（后台 / 尚未创建）→ 跳过本帧，不是错误
  NeedRebuild,  // 尺寸变化 / OUT_OF_DATE / SUBOPTIMAL → 调 rebuild()
  SurfaceLost,  // VK_ERROR_SURFACE_LOST 之类的不可恢复错误
  SubmitFailed,
  PresentFailed,
  InvalidState,
};

const char* to_string(SinkStatus status);

// 一份已 acquire 的可写图像（由壳层定义内部含义，SDK **不得解释**其 id）。
struct SinkImage {
  std::uint64_t id = 0;
  device::Extent2D extent{};
  device::Format format = device::Format::RGBA8_UNORM;
  bool valid = false;
};

// 壳层实现：把「场景渲染进某个外部 surface」的能力交给 SDK。
//
// 实现方注意：
//   - `submit_frame()` 必须是**每帧场景恰好一次**；壳层内部负责 fence / 信号量；
//   - 不得在 `submit_frame()` 里做 CPU 回读；
//   - `acquire_image()` 失败时要通过 `last_status()` 给出**明确原因**，
//     让 SDK 能区分「没 surface」与「surface 丢了」。
class ISceneFrameSink {
 public:
  virtual ~ISceneFrameSink() = default;

  // 当前 surface 的尺寸（0×0 = 尚无 surface）。
  virtual device::Extent2D extent() const = 0;

  // 当前 surface 的颜色格式。
  virtual device::Format format() const = 0;

  // 是否具备渲染条件（surface 存在且未暂停）。false 时 run_frame 会跳过。
  virtual bool ready() const = 0;

  // 取下一张可写图像。失败时**不修改** out_image，并把原因写进 last_status()。
  virtual bool acquire_image(SinkImage* out_image) = 0;

  // 提交**这一帧场景**的命令流（SDK 已录制完成，壳层负责真正执行）。
  // 每帧只允许调用一次 —— 这是验收口径 UI-P8。
  virtual bool submit_frame(resources::ICommandList& list) = 0;

  // 呈现刚提交的那张图像。返回 false 时原因见 last_status()。
  virtual bool present_image(const SinkImage& image) = 0;

  // 上一次失败的明确原因（acquire / submit / present 失败时由实现给出）。
  virtual SinkStatus last_status() const = 0;
};

}  // namespace magicblocks::ui
