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
#include <unordered_set>
#include <utility>

namespace magicblocks::ui {

const char* to_string(ControlKind kind) {
  switch (kind) {
    case ControlKind::Panel: return "Panel";
    case ControlKind::Button: return "Button";
    case ControlKind::Label: return "Label";
    case ControlKind::Slider: return "Slider";
    case ControlKind::Swatch: return "Swatch";
    case ControlKind::ListItem: return "ListItem";
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

  // 列表面板 → 最近一次布局结果(测试与可见性提示都读它)
  std::unordered_map<std::uint32_t, ListColumnLayout> list_layouts;
  // ListItem id → 所属面板 id。**必须显式记录**:只靠几何位置判断归属时,
  // 另一个面板的条目矩形可能恰好落进本面板的 viewport(实测踩到过),
  // 于是"右面板的列表"会把左面板的条目也算进来。
  std::unordered_map<std::uint32_t, std::uint32_t> item_owner;
  // 被"列表布局"接管的面板 id。这类面板只由 layout_list_panel 摆几何。
  std::unordered_set<std::uint32_t> list_panels;

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
  //
  // 列表裁剪:被"滚出 viewport"的 ListItem 几何上已经挪到面板外,但仍可能落在
  // 别的控件上;而且**不可见的条目绝不能被点到**(否则用户点空白处会选中看不见
  // 的那一条)。所以 ListItem 命中前要确认自身完整落在 viewport 内。
  Control* hit(Vec2 p) {
    for (std::size_t i = controls.size(); i-- > 0;) {
      Control& c = controls[i];
      if (!c.enabled) continue;
      if (c.kind == ControlKind::ListItem && !list_item_visible(c)) continue;
      if (c.rect_px.contains(p)) return &c;
    }
    return nullptr;
  }

  // ListItem 是否落在**它所属面板**的 viewport 内(整条都在内才算)。
  bool list_item_visible(const Control& c) const {
    auto owner = item_owner.find(c.id);
    if (owner == item_owner.end()) return true;  // 不隶属于任何列表:按普通控件处理
    auto it = list_layouts.find(owner->second);
    if (it == list_layouts.end()) return true;
    const ListColumnLayout& lay = it->second;
    return c.rect_px.h > 0.f &&
           c.rect_px.x >= lay.viewport.x - 0.5f &&
           c.rect_px.right() <= lay.viewport.right() + 0.5f &&
           c.rect_px.y >= lay.viewport.y - 0.5f &&
           c.rect_px.bottom() <= lay.viewport.bottom() + 0.5f;
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
  impl_->list_layouts.clear();
  impl_->list_panels.clear();
  impl_->item_owner.clear();
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
      // 被列表布局接管的面板(左右侧栏)几何由 layout_list_panel 负责,
      // 通用通道不许再把它按到底部矩形上 —— 否则左右面板会叠成一块。
      if (impl_->list_panels.count(c.id)) continue;
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
    // 列表条目由 layout_list_panel 排布,绝不进通用通道
    if (c.kind == ControlKind::ListItem) continue;
    // 其余(按钮/标签)若已在顶栏排过就不再动;未排过的摆进底部面板
    if (c.rect_px.w <= 0.f || c.rect_px.h <= 0.f) {
      const f32 w = (c.kind == ControlKind::Button) ? std::min(btn_w, inner_w)
                                                    : std::max(40.f, inner_w * 0.2f);
      if (inner_x + w > bottom.x + bottom.w - pad) continue;
      c.rect_px = Rect{inner_x, inner_y, w, inner_h};
      inner_x += w + gap;
    }
  }
  (void)top_bar_dp;
}

// ---------------------------------------------------------------- 列表面板

ListColumnLayout Hud::layout_list_panel(std::uint32_t panel_id, std::uint32_t first_row,
                                        const HudLayoutConfig& cfg) {
  ListColumnLayout out;
  Control* panel = find(panel_id);
  if (!panel || panel->kind != ControlKind::Panel) return out;

  impl_->list_panels.insert(panel_id);  // 从此这个面板归列表布局管
  const f32 d = 1.f;  // 像素空间:面板几何已经是像素,行高在构建期由 dp*density 换算
  const f32 pad = cfg.panel_padding_dp * d;
  out.panel = panel->rect_px;
  out.row_height_px = cfg.list_row_height_dp * d;

  // viewport = 面板内边距之后;去掉顶部标题行,避免条目压住标题
  const f32 title_h = cfg.list_row_height_dp * d;
  out.viewport = Rect{panel->rect_px.x + pad, panel->rect_px.y + pad + title_h,
                      std::max(0.f, panel->rect_px.w - 2.f * pad),
                      std::max(0.f, panel->rect_px.h - 2.f * pad - title_h)};

  // 收集**本面板**的 ListItem(按加入顺序 = 列表顺序)。
  // 归属优先看 item_owner(显式登记);没登记过的条目则按"几何落在面板内"认领。
  std::vector<std::size_t> items;
  for (std::size_t i = 0; i < impl_->controls.size(); ++i) {
    const Control& c = impl_->controls[i];
    if (c.kind != ControlKind::ListItem) continue;
    auto owner = impl_->item_owner.find(c.id);
    if (owner != impl_->item_owner.end()) {
      if (owner->second == panel_id) items.push_back(i);
      continue;
    }
    // 未登记:由本面板认领(此后固定归属,不再漂移)
    impl_->item_owner[c.id] = panel_id;
    items.push_back(i);
  }
  out.item_ids.reserve(items.size());
  out.total_rows = static_cast<std::uint32_t>(items.size());
  out.visible_rows = out.row_height_px > 0.f
                         ? static_cast<std::uint32_t>(out.viewport.h / out.row_height_px)
                         : 0;
  out.scrollable = out.total_rows > out.visible_rows;

  // 起始行夹到合法范围:越界的滚动值不许把列表滚出面板(也不许回绕)
  std::uint32_t max_first = out.scrollable ? (out.total_rows - out.visible_rows) : 0;
  out.first_visible_row = std::min(first_row, max_first);
  out.clamped_first_row = out.first_visible_row;

  // 逐条排布:可见的落进 viewport 内,不可见的挪到面板外(仍不可命中)
  for (std::uint32_t k = 0; k < items.size(); ++k) {
    Control& c = impl_->controls[items[k]];
    out.item_ids.push_back(c.id);
    if (k < out.first_visible_row ||
        k >= out.first_visible_row + out.visible_rows) {
      // 移出可见区:几何收成 0 高,绘制阶段自然跳过(不越界画)
      c.rect_px = Rect{out.viewport.x, out.viewport.y, out.viewport.w, 0.f};
      continue;
    }
    const std::uint32_t row = k - out.first_visible_row;
    c.rect_px = Rect{out.viewport.x, out.viewport.y + static_cast<f32>(row) * out.row_height_px,
                     out.viewport.w, out.row_height_px};
    c.index = k;
    c.index_total = out.total_rows;
  }

  // 滚动条(可见性提示):面板右缘一条细轨道 + 按可见比例缩放的滑块
  if (out.scrollable && out.visible_rows > 0) {
    const f32 bar_w = 4.f;
    out.scrollbar_track = Rect{panel->rect_px.right() - pad - bar_w, out.viewport.y, bar_w,
                               out.viewport.h};
    const f32 frac = static_cast<f32>(out.visible_rows) /
                     static_cast<f32>(out.total_rows);
    const f32 thumb_h = std::max(8.f, out.viewport.h * frac);
    const f32 travel = std::max(0.f, out.viewport.h - thumb_h);
    const f32 t = max_first > 0 ? static_cast<f32>(out.first_visible_row) /
                                      static_cast<f32>(max_first)
                                : 0.f;
    out.scrollbar_thumb = Rect{out.scrollbar_track.x,
                               out.viewport.y + travel * t, bar_w, thumb_h};
    out.has_scrollbar = true;
  }

  impl_->list_layouts[panel_id] = out;
  return out;
}

ListColumnLayout Hud::list_layout(std::uint32_t panel_id) const {
  auto it = impl_->list_layouts.find(panel_id);
  return it == impl_->list_layouts.end() ? ListColumnLayout{} : it->second;
}

std::vector<std::uint32_t> Hud::visible_list_items(std::uint32_t panel_id) const {
  std::vector<std::uint32_t> out;
  const ListColumnLayout lay = list_layout(panel_id);
  for (std::uint32_t id : lay.item_ids) {
    const Control* c = find(id);
    if (!c || c->rect_px.h <= 0.f) continue;  // 被滚动裁掉的条目不进"可见"
    out.push_back(id);
  }
  return out;
}

std::vector<std::uint32_t> Hud::add_list_items(std::uint32_t panel_id,
                                               const std::vector<std::string>& labels) {
  std::vector<std::uint32_t> ids;
  if (!find(panel_id)) return ids;  // 没有面板 = 明确什么都不做(不静默建孤儿控件)
  ids.reserve(labels.size());
  for (std::size_t i = 0; i < labels.size(); ++i) {
    Control item;
    item.kind = ControlKind::ListItem;
    item.label = labels[i];
    item.index = static_cast<std::uint32_t>(i);
    item.index_total = static_cast<std::uint32_t>(labels.size());
    item.fill = palette::kPanelStroke;   // 条目底色(比面板亮一档)
    item.stroke = Rgba8{0, 0, 0, 0};
    item.text_color = palette::kText;
    const std::uint32_t id = add(item);
    // 归属在**加入那一刻**就登记,不能等到布局时再认领 ——
    // 否则先布局左面板会把右面板的条目也一起认领走(实测踩到过)。
    impl_->item_owner[id] = panel_id;
    ids.push_back(id);
  }
  return ids;
}

bool Hud::select_list_item(std::uint32_t panel_id, std::uint32_t selected_index) {
  if (!find(panel_id)) return false;
  // 先保证本面板的条目归属已建立(否则 item_ids 为空)
  ListColumnLayout lay = list_layout(panel_id);
  if (lay.item_ids.empty()) lay = layout_list_panel(panel_id, lay.first_visible_row, impl_->cfg);
  bool found = false;
  for (std::uint32_t id : lay.item_ids) {
    Control* c = find(id);
    if (!c) continue;
    if (c->index == selected_index) {
      c->selected = true;
      found = true;
    } else {
      c->selected = false;
    }
  }
  return found;
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
      case ControlKind::ListItem: {
        // 不可见(被滚动裁掉)的条目几何高度为 0 → 一个图元都不画
        if (c.rect_px.h <= 0.f) break;
        // 选中态用强调色条 + 提亮的底色;按下再提亮一档(确定性,无动画)
        Rgba8 fill = c.selected ? palette::kAccentDim : c.fill;
        if (c.pressed) {
          fill = Rgba8{static_cast<std::uint8_t>(std::min(255, fill.r + 40)),
                       static_cast<std::uint8_t>(std::min(255, fill.g + 40)),
                       static_cast<std::uint8_t>(std::min(255, fill.b + 40)), fill.a};
        }
        if (c.selected) fill.a = 255;  // 选中必须不透明,否则像素断言抓不稳
        if (!c.enabled) fill.a = static_cast<std::uint8_t>(fill.a / 2);
        if (fill.a) {
          overlay.fill_rect(c.rect_px, fill);
          ++impl_->counts.quads;
        }
        if (c.selected) {
          // 左侧 3px 强调条:选中态在像素上是一条独立的竖直色块
          overlay.fill_rect(Rect{c.rect_px.x, c.rect_px.y, 3.f, c.rect_px.h}, palette::kAccent);
          ++impl_->counts.quads;
        }
        if (c.draw_text && !c.label.empty()) {
          draw_label(overlay, c.rect_px, c.label, c.text_color);
          ++impl_->counts.text_runs;
        }
        break;
      }
    }
  }

  // 滚动条(可见性提示):画在条目之后,保证在最上层,且夹在面板内。
  // 只画 track/thumb 两条实心矩形 —— 没有新图元类型,后端无需改动。
  for (const auto& kv : impl_->list_layouts) {
    const ListColumnLayout& lay = kv.second;
    if (!lay.has_scrollbar || lay.scrollbar_track.empty()) continue;
    overlay.fill_rect(lay.scrollbar_track, palette::kPanelStroke);
    ++impl_->counts.quads;
    if (!lay.scrollbar_thumb.empty()) {
      overlay.fill_rect(lay.scrollbar_thumb, palette::kAccent);
      ++impl_->counts.quads;
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

// ---------------------------------------------------------------- 侧栏(左列表 + 右列表)

SidebarIds build_sidebar_hud(Hud* hud, const HudLayoutConfig& cfg, DisplayInfo display,
                             const SidebarConfig& sidebar) {
  SidebarIds ids;
  if (!hud) return ids;
  hud->clear();

  const f32 d = display.density > 0.f ? display.density : 1.f;
  const f32 W = static_cast<f32>(display.width);
  const f32 H = static_cast<f32>(display.height);
  const f32 m = cfg.margin_dp * d;
  const f32 gap = cfg.gap_dp * d;
  const f32 row_h = cfg.row_height_dp * d;

  // ---- 顶部一行:返回按钮 + 两侧标题标签(标题文案由外部注入) ----
  Control back;
  back.kind = ControlKind::Button;
  back.label = sidebar.back_button_label.empty() ? std::string("back")
                                                 : sidebar.back_button_label;
  back.fill = palette::kAccentDim;
  back.stroke = palette::kPanelStroke;
  back.rect_px = Rect{m, m, std::min(cfg.button_width_dp * d, std::max(0.f, W - 2 * m)),
                      row_h};
  ids.back_button = hud->add(back);

  const f32 top_bar_bottom = m + row_h + gap;

  // ---- 两块面板:左右分列,中间留 gap,两侧留 margin,不越界 ----
  const f32 avail = std::max(0.f, W - 2 * m - gap);
  f32 panel_w = avail * std::clamp(cfg.list_panel_width_ratio, 0.05f, 0.49f);
  panel_w = std::min(panel_w, avail * 0.5f);  // 任何比例都不许挤掉另一块
  const f32 panel_h = std::max(0.f, H - top_bar_bottom - m);

  Control left;
  left.kind = ControlKind::Panel;
  left.fill = palette::kPanelFill;
  left.stroke = palette::kPanelStroke;
  left.draw_text = false;
  left.rect_px = Rect{m, top_bar_bottom, panel_w, panel_h};
  ids.left_panel = hud->add(left);

  Control right = left;
  right.rect_px = Rect{m + panel_w + gap, top_bar_bottom, panel_w, panel_h};
  ids.right_panel = hud->add(right);

  // ---- 标题:贴在各自面板顶部 ----
  if (!sidebar.left_title.empty()) {
    Control t;
    t.kind = ControlKind::Label;
    t.label = sidebar.left_title;
    t.text_color = palette::kTextDim;
    t.draw_text = true;
    t.rect_px = Rect{left.rect_px.x + cfg.panel_padding_dp * d, left.rect_px.y,
                     left.rect_px.w, cfg.list_row_height_dp * d};
    hud->add(t);
  }
  if (!sidebar.right_title.empty()) {
    Control t;
    t.kind = ControlKind::Label;
    t.label = sidebar.right_title;
    t.text_color = palette::kTextDim;
    t.draw_text = true;
    t.rect_px = Rect{right.rect_px.x + cfg.panel_padding_dp * d, right.rect_px.y,
                     right.rect_px.w, cfg.list_row_height_dp * d};
    hud->add(t);
  }

  // ---- 两个列表:条目文案全部由外部注入 ----
  ids.left_items = hud->add_list_items(ids.left_panel, sidebar.left_items);
  ids.right_items = hud->add_list_items(ids.right_panel, sidebar.right_items);

  // 回调:点击 → 通知外部(用列表内下标,不用控件 id,外部不必知道 HUD 内部编号)
  for (std::size_t i = 0; i < ids.left_items.size(); ++i) {
    if (auto* c = hud->find(ids.left_items[i])) {
      const std::uint32_t idx = static_cast<std::uint32_t>(i);
      auto cb = sidebar.on_left_item;
      if (cb) c->on_click = [cb, idx]() { cb(idx); };
    }
  }
  for (std::size_t i = 0; i < ids.right_items.size(); ++i) {
    if (auto* c = hud->find(ids.right_items[i])) {
      const std::uint32_t idx = static_cast<std::uint32_t>(i);
      auto cb = sidebar.on_right_item;
      if (cb) c->on_click = [cb, idx]() { cb(idx); };
    }
  }

  // 初始滚动位置(越界会被夹住,不崩)
  hud->layout_list_panel(ids.left_panel, sidebar.left_first_row, cfg);
  hud->layout_list_panel(ids.right_panel, sidebar.right_first_row, cfg);

  // 初始选中(空列表自然什么都不选中)
  if (!ids.left_items.empty()) hud->select_list_item(ids.left_panel, sidebar.left_selected);
  if (!ids.right_items.empty()) hud->select_list_item(ids.right_panel, sidebar.right_selected);
  return ids;
}

}  // namespace magicblocks::ui
