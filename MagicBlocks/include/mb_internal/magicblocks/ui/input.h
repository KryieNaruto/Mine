// MagicBlocks UI 消费层 —— 输入路由与手势(spec §7)
//
// 仲裁顺序(spec §7 明文,必须显式):
//
//   TouchEvent
//      ├─ ① HUD 命中(按钮/面板)  ── 是 ─► 消费,不再下发
//      ├─ ② 工具激活(笔刷画/盖章) ── 是 ─► 交给当前特性(handler 返回 true = 激活)
//      └─ ③ 相机/场景交互(旋转/缩放/拾取) ── 交给 ICamera / IPicker(L3,尚未落地)
//
// **手势识别统一在 UI 层做**,不在每个玩法里重复(spec §7)。同一个原始事件流
// 既喂给 `device::EventStream`(命令复现回放),也喂给本路由器;复现时优先用
// 手势结果、必要时回落到原始事件 —— 因此 `Gesture` 必须**自包含**且可比对。
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "mb_internal/magicblocks/device/events.h"
#include "mb_internal/magicblocks/ui/types.h"

namespace magicblocks::ui {

// 事件归属:同一个触摸事件在三处中的落点。用于计数核对(§7 要求"所有事件
// 同时喂给 InputEvent 队列",即每个事件都恰好有一个归属,不许丢)。
enum class InputOwner : std::uint8_t {
  Unrouted = 0,
  Hud = 1,
  Tool = 2,
  Camera = 3,
};

const char* to_string(InputOwner owner);

// ---------------------------------------------------------------- 手势

enum class GestureKind : std::uint8_t {
  None = 0,
  Tap,         // 单指按下未移动即抬起
  Drag,        // 单指移动超过阈值
  Pinch,       // 双指距离变化(缩放)
  TwoFingerPan,// 双指重心平移
  LongPress,   // 单指长按(拾取)
};

const char* to_string(GestureKind kind);

// 一个已识别的完整手势。**自包含**:不含任何指针或时间引用,可直接序列化进
// 命令流(§7「输入到命令的一致性」)。
struct Gesture {
  GestureKind kind = GestureKind::None;
  InputOwner owner = InputOwner::Unrouted;  // 同一手势内所有事件都归它
  Vec2 start{};                            // 屏像素
  Vec2 end{};
  Vec2 delta{};                            // 重心/主指点位移
  f32 scale = 1.f;                         // Pinch 用:相对起始的距离比
  f32 angle = 0.f;                         // 预留(本轮不做旋转手势)
  std::int64_t duration_us = 0;
  std::uint32_t pointer_count = 0;
  // 参与手势的**原始**事件(§7:记录的是"已解析的手势结果 + 原始事件")
  std::vector<std::int64_t> raw_timestamps_us;
};

// ---------------------------------------------------------------- 配置

struct InputRouterConfig {
  // 拖动判定阈值(dp,按 density 换算成像素);小于阈值视为抖动不进 Drag。
  f32 drag_threshold_dp = 6.f;
  // 长按判定时长(微秒)。
  std::int64_t long_press_us = 500'000;
  // 双指缩放/平移的最小有效距离比变化。
  f32 pinch_min_scale_delta = 0.02f;
};

// ---------------------------------------------------------------- 工具/相机出口(桩)

// 玩法工具接口:② 层。返回 true = 自己消费了这个事件,不再下发给相机。
// 与 `IFeatureHost` 分开,因为"工具激活"是全局开关(当前玩法是否有激活工具),
// 而相机交互在任何玩法下都存在。
class IToolSink {
 public:
  virtual ~IToolSink() = default;
  virtual bool tool_active() const = 0;
  virtual bool on_input(const device::InputEvent& ev) = 0;
};

// 相机/场景交互出口(③ 层)。真实实现是 SDK L3 的 `ICamera`/`IPicker`(尚未落地),
// 本轮以桩声明让仲裁顺序可测。
class ICameraSink {
 public:
  virtual ~ICameraSink() = default;
  virtual bool on_input(const device::InputEvent& ev) = 0;
};

// ---------------------------------------------------------------- 路由统计

// 每帧路由统计。G6(spec §6)要求暴露 input→present 延迟,这里给出"帧内
// 最后一个被消费的输入时间戳",由 PerfGovernor 换成帧差。
struct InputRouteStats {
  u32 events_total = 0;
  u32 routed_hud = 0;
  u32 routed_tool = 0;
  u32 routed_camera = 0;
  u32 unrouted = 0;
  u32 gestures_emitted = 0;
  u32 dropped = 0;  // 归属 None 且无人消费的事件数(必须为 0;非 0 说明仲裁有洞)
};

class Hud;  // fwd(见 hud.h)

// 输入路由器(spec §7)。**有状态**:多指手势需要跨事件保留按压起点。
class InputRouter {
 public:
  InputRouter();
  ~InputRouter();

  InputRouter(const InputRouter&) = delete;
  InputRouter& operator=(const InputRouter&) = delete;

  void set_config(const InputRouterConfig& cfg);
  const InputRouterConfig& config() const;

  // 设置①层命中源(HUD 树)与②③层出口。三者都可为空(null = 不存在该层)。
  void set_hud(Hud* hud);
  void set_tool(IToolSink* tool);
  void set_camera(ICameraSink* camera);

  // 喂一个原始事件。返回它的归属(便于测试断言与 §7 的"每事件必归属")。
  InputOwner route(const device::InputEvent& ev);

  // 取走自上次调用以来识别出的手势(调用方持有时长自定,不清空 = 只读)。
  const std::vector<Gesture>& gestures() const;

  // 帧尾:重置每帧统计,并把跨帧未完成的手势推进超时判定。
  void end_frame();

  const InputRouteStats& stats() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace magicblocks::ui
