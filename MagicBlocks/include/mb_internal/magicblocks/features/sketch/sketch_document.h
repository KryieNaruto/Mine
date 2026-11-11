// MagicBlocks L4 —— 魔法涂鸦：文档模型（F-SK 基座）
//
// ============================ 层归属 ============================
// 这是 **L4 玩法层**。整个 MagicBlocks 里只有 features/ 可以出现玩法语义
// （"涂鸦""笔画"这类词）。L0-L3 一律不得引用本文件。
// 机械门控见 tools/check_layer_purity.py。
//
// ============================ 为什么存点不存网格 ============================
// 文档存的是**逻辑采样点**，不是三角化后的 MeshData。理由：
//   1. 撤销/重做要能精确回到"之前那几笔"，存网格则每次重三角化结果
//      哪怕浮点有一点抖动也算不同状态，比较和去重都失效；
//   2. 笔宽/颜色是**可后编辑参数**。存网格等于把参数烘焙死了，
//      改一次笔宽就得重录整笔；
//   3. 涂鸦的笔画数远小于三角形数，存点让文档小两个数量级。
// 因此：文档 = 真源；网格 = 派生缓存（可随时丢弃重建）。
// 这条"真源 vs 派生"的分界是本模块的核心约束，实现里任何缓存都必须
// 能由文档完全重建，否则就是隐形的第二真源。
//
// ============================ 双表示（Q-SK-2 用户裁定 / R-SK-12）============================
// 用户明确要求：**后续的笔画路径分为路径，和路径包围的布尔区域，需要预留。**
// 同一串采样点有两种几何解读：
//
//   PathStyle::Ribbon  —— "路径"：把折线按笔宽加粗成带状（描边）。
//   PathStyle::Region  —— "路径包围的布尔区域"：把闭合折线的**内部**填满（填充）。
//
// 这两个概念的设计 §6.3.1(c) 明确要求属 **L3 primitives**（`OutlineSet` 必须
// 能标注自己是怎么来的）。本文件因此**不再自定义 PathStyle/RegionOp**，而是
// 直接复用 `primitives::PathStyle` / `primitives::RegionOp`：
// 类型只有一份，L4 只负责"选哪一种"，不写玩法分支。
// 未实现的分支（Difference/Intersect/Xor、未闭合 Region）会在调用时
// **显式报 Unsupported / NotClosed**，绝不静默退化到 Ribbon（D8：失败要响）。
//
// ============================ 不做的事 ============================
//   - 不做笔刷纹理 / 压感 / 倾斜（spec §15 out-of-scope）
//   - 不做笔画内插样条（采样点即几何）
//   - 不做多画布 / 图层合成
#pragma once

#include <cstdint>
#include <vector>

#include "mb_internal/magicblocks/primitives/mesh_factory.h"
#include "mb_internal/magicblocks/primitives/types.h"

namespace magicblocks::features::sketch {

using primitives::PathStyle;
using primitives::RegionOp;
using primitives::Vec2;
using primitives::Vec3;

// 笔画归属与身份。
//
// stroke_id **单调递增且永不复用**（D-SK-3）。理由：撤销后重画会拿到新 id，
// 于是"撤销栈里的 id"和"画布上的 id"永远不会撞车 —— 如果用可复用的 id，
// 重做时会出现"新笔画套用旧 id"的幽灵状态，而且只在特定撤销序列下复现。
struct StrokeId {
  std::uint64_t value = 0;
  bool valid() const { return value != 0; }
  friend bool operator==(StrokeId a, StrokeId b) { return a.value == b.value; }
  friend bool operator!=(StrokeId a, StrokeId b) { return a.value != b.value; }
};

// 一笔涂鸦。采样点用**画布平面 2D 世界坐标**，不是屏幕像素（D-SK-2）：
// 像素坐标一旦改变窗口大小就失效，而文档要能跨分辨率回放。
struct Stroke {
  StrokeId id{};
  std::vector<Vec2> points;   // 采样点（画布平面世界坐标）
  float width = 0.02f;        // 笔宽（世界单位，直径）
  primitives::Color4 color{1.0f, 1.0f, 1.0f, 1.0f};
  bool round_cap = true;
  PathStyle style = PathStyle::Ribbon;
  bool closed = false;        // Region 用：折线是否首尾相接

  // 采样点数上限之外的点会被丢弃，用于防御异常长笔画撑爆文档。
  // 0 表示不限。
  std::uint32_t max_points = 4096;
};

// 撤销/重做的一项。
//
// 用"整笔增删"而不是"逐点增删"作为粒度：涂鸦的撤销单位天然是一笔，
// 逐点撤销会让用户按 200 次 Ctrl+Z 才能撤掉一条线。
struct SketchEdit {
  enum class Kind : std::uint8_t {
    AddStroke,     // 加了一笔
    RemoveStroke,  // 删了一笔
    Recolor,       // 改了某一笔的颜色（A10：不动几何）
  };
  Kind kind = Kind::AddStroke;
  Stroke stroke{};   // AddStroke 存被加的；RemoveStroke 存被删的（供重做）
  // Recolor 用：改之前的颜色（撤销时恢复）。新颜色在 `stroke.color` 里。
  primitives::Color4 previous_color{1.0f, 1.0f, 1.0f, 1.0f};
  std::size_t index = 0;  // 在 strokes 里的位置（重做时插回原处，保持顺序稳定）
};

// 涂鸦文档。
//
// 刻意做成**值语义 + 可平凡拷贝**：快照 = 拷贝一份。这换来回放验证的
// 简单（两份文档逐字段比较即可），代价是内存翻倍 —— 但一笔涂鸦的点数
// 在 KB 级，不值得为省这点内存引入写时复制。
class SketchDocument {
 public:
  SketchDocument() = default;

  // ---- 笔画 ----
  const std::vector<Stroke>& strokes() const { return strokes_; }
  std::size_t stroke_count() const { return strokes_.size(); }
  // 按 id 查笔画；找不到返回 nullptr（不抛异常 —— 命令流可能带旧 id）。
  const Stroke* find(StrokeId id) const;

  // 加一笔。返回新分配的 id（单调递增，永不复用）。
  // points 为空或点数不足 → 返回无效 id 且**不改变文档**。
  StrokeId add_stroke(Stroke stroke);

  // 按 id 删一笔。成功返回 true 并压入撤销栈。
  bool remove_stroke(StrokeId id);

  // 改一笔的颜色（R-SK-9 / A10）。
  //
  // **只改 color 字段**：不 touch points / width / style，因此
  // `geometry_hash()` 不变、挤出网格逐字节不变。找不到 id → 返回 false
  // 且不改文档（不静默忽略成"成功"）。
  // 颜色相同 → 返回 true 但**不压栈**（空操作不该占用一次撤销）。
  bool set_stroke_color(StrokeId id, primitives::Color4 color);

  // ---- 撤销 / 重做 ----
  bool can_undo() const { return !undo_.empty(); }
  bool can_redo() const { return !redo_.empty(); }
  // 撤销一步。返回是否真的撤销了。
  bool undo();
  bool redo();
  void clear_history();

  // ---- 快照（回放校验用）----
  // 文档指纹：对"笔画 id + 点数 + 每点坐标 + 参数 + 颜色"按顺序做 64 位混合。
  // 不含任何指针/地址，因此跨进程稳定；浮点按位比较（不是 epsilon），
  // 因为回放要求**逐位一致**而不是"看起来一样"。
  std::uint64_t hash() const;

  // **几何**指纹：hash() 去掉颜色分量。
  // A10 用它断言"改色不影响几何"：两次改色前后这个值必须一致，
  // 而 hash() 必须变化（否则说明改色根本没生效 —— 两个方向都要钉住）。
  std::uint64_t geometry_hash() const;

  // 清空（同时清历史）。
  void clear();

  // ---- Region 语义约束（能力边界）----
  // 委托给 L3：`primitives::supports_region_op`。L4 不自己维护第二份能力表，
  // 否则"L3 说支持、L4 说不支持"这类分叉迟早出现。
  static bool supports_region_op(RegionOp op);

  // 下一笔将拿到的 id（供命令流预分配/回放对齐）。
  std::uint64_t next_stroke_id() const { return next_id_; }
  void set_next_stroke_id(std::uint64_t v) { next_id_ = v; }

 private:
  void push_edit(SketchEdit edit);

  std::vector<Stroke> strokes_;
  std::vector<SketchEdit> undo_;
  std::vector<SketchEdit> redo_;
  std::uint64_t next_id_ = 1;  // 0 保留给"无效 id"
};

// 双表示的可选性检查：给定笔画能否用指定 style 生成几何。
// 返回 false 时不产生任何几何，调用方必须**显式处理**（报错或跳过），
// 不允许当成空轮廓静默继续。
//
// 实现委托给 L3 的 `make_outline`：检查逻辑与几何生成必须是**同一份**，
// 两处各写一套就会出现"检查说行、生成说不行"。
struct StyleCheck {
  bool ok = false;
  const char* reason = "";  // ok=false 时给出人类可读原因（静态字符串，无分配）
};
StyleCheck check_style(const Stroke& stroke, float close_tolerance = 1e-3f);

}  // namespace magicblocks::features::sketch
