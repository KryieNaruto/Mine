// 数值 golden 独立 oracle:直接驱动 ink::stroke_model::StrokeModeler(原生 API),
// 与 easypainter 的 Predictor 封装完全独立。用它生成 tests/data/golden_points.txt。
//
// 参数与输入刻意与本工程 PredictorConfig 默认值 / example_points() 一致,
// 但此处不引用 easypainter 任何代码,是第二条独立编译路径。
#include <cstdio>
#include <vector>

#include "ink_stroke_modeler/params.h"
#include "ink_stroke_modeler/stroke_modeler.h"
#include "ink_stroke_modeler/types.h"

namespace ism = ink::stroke_model;

namespace {

// 与 example_points() 相同(已归一化 [0,1]×[0,1] 的 S 形)。
const std::vector<ism::Vec2> kPoints = {
    {0.0f, 1.0f},    {0.125f, 0.822f}, {0.25f, 0.511f}, {0.375f, 0.222f},
    {0.5f, 0.0f},    {0.625f, 0.222f}, {0.75f, 0.511f}, {0.875f, 0.822f},
    {1.0f, 1.0f},
};

// 与 PredictorConfig 默认值 / BuildParams 一致。
ism::StrokeModelParams BuildParams() {
  ism::StrokeModelParams p;
  p.wobble_smoother_params.is_enabled = true;
  p.wobble_smoother_params.timeout = ism::Duration(0.04);
  p.wobble_smoother_params.speed_floor = 1.31f;
  p.wobble_smoother_params.speed_ceiling = 1.44f;
  p.position_modeler_params.spring_mass_constant = 11.f / 32400.f;
  p.position_modeler_params.drag_constant = 72.f;
  p.position_modeler_params.loop_contraction_mitigation_params.is_enabled = false;
  p.position_modeler_params.loop_contraction_mitigation_params
      .min_speed_sampling_window = ism::Duration(0);
  p.sampling_params.min_output_rate = 180.0;
  p.sampling_params.end_of_stroke_stopping_distance = 0.001f;
  p.sampling_params.end_of_stroke_max_iterations = 20;
  p.stylus_state_modeler_params.use_stroke_normal_projection = false;
  p.prediction_params = ism::StrokeEndPredictorParams{};
  return p;
}

}  // namespace

int main() {
  ism::StrokeModeler modeler;
  const auto params = BuildParams();
  if (!modeler.Reset(params).ok()) {
    std::fprintf(stderr, "oracle: Reset failed\n");
    return 1;
  }

  std::vector<ism::Result> results;
  for (size_t i = 0; i < kPoints.size(); ++i) {
    ism::Input in;
    in.event_type = (i == 0)        ? ism::Input::EventType::kDown
                    : (i + 1 == kPoints.size()) ? ism::Input::EventType::kUp
                                                : ism::Input::EventType::kMove;
    in.position = kPoints[i];
    // 与 build_events 的 float 步长 0.05f 逐位一致(先 float 计算再升 double)
    in.time = ism::Time(static_cast<double>(static_cast<float>(i) * 0.05f));
    if (!modeler.Update(in, results).ok()) {
      std::fprintf(stderr, "oracle: Update %zu failed\n", i);
      return 1;
    }
  }

  // 输出全部建模点(每行 x,y)
  for (const auto& r : results) {
    std::printf("%.9f,%.9f\n", r.position.x, r.position.y);
  }
  std::fprintf(stderr, "oracle: %zu points\n", results.size());
  return 0;
}
