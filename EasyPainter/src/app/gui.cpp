#include "app/gui.h"

#include <vector>

#include "core/bench/bench.h"
#include "core/stroke/input_source.h"
#include "imgui.h"

namespace easypainter::app {

void render_tuning_panel(stroke::PredictorConfig& cfg, bool& dirty, float latency_ms,
                         stroke::Predictor* predictor) {
  ImGui::Begin("EasyPainter 调参");
  ImGui::Text("最近预测延迟: %.2f ms", latency_ms);
  // 各滑杆的(min,max)即"可调范围";每个参数的物理含义与调大/调小效果见
  // predictor.h 中 PredictorConfig 的逐字段注释。
  // 位置弹簧模型(影响跟手度/平滑度)。
  dirty |= ImGui::SliderFloat("spring_mass_constant", &cfg.spring_mass_constant,
                              0.0001f, 0.01f, "%.5f");
  dirty |= ImGui::SliderFloat("drag_constant", &cfg.drag_constant, 1.f, 200.f, "%.1f");
  // 采样(输出点密度/笔画收尾)。
  float mor = static_cast<float>(cfg.min_output_rate);
  if (ImGui::SliderFloat("min_output_rate", &mor, 10.f, 500.f, "%.0f")) {
    cfg.min_output_rate = mor;
    dirty = true;
  }
  float eosd = static_cast<float>(cfg.end_of_stroke_stopping_distance);
  if (ImGui::SliderFloat("end_of_stroke_stopping_distance", &eosd, 0.0001f, 0.01f,
                         "%.4f")) {
    cfg.end_of_stroke_stopping_distance = eosd;
    dirty = true;
  }
  // wobble 平滑(抖动抑制,按速度阈值插值平滑强度)。
  float wob_t = static_cast<float>(cfg.wobble_timeout_s);
  if (ImGui::SliderFloat("wobble_timeout_s", &wob_t, 0.f, 0.2f, "%.3f")) {
    cfg.wobble_timeout_s = wob_t;
    dirty = true;
  }
  dirty |= ImGui::SliderFloat("wobble_speed_floor", &cfg.wobble_speed_floor, 0.0f,
                              5.0f, "%.2f");
  dirty |= ImGui::SliderFloat("wobble_speed_ceiling", &cfg.wobble_speed_ceiling,
                              0.0f, 10.0f, "%.2f");
  ImGui::Separator();
  if (predictor != nullptr) {
    ImGui::Text("Benchmark(单次 update 延迟/吞吐)");
    if (ImGui::Button("Run bench")) {
      std::vector<stroke::Vec2> out;
      predictor->update({stroke::InputType::kDown, {0.f, 0.f}, 0.0f}, out);
      for (int i = 1; i <= 10; ++i) {
        predictor->update(
            {stroke::InputType::kMove, {i * 0.1f, i * 0.05f}, i * 0.05f}, out);
      }
      const auto stats = bench::measure_update_latency(
          *predictor, {stroke::InputType::kMove, {1.5f, 0.8f}, 0.6f}, 200);
      ImGui::Text("update p50=%.3fms p99=%.3fms mean=%.3fms", stats.p50_ms,
                  stats.p99_ms, stats.mean_ms);
      const std::vector<stroke::InputEvent> evs = {
          {stroke::InputType::kDown, {0.f, 0.f}, 0.0f},
          {stroke::InputType::kMove, {1.f, 1.f}, 0.05f},
          {stroke::InputType::kMove, {2.f, 1.f}, 0.10f},
          {stroke::InputType::kUp, {3.f, 1.f}, 0.15f},
      };
      const double t = bench::measure_throughput_pts_per_s(*predictor, evs);
      ImGui::Text("throughput=%.0f pts/s", t);
    }
  }
  ImGui::Text("提示: 在窗口内按住鼠标左键拖动画笔画。");
  ImGui::End();
}

}  // namespace easypainter::app
