// MagicBlocks SDK —— L2 S1 `IScene`:场景节点树
//
// 设计约束(spec §4 L2 S1 / plans-l2 §4.2):
//   - **NodeId 单调不复用**:`remove()` 之后旧 id 不会重新分配给新节点
//     (Scene.NodeIdIsStableAcrossMutate)。id 不复用是命令流可复现的前提 ——
//     否则"同一个 id"在两帧里可能指向不同节点,哈希就没有意义。
//   - **世界包围盒含子节点**:父节点的 world_bounds 必须包住所有可见子节点的
//     world_bounds(Scene.WorldBoundsIncludeChildren)。
//   - **visible=false 连带子树**:一个不可见节点,其整棵子树都不进提交视图
//     (Scene.InvisibleNodesAreSkipped)。
//   - **快照按 id 升序**:`snapshot()` 的输出顺序必须稳定(Scene.SnapshotIsIdOrdered)。
//   - **restore() 全或无**:任何非法快照(环、悬空 parent、id 冲突)返回 false 且
//     **场景保持原样**,不允许"恢复了一半"(Scene.RestoreRejectsBadSnapshot)。
//   - 本接口**不读时钟**、不发起 GPU 操作、不分配显存。纯 CPU 逻辑。
#pragma once

#include "mb_internal/magicblocks/scene/types.h"

namespace magicblocks::scene {

class IScene {
 public:
  IScene(const IScene&) = delete;
  IScene& operator=(const IScene&) = delete;
  virtual ~IScene() = default;

  // -------------------------------------------------------------- 增删改

  // 新建一个节点并返回其 id。parent 必须是已存在的 id 或 kInvalidNodeId(= 根)。
  // 节点从创建起就带一个空的局部包围盒。
  virtual Result<NodeId> add_node(NodeId parent, const std::string& name) = 0;

  // 删除节点及其**整棵子树**。返回 false = id 不存在。
  // 被删的 id 永不复活(后续 add_node 只会发更大的号)。
  virtual bool remove_node(NodeId id) = 0;

  // 修改变换 / 局部包围盒 / 可见性。返回 false = id 不存在。
  virtual bool set_transform(NodeId id, const Transform& transform) = 0;
  virtual bool set_local_bounds(NodeId id, const Aabb& bounds) = 0;
  virtual bool set_visible(NodeId id, bool visible) = 0;

  // 重新挂到新的父节点下。返回 false = id 或 parent 不存在、或会形成环。
  virtual bool reparent(NodeId id, NodeId new_parent) = 0;

  // -------------------------------------------------------------- 查询

  virtual bool has_node(NodeId id) const = 0;
  virtual std::size_t node_count() const = 0;

  // 单个节点的**局部**空间包围盒(不含子节点)。id 不存在返回空盒。
  virtual Aabb local_bounds(NodeId id) const = 0;

  // 单个节点的**世界**空间包围盒(含整棵子树,且跳过不可见子节点)。
  // id 不存在返回空盒。父亲会把子的世界盒并进来。
  virtual Aabb world_bounds(NodeId id) const = 0;

  // 单个节点的世界空间包围球(id 不存在 → empty=true)。
  virtual BoundingSphere world_bounding_sphere(NodeId id) const = 0;

  // 节点自身是否可见。**注意**:本站可见 != 在提交视图里 —— 祖先不可见时同样被剔除。
  virtual bool visible(NodeId id) const = 0;

  // 节点与其所有祖先是否都可见(即"会不会被提交")。
  virtual bool effectively_visible(NodeId id) const = 0;

  virtual NodeId parent_of(NodeId id) const = 0;

  // -------------------------------------------------------------- 快照(S8)

  // 按 id 升序的完整快照。这是 UI 状态落盘与命令流内嵌场景的共同载体。
  virtual SceneSnapshot snapshot() const = 0;

  // 从快照恢复。**全或无**:非法快照返回 false 且场景不变。
  // 合法条件:id 都非 0 且互不重复;parent 要么是 kInvalidNodeId 要么在快照里;
  // parent 链不构成环。
  virtual bool restore(const SceneSnapshot& snapshot) = 0;

  // -------------------------------------------------------------- 增量

  // 自上次 clear_changes() 以来的变更序列(按发生顺序)。
  virtual const std::vector<NodeChange>& changes() const = 0;
  virtual void clear_changes() = 0;

  // 清空全部节点(只删根,子树连带消失)。已发出的 id 仍然不复用。
  virtual void clear() = 0;

 protected:
  IScene() = default;
};

}  // namespace magicblocks::scene
