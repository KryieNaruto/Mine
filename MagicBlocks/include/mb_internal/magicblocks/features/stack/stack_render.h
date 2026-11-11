// MagicBlocks L4 —— 魔法叠叠乐：双 pass 渲染（表面 pass + 内面 pass）
//
// ======================== 为什么是两个 pass，不是一个 ========================
// 积木是**半透明**的彩色盒子。一个 pass 画不出"看到盒子内部"的效果：
//   - 只有表面（depthTest/depthWrite、cull=Back）：近处的盒子把远处整个挡住，
//     内部结构与叠层关系全看不见 —— 那就不叫叠叠乐了；
//   - 只有内面（关深度、cull=None）：背面板与正面板按提交顺序互盖，
//     顺序一变画面就变，且远处的盒子会盖住近处的（没有深度裁决）。
// 两个一起画才有"外壳实体 + 内壁透光"的观感。
//
// ======================== 顺序是正确性的一部分，不是观感调优 ========================
// **表面 pass 必须在前、内面 pass 必须在后。** 反过来（内面先画）等于白画：
// 内面画完之后表面 pass 带着 depthWrite 把那些像素整片覆盖掉，
// 最终画面与"只画表面"逐像素相同。
// 这条契约由 `StackRender.InnerPassIsAfterSurfacePass` 反证钉住。
//
// ======================== D-VX-4′：不碰 depthConstWrite ========================
// 内面 pass 的正确性来源只有三件事：
//   ① depthTest = false   —— 不做深度裁决，内壁才画得出来
//   ② CullMode::None      —— 背面也要
//   ③ 排在表面 pass 之后  —— 见上
// 并且 `DepthState` 至少一个写位为真（`depthWrite = true`），否则后端根本不挂
// depth-stencil 状态（`vulkan_pipeline.cpp:668-671`），深度测试会被静默当成恒通过。
// **`depthConstWrite` 全程保持 false**：它被落成 `depthBounds`，而该设备特性
// 从未被请求（`feats{}` 全 0），对未启用特性置位是未定义行为。
// 本文件与 CLI 都不提供任何 depth_const 相关开关或验收判据。
//
// ======================== 零后端私有头（D-VX-8′） ========================
// 两个 PipelineDesc 都走公共 `create_pipeline(desc)`，深度行为完全由
// `DepthState`/`RasterState` 决定，与 `has_depth` 附件形状字段无关。
// 因此 L4 不需要、也禁止调用 `create_pipeline_for_shape`（那会引入后端私有头）。
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "mb_internal/magicblocks/features/stack/block_set.h"
#include "mb_internal/magicblocks/features/stack/grid_overlay.h"
#include "mb_internal/magicblocks/features/stack/stack_types.h"
#include "mb_internal/magicblocks/primitives/batch.h"
#include "mb_internal/magicblocks/primitives/camera.h"
#include "mb_internal/magicblocks/primitives/overlay2d.h"
#include "mb_internal/magicblocks/device/render_device.h"
#include "mb_internal/magicblocks/resources/command.h"
#include "mb_internal/magicblocks/resources/pipeline.h"


namespace magicblocks::features::stack {

namespace mbp = magicblocks::primitives;
namespace res = magicblocks::resources;
namespace dev = magicblocks::device;
using dev::Status;
// IRenderTarget 在 device 层（L0），不在 resources（L1）—— 渲染目标属于设备。
using dev::IRenderTarget;
using dev::IRenderDevice;

// 一个积木盒子的世界尺寸相对格边长的缩放。
// < 1 让相邻盒子之间留出缝，肉眼能数清格子 —— 这对"叠层示意"是刚需，
// 缝隙里露出的正是内面 pass 画出的内壁。
inline constexpr float kBlockFillRatio = 0.92f;

// 由 GridSpec + Cell 算出该块的 model 矩阵（列主序，与 InstanceAttribs::model 一致）。
// 纯函数，可单测；不读时钟、不依赖相机。
void block_model_matrix(const GridSpec& grid, const Cell& cell, float out[16]);

// 两个 pass 的身份。**只暴露 pipeline_hash，不暴露任何附件形状** ——
// 公共面的 hash 与附件形状无关（`hash_pipeline_desc_with_shape` 在默认形状下
// 退化），所以这里能断言的只有"两个 pass 是两条不同的管线"。
struct StackPassPair {
  resources::IPipeline* surface_pass = nullptr;  // 表面：depthTest/Write、cull=Back
  resources::IPipeline* inner_pass = nullptr;    // 内面：depthTest=false、cull=None
  std::uint64_t surface_hash = 0;
  std::uint64_t inner_hash = 0;
};

// 两个 pass 的 PipelineDesc。分开成函数是为了让测试能直接比较 desc 内容，
// 不必先建管线。
resources::PipelineDesc surface_pipeline_desc(std::uint32_t samples);
resources::PipelineDesc inner_pipeline_desc(std::uint32_t samples);

// 两个 hash 必须不同 —— 否则合批器会把两个 pass 合并成一个 draw（顺序契约消失）。
std::uint64_t compute_pass_hash(const resources::PipelineDesc& desc, std::uint64_t salt);

// 网格指示（需求「立方体…带网格指示」）的接线口。
//
// ======================== 为什么不直接塞一个 IGridOverlay* ========================
// 覆盖层是**屏幕空间**的 2D 图元（`IOverlay2D`），它有自己的一条管线与一次 draw，
// 并且明确**不计入**双 pass 的"恰好 2 次 drawcall"预算（见 grid_overlay.h 顶部）。
// 如果把 `IGridOverlay` 直接交给 `IStackRenderer`，渲染器就要同时持有 L3 的覆盖层
// 收集器、覆盖层几何与覆盖层缓冲 —— 那等于把两套生命周期塞进一个对象，
// 而且"覆盖层到底画没画"会退化成读某个内部统计字段。
//
// 所以接线口是这样的（所有权都在调用方，渲染器只借用来"收集"）：
//
//   1. 调用方建 `IGridOverlay` 与 `IOverlay2D`，填好本结构；
//   2. `record_frame()` 在录完表面 + 内面两个 pass 之后，调 `overlay->collect(collector, ...)`，
//      把网格线/锁定层/光标格收进 `collector`；
//   3. 调用方 `collector->build()` 后调 `overlay->record(cmd, geometry, w, h, &buffers)`
//      录成**自己的**一次 draw —— 缓冲由调用方持有到 submit 之后。
//
// 这样"网格指示有没有进出图"是可观测的：`StackRenderStats::overlay_collected` 与
// 调用方 `overlay->stats()`。像素级证据见用例 `StackGridOverlay.WiredOverlayChangesPixels`。
//
// `grid` 刻意在这里再给一次而不是复用 `IBlockSet` 的：`IBlockSet` 不暴露 GridSpec，
// 而覆盖层要画**整张网格**（包括没有方块的那些格），不能从真源反推。
struct StackFrameOverlay {
  IGridOverlay* overlay = nullptr;                         // 覆盖层（管线 + 收集 + 录制）
  magicblocks::primitives::IOverlay2D* collector = nullptr;             // 收集器（调用方持有，活到 submit）
  GridSpec grid{};                                         // 网格规格（与 IBlockSet 同一份）
  StackOverlayDesc desc{};                                 // 画哪层 / 光标格 / 开关
  // 叠加用的 view_proj：**必须与 StackFrameDesc::view_proj 是同一份快照**，
  // 否则覆盖层与积木会错位（错位的画面看起来"只是有点歪"，不会报错）。
  bool project_with_frame_matrix = true;
};

// 一帧的渲染输入。
struct StackFrameDesc {
  const IBlockSet* set = nullptr;
  // 列主序 view_proj（16 个 float），由调用方从 ICamera::view_proj_matrix() 取。
  //
  // 为什么传裸 float[16] 而不是 ICamera*/CameraSnapshot：
  //   - 传 ICamera* 会让渲染器在录制期间再读相机状态，而「录制用的矩阵」与
  //     「出图时的矩阵」必须是同一份快照，否则 golden 会随相机微调而飘；
  //   - CameraSnapshot 只含 desc/pose，**取不出矩阵**（view_proj_matrix()
  //     是 ICamera 的虚函数），自己重算一遍等于在 L4 里复制投影数学 ——
  //     两处一旦分叉就是"画面歪了但没报错"，正是 D8 要杜绝的那类假绿。
  //   传数组最笨也最不会错：调用方在哪一刻取值，渲染用的就是哪一刻的值。
  float view_proj[16] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                         0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
  std::uint32_t width = 640;
  std::uint32_t height = 480;
  float clear_color[4] = {0.06f, 0.07f, 0.10f, 1.0f};
  // **诊断开关，只为一件事存在**：反证 pass 顺序契约。
  //   默认 false = 生产路径（表面在前、内面在后）。
  //   true  = 故意把内面放到前面，供 `InnerPassIsAfterSurfacePass` 证明
  //           "内面先画等于白画"。CLI **不**暴露它（生产代码不该有关掉正确性的旋钮）。
  bool debug_inner_pass_first = false;
  // 内面 pass 开关。关掉只为 `InnerPassChangesPixels` 的对照图。
  bool inner_pass_enabled = true;

  // 实例写入顺序（设计 §10.6.1 / D-VX-5）。
  //
  // surface_order 默认 Index：表面 pass 开深度测试，顺序不影响结果。
  // inner_order 默认 **YZX**：内面 pass 是 depthTest=OFF，绘制顺序**就是**
  // 遮挡顺序，所以必须由空间坐标决定而不是由落笔历史决定。
  // 把 inner_order 改成 Index 只为让用例"产出不同实例顺序"可被观测，
  // 生产路径不给关掉它的旋钮。
  CellOrder surface_order = CellOrder::Index;
  CellOrder inner_order = CellOrder::YZX;

  // 网格指示接线（需求「带网格指示」）。`overlay.overlay == nullptr` = 这一帧不画网格，
  // 是**显式**的"不画"，不是"忘了接"：用例 `StackGridOverlay.OverlayIsOptIn` 钉住默认值。
  StackFrameOverlay overlay{};
};

// 一帧的渲染统计 —— 供用例断言"没有偷偷多发 draw / 多提交"。
struct StackRenderStats {
  std::uint32_t draw_calls = 0;      // 期望恰好 2（表面 1 + 内面 1）
  std::uint32_t begin_passes = 0;    // 期望恰好 2
  std::uint32_t submit_count = 0;    // 期望恰好 1（一帧一次 submit）
  std::uint32_t instances = 0;       // 实例数 = 块数（或其 pad 后数量）
  std::uint32_t pipeline_binds = 0;
  // 单实例旁路是否被触发（见下方 M9 裁定）。默认策略是 **pad**，所以恒 0。
  std::uint32_t single_instance_bypass = 0;
  bool instance_buffer_bound = true;
  // 内面 pass 这一帧实际用的实例顺序（D-VX-5 的观测点）。
  CellOrder inner_order = CellOrder::YZX;
  // 本帧内面 pass 是否真的绑了**独立**的内面实例缓冲
  // （false = 两种顺序相同，复用了表面缓冲）。
  bool inner_buffer_distinct = false;
  // 网格指示是否真的被收进了调用方的覆盖层收集器（需求「带网格指示」的观测点）。
  // false = 这一帧没配 StackFrameDesc::overlay，或 collect 走了空路径。
  bool overlay_collected = false;
  std::uint32_t overlay_lines = 0;
  std::uint32_t overlay_rects = 0;
};

// 实例缓冲的最小实例数。
//
// **M9 裁定：选 pad，不选旁路。**
// 理由：`primitive_renderer.cpp:102` 只在 `instance_count > 1` 时绑定 binding 1。
// 旁路方案要求单实例批次走一条**非实例化**的顶点路径，那需要第二个顶点布局
// （没有 per-instance model 属性，就得靠 push constant 传矩阵），于是
//   (a) 多一条管线 → 多一个 pipeline_hash → draw 数从 2 变 3，破坏本阶段的
//       "恰好 2 个 drawcall" 契约；
//   (b) 两种画法各有一套着色器路径，像素结果可能不一致 → 与双 pass 的顺序
//       契约纠缠，排查成本高。
// pad 方案只多一条退化实例（全零矩阵 → 投影后零面积 → 不产生任何像素），
// 代价是一次多余的顶点着色 × 36 顶点，换来的是**只有一条画法**。
// 因此本实现恒定 pad 到 >= 2，并且用例断言"永不出现
// instance_buffer != nullptr && instance_count == 1"。
inline constexpr std::uint32_t kMinInstanceCount = 2;

// 渲染器：只**录制**命令，不提交 —— 一帧一次 `submit_command_list` 由调用方做
// （与 L3 的 IPrimitiveRenderer 同一纪律，UIG1/UIG2）。
class IStackRenderer {
 public:
  virtual ~IStackRenderer() = default;

  // 建两条管线 + 立方体网格。失败即整帧不可用（不许"关掉内面 pass 降级"）。
  // device 由调用方持有，渲染器只借用。
  virtual StackStatus create_passes(IRenderDevice* device, std::uint32_t samples) = 0;
  virtual const StackPassPair& passes() const = 0;

  // 把 set 烘成实例缓冲（每块一条 InstanceAttribs，末端按 M9 pad 到 >= 2）。
  //
  // **同时烘两份**（§10.6 + D-VX-5）：
  //   surface_instances_ —— 真源下标序（CellOrder::Index）
  //   inner_instances_   —— `(y,z,x)` 升序稳定排序（CellOrder::YZX）
  // 两者各有一个 Buffer。表面 pass 绑第一份、内面 pass 绑第二份。
  // 顺序相同时（例如本来就按 yzx 插入）**复用同一个 Buffer**，省一次创建。
  virtual StackStatus build_instances(const GridSpec& grid, const IBlockSet& set) = 0;

  // 两种顺序各写成了什么格子序列 —— 供用例直接比对顺序本身，
  // 而不必从像素反推。
  virtual const std::vector<Cell>& surface_cells() const = 0;
  virtual const std::vector<Cell>& inner_cells() const = 0;

  // 两种顺序的实例数据哈希。相等 ⇔ 两份实例缓冲逐字节相同。
  virtual std::uint64_t surface_instance_hash() const = 0;
  virtual std::uint64_t inner_instance_hash() const = 0;

  // 实例数量（含 pad）。用例据此断言 pad 生效、且永不等于 1。
  virtual std::uint32_t instance_count() const = 0;

  // 录制一帧：2 次 begin_pass、2 次 draw、每 pass 一次 end_pass。
  // **不** submit —— 提交权在调用方（UIG1/UIG2：一帧一次提交）。
  virtual StackStatus record_frame(res::ICommandList& cmd, IRenderTarget* target,
                                   const StackFrameDesc& desc) = 0;

  virtual const StackRenderStats& stats() const = 0;
  virtual void reset_stats() = 0;
};

std::unique_ptr<IStackRenderer> create_stack_renderer();

}  // namespace magicblocks::features::stack
