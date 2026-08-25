#pragma once

#include "core/stroke/predictor.h"

namespace easypainter::app {

// 绘制 ImGui 调参面板。cfg 为当前预测器参数,用户改动时置 dirty=true。
// latency_ms 为最近一次预测延迟(ms),用于面板显示;predictor 非空时提供 "Run bench"。
void render_tuning_panel(stroke::PredictorConfig& cfg, bool& dirty, float latency_ms,
                         stroke::Predictor* predictor = nullptr);

}  // namespace easypainter::app
