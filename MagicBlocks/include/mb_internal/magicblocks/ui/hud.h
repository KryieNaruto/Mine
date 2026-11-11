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

#include "mb_internal/magicblocks/device/events.h"
#include "mb_internal/magicblocks/ui/overlay.h"
#include "mb_internal/magicblocks/ui/types.h"

namespace magicblocks::ui {

enum class ControlKind : std::uint8_t {
  Panel = 0,     // 容器:填色 + 1px 边框,可接收点击(先于子控件)
  Button = 1,    // 点击回调
  Label = 2,     // 不可交互,只画文本
  Slider = 3,    // 水平滑条(值域 [0,1] 或自定义)
  Swatch = 4,    // 色板单格(点选即回调颜色)
  // 列表条目:可点击、可选中。**通用控件**,不知道自己列的是什么条目 ——
  // 条目文案/数据由外部注入(`Control::label` / `impl data`),UI 层不出现任何
  // 具体业务名(spec §2 命名纪律)。
  ListItem = 5,
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
  bool selected = false;  // ListItem 用:选中态由调用方按业务决定并写入
  f32 value = 0.f;        // Slider 当前值 [0,1]
  // ListItem 用:条目在列表里的序号(与 `selected` 一起构成"可见的高亮"视觉;
  // 具体语义由外部决定,UI 层只负责画)
  std::uint32_t index = 0;
  std::uint32_t index_total = 0;  // 列表总条数(用于绘制滚动提示条)

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
  // 左右列表面板的宽度(占屏宽的比例)。两块面板 + 中间留白 + 两侧边距必须
  // 能同时放下;放不下时 layout_sidebar_lists() 会按比例收缩(不越界)。
  f32 list_panel_width_ratio = 0.22f;
  // 列表条目行高(dp)。
  f32 list_row_height_dp = 26.f;
};

// 列表面板的一次布局结果。**刻度坐标系统一用"行"(row)**,便于判断
// "哪些条目落在可见区内"和"滚动偏移是否合法"。
struct ListColumnLayout {
  Rect panel{};             // 面板外框(像素)
  Rect viewport{};          // 可见区(面板内边距之后,像素)
  f32 row_height_px = 0.f;  // 单行像素高
  std::uint32_t first_visible_row = 0;  // viewport 顶部对应的行号
  std::uint32_t visible_rows = 0;       // viewport 一次能放下几行
  std::uint32_t total_rows = 0;         // 列表总行数(含不可见的)
  bool scrollable = false;              // total_rows > visible_rows
  bool has_scrollbar = false;           // 有滚动条图元(见 Hud::scrollbar_rect)
  Rect scrollbar_track{};
  Rect scrollbar_thumb{};
  std::uint32_t clamped_first_row = 0;  // 实际生效的起始行(已夹住)
  // 本面板拥有的条目 id(按加入顺序,**不是**按行截断后)。命中裁剪与
  // visible_list_items() 都靠它区分"这块面板的条目"和"另一块面板的条目"。
  std::vector<std::uint32_t> item_ids;
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

  // ---------------------------------------------------------- 左右列表面板(spec: UI 布局 v1)
  // 把面板 `panel_id` 的所有 `ListItem` 子控件排成**竖列表**:
  //   * 条目从 `first_row` 行开始,按 `list_row_height_dp` 逐行往下排;
  //   * 每行高度固定,超出 viewport 的条目不画、不可命中(裁剪,不越界);
  //   * 条目 `label` 由调用方注入(UI 层不出现任何业务/玩法名);
  //   * `first_row` 会被夹到 [0, total-visible],越界的滚动值不会把列表滚出面板;
  //   * `scrollable`(total > visible)时输出滚动条轨道 + 滑块几何,供绘制可见性提示。
  //
  // **行是离散的**:`first_row` 按整行滚动。做连续滚动需要在本函数外再乘一个
  // 像素偏移;本轮只做"最小处理",所以保持整行 —— 这样"滚动后可见条目集合"
  // 在数值上是确定的,测试才能断言。
  ListColumnLayout layout_list_panel(std::uint32_t panel_id, std::uint32_t first_row,
                                     const HudLayoutConfig& cfg);

  // 取上一次 layout_list_panel 的结果(找不到该面板时返回空结果)。
  ListColumnLayout list_layout(std::uint32_t panel_id) const;
  // 面板内当前**可见**的 ListItem id(自上而下)。用于测试与命中核对。
  std::vector<std::uint32_t> visible_list_items(std::uint32_t panel_id) const;

  // 列表条目 id 的分配辅助:把 n 个 ListItem 作为面板子控件追加进去,
  // labels 由调用方给出,返回新建的 id(按顺序)。
  std::vector<std::uint32_t> add_list_items(std::uint32_t panel_id,
                                            const std::vector<std::string>& labels);

  // 选中态:把该面板内 index == selected_index 的条目标为 selected,其余清除。
  // 返回是否真的找到了这样一条(找不到返回 false,不静默)。
  bool select_list_item(std::uint32_t panel_id, std::uint32_t selected_index);

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

// 侧栏列表配置:**条目数据由外部注入**(spec §2:UI 层不出现玩法/模型名)。
//
//   left_items  —— 左侧列表的条目文案(测试模型等;文案原样绘制,不参与逻辑)
//   right_items —— 右侧列表的条目文案
//
// UI 层只把它们当字符串画出来并回调 "第几条被点了";点完之后做什么(路由到哪个
// 玩法、切哪个模型)**全部由调用方通过回调决定**,Hud 不认识任何业务概念。
struct SidebarConfig {
  std::vector<std::string> left_items;
  std::vector<std::string> right_items;
  std::string left_title;
  std::string right_title;
  // 顶部返回按钮文案(空 = back;文案由外部注入)
  std::string back_button_label;
  // 点中左侧第 index 条 / 右侧第 index 条。index 是"列表内下标"(不是控件 id)。
  std::function<void(std::uint32_t)> on_left_item;
  std::function<void(std::uint32_t)> on_right_item;
  // 两侧列表当前选中下标(默认 0 = 第一条;空列表时不生效)
  std::uint32_t left_selected = 0;
  std::uint32_t right_selected = 0;
  // 初始滚动起始行(0 = 从头)。越界会被夹住。
  std::uint32_t left_first_row = 0;
  std::uint32_t right_first_row = 0;
};

// 侧栏构图结果:HUD 内部用 id 表示的两块面板 + 顶部返回按钮。
struct SidebarIds {
  std::uint32_t left_panel = 0;
  std::uint32_t right_panel = 0;
  std::uint32_t back_button = 0;
  std::vector<std::uint32_t> left_items;
  std::vector<std::uint32_t> right_items;
};

// 在已清空的 HUD 上构建"左列表 + 右列表 + 顶部返回按钮"。
//
// 布局约束(硬):
//   * 两块面板左右分列,中间留 gap,两侧留 margin —— 任何 dpr/尺寸下都不越界;
//   * 面板高度取顶部按钮行(含 gap)以下到屏幕底部减去 margin;
//   * 屏幕太窄时按 `list_panel_width_ratio` 收缩,宽度不足一行文字也不越界。
//
// 返回各控件 id(便于测试与外部改回调)。`hud` 为空则返回全 0。
SidebarIds build_sidebar_hud(Hud* hud, const HudLayoutConfig& cfg, DisplayInfo display,
                             const SidebarConfig& sidebar);

}  // namespace magicblocks::ui
