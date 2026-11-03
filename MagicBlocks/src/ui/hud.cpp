// MagicBlocks UI 消费层 —— HUD 最小控件集实现(spec §8、§6 G3)
//
// 三个刻意的取舍:
//   1. **几何是绝对像素**:布局在 `relayout()` 里一次算好,命中判定就是
//      `Rect::contains` —— 输入热路径上没有矩阵、没有相机、没有 DP 换算。
//   2. **绘制顺序 = 容器顺序**:面板先画(背景),控件后画。顺序固定 → 图元序列
//      固定 → 命令流可复现(spec §5.3 / 命令模式要求)。
//   3. **无文本图集就画文本占位条**:spec §9 明确不做系统键盘,字体图集是后续
//      待办(本轮无 L3 纹理)。`label` 每字符一条短横线占位,保证"有文字的地方
//      有可见且确定性的像素",golden 才能锁住布局。
#include "magicblocks/ui/hud.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <utility>

namespace magicblocks::ui {

const char* to_string(ControlKind kind) {
  switch (kind) {
    case ControlKind::Panel: return "Panel";
    case ControlKind::Button: return "Button";
    case ControlKind::Label: return "Label";
    case ControlKind::Slider: return "Slider";
    case ControlKind::Swatch: return "Swatch";
  }
  return "Unknown";
}

namespace {

// 文本占位:每个字符一个 6x2 的短横线,字距 2px,行高 12px。
// 只画在 rect 内部,超出即裁剪(与真实文本渲染的裁剪语义一致)。
void draw_label(stub::IOverlay2D& ov, const Rect& r, const std::string& text, Rgba8 color) {
  constexpr f32 kAdvance = 8.f;
  constexpr f32 kGlyphW = 6.f;
  constexpr f32 kGlyphH = 2.f;
  constexpr f32 kGlyphY = 4.f;
  f32 x = r.x + 4.f;
  const f32 y = r.y + kGlyphY;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (x + kGlyphW > r.right()) break;  // 裁剪
    if (text[i] != ' ') {
      ov.fill_rect(Rect{x, y, kGlyphW, kGlyphH}, color);
    }
    x += kAdvance;
  }
}

}  // namespace

struct Hud::Impl {
  std::vector<Control> controls;
  std::unordered_map<std::uint32_t, std::size_t> index_by_id;
  std::uint32_t next_id = 1;

  // 输入会话状态
  std::uint32_t active_id = 0;   // 当前按下的控件
  bool dragging_slider = false;
  HudLayoutConfig cfg{};

  mutable DrawCounts counts{};

  std::uint32_t alloc_id() { return next_id++; }

  void reindex() {
    index_by_id.clear();
    for (std::size_t i = 0; i < controls.size(); ++i) index_by_id[controls[i].id] = i;
  }

  // 命中:后加入的控件在上层(与绘制顺序相反),所以**从后往前**找第一个命中。
  // 容器 Panel 也参与命中:点在面板空白处时由面板消费,避免穿透到相机(spec §7 ①)。
  Control* hit(Vec2 p) {
    for (std::size_t i = controls.size(); i-- > 0;) {
      Control& c = controls[i];
      if (!c.enabled) continue;
      if (c.rect_px.contains(p)) return &c;
    }
    return nullptr;
  }

  void update_slider(Control& c, Vec2 p) {
    if (c.rect_px.empty()) return;
    f32 v = (p.x - c.rect_px.x) / c.rect_px.w;
    v = std::clamp(v, 0.f, 1.f);
    if (v != c.value) {
      c.value = v;
      if (c.on_value_changed) c.on_value_changed(v);
    }
  }
};

Hud::Hud() : impl_(std::make_unique<Impl>()) {}
Hud::~Hud() = default;

void Hud::clear() {
  impl_->controls.clear();
  impl_->index_by_id.clear();
  impl_->next_id = 1;
  impl_->active_id = 0;
  impl_->dragging_slider = false;
}

std::uint32_t Hud::add(const Control& control) {
  Control c = control;
  if (c.id == 0) c.id = impl_->alloc_id();
  else impl_->next_id = std::max(impl_->next_id, c.id + 1);
  impl_->controls.push_back(c);
  impl_->reindex();
  return c.id;
}

Control* Hud::find(std::uint32_t id) {
  auto it = impl_->index_by_id.find(id);
  if (it == impl_->index_by_id.end()) return nullptr;
  return &impl_->controls[it->second];
}

const Control* Hud::find(std::uint32_t id) const {
  auto it = impl_->index_by_id.find(id);
  if (it == impl_->index_by_id.end()) return nullptr;
  return &impl_->controls[it->second];
}

std::size_t Hud::size() const { return impl_->controls.size(); }

const std::vector<Control>& Hud::controls() const { return impl_->controls; }

void Hud::relayout(const HudLayoutConfig& cfg, DisplayInfo display, f32 top_bar_dp,
                    f32 bottom_panel_dp) {
  impl_->cfg = cfg;
  const f32 d = display.density > 0.f ? display.density : 1.f;
  const f32 W = static_cast<f32>(display.width);
  const f32 H = static_cast<f32>(display.height);
  const f32 m = cfg.margin_dp * d;
  const f32 row_h = cfg.row_height_dp * d;
  const f32 btn_w = cfg.button_width_dp * d;
  const f32 gap = cfg.gap_dp * d;

  // 顶栏:从左到右排一行(超宽则换行 —— 旋转到竖屏时仍可用)
  // 任何情况下都不许越出 `[m, W-m]`:按钮宽度先夹到可用宽度,
  // 否则竖屏(320 宽)时一个 80dp 按钮就能把控件推到屏幕外。
  const f32 max_w = std::max(0.f, W - 2 * m);
  f32 x = m;
  f32 y = m;
  for (auto& c : impl_->controls) {
    const bool is_top = (c.kind == ControlKind::Button || c.kind == ControlKind::Label);
    if (!is_top) continue;
    const f32 w = (c.kind == ControlKind::Button)
                      ? std::min(btn_w, max_w)
                      : std::min(c.rect_px.w > 0.f ? c.rect_px.w : btn_w, max_w);
    // 只有"本行已经有东西"时才换行,否则第一列永远排不进去会死循环式换行。
    if (x > m && x + w > W - m) {
      x = m;
      y += row_h + gap;
    }
    c.rect_px = Rect{x, y, w, row_h};
    x += w + gap;
  }
  (void)top_bar_dp;

  // 底部面板:整宽减去边距,高度由调用方给
  const f32 panel_h = std::max(row_h, bottom_panel_dp * d);
  const Rect bottom{m, H - m - panel_h, std::max(0.f, W - 2 * m), panel_h};
  f32 inner_x = bottom.x + cfg.panel_padding_dp * d;
  const f32 inner_y = bottom.y + cfg.panel_padding_dp * d;
  const f32 inner_w = std::max(0.f, bottom.w - 2 * cfg.panel_padding_dp * d);
  const f32 pad = cfg.panel_padding_dp * d;
  const f32 inner_h = std::max(8.f, panel_h - 2 * pad);
  for (auto& c : impl_->controls) {
    if (c.kind == ControlKind::Panel) {
      c.rect_px = bottom;
      continue;
    }
    if (c.kind == ControlKind::Slider) {
      // 滑条贴面板**右边缘**:必须从 bottom 的固定右边算,不能用会随色板推进的
      // inner_x —— 否则色板排完之后滑条会被推到屏幕外(674 > 480 的由来)。
      const f32 sw = std::max(0.f, inner_w * 0.45f);
      c.rect_px = Rect{bottom.x + pad + inner_w - sw, inner_y, sw, inner_h};
      continue;
    }
    if (c.kind == ControlKind::Swatch) {
      // 色板左对齐连排;放不下就整体停在这一行,不越出面板右边。
      const f32 s = std::max(8.f, std::min(row_h, panel_h) - 2 * pad);
      if (inner_x + s > bottom.x + bottom.w - pad) continue;
      c.rect_px = Rect{inner_x, inner_y, s, s};
      inner_x += s + gap;
      continue;
    }
    // 其余(按钮/标签)若已在顶栏排过就不再动;未排过的摆进底部面板
    if (c.rect_px.w <= 0.f || c.rect_px.h <= 0.f) {
      const f32 w = (c.kind == ControlKind::Button) ? std::min(btn_w, inner_w)
                                                    : std::max(40.f, inner_w * 0.2f);
      if (inner_x + w > bottom.x + bottom.w - pad) continue;
      c.rect_px = Rect{inner_x, inner_y, w, inner_h};
      inner_x += w + gap;
    }
  }
}

bool Hud::on_input(const device::InputEvent& ev) {
  switch (ev.kind) {
    case device::InputEventKind::PointerDown: {
      Control* c = impl_->hit(from_device(ev.position));
      if (!c) return false;
      impl_->active_id = c->id;
      c->pressed = true;
      if (c->kind == ControlKind::Slider) {
        impl_->dragging_slider = true;
        impl_->update_slider(*c, from_device(ev.position));
      }
      // 所有命中(含纯 Panel)都返回 true:命中即消费,不再下发给工具/相机(spec §7 ①)
      return true;
    }
    case device::InputEventKind::PointerMove: {
      // hover 跟踪(不消费:移动本身不影响归属,归属已在按下时定)
      for (auto& c : impl_->controls) c.hovered = c.rect_px.contains(from_device(ev.position));
      if (impl_->dragging_slider && impl_->active_id) {
        if (Control* c = find(impl_->active_id)) impl_->update_slider(*c, from_device(ev.position));
      }
      return false;
    }
    case device::InputEventKind::PointerUp: {
      if (!impl_->active_id) return false;
      Control* c = find(impl_->active_id);
      impl_->active_id = 0;
      impl_->dragging_slider = false;
      if (!c) return true;
      const bool was_pressed = c->pressed;
      c->pressed = false;
      // 只有"在控件内抬起"才算点击(手指滑出后抬起 = 取消,与常见 UI 一致)
      if (was_pressed && c->rect_px.contains(from_device(ev.position)) && c->enabled) {
        if (c->kind == ControlKind::Swatch) {
          // 色板:点选即回调该格颜色(用一张常量色表,不做换肤)
          if (c->on_value_changed) c->on_value_changed(0.f);
        }
        if (c->on_click) c->on_click();
      }
      return true;
    }
    case device::InputEventKind::PointerCancel: {
      if (impl_->active_id) {
        if (Control* c = find(impl_->active_id)) c->pressed = false;
      }
      impl_->active_id = 0;
      impl_->dragging_slider = false;
      return true;
    }
    default:
      return false;
  }
}

void Hud::draw(stub::IOverlay2D& overlay) const {
  impl_->counts = DrawCounts{};
  for (const auto& c : impl_->controls) {
    switch (c.kind) {
      case ControlKind::Panel:
        if (c.fill.a) {
          overlay.fill_rect(c.rect_px, c.fill);
          ++impl_->counts.quads;
        }
        if (c.stroke.a) {
          overlay.stroke_rect(c.rect_px, c.stroke);
          ++impl_->counts.strokes;
        }
        break;
      case ControlKind::Button: {
        Rgba8 fill = c.fill;
        if (c.pressed) {
          // 按下视觉:提亮(确定性,无动画)
          fill = Rgba8{static_cast<std::uint8_t>(std::min(255, fill.r + 40)),
                       static_cast<std::uint8_t>(std::min(255, fill.g + 40)),
                       static_cast<std::uint8_t>(std::min(255, fill.b + 40)), fill.a};
        }
        if (!c.enabled) fill.a = static_cast<std::uint8_t>(fill.a / 2);
        if (fill.a) {
          overlay.fill_rect(c.rect_px, fill);
          ++impl_->counts.quads;
        }
        if (c.stroke.a) {
          overlay.stroke_rect(c.rect_px, c.stroke);
          ++impl_->counts.strokes;
        }
        if (c.draw_text && !c.label.empty()) {
          draw_label(overlay, c.rect_px, c.label, c.text_color);
          ++impl_->counts.text_runs;
        }
        break;
      }
      case ControlKind::Label: {
        if (c.fill.a) {
          overlay.fill_rect(c.rect_px, c.fill);
          ++impl_->counts.quads;
        }
        if (c.draw_text && !c.label.empty()) {
          draw_label(overlay, c.rect_px, c.label, c.text_color);
          ++impl_->counts.text_runs;
        }
        break;
      }
      case ControlKind::Slider: {
        if (c.fill.a) {
          overlay.fill_rect(c.rect_px, c.fill);
          ++impl_->counts.quads;
        }
        // 轨道 + 填充 + 滑块
        const f32 track_h = 4.f;
        const Rect track{c.rect_px.x, c.rect_px.y + (c.rect_px.h - track_h) / 2.f, c.rect_px.w,
                         track_h};
        overlay.fill_rect(track, palette::kPanelStroke);
        overlay.fill_rect(Rect{track.x, track.y, track.w * std::clamp(c.value, 0.f, 1.f), track_h},
                          palette::kAccent);
        const f32 knob = 12.f;
        const f32 kx = c.rect_px.x + std::clamp(c.value, 0.f, 1.f) * (c.rect_px.w - knob);
        overlay.fill_rect(Rect{kx, c.rect_px.y + (c.rect_px.h - knob) / 2.f, knob, knob},
                          palette::kText);
        impl_->counts.quads += 3;
        break;
      }
      case ControlKind::Swatch: {
        overlay.fill_rect(c.rect_px, c.swatch);
        overlay.stroke_rect(c.rect_px, c.hovered || c.pressed ? palette::kText : palette::kPanelStroke);
        ++impl_->counts.quads;
        ++impl_->counts.strokes;
        break;
      }
    }
  }
}

Hud::DrawCounts Hud::draw_counts() const { return impl_->counts; }

std::uint32_t build_minimal_hud(Hud* hud, const HudLayoutConfig& cfg, DisplayInfo display) {
  if (!hud) return 0;
  hud->clear();
  (void)cfg;

  // 底部面板(先加 → 先画 → 在最底层)
  Control panel;
  panel.kind = ControlKind::Panel;
  panel.fill = palette::kPanelFill;
  panel.stroke = palette::kPanelStroke;
  panel.draw_text = false;
  hud->add(panel);

  // 色板:8 个固定色(不做换肤,spec §9)
  static const Rgba8 kSwatches[8] = {
      {236, 72, 96, 255}, {255, 152, 64, 255}, {255, 218, 96, 255}, {120, 208, 112, 255},
      {88, 176, 240, 255}, {136, 120, 248, 255}, {232, 120, 216, 255}, {248, 248, 248, 255},
  };
  for (auto color : kSwatches) {
    Control sw;
    sw.kind = ControlKind::Swatch;
    sw.swatch = color;
    hud->add(sw);
  }

  // 层数滑条
  Control slider;
  slider.kind = ControlKind::Slider;
  slider.label = "depth";
  slider.value = 0.5f;
  hud->add(slider);

  // 顶栏:菜单按钮(回 HomeScreen)
  Control back;
  back.kind = ControlKind::Button;
  back.label = "back";
  back.fill = palette::kAccentDim;
  back.stroke = palette::kPanelStroke;
  hud->add(back);

  // 顶栏:统计开关
  Control perf;
  perf.kind = ControlKind::Button;
  perf.label = "stats";
  perf.fill = palette::kPanelFill;
  perf.stroke = palette::kPanelStroke;
  hud->add(perf);

  hud->relayout(cfg, display, 44.f, 64.f);
  return hud->size();
}

}  // namespace magicblocks::ui
