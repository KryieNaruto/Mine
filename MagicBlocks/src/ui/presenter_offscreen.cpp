// MagicBlocks UI 消费层 —— 离屏呈现后端(spec §5.5)
//
// 这是 UI 层与 GPU 之间的**离屏**桥:把 HUD/Router 产出的图元交给后端桩
// (`backend::UiOverlayPass`)渲染并读回。**只有这条路径存在 CPU 回读**;
// 主路径(`presenter.cpp` + `presenter_host.cpp`)里一次都没有。
#include "offscreen_target.h"

#include <cstring>
#include <vector>

#include "backend/vulkan/vulkan_ui_overlay.h"
#include "overlay_impl.h"
#include "ui_app_internal.h"

namespace magicblocks::ui {

OffscreenTargetRenderer::OffscreenTargetRenderer() = default;
OffscreenTargetRenderer::~OffscreenTargetRenderer() = default;

bool OffscreenTargetRenderer::attach(backend::VulkanFrameRenderer* renderer) {
  if (!renderer) return false;
  renderer_ = renderer;
  return true;
}

bool OffscreenTargetRenderer::present(UiApp& app, u32 width, u32 height, std::uint8_t* out_rgba8,
                                      u32 row_bytes) {
  if (!renderer_ || !out_rgba8 || width == 0 || height == 0) return false;
  if (row_bytes < width * 4u) return false;

  // 1) UI 层产出本帧图元(与主路径**同一份**绘制代码)。
  UiAppInternal* internal = as_internal(&app);
  if (!internal) return false;
  internal->draw_ui_for_current_target();

  // 2) 图元 → 后端实例(顺序即绘制顺序;合批在 UiOverlayPass 内完成)。
  auto& pass = renderer_->overlay_pass();
  pass.reset_instances();
  for (const auto& p : internal->overlay().primitives()) {
    const float r = static_cast<float>(p.color.r) / 255.f;
    const float g = static_cast<float>(p.color.g) / 255.f;
    const float b = static_cast<float>(p.color.b) / 255.f;
    const float a = static_cast<float>(p.color.a) / 255.f;
    switch (p.kind) {
      case detail::PrimitiveKind::Fill:
        pass.append_rect(p.a.x, p.a.y, p.b.x, p.b.y, r, g, b, a);
        break;
      case detail::PrimitiveKind::Stroke:
        pass.append_stroke(p.a.x, p.a.y, p.b.x, p.b.y, r, g, b, a);
        break;
      case detail::PrimitiveKind::Line:
        // 后端桩把线段退化成细矩形;真正的线段图元等 L3 IOverlay2D 落地后替换。
        pass.append_line(p.a.x, p.a.y, p.b.x, p.b.y, p.width, r, g, b, a);
        break;
    }
  }

  // 3) 渲染 + 读回(离屏专用)
  return renderer_->render_to_memory(width, height, out_rgba8, row_bytes);
}

}  // namespace magicblocks::ui
