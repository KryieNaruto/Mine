// MagicBlocks L3 —— 图元渲染器（spec §13 M1 第 6 项 / M2 第 11 项）
//
// 职责：把"合批器的输出"翻译成 L2 命令流里的 draw 调用。
// 它是 L3 唯一**会碰 GPU** 的组件，其余（camera/material/mesh/batch/world/painter）
// 全部是纯 CPU 可单测的。
//
// 为什么单独一层而不是让 IBatchBuilder 直接出 draw：
//   合批是"排序与合并"的纯数据变换，能脱离设备测试（已有 9 个用例）；
//   渲染器要绑定管线、绑缓冲、推 push constant，必须持有设备资源。
//   混在一起会让合批器不可测，也让 M3 的 gizmo 无法复用合批逻辑。
//
// 命令模式约定（spec §11 / L2 的 R3）：
//   渲染器**只调用 ICommandList 的录制接口**，不自己 submit。
//   帧的提交由调用方（CLI / 宿主）统一做 —— 保证"一帧一次 submit"（UIG2 门控）。
#pragma once

#include <memory>
#include <vector>

#include "magicblocks/primitives/alloc.h"
#include "magicblocks/primitives/batch.h"
#include "magicblocks/primitives/camera.h"
#include "magicblocks/primitives/material.h"
#include "magicblocks/primitives/mesh.h"
#include "magicblocks/primitives/painter.h"
#include "magicblocks/primitives/types.h"
#include "magicblocks/resources/command.h"
#include "magicblocks/resources/pipeline.h"

namespace magicblocks::primitives {

using device::Status;

// 一帧要画的东西。全部由调用方填好，渲染器只读。
struct PrimitiveFrameDesc {
  u32 width = 1280;
  u32 height = 720;
  // 场景 pass 的清理色。alpha 也清（离屏目标需要确定的 alpha）。
  float clear_color[4] = {0.05f, 0.08f, 0.20f, 1.0f};
  bool clear_depth = true;
  // 视图/投影已由 ICamera 算好；这里用快照避免渲染器再读相机状态。
  CameraSnapshot view_proj{};
  // 是否有深度附件（2D 画笔场景可以没有）。
  bool has_depth = true;
  // 不透明 pass 在前、透明 pass 在后（D-L3-3）。
  bool draw_transparent_pass = true;
  // 画笔笔迹：非空则在本帧额外画一次笔画（屏幕空间，无深度）。
  const StrokeGeometry* strokes = nullptr;
};

// 渲染统计 —— 用于断言"渲染器没有偷偷多发 draw"。
struct RenderStats {
  u32 scene_draws = 0;      // 场景实例化 draw 数
  u32 stroke_draws = 0;     // 笔画 draw 数（0 或 1）
  u32 pipeline_binds = 0;   // 管线绑定次数
  u32 vertex_binds = 0;
  u32 index_binds = 0;
  u32 push_constant_sets = 0;
  // 接线错误计数：批次引用了未注册的网格/管线。
  // 单独计数而不是立刻返回错误 —— 一帧里可能有成百上千个这样的批次，
  // 每个都返回会淹没真正的失败原因。调用方在帧末检查。
  u32 missing_meshes = 0;
  u32 missing_pipelines = 0;
  Status last_status = Status::Ok_();
};

class IPrimitiveRenderer {
 public:
  IPrimitiveRenderer() = default;
  IPrimitiveRenderer(const IPrimitiveRenderer&) = delete;
  IPrimitiveRenderer& operator=(const IPrimitiveRenderer&) = delete;
  virtual ~IPrimitiveRenderer() = default;

  // 注册网格。渲染器不拥有网格生命周期，只记 MeshId → IMesh* 的映射。
  // 同一 MeshId 重复注册视为错误（返回 false），避免两个网格争用同一排序键。
  virtual bool register_mesh(const IMesh* mesh) = 0;
  virtual void unregister_mesh(MeshId id) = 0;
  virtual const IMesh* find_mesh(MeshId id) const = 0;

  // 注册管线。pipeline_key 是合批排序键里的 pipeline_hash，
  // 必须与 DrawRequest::pipeline_hash 用同一套取值 —— 这是渲染器
  // 唯一容易被接错的地方，所以显式传入而不是从管线推导。
  virtual bool register_pipeline(u64 pipeline_key, resources::IPipeline* pipeline) = 0;
  virtual resources::IPipeline* find_pipeline(u64 pipeline_key) const = 0;

  // 设置实例属性缓冲（IBatchPool 的某个槽）。实例化 draw 的 binding 1 从这里取。
  virtual void set_instance_buffer(resources::IBuffer* buffer) = 0;

  // 把一帧的合批结果 + 笔画录进命令列表。
  // **不**调用 submit —— 调用方负责一帧一次提交。
  virtual Status record_frame(resources::ICommandList& cmd, const IBatchBuilder& batches,
                              const PrimitiveFrameDesc& desc) = 0;

  virtual const RenderStats& stats() const = 0;
  virtual void reset_stats() = 0;
};

std::unique_ptr<IPrimitiveRenderer> create_primitive_renderer();

}  // namespace magicblocks::primitives
