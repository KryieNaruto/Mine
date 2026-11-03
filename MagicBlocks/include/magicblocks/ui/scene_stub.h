// MagicBlocks UI 消费层 —— L2 场景 / 渲染图接口的桩(spec §4 IFeatureHost / §5.3)
//
// 对齐 reference 设计 §4 L2 的 S1 `IScene` 与 S9 `IRenderFeature` 注册表。
//
// 本轮状态:**SDK 的 L2 尚未落地**(见 sdk_stub.h 顶部说明)。这里给出
// "UI 层消费方需要的最小形状",以便:
//   - `IFeatureHost::on_enter/on_update/on_render` 的签名今天就定型;
//   - 后续 L2 落地时,只需把桩换成真实现(同名),UI 层调用点零改动。
//
// 桩的生命周期语义已按 reference 设计固定:
//   - Scene 由 SDK(将来的 L2)持有并通过 context 借给 UI 层,**UI 层不拥有**;
//   - 节点 ID 稳定(S1),因此 `IStateStore` 存节点 ID 才有意义;
//   - 玩法层禁止直接构造 GPU 命令(G3 对 features 的要求),只能通过 IRenderFeature
//     把自身注册进渲染图,具体提交由渲染图统一录制。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "magicblocks/ui/sdk_stub.h"

namespace magicblocks::ui::stub {

using InputEvent = device::InputEvent;

// ---------------------------------------------------------------- S1 IScene 桩

// 节点 ID:稳定、进程内唯一。`IStateStore` 用它在退出/重进之间做引用。
using NodeId = std::uint64_t;
inline constexpr NodeId kInvalidNode = 0;

// 节点变换(TRS)。UI 层只需要读写,不做数学推导(推导属 SDK L3)。
struct Transform {
  f32 translation[3]{0.f, 0.f, 0.f};
  f32 rotation[4]{0.f, 0.f, 0.f, 1.f};
  f32 scale[3]{1.f, 1.f, 1.f};
};

struct NodeSnapshot {
  NodeId id = kInvalidNode;
  std::string name;
  Transform transform{};
  bool visible = true;
  NodeId parent = kInvalidNode;
};

enum class SceneNodeOp : std::uint8_t {
  Add = 0,
  Remove = 1,
  Update = 2,
};

struct SceneNodeChange {
  SceneNodeOp op = SceneNodeOp::Update;
  NodeSnapshot node{};
};

// 场景状态快照:节点的扁平序列(父子关系用 parent 字段表达)。
// **故意用扁平列表**:序列化时不需要递归,落盘与 diff 都简单。
struct SceneSnapshot {
  std::vector<NodeSnapshot> nodes;
};

// S1 `IScene` 桩。UI 层只用到"读写节点快照"这一面,渲染细节归 SDK。
class IScene {
 public:
  virtual ~IScene() = default;

  // 全部节点快照(按 NodeId 升序 —— 排序稳定才能做 golden 与命令哈希)。
  virtual SceneSnapshot snapshot() const = 0;

  // 增量变化(自上次 clear 以来)。UI 层**不消费**它做渲染,只用它决定
  // `IStateStore` 是否要落盘(避免每帧全量快照)。
  virtual const std::vector<SceneNodeChange>& changes() const = 0;
  virtual void clear_changes() = 0;

  // 从快照恢复(重进玩法时用)。返回 false = 快照与当前场景不兼容。
  virtual bool restore(const SceneSnapshot& snapshot) = 0;
};

// ---------------------------------------------------------------- S9 IRenderFeature 桩

enum class RenderFeatureLayer : std::uint8_t {
  // 场景内容:深度测试开,清深度(spec §5.2 Pass 1)。
  Scene = 0,
  // UI 覆盖层:深度测试关,附件 LoadOp::Load 叠加在场景上(spec §5.2 Pass 2)。
  Overlay = 1,
};

// 一个可注册进渲染图的渲染特性。spec §5.3:UI pass 只是"又一个注册项"。
class IRenderFeature {
 public:
  virtual ~IRenderFeature() = default;

  virtual RenderFeatureLayer layer() const = 0;

  // 在渲染图录制期被调用。**不得**发起单独 submit,不得触碰 swapchain(spec §5.4)。
  virtual void record() = 0;
};

// 注册表:玩法/UI 把自己的渲染特性挂进来,由渲染图统一录制。
class IRenderFeatureRegistry {
 public:
  virtual ~IRenderFeatureRegistry() = default;

  // 稳定顺序注册(同一 layer 内按注册顺序录制 —— 保证命令流可复现)。
  virtual void add(IRenderFeature* feature) = 0;
  virtual void remove(IRenderFeature* feature) = 0;
};

// ---------------------------------------------------------------- 特性宿主拿到的上下文

// 玩法实现能拿到的东西:**只有** SDK 场景与渲染特性注册表,外加只读的显示信息。
// 刻意不给 command list / swapchain / 读回入口 —— 那条路只有 Presenter 有。
struct FeatureContext {
  IScene* scene = nullptr;
  IRenderFeatureRegistry* render_features = nullptr;
  DisplayInfo display{};
};

}  // namespace magicblocks::ui::stub
