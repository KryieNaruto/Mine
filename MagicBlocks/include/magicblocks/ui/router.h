// MagicBlocks UI 消费层 —— 功能路由与特性宿主(spec §4)
//
//   HomeScreen ──(选择玩法)──► FeatureHost(Sketch | Blocks | Voxel | Stamp)
//        ▲                              │
//        └──────────(返回/退出)──────────┘
//
// 三条铁律:
//   1. **唯一入口**:功能切换只经过 `IRouter`。玩法之间不互相感知,也不直接
//      操作 HUD/输入/Presenter —— 它们只实现 `IFeatureHost`。
//   2. **UI 层不做玩法分支**:`FeatureId` 是字符串,Router 只查表。UI 层源码里
//      不出现任何具体玩法名(spec §2 / G3 对 backend 的同类要求,这里对 UI 层自加)。
//   3. **生命周期明确**:`on_enter` 挂载到 `IRenderFeature` 注册表,`on_exit` 卸载;
//      退出时经 `IStateStore` 保存场景状态,重进恢复(spec §4 IStateStore)。
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "magicblocks/device/events.h"
#include "magicblocks/ui/scene_stub.h"
#include "magicblocks/ui/types.h"

namespace magicblocks::ui {

// 桩形状的实体都在 `ui::stub` 里(见 scene_stub.h 顶部说明)。这里做**只增别名**,
// 让 UI 层自己的代码读起来不必处处写 `stub::`;SDK L2 落地后只需把别名指向真实现。
using stub::FeatureContext;
using stub::IRenderFeature;
using stub::IRenderFeatureRegistry;
using stub::IScene;
using stub::NodeId;
using stub::NodeSnapshot;
using stub::RenderFeatureLayer;
using stub::SceneNodeChange;
using stub::SceneSnapshot;

// spec §4 `IFeatureHost`:特性生命周期。**只依赖** SDK 的 IScene / 输入事件 /
// IRenderFeature(本轮由 scene_stub.h 以桩形状承载,见该文件顶部说明)。
class IFeatureHost {
 public:
  virtual ~IFeatureHost() = default;

  // 进入玩法。此时 `scene` 已就绪;实现可在此挂自己的 IRenderFeature。
  virtual void on_enter(const FeatureContext& ctx) = 0;

  // 退出玩法。**不得**在这里做 GPU 同步等待(spec §5.4);持久化走 IStateStore。
  virtual void on_exit() = 0;

  // 每帧推进(秒)。
  virtual void on_update(f32 dt_seconds) = 0;

  // 该玩法自己的输入出口(InputRouter 的 ② 层)。返回 true = 已消费。
  // 相机/缩放之类的通用交互不在玩法里实现(spec §7:手势统一在 UI 层)。
  virtual bool on_input(const device::InputEvent& ev) = 0;
};

// 一个玩法槽位:ID → 工厂。Router 只按字符串查表,不认识具体玩法。
struct FeatureRegistration {
  FeatureId id;
  std::function<std::unique_ptr<IFeatureHost>()> factory;
};

// ---------------------------------------------------------------- 状态存储(spec §4 IStateStore)

// 跨"退出→重进"的状态块。
//   **存内存为主,落盘异步**(spec 原文):`serialize()` 只在需要落盘时才被调用,
//   由外部持久化线程处理;UI 层的热路径只做内存里的 `restore()`。
struct StateBlob {
  SceneSnapshot scene;
  std::uint64_t tool_params = 0;   // 工具参数位(具体编解码归玩法)
  f32 camera[9]{};                 // 相机位姿(3x3,行主序)

  bool empty() const { return scene.nodes.empty() && tool_params == 0 && !has_camera; }
  bool has_camera = false;
};

class IStateStore {
 public:
  virtual ~IStateStore() = default;

  virtual void put(const FeatureId& id, StateBlob blob) = 0;
  // 取出并**保留**(重进可反复取);没有则返回 nullptr。
  virtual const StateBlob* get(const FeatureId& id) const = 0;
  virtual void remove(const FeatureId& id) = 0;
  virtual std::size_t size() const = 0;

  // 序列化块(供异步落盘)。**热路径不调用**。
  virtual std::string serialize(const StateBlob& blob) const = 0;
  virtual bool deserialize(const std::string& text, StateBlob* out) const = 0;
};

// 内存实现(本轮唯一实现;落盘留给后续 `FileStateStore` 在外部线程包装它)。
class MemoryStateStore final : public IStateStore {
 public:
  MemoryStateStore();
  ~MemoryStateStore() override;

  void put(const FeatureId& id, StateBlob blob) override;
  const StateBlob* get(const FeatureId& id) const override;
  void remove(const FeatureId& id) override;
  std::size_t size() const override;

  std::string serialize(const StateBlob& blob) const override;
  bool deserialize(const std::string& text, StateBlob* out) const override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------- 路由(spec §4 IRouter)

// 路由动作的返回值:让上层(UI 层 Loop)知道"需要做什么",而不是路由自己驱动渲染。
enum class RouteAction : std::uint8_t {
  None = 0,
  Pushed,   // 进入了某个玩法(HomeScreen → FeatureHost)
  Popped,   // 退回了 HomeScreen(FeatureHost → HomeScreen)
  Failed,   // 请求无效(未注册的 ID / 空栈再 Pop)
};

const char* to_string(RouteAction action);

// 路由事件回调:HUD 通过它发命令,而不是直接操作 Router —— 这样 HUD 不依赖 Router,
// Router 也不依赖 HUD,两边都只依赖这个函数对象。
using RouteCallback = std::function<RouteAction(const FeatureId&)>;

class Router {
 public:
  Router();
  ~Router();

  Router(const Router&) = delete;
  Router& operator=(const Router&) = delete;

  // 注册玩法(ID 重复 = 覆盖,便于测试)。
  void register_feature(FeatureRegistration reg);
  std::size_t feature_count() const;

  void set_state_store(IStateStore* store);  // null = 不保存
  void set_context(FeatureContext ctx);

  // spec §4 三动作。栈深度上限 = 1 层玩法(HomeScreen 恒在栈底,不入栈)。
  RouteAction push(const FeatureId& id);
  RouteAction pop();
  const FeatureId& current() const;  // 空 = HomeScreen

  // 每帧:当前玩法 on_update(HomeScreen 下为空操作)。
  void update(f32 dt_seconds);

  // 输入出口(InputRouter 的 ② 层)。HomeScreen 下恒 false(交给相机/UI 自己)。
  bool on_input(const device::InputEvent& ev);

  // 当前玩法宿主(测试/调试用;可能为 nullptr)。
  IFeatureHost* current_host() const;

  // 回调工厂:`router.callback()` 返回一个 RouteCallback,可直接塞进 HUD 按钮。
  RouteCallback callback();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace magicblocks::ui
