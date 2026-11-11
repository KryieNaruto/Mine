// MagicBlocks SDK —— L2 场景层基础类型
//
// 设计约束(spec §2.1 / §4 L2 S1-S9 / plans-l2 §4.4.1,N7 裁决):
//   - **本层公共头零 Vulkan 类型**(G3 门控机械保证):只收 L0/L1 的公共值类型。
//   - **整数别名只用 std::uint*_t / std::int*_t,不新起 `u8`/`f32` 之类短名**:
//     `device/types.h` 只提供 u32/u64/i32 三个别名,而 `ui::f32` 属 UI 层,
//     scene 反向依赖 UI 会违反分层(spec §2.1)。因此本层一律写全名。
//   - 命名空间固定 `magicblocks::scene`(A2 裁决)。`ui::stub` 里那套**同名但不同集**
//     的桩形状(`ui/scene_stub.h`)本轮**不动**,两套并存(§5.3.1 / R11)。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "mb_internal/magicblocks/device/render_target.h"
#include "mb_internal/magicblocks/device/result.h"
#include "mb_internal/magicblocks/device/types.h"

namespace magicblocks::scene {

using device::Extent2D;
using device::Format;
using device::IRenderTarget;
using device::Result;
using device::Status;

// ---------------------------------------------------------------- 节点标识(S1)

// 节点 id。**单调递增、永不复用**:remove 之后旧 id 不会复活,
// 这是命令流可复现(S8)与 UI 状态落盘(ui::stub::NodeId 同底层类型)的共同前提。
using NodeId = std::uint64_t;

// 无效节点 id。真实 id 从 1 开始发号,因此 0 恒可作哨兵。
inline constexpr NodeId kInvalidNodeId = 0;

// ---------------------------------------------------------------- 变换(S1)

// TRS 变换(先缩放、后旋转、再平移)。四元数存储顺序 (x, y, z, w),**不归一化**:
// 本层如实保存调用方给的值,归一化由使用方决定 —— 这样序列化与哈希才无隐藏副作用。
struct Transform {
  float translation[3] = {0.0f, 0.0f, 0.0f};
  float rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};  // (x,y,z,w)
  float scale[3] = {1.0f, 1.0f, 1.0f};

  bool operator==(const Transform& o) const;
  bool operator!=(const Transform& o) const { return !(*this == o); }
};

// 轴对齐包围盒(局部空间)。empty() 为真时 min/max 全是 0。
struct Aabb {
  float min[3] = {0.0f, 0.0f, 0.0f};
  float max[3] = {0.0f, 0.0f, 0.0f};
  bool empty = true;

  bool operator==(const Aabb& o) const;
  bool operator!=(const Aabb& o) const { return !(*this == o); }
};

// ---------------------------------------------------------------- 节点快照(S1/S8)

// 一个节点的**完整本体**(值语义、无指针、无递归)。扁平列表是刻意设计:
// 序列化时不需要递归,落盘与 diff 都简单,命令流的"自包含"(B7)也靠它。
struct NodeSnapshot {
  NodeId id = kInvalidNodeId;
  NodeId parent = kInvalidNodeId;   // kInvalidNodeId = 根节点
  std::string name;                 // UTF-8,原样保存(不 trim、不规范化)
  Transform transform{};
  Aabb local_bounds{};              // 局部空间包围盒(不含子节点)
  bool visible = true;

  bool operator==(const NodeSnapshot& o) const;
  bool operator!=(const NodeSnapshot& o) const { return !(*this == o); }
};

// 整场景快照。**按 id 升序**排序后返回,排序稳定才能做 golden 与命令哈希
// (Scene.SnapshotIsIdOrdered)。
struct SceneSnapshot {
  std::vector<NodeSnapshot> nodes;

  bool operator==(const SceneSnapshot& o) const { return nodes == o.nodes; }
  bool operator!=(const SceneSnapshot& o) const { return !(*this == o); }
};

// ---------------------------------------------------------------- 节点增量(S1)

enum class NodeOp : std::uint8_t {
  Added = 0,
  Removed = 1,
  Updated = 2,
};

// 自上次 clear_changes() 以来的一条变更。UI 层只用它决定 IStateStore 是否需要落盘,
// 不用它做渲染。
struct NodeChange {
  NodeOp op = NodeOp::Added;
  NodeId id = kInvalidNodeId;
  std::uint32_t index = 0;   // 快照列表里的序号(Added/Updated 有效)
};

// ---------------------------------------------------------------- 包围球(S1)

// 世界空间包围球。包围盒用于精确裁剪,包围球用于快速剔除。
struct BoundingSphere {
  float center[3] = {0.0f, 0.0f, 0.0f};
  float radius = 0.0f;
  bool empty = true;
};

}  // namespace magicblocks::scene
