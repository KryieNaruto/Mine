#pragma once

#include <vector>

#include "core/stroke/types.h"

namespace easypainter::cli {

// 内置示例点(归一化 [0,1]):一条 S 形笔画。零参数 CLI 与图像 golden 共用,保证确定性。
inline std::vector<stroke::Vec2> example_points() {
  return {
      {0.10f, 0.80f}, {0.20f, 0.72f}, {0.30f, 0.58f}, {0.40f, 0.45f},
      {0.50f, 0.35f}, {0.60f, 0.45f}, {0.70f, 0.58f}, {0.80f, 0.72f},
      {0.90f, 0.80f},
  };
}

}  // namespace easypainter::cli
