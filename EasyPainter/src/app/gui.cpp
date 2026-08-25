#include "app/gui.h"

#include "imgui.h"

namespace easypainter::app {

void render_tuning_panel(stroke::PredictorConfig& cfg, bool& dirty, float latency_ms) {
  ImGui::Begin("EasyPainter 调参");
  ImGui::Text("最近预测延迟: %.2f ms", latency_ms);
  dirty |= ImGui::SliderFloat("spring_mass_constant", &cfg.spring_mass_constant,
                              0.0001f, 0.01f, "%.5f");
  dirty |= ImGui::SliderFloat("drag_constant", &cfg.drag_constant, 1.f, 200.f, "%.1f");
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
  float wob_t = static_cast<float>(cfg.wobble_timeout_s);
  if (ImGui::SliderFloat("wobble_timeout_s", &wob_t, 0.f, 0.2f, "%.3f")) {
    cfg.wobble_timeout_s = wob_t;
    dirty = true;
  }
  dirty |= ImGui::SliderFloat("wobble_speed_floor", &cfg.wobble_speed_floor, 0.0f,
                              5.0f, "%.2f");
  dirty |= ImGui::SliderFloat("wobble_speed_ceiling", &cfg.wobble_speed_ceiling,
                              0.0f, 10.0f, "%.2f");
  ImGui::Text("提示: 在窗口内按住鼠标左键拖动画笔画。");
  ImGui::End();
}

}  // namespace easypainter::app
