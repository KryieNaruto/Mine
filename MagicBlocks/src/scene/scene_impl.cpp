// MagicBlocks SDK —— L2 S1 场景实现(节点树 / TRS / 世界包围盒 / 快照 / 增量)
//
// 纯 CPU 逻辑:零 Vulkan、零时钟、零显存(D13 / G3 / G4)。
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <unordered_map>
#include <vector>

#include "scene/scene_internal.h"

namespace magicblocks::scene {
namespace {

// ---------------------------------------------------------------- 小矩阵工具

// TRS 组合成 4x4(行主序)。只在本文件内用,不对外暴露。
struct Mat4 {
  float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

  static Mat4 identity() { return Mat4{}; }

  static Mat4 from_trs(const Transform& t) {
    // 四元数 (x,y,z,w) → 3x3 旋转,再左乘缩放、右乘平移。
    const float x = t.rotation[0];
    const float y = t.rotation[1];
    const float z = t.rotation[2];
    const float w = t.rotation[3];

    const float xx = x * x, yy = y * y, zz = z * z;
    const float xy = x * y, xz = x * z, yz = y * z;
    const float wx = w * x, wy = w * y, wz = w * z;

    float r[3][3];
    r[0][0] = 1.0f - 2.0f * (yy + zz);
    r[0][1] = 2.0f * (xy - wz);
    r[0][2] = 2.0f * (xz + wy);
    r[1][0] = 2.0f * (xy + wz);
    r[1][1] = 1.0f - 2.0f * (xx + zz);
    r[1][2] = 2.0f * (yz - wx);
    r[2][0] = 2.0f * (xz - wy);
    r[2][1] = 2.0f * (yz + wx);
    r[2][2] = 1.0f - 2.0f * (xx + yy);

    Mat4 out{};
    for (int row = 0; row < 3; ++row) {
      for (int col = 0; col < 3; ++col) {
        out.m[row * 4 + col] = r[row][col] * t.scale[col];
      }
    }
    out.m[0 * 4 + 3] = t.translation[0];
    out.m[1 * 4 + 3] = t.translation[1];
    out.m[2 * 4 + 3] = t.translation[2];
    out.m[3 * 4 + 3] = 1.0f;
    return out;
  }

  Mat4 mul(const Mat4& b) const {
    Mat4 out{};
    for (int row = 0; row < 4; ++row) {
      for (int col = 0; col < 4; ++col) {
        float acc = 0.0f;
        for (int k = 0; k < 4; ++k) acc += m[row * 4 + k] * b.m[k * 4 + col];
        out.m[row * 4 + col] = acc;
      }
    }
    return out;
  }

  void transform_point(const float in[3], float out[3]) const {
    // 与 from_trs / mul 一致的**行主序**:第 r 行第 c 列在 m[r*4+c],
    // 因此 out[r] = Σ_c m[r*4+c] * in[c] + m[r*4+3]。
    // (早期版本按列主序读取,和行主序的写入互为转置,平移与缩放全部错位。)
    for (int row = 0; row < 3; ++row) {
      out[row] = m[row * 4 + 0] * in[0] + m[row * 4 + 1] * in[1] + m[row * 4 + 2] * in[2] +
                 m[row * 4 + 3];
    }
  }

  // 取 3x3 线性部分的**第 col 列**(行主序)。只用于把局部 AABB 变换成世界 AABB,
  // 因此访问步长是 4(跨行),与 m[r*4+c] 的行主序布局一致。
  void basis(int col, float out[3]) const {
    out[0] = m[0 * 4 + col];
    out[1] = m[1 * 4 + col];
    out[2] = m[2 * 4 + col];
  }
};

void merge_aabb(Aabb* into, const Aabb& other) {
  if (other.empty) return;
  if (into->empty) {
    *into = other;
    return;
  }
  for (int k = 0; k < 3; ++k) {
    into->min[k] = std::min(into->min[k], other.min[k]);
    into->max[k] = std::max(into->max[k], other.max[k]);
  }
}

// 把局部 AABB 变换到世界空间(取变换后 8 个角点的 AABB)。
Aabb transform_aabb(const Mat4& world, const Aabb& local) {
  if (local.empty) return local;
  Aabb out{};
  out.empty = false;
  bool first = true;
  for (int corner = 0; corner < 8; ++corner) {
    const float p[3] = {
        (corner & 1) ? local.max[0] : local.min[0],
        (corner & 2) ? local.max[1] : local.min[1],
        (corner & 4) ? local.max[2] : local.min[2],
    };
    float q[3];
    world.transform_point(p, q);
    for (int k = 0; k < 3; ++k) {
      if (first) {
        out.min[k] = q[k];
        out.max[k] = q[k];
      } else {
        out.min[k] = std::min(out.min[k], q[k]);
        out.max[k] = std::max(out.max[k], q[k]);
      }
    }
    first = false;
  }
  return out;
}

BoundingSphere sphere_of(const Aabb& box) {
  BoundingSphere s{};
  if (box.empty) return s;
  for (int k = 0; k < 3; ++k) s.center[k] = 0.5f * (box.min[k] + box.max[k]);
  const float dx = box.max[0] - s.center[0];
  const float dy = box.max[1] - s.center[1];
  const float dz = box.max[2] - s.center[2];
  s.radius = std::sqrt(dx * dx + dy * dy + dz * dz);
  s.empty = false;
  return s;
}

// ---------------------------------------------------------------- 场景

class SceneImpl final : public IScene {
 public:
  SceneImpl() = default;

  // ------------------------------------------------------------ 增删改

  Result<NodeId> add_node(NodeId parent, const std::string& name) override {
    if (parent != kInvalidNodeId && nodes_.find(parent) == nodes_.end()) {
      return Status::Invalid("IScene::add_node: parent id " + std::to_string(parent) + " 不存在");
    }
    const NodeId id = next_id_++;
    Node node;
    node.snapshot.id = id;
    node.snapshot.parent = parent;
    node.snapshot.name = name;
    nodes_.emplace(id, std::move(node));
    if (parent != kInvalidNodeId) nodes_[parent].children.push_back(id);
    roots_.push_back(id);
    // id 单调递增 → 追加到末尾即保持升序;但仍显式排序,防御将来改动。
    std::sort(roots_.begin(), roots_.end());
    record_change(NodeOp::Added, id);
    return id;
  }

  bool remove_node(NodeId id) override {
    auto it = nodes_.find(id);
    if (it == nodes_.end()) return false;

    // 先收集整棵子树(要先拷出来,erase 会让迭代器失效)。
    std::vector<NodeId> doomed;
    collect_subtree(id, &doomed);

    const NodeId parent = it->second.snapshot.parent;
    if (parent != kInvalidNodeId) {
      auto pit = nodes_.find(parent);
      if (pit != nodes_.end()) {
        auto& kids = pit->second.children;
        kids.erase(std::remove(kids.begin(), kids.end(), id), kids.end());
      }
    } else {
      roots_.erase(std::remove(roots_.begin(), roots_.end(), id), roots_.end());
    }

    for (NodeId dead : doomed) {
      nodes_.erase(dead);
      record_change(NodeOp::Removed, dead);
    }
    return true;
  }

  bool set_transform(NodeId id, const Transform& transform) override {
    auto it = nodes_.find(id);
    if (it == nodes_.end()) return false;
    it->second.snapshot.transform = transform;
    record_change(NodeOp::Updated, id);
    return true;
  }

  bool set_local_bounds(NodeId id, const Aabb& bounds) override {
    auto it = nodes_.find(id);
    if (it == nodes_.end()) return false;
    it->second.snapshot.local_bounds = bounds;
    record_change(NodeOp::Updated, id);
    return true;
  }

  bool set_visible(NodeId id, bool visible) override {
    auto it = nodes_.find(id);
    if (it == nodes_.end()) return false;
    it->second.snapshot.visible = visible;
    record_change(NodeOp::Updated, id);
    return true;
  }

  bool reparent(NodeId id, NodeId new_parent) override {
    auto it = nodes_.find(id);
    if (it == nodes_.end()) return false;
    if (new_parent != kInvalidNodeId && nodes_.find(new_parent) == nodes_.end()) return false;
    if (new_parent == id) return false;
    // 环检测:新父不能在自己的子树里。
    if (new_parent != kInvalidNodeId && is_descendant(new_parent, id)) return false;

    const NodeId old_parent = it->second.snapshot.parent;
    if (old_parent == new_parent) return true;

    if (old_parent != kInvalidNodeId) {
      auto& kids = nodes_[old_parent].children;
      kids.erase(std::remove(kids.begin(), kids.end(), id), kids.end());
    } else {
      roots_.erase(std::remove(roots_.begin(), roots_.end(), id), roots_.end());
    }

    it->second.snapshot.parent = new_parent;
    if (new_parent != kInvalidNodeId) {
      nodes_[new_parent].children.push_back(id);
    } else {
      roots_.push_back(id);
      std::sort(roots_.begin(), roots_.end());
    }
    record_change(NodeOp::Updated, id);
    return true;
  }

  // ------------------------------------------------------------ 查询

  bool has_node(NodeId id) const override { return nodes_.find(id) != nodes_.end(); }

  std::size_t node_count() const override { return nodes_.size(); }

  Aabb local_bounds(NodeId id) const override {
    auto it = nodes_.find(id);
    if (it == nodes_.end()) return Aabb{};
    return it->second.snapshot.local_bounds;
  }

  Aabb world_bounds(NodeId id) const override {
    if (nodes_.find(id) == nodes_.end()) return Aabb{};
    return compute_world_bounds(id, /*include_invisible_children=*/false);
  }

  BoundingSphere world_bounding_sphere(NodeId id) const override {
    return sphere_of(world_bounds(id));
  }

  bool visible(NodeId id) const override {
    auto it = nodes_.find(id);
    if (it == nodes_.end()) return false;
    return it->second.snapshot.visible;
  }

  bool effectively_visible(NodeId id) const override {
    NodeId cur = id;
    // 沿 parent 链上溯;祖先链有环也只可能来自 restore 未拦截,这里加步数上限兜底。
    std::size_t guard = 0;
    while (cur != kInvalidNodeId && guard++ <= nodes_.size()) {
      auto it = nodes_.find(cur);
      if (it == nodes_.end()) return false;
      if (!it->second.snapshot.visible) return false;
      cur = it->second.snapshot.parent;
    }
    return nodes_.find(id) != nodes_.end();
  }

  NodeId parent_of(NodeId id) const override {
    auto it = nodes_.find(id);
    if (it == nodes_.end()) return kInvalidNodeId;
    return it->second.snapshot.parent;
  }

  // ------------------------------------------------------------ 快照

  SceneSnapshot snapshot() const override {
    SceneSnapshot out;
    out.nodes.reserve(nodes_.size());
    for (const auto& kv : nodes_) out.nodes.push_back(kv.second.snapshot);
    // std::map 已按 id 升序,这里显式再排一次把契约写死。
    std::sort(out.nodes.begin(), out.nodes.end(),
              [](const NodeSnapshot& a, const NodeSnapshot& b) { return a.id < b.id; });
    return out;
  }

  bool restore(const SceneSnapshot& snapshot) override {
    // **全或无**:先在副本上建,全部校验通过才替换。
    std::map<NodeId, Node> fresh;
    for (const NodeSnapshot& n : snapshot.nodes) {
      if (n.id == kInvalidNodeId) return false;
      if (fresh.find(n.id) != fresh.end()) return false;  // id 冲突
      Node node;
      node.snapshot = n;
      fresh.emplace(n.id, std::move(node));
    }
    // parent 必须存在(或为根)。
    for (const NodeSnapshot& n : snapshot.nodes) {
      if (n.parent == kInvalidNodeId) continue;
      if (fresh.find(n.parent) == fresh.end()) return false;  // 悬空 parent
    }
    // 建 children 索引 + 根列表。
    std::vector<NodeId> fresh_roots;
    for (const NodeSnapshot& n : snapshot.nodes) {
      if (n.parent == kInvalidNodeId) {
        fresh_roots.push_back(n.id);
      } else {
        fresh[n.parent].children.push_back(n.id);
      }
    }
    // 环检测:每个节点沿 parent 上溯,步数不得超过节点数。
    for (const NodeSnapshot& n : snapshot.nodes) {
      NodeId cur = n.id;
      std::size_t steps = 0;
      while (cur != kInvalidNodeId) {
        if (++steps > fresh.size()) return false;  // 环
        cur = fresh[cur].snapshot.parent;
      }
    }
    std::sort(fresh_roots.begin(), fresh_roots.end());

    nodes_ = std::move(fresh);
    roots_ = std::move(fresh_roots);
    // id 发号器必须跳过快照里出现过的最大 id(单调不复用)。
    for (const NodeSnapshot& n : snapshot.nodes) {
      if (n.id >= next_id_) next_id_ = n.id + 1;
    }
    changes_.clear();
    return true;
  }

  // ------------------------------------------------------------ 增量

  const std::vector<NodeChange>& changes() const override { return changes_; }
  void clear_changes() override { changes_.clear(); }

  void clear() override {
    // 逐根删(会连带子树),这样 changes 里能如实记下每个消失的节点。
    const std::vector<NodeId> roots = roots_;
    for (NodeId r : roots) remove_node(r);
    nodes_.clear();
    roots_.clear();
  }

 private:
  struct Node {
    NodeSnapshot snapshot{};
    std::vector<NodeId> children;
  };

  void record_change(NodeOp op, NodeId id) {
    NodeChange c;
    c.op = op;
    c.id = id;
    c.index = static_cast<std::uint32_t>(changes_.size());
    changes_.push_back(c);
  }

  void collect_subtree(NodeId id, std::vector<NodeId>* out) const {
    out->push_back(id);
    auto it = nodes_.find(id);
    if (it == nodes_.end()) return;
    for (NodeId child : it->second.children) collect_subtree(child, out);
  }

  bool is_descendant(NodeId maybe_descendant, NodeId ancestor) const {
    NodeId cur = maybe_descendant;
    std::size_t guard = 0;
    while (cur != kInvalidNodeId && guard++ <= nodes_.size() + 1) {
      if (cur == ancestor) return true;
      auto it = nodes_.find(cur);
      if (it == nodes_.end()) return false;
      cur = it->second.snapshot.parent;
    }
    return false;
  }

  Mat4 world_matrix(NodeId id) const {
    // 递归组合:world = parent.world * local。根 = 自己的 local。
    auto it = nodes_.find(id);
    if (it == nodes_.end()) return Mat4::identity();
    const Mat4 local = Mat4::from_trs(it->second.snapshot.transform);
    const NodeId parent = it->second.snapshot.parent;
    if (parent == kInvalidNodeId) return local;
    return world_matrix(parent).mul(local);
  }

  Aabb compute_world_bounds(NodeId id, bool include_invisible_children) const {
    auto it = nodes_.find(id);
    if (it == nodes_.end()) return Aabb{};
    const Mat4 world = world_matrix(id);

    Aabb acc = transform_aabb(world, it->second.snapshot.local_bounds);
    for (NodeId child : it->second.children) {
      // 父的包围盒必须含子(Scene.WorldBoundsIncludeChildren);
      // 但不可见子节点按 S1 的"可见性连带子树"不进提交视图,
      // 因此默认把不可见子树排除在世界盒之外。
      if (!include_invisible_children && !nodes_.at(child).snapshot.visible) continue;
      merge_aabb(&acc, compute_world_bounds(child, include_invisible_children));
    }
    return acc;
  }

  std::map<NodeId, Node> nodes_;   // 有序:snapshot() 直接升序
  std::vector<NodeId> roots_;
  std::vector<NodeChange> changes_;
  NodeId next_id_ = 1;             // 从 1 开始;0 恒为哨兵
};

}  // namespace

std::unique_ptr<IScene> create_scene() { return std::make_unique<SceneImpl>(); }

}  // namespace magicblocks::scene
