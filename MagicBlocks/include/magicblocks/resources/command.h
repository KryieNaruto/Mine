// MagicBlocks SDK —— L1 命令列表(R3)
//
// 设计约束(spec R3 / §5.2 / §6.3 / G5, plans-l1 §3):
//   - **R3 的 11 类核心命令** + **P0 的 1 个纹理采样绑定(裁定 X1,共 12 个方法)**:
//     bind_pipeline / bind_vertex_buffer / bind_index_buffer /
//     draw / draw_indexed / draw_instanced / push_constants / set_viewport /
//     set_scissor / begin_pass / end_pass,加上第 12 个 bind_sampled_texture。
//     R3 列的 11 个是**能力**;X1 裁定把"绑定采样器"作为 bindPipeline 职责的
//     自然扩展加进来(纹理是逐 draw 的,塞进 RenderPassDesc 会埋坑)。
//     除这 12 个之外,多余的便捷命令一律不加 —— 每多一条就要在 L2 命令流
//     格式里多一个 op 编号。
//   - **命令流记录偏移而不是指针(M3 / spec §5.2)**:每个接受资源的命令都收
//     ResourceId(值语义),录制层把它压成紧凑整数存进命令流,回放时同一
//     page_id/offset 指向同一片内存。
//   - **单队列单线程录制(G5)**:不做命令缓冲并发录制保护;跨帧复用由
//     IFrameSync 的槽位 fence 负责。
//   - begin_pass / end_pass 的 render area 必须落在 IRenderTarget 的附件内,
//     越界 → InvalidArgument(离屏是 golden 的唯一来源,不允许"画到边界外"被静默裁剪)。
#pragma once

#include <cstdint>

#include "magicblocks/device/render_target.h"
#include "magicblocks/device/result.h"
#include "magicblocks/device/types.h"
#include "magicblocks/resources/handles.h"

namespace magicblocks::resources {

using device::Extent2D;
using device::Format;
using device::IRenderTarget;
using device::Result;
using device::Status;
using device::u32;
using device::u64;

class IBuffer;
class ICommandList;
class IPipeline;

// ---------------------------------------------------------------- 附件与 pass(R3)

// 一次 pass 要写哪些附件。clear=false 时保留目标已有内容(离屏叠加/合成常用),
// clear=true 且 clear_color_rgba 有效时先清色。
struct RenderPassDesc {
  IRenderTarget* target = nullptr;          // 必填;nullptr → InvalidArgument
  bool clear_color = true;
  float clear_color_rgba[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  bool clear_depth = true;
  float clear_depth_value = 1.0f;
  // 只画目标的一个矩形区域(默认整图)。空矩形会被当成整图。
  // 越出 target->extent() → InvalidArgument。
  u32 area_x = 0;
  u32 area_y = 0;
  u32 area_width = 0;    // 0 = 用 target->extent().width
  u32 area_height = 0;   // 0 = 用 target->extent().height
};

// ---------------------------------------------------------------- 绘制参数

// 视口:像素坐标(原点左上),depth 区间 [min_depth, max_depth]。
struct Viewport {
  float x = 0.0f;
  float y = 0.0f;
  float width = 0.0f;
  float height = 0.0f;
  float min_depth = 0.0f;
  float max_depth = 1.0f;
};

// 裁剪矩形(像素坐标)。
struct ScissorRect {
  u32 x = 0;
  u32 y = 0;
  u32 width = 0;
  u32 height = 0;
};

// draw / draw_indexed / draw_instanced 共用的计数参数。
// instance_count = 1 时是普通绘制;> 1 即 G2 的实例化路径。
struct DrawArgs {
  u32 vertex_count = 0;
  u32 index_count = 0;
  u32 instance_count = 1;
  u32 first_vertex = 0;
  u32 first_index = 0;
  std::int32_t vertex_offset = 0;  // 索引相对顶点缓冲的基址偏移(有符号)
  u32 first_instance = 0;
};

// 索引缓冲的索引宽度。
enum class IndexFormat : std::uint8_t {
  UInt16 = 0,
  UInt32 = 1,
};

// ---------------------------------------------------------------- 接口(R3)

class ICommandList {
 public:
  ICommandList(const ICommandList&) = delete;
  ICommandList& operator=(const ICommandList&) = delete;
  virtual ~ICommandList() = default;

  // R3 命令 1:绑定管线。绑定前 draw → InvalidArgument(R3 的"录制期校验")。
  virtual Status bind_pipeline(IPipeline* pipeline, u64 pipeline_hash) = 0;

  // R3 命令 2 / 3:绑定顶点/索引缓冲,参数是句柄(偏移)而不是指针(M3)。
  virtual Status bind_vertex_buffer(const ResourceId& buffer, u32 binding = 0,
                                    u64 offset = 0) = 0;
  virtual Status bind_index_buffer(const ResourceId& buffer, IndexFormat format,
                                   u64 offset = 0) = 0;

  // R3 命令 4 / 5 / 6:绘制。
  virtual Status draw(const DrawArgs& args) = 0;
  virtual Status draw_indexed(const DrawArgs& args) = 0;
  virtual Status draw_instanced(const DrawArgs& args) = 0;

  // R3 命令 7:push constant 更新(R4 的描述符策略是 push constant 优先)。
  // stage_mask 只允许 ShaderStage 的位组合;本轮只支持 Vertex/Fragment。
  // offset + size 超出管线 push_constant_bytes → InvalidArgument。
  virtual Status push_constants(u32 stage_mask, u32 offset, u32 size_bytes,
                                const void* data) = 0;

  // R3 命令 8 / 9:视口与裁剪。
  virtual Status set_viewport(const Viewport& viewport) = 0;
  virtual Status set_scissor(const ScissorRect& rect) = 0;

  // P0 命令 12(X1 裁定):把一张可采样 2D 纹理连同它的采样器绑到 slot。
  //
  // 契约:
  //   - 当前绑定的管线必须声明过 PipelineDesc::sampled_texture_slots > slot,
  //     否则 → InvalidArgument(不静默丢弃,否则画面"没纹理"且不报错)。
  //   - 未 bind_pipeline 就调用 → InvalidArgument;不在 pass 内 → InvalidArgument。
  //   - texture 必须是 TextureUsage::Sampled 的 2D 纹理;sampler 必须是采样器句柄。
  //     句柄非法/类型不对 → InvalidArgument。
  //   - **计入 command_count()**(与 bind_pipeline 同口径,见 command_count 的注释)。
  //   - 描述符集布局固定 set=0 / binding=slot。
  virtual Status bind_sampled_texture(const ResourceId& texture, const ResourceId& sampler,
                                      u32 slot) = 0;

  // P0 命令 12 的**重新同步**(R1 扩展,立方体贴图落地时补上)。
  //
  // 为什么要这条:句柄 → 指针的反查表在命令列表创建时就固化了,而资源可能是
  // **之后**才创建的(典型顺序:create_command_list() → create_texture() →
  // bind_sampled_texture)或者由别的会话归属。没有这条,设备态只能报
  // "未登记该纹理/采样器对象",调用方被迫去调 backend 私有入口 —— 那条路
  // L3 根本看不到。
  //
  // 契约:
  //   - 只对**本设备**创建的资源生效(按设备的登记表刷新);纯软件态是空操作;
  //   - 不改变命令流(不是一条命令,不计入 command_count),可以在任意时刻调用;
  //   - 设备实现可以自动在 begin_pass() 时调用一次(本轮就是这样),
  //     所以普通调用方**通常不需要**显式调它。
  virtual Status sync_resources() = 0;

  // R3 命令 10 / 11:pass 边界。未在 pass 内录制绘制命令 → InvalidArgument;
  // 未 end_pass 就 submit → InvalidArgument(录制期校验,不放过半个 pass)。
  virtual Status begin_pass(const RenderPassDesc& pass) = 0;
  virtual Status end_pass() = 0;

  // 录制状态查询(用于 Command.RecordsAllOps 的命令计数断言)。
  virtual bool recording() const = 0;
  virtual bool in_pass() const = 0;
  // 已录制的命令条数。口径:上面 12 类命令每成功调用一次即 +1
  // (含 begin_pass/end_pass;录制失败的命令不计)。
  // bind_sampled_texture **计入**(与 bind_pipeline 同口径,裁定 X1)。
  virtual u32 command_count() const = 0;

  // 复位:丢弃所有已录制命令并回到录制初态。
  virtual void reset() = 0;

  // 提交本命令列表到单队列并等待完成(G5 单队列;离屏 CI 路径要确定性的完成语义)。
  // 未 end_pass → InvalidArgument。提交后命令列表回到可复用状态。
  virtual Status submit_and_wait() = 0;

 protected:
  ICommandList() = default;
};

}  // namespace magicblocks::resources
