// MagicBlocks UI 消费层 —— 性能治理(spec §6 PerfGovernor)
//
// 目标(spec §1.1 U2/U3):**demo 但性能优先**,SDK 不是瓶颈,瓶颈在"上屏"。
// 因此本模块只做三件"不花钱"的事:计时、判预算、按**明确档位**降级 ——
// 绝不静默降级(reference 设计 D8 / 不回退原则)。
//
// 档位(spec §6 G2,必须明确可见):
//   L0 Full       全质量
//   L1 NoMsaa     关 MSAA
//   L2 SimpleUi   去阴影/去动画(UI 简化)
//   L3 LowRes     降低场景分辨率 —— **仅离屏路径可做**(spec §6 G2 原文)
//
// 降级是**单向的滞后确认**:连续 N 帧超预算才降一档(避免抖动),恢复需要连续 M 帧
// 远低于预算(带迟滞)。所有档位切换都记事件,供统计面板与测试断言。
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "mb_internal/magicblocks/ui/types.h"

namespace magicblocks::ui {

enum class QualityTier : std::uint8_t {
  Full = 0,
  NoMsaa = 1,
  SimpleUi = 2,
  LowRes = 3,
};

const char* to_string(QualityTier tier);

// 帧内分段(对应 spec §6 G1「分桶」)。
struct FrameBuckets {
  f32 scene_ms = 0.f;    // 场景渲染
  f32 ui_ms = 0.f;       // UI 录制 + 合批
  f32 present_ms = 0.f;  // present 调用
  f32 cpu_ms = 0.f;      // 其它 CPU(路由/更新)

  f32 total_ms() const { return scene_ms + ui_ms + present_ms + cpu_ms; }
};

// 每帧的原始观测。**计时由外部注入**(UI 层不内建时钟:CLI/golden/命令复现
// 需要可注入的确定性时间,同 device::EventStream 的纪律)。
struct FrameSample {
  u64 frame_index = 0;
  FrameBuckets buckets{};
  u32 draw_calls = 0;
  u32 vertices = 0;
  u32 batches = 0;
  u32 submit_count = 0;       // 主路径必须 == 1(spec §10 验收门 2)
  u64 vma_alloc_count = 0;    // 稳态帧必须 == 0(spec §6 G5 / SDK M5)
  u32 present_queue_depth = 0;
  u32 input_latency_frames = 0;  // input→present 帧数(spec §6 G6,目标 ≤ 2)
  u32 ui_bytes_touched = 0;   // G3:UI 每帧顶点/索引写入字节数
};

struct GovConfig {
  f32 frame_budget_ms = 16.6f;      // G1:60fps
  f32 scene_budget_ratio = 0.60f;   // 场景占比
  f32 ui_budget_ratio = 0.15f;      // UI 占比
  f32 present_budget_ratio = 0.20f; // present 占比
  u32 ui_max_draw_calls = 50;       // G3:UI drawcall 上限
  u32 ui_max_vertices = 8000;       // G3:UI 顶点上限
  u32 max_input_latency_frames = 2; // G6 目标
  u32 degrade_after_frames = 30;    // 连续超预算帧数 → 降一档
  u32 recover_after_frames = 120;   // 连续低负载帧数 → 升一档
  f32 recover_margin_ratio = 0.5f;  // "低负载"= total < budget * 该值
};

// 档位变化事件(可观察、可断言 —— 不静默)。
struct TierChange {
  u64 frame_index = 0;
  QualityTier from = QualityTier::Full;
  QualityTier to = QualityTier::Full;
  std::string reason;
};

// 统计汇总(面板与测试都读这个)。
struct GovStats {
  u64 frames = 0;
  f32 avg_total_ms = 0.f;
  f32 max_total_ms = 0.f;
  u32 over_budget_frames = 0;
  u32 submit_violations = 0;    // submit_count != 1 的帧数
  u32 vma_alloc_violations = 0; // 稳态帧 VMA != 0 的次数
  u32 ui_drawcall_violations = 0;
  u32 ui_vertex_violations = 0;
  u32 latency_violations = 0;
  QualityTier tier = QualityTier::Full;
};

class PerfGovernor {
 public:
  PerfGovernor();
  explicit PerfGovernor(const GovConfig& cfg);
  ~PerfGovernor();

  PerfGovernor(const PerfGovernor&) = delete;
  PerfGovernor& operator=(const PerfGovernor&) = delete;

  const GovConfig& config() const;
  void set_config(const GovConfig& cfg);

  // 单帧上报。返回该帧是否被判定为"健康"(所有门槛都过)。
  bool report_frame(const FrameSample& sample);

  // 当前档位与调整后的有效参数(渲染/UI 侧据此改行为)。
  QualityTier tier() const;
  bool ui_msaa_enabled() const;      // Full 才开
  bool ui_effects_enabled() const;   // Full/NoMsaa 才有阴影与动画
  bool ui_simplified() const;        // SimpleUi/LowRes
  f32 render_scale() const;          // LowRes 才 < 1(且只对离屏生效)

  const GovStats& stats() const;
  const std::vector<TierChange>& tier_changes() const;

  // 统计面板的文本(ASCII,一行一条;CLI 与 HUD 都可用)。
  std::string format_panel() const;

  void reset();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace magicblocks::ui
