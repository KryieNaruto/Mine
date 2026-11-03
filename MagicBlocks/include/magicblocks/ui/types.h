// MagicBlocks UI 消费层 —— 基础类型(spec §2、§4、§7)
//
// 分层法律(spec §2 铁律):UI 消费层是 SDK 的**第一个使用者**,只依赖
// `include/magicblocks/...` 公共头。本头不得出现任何后端类型(Vk*/VMA),
// 也不得引入 L1~L4 的内部头 —— 目前 SDK 只落地了 L0,L2/L3 接口按 spec
// 预留成 **桩接口**(见 `scene_stub.h` / `overlay.h`),并不代表 UI 层自造引擎。
//
// 命名纪律(spec §2 / G3):UI 层**不得出现玩法专用分支**。玩法只以字符串 ID
// 出现(`FeatureId`),由 Router 路由到 `IFeatureHost` 实现;UI 层内不出现
// sketch/blocks/voxel/stamp 这类名字。
#pragma once

#include <cstdint>
#include <string>

#include "magicblocks/device/events.h"

namespace magicblocks::ui {

using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i32 = std::int32_t;
using f32 = float;

// ---------------------------------------------------------------- 屏幕坐标

// UI 层所有 2D 坐标都是**像素、左上原点、y 向下**(与输入事件的屏幕坐标同系,
// 与 `IRenderTarget::readback_rgba8` 的行序契约一致)。到 NDC 的换算只发生在
// `src/ui/overlay_stub.cpp` 一处,别的地方一律不自算。
struct Vec2 {
  f32 x = 0.f;
  f32 y = 0.f;
};

// 输入事件带的是 `device::Vec2f`。两者是同一个概念、同一坐标系,只差命名空间;
// 这里给一对显式转换,免得每个控件都自己抄一遍 `{ev.position.x, ev.position.y}`
// (抄多了就会有人漏掉 y 翻转或 x/y 写反)。
inline Vec2 from_device(device::Vec2f v) { return Vec2{v.x, v.y}; }
inline device::Vec2f to_device(Vec2 v) { return device::Vec2f{v.x, v.y}; }

struct Rect {
  f32 x = 0.f;
  f32 y = 0.f;
  f32 w = 0.f;
  f32 h = 0.f;

  bool contains(Vec2 p) const { return p.x >= x && p.x < x + w && p.y >= y && p.y < y + h; }
  bool empty() const { return w <= 0.f || h <= 0.f; }
  f32 right() const { return x + w; }
  f32 bottom() const { return y + h; }
};

struct Rgba8 {
  std::uint8_t r = 0;
  std::uint8_t g = 0;
  std::uint8_t b = 0;
  std::uint8_t a = 255;

  constexpr Rgba8() = default;
  constexpr Rgba8(std::uint8_t rr, std::uint8_t gg, std::uint8_t bb, std::uint8_t aa = 255)
      : r(rr), g(gg), b(bb), a(aa) {}

  bool operator==(const Rgba8& o) const { return r == o.r && g == o.g && b == o.b && a == o.a; }
  bool operator!=(const Rgba8& o) const { return !(*this == o); }
};

// 语义色板(demo 期固定;spec §9 明确不做换肤)。
namespace palette {
inline constexpr Rgba8 kBackground{16, 18, 22, 255};
inline constexpr Rgba8 kPanelFill{32, 36, 44, 235};
inline constexpr Rgba8 kPanelStroke{72, 80, 96, 255};
inline constexpr Rgba8 kText{235, 238, 245, 255};
inline constexpr Rgba8 kTextDim{150, 158, 172, 255};
inline constexpr Rgba8 kAccent{64, 160, 255, 255};
inline constexpr Rgba8 kAccentDim{44, 96, 152, 255};
inline constexpr Rgba8 kWarn{255, 176, 64, 255};
}  // namespace palette

// ---------------------------------------------------------------- 渲染目标模式

// spec §3 关键结论:运行时要区分「上屏」与「离屏」。
//   Swapchain —— 主路径:场景 pass + UI pass 同帧直接渲染进 swapchain image,
//                零 copy、零 CPU 回读(spec §5.2 方案 C)。
//   Offscreen —— 测试/录制/golden 专用(spec §5.5)。只有这条路径允许读回。
enum class TargetMode : std::uint8_t {
  Swapchain = 0,
  Offscreen = 1,
};

// ---------------------------------------------------------------- 能力

struct DisplayInfo {
  u32 width = 0;
  u32 height = 0;
  f32 density = 1.f;  // 物理像素 / dp;HUD 尺寸按 dp 定义,乘这个得到像素
};

// ---------------------------------------------------------------- 玩法标识

// 玩法 ID 是**字符串**而不是枚举:UI 层不认识具体玩法(不做玩法分支),
// 它只把字符串路由给注册进来的 `IFeatureHost`。四个玩法由 SDK 层之后的组合阶段
// 决定是否注册,UI 层不写死(spec §4「功能模块不互相感知」)。
struct FeatureId {
  std::string value;

  FeatureId() = default;
  explicit FeatureId(std::string v) : value(std::move(v)) {}

  bool empty() const { return value.empty(); }
  bool operator==(const FeatureId& o) const { return value == o.value; }
  bool operator!=(const FeatureId& o) const { return !(*this == o); }
};

}  // namespace magicblocks::ui
