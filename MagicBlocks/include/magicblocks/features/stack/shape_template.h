#pragma once
// MagicBlocks L4 —— 魔法叠叠乐：形状模板（设计 §12）
//
// 模板 = 一组**相对锚点的整数格偏移**，不是一张网格。
//   · 全部整数运算（无浮点判据）→ 跨平台逐位可复算；
//   · 偏移按 (dy, dz, dx) 升序输出 → 插入顺序固定 → IBlockSet::hash 稳定；
//   · 只出现在 features/stack/，不进 primitives/（G7 门禁会查）。
//
// 与列高互斥：`--column-height` 与 `--template` 同时给是**参数错误**。
// 两种堆叠规则不能混 —— 混起来的结果没有唯一解释，属于必须响的那类失败。

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "magicblocks/features/stack/block_set.h"
#include "magicblocks/features/stack/stack_types.h"

namespace magicblocks::features::stack {

// 模板数据。offsets 指向稳定的常量存储（内置模板是编译期常量表）。
struct ShapeTemplate {
  const char* name = "";
  const Cell* offsets = nullptr;
  std::size_t count = 0;
};

// 输出顺序的判据：(dy, dz, dx) 字典序升序。Cell::operator< 已经是这个序
// （见 stack_types.h），所以直接用即可 —— 但这里显式再写一遍比较函数，
// 免得将来有人改了 Cell 的序却没人发现模板输出顺序跟着变了。
bool template_order_less(const Cell& a, const Cell& b);

class IShapeTemplate {
 public:
  IShapeTemplate() = default;
  IShapeTemplate(const IShapeTemplate&) = delete;
  IShapeTemplate& operator=(const IShapeTemplate&) = delete;
  virtual ~IShapeTemplate() = default;

  // 按名字找模板。未知名字 → StackCode::UnknownTemplate（不做模糊匹配、
  // 不给默认值 —— 打错字必须响）。
  virtual StackResult<ShapeTemplate> find(const std::string& name) const = 0;

  // 稳定顺序（按注册序，不回退到 unordered_map 的遍历序）。
  virtual std::vector<const ShapeTemplate*> list() const = 0;

  // 按半径生成偏移集（整数判据，见设计 §12.2）。
  // radius < 1 → ColorOutOfRange 之外的明确错误（OutOfGrid）。
  virtual StackResult<std::vector<Cell>> generate(const std::string& name, std::int32_t radius) const = 0;
};

std::unique_ptr<IShapeTemplate> create_shape_template();

// 把一个模板以 `anchor` 为锚点展开成绝对格集合。
// 任一格越出 GridSpec → StackCode::TemplateOutOfGrid，detail 给出**第一个**
// 越界的偏移；**不裁剪、不跳过**（裁剪会让"画出来的形状"与"要求的形状"不同，
// 而且没有任何地方会报错）。
StackResult<std::vector<Cell>> expand_template(const GridSpec& grid, const Cell& anchor,
                                               const ShapeTemplate& tmpl);

// 模板展开用的分量列表（供 CLI 参数校验：--template 与 --column-height 互斥）。
inline constexpr const char* kTemplateNames[] = {"square", "circle", "cross", "sphere"};
inline constexpr std::size_t kTemplateNameCount = sizeof(kTemplateNames) / sizeof(kTemplateNames[0]);

}  // namespace magicblocks::features::stack
