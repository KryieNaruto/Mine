#include "core/stroke/predictor.h"

#include <memory>
#include <vector>

#include "ink_stroke_modeler/params.h"
#include "ink_stroke_modeler/stroke_modeler.h"
#include "ink_stroke_modeler/types.h"

namespace easypainter::stroke {

namespace ism = ink::stroke_model;

namespace {

// 由 PredictorConfig 构造合法可运行的 StrokeModelParams(基准对齐 ink 测试 kDefaultParams)。
ism::StrokeModelParams BuildParams(const PredictorConfig& cfg) {
  ism::StrokeModelParams p;
  p.wobble_smoother_params.is_enabled = true;
  p.wobble_smoother_params.timeout = ism::Duration(cfg.wobble_timeout_s);
  p.wobble_smoother_params.speed_floor = cfg.wobble_speed_floor;
  p.wobble_smoother_params.speed_ceiling = cfg.wobble_speed_ceiling;
  p.position_modeler_params.spring_mass_constant = cfg.spring_mass_constant;
  p.position_modeler_params.drag_constant = cfg.drag_constant;
  p.position_modeler_params.loop_contraction_mitigation_params.is_enabled = false;
  p.position_modeler_params.loop_contraction_mitigation_params.min_speed_sampling_window =
      ism::Duration(0);
  p.sampling_params.min_output_rate = cfg.min_output_rate;
  p.sampling_params.end_of_stroke_stopping_distance =
      static_cast<float>(cfg.end_of_stroke_stopping_distance);
  p.sampling_params.end_of_stroke_max_iterations = 20;
  p.stylus_state_modeler_params.use_stroke_normal_projection = false;
  p.prediction_params = ism::StrokeEndPredictorParams{};
  return p;
}

ism::Input::EventType ToInkType(InputType t) {
  switch (t) {
    case InputType::kDown:
      return ism::Input::EventType::kDown;
    case InputType::kUp:
      return ism::Input::EventType::kUp;
    default:
      return ism::Input::EventType::kMove;
  }
}

}  // namespace

struct Predictor::Impl {
  ism::StrokeModeler modeler;
  ism::StrokeModelParams params;
};

Predictor::Predictor(PredictorConfig cfg) : impl_(std::make_unique<Impl>()) {
  impl_->params = BuildParams(cfg);
  impl_->modeler.Reset(impl_->params);
}

Predictor::~Predictor() = default;

void Predictor::update(const InputEvent& event, std::vector<Vec2>& out) {
  ism::Input in;
  in.event_type = ToInkType(event.type);
  in.position = {event.pos.x, event.pos.y};
  in.time = ism::Time(event.time_s);
  std::vector<ism::Result> results;
  if (!impl_->modeler.Update(in, results).ok()) return;
  out.reserve(out.size() + results.size());
  for (const auto& r : results) {
    out.push_back(Vec2{r.position.x, r.position.y});
  }
}

std::vector<Vec2> Predictor::predict(const std::vector<InputEvent>& events) {
  std::vector<Vec2> pts;
  if (events.empty()) return pts;
  reset();
  for (const auto& e : events) update(e, pts);
  return pts;
}

void Predictor::reset() { impl_->modeler.Reset(impl_->params); }

}  // namespace easypainter::stroke
