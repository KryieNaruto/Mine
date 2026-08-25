#include "core/bench/bench.h"

#include <algorithm>
#include <chrono>
#include <numeric>

namespace easypainter::bench {

LatencyStats measure_update_latency(stroke::Predictor& p,
                                    const stroke::InputEvent& ev, int iters) {
  LatencyStats stats;
  if (iters <= 0) return stats;

  std::vector<stroke::Vec2> out;
  out.reserve(64);
  std::vector<double> ms;
  ms.reserve(static_cast<size_t>(iters));

  for (int i = 0; i < iters; ++i) {
    stroke::InputEvent e = ev;
    e.time_s = ev.time_s + static_cast<float>(i) * 0.05f;
    e.pos.x = ev.pos.x + static_cast<float>(i) * 0.01f;
    e.pos.y = ev.pos.y + static_cast<float>(i) * 0.005f;

    const auto t0 = std::chrono::steady_clock::now();
    p.update(e, out);
    const auto t1 = std::chrono::steady_clock::now();
    ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
  }

  std::sort(ms.begin(), ms.end());
  stats.p50_ms = ms[ms.size() / 2];
  stats.p99_ms = ms[static_cast<size_t>(ms.size() * 0.99)];
  stats.mean_ms =
      std::accumulate(ms.begin(), ms.end(), 0.0) / static_cast<double>(ms.size());
  return stats;
}

double measure_throughput_pts_per_s(
    stroke::Predictor& p, const std::vector<stroke::InputEvent>& events) {
  std::vector<stroke::Vec2> out;
  out.reserve(64);
  const auto t0 = std::chrono::steady_clock::now();
  for (const auto& e : events) p.update(e, out);
  const auto t1 = std::chrono::steady_clock::now();
  const double sec =
      std::chrono::duration<double>(t1 - t0).count();
  if (sec <= 0.0) return 0.0;
  return static_cast<double>(out.size()) / sec;
}

}  // namespace easypainter::bench
