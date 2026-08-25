#pragma once

#include <vector>

#include "core/stroke/predictor.h"

namespace easypainter::bench {

// 延迟统计(毫秒)。
struct LatencyStats {
  double p50_ms = 0.0;
  double p99_ms = 0.0;
  double mean_ms = 0.0;
};

// 循环调用 predictor.update(event) iters 次(每次时间/位置递增,保持笔画状态有效),
// 测量单次 update 延迟的 p50/p99/均值。
LatencyStats measure_update_latency(stroke::Predictor& p, const stroke::InputEvent& ev,
                                    int iters);

// 对事件序列重放 update,统计单位时间产出的建模点数(points/s)。
double measure_throughput_pts_per_s(
    stroke::Predictor& p, const std::vector<stroke::InputEvent>& events);

}  // namespace easypainter::bench
