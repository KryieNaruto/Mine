#include <gtest/gtest.h>

#include <vector>

#include "core/bench/bench.h"

using namespace easypainter;

TEST(Bench, UpdateLatencyUnderThreshold) {
  stroke::Predictor p;
  // 先画一段,使 modeler 进入进行中的笔画状态
  std::vector<stroke::Vec2> out;
  p.update({stroke::InputType::kDown, {0.f, 0.f}, 0.0f}, out);
  for (int i = 1; i <= 10; ++i) {
    p.update({stroke::InputType::kMove, {i * 0.1f, i * 0.05f}, i * 0.05f}, out);
  }
  const auto stats = bench::measure_update_latency(
      p, {stroke::InputType::kMove, {1.5f, 0.8f}, 0.6f}, 1000);
  EXPECT_LT(stats.mean_ms, 10.0);  // 宽松上限,避免抖动误报
}

TEST(Bench, ThroughputPositive) {
  stroke::Predictor p;
  const std::vector<stroke::InputEvent> events = {
      {stroke::InputType::kDown, {0.f, 0.f}, 0.0f},
      {stroke::InputType::kMove, {1.f, 1.f}, 0.05f},
      {stroke::InputType::kMove, {2.f, 1.f}, 0.10f},
      {stroke::InputType::kUp, {3.f, 1.f}, 0.15f},
  };
  const double t = bench::measure_throughput_pts_per_s(p, events);
  EXPECT_GT(t, 0.0);
}
