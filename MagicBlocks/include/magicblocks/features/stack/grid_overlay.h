#pragma once
// MagicBlocks L4 —— 魔法叠叠乐：网格覆盖层（设计 §7）
//
// 网格指示负责让玩家"看得见地在哪一格上画"。它**不进**游戏几何、**不**参与
// `IBlockSet`、**不**参与哈希 —— 纯视觉提示，改它不该让模型哈希变化。
//
// 三层画法（自下而上）：
//   ① 底层网格线：整个 half_extent 范围的细网格（默认 y=locked_layer 或 0）
//   ② 当前层高亮：刷子锁定的那一层画一个半透明色板 + 边框
//   ③ 光标格：鼠标射线命中的那一格画实心亮框
//
// 为什么单独立一条 draw 而不是塞进双 pass：
//   设计 §10 的"恰好 2 次 drawcall"是**积木几何**的契约。覆盖层是屏幕空间的
//   2D 图元（IOverlay2D），走自己的管线与自己的 pass，因此**不计入**那 2 次。
//   把它算进去会逼着人把 2D 像素坐标塞进 3D 实例缓冲，那才是真正的错误耦合。
//   这里用 `kOverlayDrawCalls = 1` 显式记录"覆盖层恰好 1 次 draw"，
//   由 `StackOverlayStats` 输出，便于用例核对而无需推断。

#include <cstdint>
#include <memory>
#include <vector>

#include "magicblocks/device/render_device.h"
#include "magicblocks/features/stack/block_brush.h"
#include "magicblocks/features/stack/block_set.h"
#include "magicblocks/features/stack/stack_types.h"
#include "magicblocks/primitives/overlay2d.h"
#include "magicblocks/resources/command.h"
#include "magicblocks/resources/pipeline.h"

namespace magicblocks::features::stack {

// 覆盖层只依赖 L0 设备 / L1 资源 / L3 覆盖层原语的**公共**接口。
namespace dev = magicblocks::device;
namespace res = magicblocks::resources;
using dev::IRenderDevice;
using res::ICommandList;
using res::IPipeline;

// 覆盖层自己的配色（与清屏色 / 积木色都不重合，便于像素断言区分）。
struct GridOverlayStyle {
  // ① 底层网格线
  primitives::Color4 grid_line{0.22f, 0.24f, 0.30f, 0.85f};
  float grid_line_px = 1.0f;
  // ② 锁定层的色板与边框
  primitives::Color4 locked_fill{0.20f, 0.45f, 0.95f, 0.16f};
  primitives::Color4 locked_edge{0.35f, 0.60f, 1.00f, 0.90f};
  float locked_edge_px = 2.0f;
  // ③ 光标格
  primitives::Color4 cursor_fill{1.00f, 0.85f, 0.30f, 0.35f};
  primitives::Color4 cursor_edge{1.00f, 0.75f, 0.10f, 1.00f};
  float cursor_edge_px = 2.0f;
};

struct StackOverlayDesc {
  // 画哪一层的网格线。通常 = 刷子的锁定层；未锁定时调用方传 0。
  std::int32_t active_layer = 0;
  // 光标所在格（世界坐标下射线命中的格）。has_cursor=false 时跳过 ③。
  Cell cursor{};
  bool has_cursor = false;
  // 画不画 ①。性能/可读性考虑允许关掉，默认开。
  bool draw_grid_lines = true;
  // 画不画 ②。未锁层时通常关掉。
  bool draw_locked_layer = true;
};

struct StackOverlayStats {
  std::uint32_t line_count = 0;
  std::uint32_t rect_count = 0;
  std::uint32_t tri_count = 0;
  std::uint32_t vertex_count = 0;
  std::uint32_t index_count = 0;
  // 覆盖层每次 record 恰好 1 次 draw。非 0/1 就是契约被破坏。
  std::uint32_t draw_calls = 0;
  // 本次是否真的画了东西（几何为空时不开 pass、不 draw）。
  bool drew = false;
};

// 把网格世界坐标 → 屏幕像素。相机投影由调用方的 4x4 矩阵给出：
// 传 view_proj 而不是 ICamera，理由与 stack_render.h 的 StackFrameDesc 相同
// —— 覆盖层与积木几何必须用**同一份**矩阵快照，否则两层会错位。
struct GridProjector {
  float view_proj[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  std::uint32_t width = 640;
  std::uint32_t height = 480;
  // 世界点 → 像素。返回 false 表示点在相机背后（w <= 0），**不写** out。
  bool project(const float world[3], float* out_x, float* out_y) const;
};

// 网格里某一格顶面的四个角（世界坐标，y = 该层顶面高度）。
void cell_top_quad(const GridSpec& grid, const Cell& cell, float out_corners[4][3]);

// 覆盖层的上传缓冲（顶点 + 索引）。调用方持有到 submit 之后。
struct OverlayBuffers {
  std::unique_ptr<res::IBuffer> vertex;
  std::unique_ptr<res::IBuffer> index;
};

class IGridOverlay {
 public:
  IGridOverlay() = default;
  IGridOverlay(const IGridOverlay&) = delete;
  IGridOverlay& operator=(const IGridOverlay&) = delete;
  virtual ~IGridOverlay() = default;

  // 建覆盖层管线（1 条）。失败即不可用，不降级。
  virtual StackStatus create_passes(IRenderDevice* device, std::uint32_t samples) = 0;
  virtual IPipeline* pipeline() const = 0;
  virtual std::uint64_t pipeline_hash() const = 0;

  virtual void set_style(const GridOverlayStyle& style) = 0;
  virtual const GridOverlayStyle& style() const = 0;

  // 把 ①②③ 收进 overlay 收集器。**不**调 begin_frame/build —— 那由调用方
  // 在帧边界统一做（同一帧可能还有其它覆盖层来源，比如 gizmo）。
  virtual StackStatus collect(primitives::IOverlay2D& overlay, const GridSpec& grid,
                              const GridProjector& projector, const StackOverlayDesc& desc) = 0;

  // 把已 build 的几何录成一次 draw。返回 Error 表示没东西可画或录制失败；
  // 调用方据此决定是否需要开 pass —— 本函数**不**开 pass。
  //
  // ⚠️ 生命周期：上传用的缓冲**归调用方持有**，必须活到 submit_command_list
  // 之后。命令流录制的是缓冲句柄，录制结束就析构缓冲 = 提交时读野内存
  // （实测表现为 submit 处 access violation）。本函数刻意不返回缓冲，
  // 而是要求调用方传入 out 参数 —— 让"谁负责活多久"在签名上就写清楚。
  virtual StackStatus record(ICommandList& cmd, const primitives::OverlayGeometry& geo,
                             std::uint32_t width, std::uint32_t height,
                             OverlayBuffers* out_buffers) = 0;

  virtual const StackOverlayStats& stats() const = 0;
  virtual void reset_stats() = 0;
};

std::unique_ptr<IGridOverlay> create_grid_overlay();

// 覆盖层每次 record 提交的 draw 数量（契约常量，便于断言与文档一致）。
inline constexpr std::uint32_t kOverlayDrawCalls = 1;

}  // namespace magicblocks::features::stack
