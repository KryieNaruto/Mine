#pragma once

#include <memory>
#include <vector>

#include "core/stroke/types.h"

namespace easypainter::stroke {

// 预测器参数(映射 ink::stroke_model::StrokeModelParams 的可调字段)。
// 默认值对齐 ink 测试 kDefaultParams(cm/秒单位),保证合法可运行与 golden 可复现。
// 调参面板(gui.cpp)可直接改这些值;各字段对轨迹的直观影响见下方逐条注释。
struct PredictorConfig {
  // —— 位置弹簧模型:决定笔尖的"跟手度"与平滑度 ——
  // 弹簧质量常数 = 质点质量 ÷ 弹簧刚度。值越大,弹簧拉动笔尖的加速度越小,
  // 轨迹越平滑、但越滞后于输入(适合慢速弧线);值越小越跟手,但容易抖动/过冲。
  float spring_mass_constant = 11.f / 32400.f;
  // 拖拽常数:每单位时间从笔尖速度中扣除的比例,模拟空气阻力。
  // 值越大,笔尖停得越快、过冲越少(轨迹更稳);值太小会在拐点处"飞过"输入点。
  float drag_constant = 72.f;
  // —— 采样:控制输出点密度与笔画收尾 ——
  // 最小输出速率(点/秒):当输入采样率低于它时内部插值补点,保证每秒至少输出
  // 这么多建模点。值越大轨迹越密(更精细),但 CPU/渲染开销越高。
  double min_output_rate = 180.0;
  // 笔画结束停止距离:收尾迭代时,若剩余位移 < 该值即停止建模。
  // 应比输入点间距小 2~3 个数量级;值过大笔画"提前收尾",过小收尾耗时更长。
  double end_of_stroke_stopping_distance = 0.001;
  // —— wobble 平滑:按移动速度抑制高频抖动 ——
  // 平滑窗口时长(秒):滑动平均的采样窗口长度。值越大越平滑,但视觉滞后越大;
  // ink 建议 ≈ 2.5 ÷ 每秒输入点数。
  double wobble_timeout_s = 0.04;
  // 速度下限:笔速 ≤ 该值视为低速抖动,施加最大平滑量。
  // 建议约为预期笔速的 2%(此处输入为归一化 [0,1] 坐标)。
  float wobble_speed_floor = 1.31f;
  // 速度上限:笔速 ≥ 该值时不平滑(快速笔画保持清晰锐利)。
  // 建议约为预期笔速的 3%;必须 ≥ wobble_speed_floor。
  float wobble_speed_ceiling = 1.44f;
  // 预测区间(秒):仅 Kalman 预测器使用;当前选用 StrokeEndPredictor(只追平、不外推),
  // 因此该值暂不生效,保留供后续扩展。
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
  // 更换参数并重建模型器(清空进行中的笔画)。
  void set_config(const PredictorConfig& cfg);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace easypainter::stroke
