#pragma once

#include <memory>
#include <vector>

#include "core/stroke/types.h"

namespace easypainter::stroke {

// 预测器参数(映射 ink::stroke_model::StrokeModelParams 的可调字段)。
// Task 3 据此构造合法可运行的 StrokeModelParams。
struct PredictorConfig {
  // 位置弹簧模型:质量/弹簧常数、拖拽常数。
  float spring_mass_constant = 11.f / 32400.f;
  float drag_constant = 72.f;
  // 最小输出速率(单位时间点数);输入慢于此时会补插值。
  double min_output_rate = 20.0;
  // 预测区间(秒),Kalman 预测超出末点的时长。
  double prediction_interval_s = 0.08;
};

// StrokeModeler 的稳定封装:屏蔽 ink 细节,便于单测/golden 注入。
class Predictor {
 public:
  explicit Predictor(PredictorConfig cfg = {});
  ~Predictor();
  Predictor(const Predictor&) = delete;
  Predictor& operator=(const Predictor&) = delete;

  // 增量更新:把本次事件建模产生的新预测点追加到 out。
  void update(const InputEvent& event, std::vector<Vec2>& out);
  // 对一段事件序列建模(内部 reset 后逐事件 update),返回全部建模点。
  std::vector<Vec2> predict(const std::vector<InputEvent>& events);
  // 清除进行中的笔画状态,保留参数。
  void reset();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace easypainter::stroke
