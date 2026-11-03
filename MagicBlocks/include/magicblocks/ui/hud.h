// MagicBlocks UI 消费层 —— HUD 最小控件集(spec §8、§6 G3)
//
// spec §8 选型:「自绘最小控件集(按钮/面板/滑条/色板/文字)」,渲染接口统一为
// `IOverlay2D` + 文本渲染,未来可替换。**不走平台原生控件**(原生控件会引入
// 一次系统合成层 = 额外一次上屏合成)。
//
// 设计要点:
//   - 控件树是**数据**(节点 + 布局),不是立即模式调用序列。原因:① 输入命中
//     需要一份稳定的几何(§7 ①层),② golden/录制要求每帧图元序列可复现,
//     ③ 布局抖动(DPR/旋转)只重算一次,不必每帧重来。
//   - 绘制是**每帧遍历**:`Hud::draw(IOverlay2D&)` 按树顺序 push 图元,后端合批。
//   - 本轮不做:动画/转场、换肤、系统键盘(spec §9 明确不做)。
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "magicblocks/device/events.h"
#include "magicblocks/ui/overlay.h"
#include "magicblocks/ui/types.h"

namespace magicblocks::ui {

enum class ControlKind : std::uint8_t {
  Panel = 0,     // 容器:填色 + 1px 边框,可接收点击(先于子控件)
  Button = 1,    // 点击回调
  Label = 2,     // 不可交互,只画文本
  Slider = 3,    // 水平滑条(值域 [0,1] 或自定义)
  Swatch = 4,    // 色板单格(点选即回调颜色)
};

const char* to_string(ControlKind kind);

// 一个控件节点。几何是**绝对像素矩形**(布局阶段算好),命中判定直接用
// `Rect::contains`,不做矩阵变换 —— 命中路径上没有浮点矩阵开销。
struct Control {
  std::uint32_t id = 0;            // 稳定 ID(构建时分配;测试与状态存引用它)
  ControlKind kind = ControlKind::Panel;
  std::string label;
  Rect rect_px{};
  bool enabled = true;

  // 视觉
  Rgba8 fill{0, 0, 0, 0};
  Rgba8 stroke{0, 0, 0, 0};
  Rgba8 text_color = palette::kText;
  Rgba8 swatch{255, 255, 255, 255};  // Swatch 用
  bool draw_text = true;             // 无字体图集时由实现决定是否真的画字

  // 交互状态(由 Hud 在输入阶段写入,绘制阶段只读 —— 单帧内无二义)
  bool pressed = false;
  bool hovered = false;
  f32 value = 0.f;  // Slider 当前值 [0,1]

  std::function<void()> on_click;
  std::function<void(f32)> on_value_changed;
};

// 布局配置:HUD 尺寸以 **dp** 定义(spec §8 跨 DPR),布局时乘 density 得像素。
struct HudLayoutConfig {
  f32 margin_dp = 12.f;
  f32 row_height_dp = 44.f;
  f32 button_width_dp = 96.f;
  f32 gap_dp = 8.f;
  f32 panel_padding_dp = 8.f;
};

// HUD:控件树 + 命中查询 + 绘制。
class Hud {
 public:
  Hud();
  ~Hud();

  Hud(const Hud&) = delete;
  Hud& operator=(const Hud&) = delete;

  // ---------------------------------------------------------- 构建
  // 清空整棵树(切屏/重建 HUD 时用;控件 id 从清零重新分配)。
  void clear();

  // 追加一个控件。返回它的 id。父节点只影响绘制顺序(子控件在父之后绘制),
  // 几何仍由调用方按绝对像素给出 —— 布局算法本轮故意保持"线性加一行",不做自动布局。
  std::uint32_t add(const Control& control);

  // 取控件(可按 id 修改视觉/回调)。找不到返回 nullptr。
  Control* find(std::uint32_t id);
  const Control* find(std::uint32_t id) const;

  std::size_t size() const;
  const std::vector<Control>& controls() const;

  // ---------------------------------------------------------- 布局
  // 按 dp 配置把控件重新排布在给定屏幕内(本轮:顶部一行 + 底部面板)。
  // 这是**唯一的布局入口**:density/尺寸变化时调一次,不在每帧调。
  void relayout(const HudLayoutConfig& cfg, DisplayInfo display, f32 top_bar_dp,
                f32 bottom_panel_dp);

  // ---------------------------------------------------------- 输入(①层)
  // HUD 命中:命中返回 true 并更新 pressed/value,调用方据此**消费事件,不再下发**。
  // 只处理 PointerDown/Move/Up/Cancel + 其它事件一律返回 false(不消费)。
  bool on_input(const device::InputEvent& ev);

  // ---------------------------------------------------------- 绘制
  // 每帧遍历树 push 图元(顺序固定 → 命令流可复现)。
  void draw(stub::IOverlay2D& overlay) const;

  // 本帧绘制统计(回传给 PerfGovernor 做 G3 门槛判定)。
  struct DrawCounts {
    std::uint32_t quads = 0;
    std::uint32_t strokes = 0;
    std::uint32_t text_runs = 0;
  };
  DrawCounts draw_counts() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// 把控件列表灌进 HUD 的便捷构建器(返回下一个可用 id 供测试断言确定性)。
std::uint32_t build_minimal_hud(Hud* hud, const HudLayoutConfig& cfg, DisplayInfo display);

}  // namespace magicblocks::ui
