// MagicBlocks L4 —— 形状模板实现（设计 §12）
#include "magicblocks/features/stack/shape_template.h"

#include <algorithm>
#include <deque>
#include <map>

namespace magicblocks::features::stack {

namespace {

std::vector<Cell> generate_square(std::int32_t r) {
  std::vector<Cell> out;
  for (std::int32_t dz = -r; dz <= r; ++dz) {
    for (std::int32_t dx = -r; dx <= r; ++dx) {
      out.push_back(Cell{dx, 0, dz});
    }
  }
  return out;
}

std::vector<Cell> generate_circle(std::int32_t r) {
  std::vector<Cell> out;
  const std::int32_t rr = r * r;
  for (std::int32_t dz = -r; dz <= r; ++dz) {
    for (std::int32_t dx = -r; dx <= r; ++dx) {
      if (dx * dx + dz * dz <= rr) out.push_back(Cell{dx, 0, dz});
    }
  }
  return out;
}

std::vector<Cell> generate_cross(std::int32_t r) {
  std::vector<Cell> out;
  for (std::int32_t dz = -r; dz <= r; ++dz) {
    for (std::int32_t dx = -r; dx <= r; ++dx) {
      const bool on_x = (dz == 0) && (dx >= -r && dx <= r);
      const bool on_z = (dx == 0) && (dz >= -r && dz <= r);
      if (on_x || on_z) out.push_back(Cell{dx, 0, dz});
    }
  }
  return out;
}

std::vector<Cell> generate_sphere(std::int32_t r) {
  std::vector<Cell> out;
  const std::int32_t rr = r * r;
  for (std::int32_t dy = -r; dy <= r; ++dy) {
    for (std::int32_t dz = -r; dz <= r; ++dz) {
      for (std::int32_t dx = -r; dx <= r; ++dx) {
        if (dx * dx + dy * dy + dz * dz <= rr) out.push_back(Cell{dx, dy, dz});
      }
    }
  }
  return out;
}

}  // namespace

bool template_order_less(const Cell& a, const Cell& b) {
  // (dy, dz, dx) 升序。
  if (a.y != b.y) return a.y < b.y;
  if (a.z != b.z) return a.z < b.z;
  return a.x < b.x;
}

class ShapeTemplateSet final : public IShapeTemplate {
 public:
  ShapeTemplateSet() {
    // 预生成"展示视图"：每个内置模板用半径 2 展开一份，供 find()/list() 用。
    // 用 deque 而不是 vector 存 Cell：deque 的元素插入不失效，
    // 于是 ShapeTemplate::offsets 这个裸指针可以长期有效。
    for (std::size_t i = 0; i < kTemplateNameCount; ++i) {
      const std::string name = kTemplateNames[i];
      auto cells = generate(name, 2);
      if (!cells) continue;
      const std::vector<Cell> v = std::move(cells).take();
      store_.emplace_back();
      std::vector<Cell>& slot = store_.back();
      slot.assign(v.begin(), v.end());
      ShapeTemplate t;
      t.name = kTemplateNames[i];
      t.offsets = slot.data();
      t.count = slot.size();
      views_.push_back(t);
      names_.emplace_back(kTemplateNames[i]);
    }
  }

  StackResult<ShapeTemplate> find(const std::string& name) const override {
    for (const auto& t : views_) {
      if (name == t.name) return StackResult<ShapeTemplate>(t);
    }
    return StackResult<ShapeTemplate>(
        StackError{StackCode::UnknownTemplate, "未知模板 \"" + name + "\""});
  }

  std::vector<const ShapeTemplate*> list() const override {
    std::vector<const ShapeTemplate*> out;
    out.reserve(views_.size());
    for (const auto& t : views_) out.push_back(&t);
    return out;
  }

  StackResult<std::vector<Cell>> generate(const std::string& name, std::int32_t radius) const override {
    if (radius < 1) {
      return StackResult<std::vector<Cell>>(StackError{
          StackCode::OutOfGrid, "模板半径必须 >= 1，实际 " + std::to_string(radius)});
    }
    std::vector<Cell> cells;
    if (name == "square") {
      cells = generate_square(radius);
    } else if (name == "circle") {
      cells = generate_circle(radius);
    } else if (name == "cross") {
      cells = generate_cross(radius);
    } else if (name == "sphere") {
      cells = generate_sphere(radius);
    } else {
      return StackResult<std::vector<Cell>>(StackError{StackCode::UnknownTemplate,
                                                       "未知模板 \"" + name + "\""});
    }
    // 输出顺序契约：(dy, dz, dx) 升序。插入顺序固定 ⇒ IBlockSet::hash 稳定。
    std::sort(cells.begin(), cells.end(), template_order_less);
    // 去重（sphere 在 r>=1 时不会重复，但这条不变量不能靠"公式没写错"维持）。
    cells.erase(std::unique(cells.begin(), cells.end(),
                            [](const Cell& a, const Cell& b) { return a == b; }),
                cells.end());
    return StackResult<std::vector<Cell>>(std::move(cells));
  }

 private:
  std::vector<ShapeTemplate> views_;
  std::deque<std::vector<Cell>> store_;
  std::vector<std::string> names_;
};

std::unique_ptr<IShapeTemplate> create_shape_template() {
  return std::unique_ptr<IShapeTemplate>(new ShapeTemplateSet());
}

StackResult<std::vector<Cell>> expand_template(const GridSpec& grid, const Cell& anchor,
                                               const ShapeTemplate& tmpl) {
  if (tmpl.count == 0 || tmpl.offsets == nullptr) {
    return StackResult<std::vector<Cell>>(StackError{StackCode::EmptyStroke, "模板为空"});
  }
  std::vector<Cell> out;
  out.reserve(tmpl.count);
  for (std::size_t i = 0; i < tmpl.count; ++i) {
    const Cell& off = tmpl.offsets[i];
    const Cell abs{anchor.x + off.x, anchor.y + off.y, anchor.z + off.z};
    // 越界 → 立刻失败，detail 给出**第一个**越界偏移。不裁剪不跳过。
    if (abs.y < 0 || abs.y >= grid.layers) {
      return StackResult<std::vector<Cell>>(StackError{
          StackCode::TemplateOutOfGrid,
          "模板第 " + std::to_string(i) + " 个偏移 (" + std::to_string(off.x) + "," +
              std::to_string(off.y) + "," + std::to_string(off.z) + ") 展开后 y=" +
              std::to_string(abs.y) + " 越出 [0, " + std::to_string(grid.layers) + ")"});
    }
    if (abs.x < -grid.half_extent || abs.x >= grid.half_extent) {
      return StackResult<std::vector<Cell>>(StackError{
          StackCode::TemplateOutOfGrid,
          "模板第 " + std::to_string(i) + " 个偏移 (" + std::to_string(off.x) + "," +
              std::to_string(off.y) + "," + std::to_string(off.z) + ") 展开后 x=" +
              std::to_string(abs.x) + " 越出 [-" + std::to_string(grid.half_extent) + ", " +
              std::to_string(grid.half_extent) + ")"});
    }
    if (abs.z < -grid.half_extent || abs.z >= grid.half_extent) {
      return StackResult<std::vector<Cell>>(StackError{
          StackCode::TemplateOutOfGrid,
          "模板第 " + std::to_string(i) + " 个偏移 (" + std::to_string(off.x) + "," +
              std::to_string(off.y) + "," + std::to_string(off.z) + ") 展开后 z=" +
              std::to_string(abs.z) + " 越出 [-" + std::to_string(grid.half_extent) + ", " +
              std::to_string(grid.half_extent) + ")"});
    }
    out.push_back(abs);
  }
  return StackResult<std::vector<Cell>>(std::move(out));
}

}  // namespace magicblocks::features::stack
