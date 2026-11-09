// MagicBlocks UI 消费层 —— 性能治理实现(spec §6)
//
// 一条原则贯穿全文件:**降级是显式的、带原因、带迟滞的**。任何档位切换都进
// `tier_changes`,统计面板与测试都能看到"为什么降了档"。绝不静默降级
// (reference 设计 D8 / 不回退原则)。
//
// 降级与恢复的方向不对称:
//   - 降级快(degrade_after_frames 连续超预算帧);
//   - 恢复慢(recover_after_frames 连续低负载帧,且低负载阈值只有预算的一半)。
// 这是为了不让 UI 在临界线附近来回抖 —— 抖动的帧率比稳定低一档更难受。
#include "magicblocks/ui/perf.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace magicblocks::ui {

const char* to_string(QualityTier tier) {
  switch (tier) {
    case QualityTier::Full: return "L0-Full";
    case QualityTier::NoMsaa: return "L1-NoMsaa";
    case QualityTier::SimpleUi: return "L2-SimpleUi";
    case QualityTier::LowRes: return "L3-LowRes";
  }
  return "Unknown";
}

struct PerfGovernor::Impl {
  GovConfig cfg{};
  GovStats stats{};
  QualityTier tier = QualityTier::Full;
  std::vector<TierChange> changes;

  u32 over_streak = 0;
  u32 under_streak = 0;

  f32 total_ms_sum = 0.f;

  void set_tier(QualityTier next, u64 frame, const char* reason) {
    if (next == tier) return;
    changes.push_back(TierChange{frame, tier, next, reason});
    tier = next;
    stats.tier = next;
    over_streak = 0;
    under_streak = 0;
  }
};

PerfGovernor::PerfGovernor() : PerfGovernor(GovConfig{}) {}

PerfGovernor::PerfGovernor(const GovConfig& cfg) : impl_(std::make_unique<Impl>()) {
  impl_->cfg = cfg;
}

PerfGovernor::~PerfGovernor() = default;

const GovConfig& PerfGovernor::config() const { return impl_->cfg; }

void PerfGovernor::set_config(const GovConfig& cfg) { impl_->cfg = cfg; }

bool PerfGovernor::report_frame(const FrameSample& s) {
  Impl& d = *impl_;
  GovStats& st = d.stats;
  ++st.frames;

  const f32 total = s.buckets.total_ms();
  d.total_ms_sum += total;
  st.avg_total_ms = d.total_ms_sum / static_cast<f32>(st.frames);
  st.max_total_ms = std::max(st.max_total_ms, total);

  bool healthy = true;

  // G1 帧预算(分桶占比只记录不做硬判,因为"哪个桶超了"要由具体模块负责)
  if (total > d.cfg.frame_budget_ms) {
    ++st.over_budget_frames;
    ++d.over_streak;
    healthy = false;
  } else {
    d.over_streak = 0;
  }

  // 验收门 2：每帧**最多**一次 submit。
  //
  // 旧口径是"恰好 1 次（==0 也算违规，该上屏没上屏）"。调研 v2 §7 改路线后
  // 这个口径不再成立：UI 合成归壳层，SDK 只提交场景帧；而离屏/无 surface/
  // 后台场景下本就**没有**场景提交（G7 跳过）。真正要守的不变量是
  // "不能一帧提交多次"—— 那才是把 UI pass 塞进同一帧的旧路线特征。
  if (s.submit_count > 1) {
    ++st.submit_violations;
    healthy = false;
  }

  // G5 / SDK M5:稳态帧 VMA alloc/free == 0
  if (s.vma_alloc_count != 0) {
    ++st.vma_alloc_violations;
    healthy = false;
  }

  // G3:UI drawcall 与顶点上限
  if (s.draw_calls > d.cfg.ui_max_draw_calls) {
    ++st.ui_drawcall_violations;
    healthy = false;
  }
  if (s.vertices > d.cfg.ui_max_vertices) {
    ++st.ui_vertex_violations;
    healthy = false;
  }

  // G6:输入→呈现延迟
  if (s.input_latency_frames > d.cfg.max_input_latency_frames) {
    ++st.latency_violations;
    healthy = false;
  }

  // --- 档位决策 ---
  if (d.over_streak >= d.cfg.degrade_after_frames &&
      d.tier != QualityTier::LowRes) {
    const QualityTier next = static_cast<QualityTier>(static_cast<std::uint8_t>(d.tier) + 1);
    d.set_tier(next, s.frame_index, "continuous over-budget");
  } else if (total <= d.cfg.frame_budget_ms * d.cfg.recover_margin_ratio &&
             d.tier != QualityTier::Full) {
    ++d.under_streak;
    if (d.under_streak >= d.cfg.recover_after_frames) {
      const QualityTier next = static_cast<QualityTier>(static_cast<std::uint8_t>(d.tier) - 1);
      d.set_tier(next, s.frame_index, "continuous under-budget");
    }
  } else {
    d.under_streak = 0;
  }

  return healthy;
}

QualityTier PerfGovernor::tier() const { return impl_->tier; }
bool PerfGovernor::ui_msaa_enabled() const { return impl_->tier == QualityTier::Full; }
bool PerfGovernor::ui_effects_enabled() const {
  return impl_->tier == QualityTier::Full || impl_->tier == QualityTier::NoMsaa;
}
bool PerfGovernor::ui_simplified() const {
  return impl_->tier == QualityTier::SimpleUi || impl_->tier == QualityTier::LowRes;
}
f32 PerfGovernor::render_scale() const {
  // spec §6 G2:降低分辨率**仅离屏路径可做**。这里只暴露比例值,
  // "是否真的降"由调用方按 TargetMode 决定(Presenter 主路径不读它)。
  return impl_->tier == QualityTier::LowRes ? 0.75f : 1.f;
}

const GovStats& PerfGovernor::stats() const { return impl_->stats; }
const std::vector<TierChange>& PerfGovernor::tier_changes() const { return impl_->changes; }

std::string PerfGovernor::format_panel() const {
  const GovStats& st = impl_->stats;
  const GovConfig& c = impl_->cfg;
  char buf[256];
  std::string out;
  std::snprintf(buf, sizeof(buf), "frames=%llu tier=%s\n",
                static_cast<unsigned long long>(st.frames), to_string(st.tier));
  out += buf;
  std::snprintf(buf, sizeof(buf), "cpu_ms avg=%.2f max=%.2f budget=%.1f over=%u\n",
                static_cast<double>(st.avg_total_ms), static_cast<double>(st.max_total_ms),
                static_cast<double>(c.frame_budget_ms), st.over_budget_frames);
  out += buf;
  std::snprintf(buf, sizeof(buf), "submit_viol=%u vma_viol=%u\n", st.submit_violations,
                st.vma_alloc_violations);
  out += buf;
  std::snprintf(buf, sizeof(buf), "ui_dc_viol=%u ui_vtx_viol=%u lat_viol=%u\n",
                st.ui_drawcall_violations, st.ui_vertex_violations, st.latency_violations);
  out += buf;
  return out;
}

void PerfGovernor::reset() {
  const GovConfig keep = impl_->cfg;
  impl_ = std::make_unique<Impl>();
  impl_->cfg = keep;
  impl_->stats.tier = QualityTier::Full;
}

}  // namespace magicblocks::ui
