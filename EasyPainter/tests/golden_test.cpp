#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "cli/example_points.h"
#include "core/stroke/input_source.h"
#include "core/stroke/predictor.h"

using namespace easypainter;

#ifndef GOLDEN_DIR
#define GOLDEN_DIR "."
#endif

namespace {

std::vector<stroke::Vec2> LoadPoints(const std::string& path) {
  std::vector<stroke::Vec2> pts;
  std::ifstream f(path);
  std::string line;
  while (std::getline(f, line)) {
    if (line.empty()) continue;
    float x = 0.f, y = 0.f;
    if (std::sscanf(line.c_str(), "%f,%f", &x, &y) == 2) pts.push_back({x, y});
  }
  return pts;
}

}  // namespace

// 数值 golden:本工程 Predictor 的输出与独立 oracle(直接驱动 ink 原生 API)生成的
// golden_points.txt 逐点比对(容差 1e-4)。禁止自产自比 —— golden 由 tools/oracle_golden.cpp 生成。
TEST(NumericGolden, MatchesIndependentInkOracle) {
  const auto golden = LoadPoints(std::string(GOLDEN_DIR) + "/golden_points.txt");
  ASSERT_GT(golden.size(), 0u) << "golden_points.txt 为空或缺失";

  stroke::Predictor p;  // 默认参数,与 oracle BuildParams 一致
  const auto computed = p.predict(stroke::build_events(cli::example_points()));
  ASSERT_EQ(computed.size(), golden.size());

  for (size_t i = 0; i < golden.size(); ++i) {
    EXPECT_NEAR(computed[i].x, golden[i].x, 1e-4) << "点 " << i << " x";
    EXPECT_NEAR(computed[i].y, golden[i].y, 1e-4) << "点 " << i << " y";
  }
}
