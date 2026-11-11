// MagicBlocks L3 —— 网格（spec §2 第 1 步"定能力"、§13 里程碑 M2 第 9 项）
//
// 职责：给"画一个东西"提供几何。L3 只负责**程序化生成**与**持有 GPU 缓冲**，
// 不负责变换（那是 TransformComp + 实例属性的事）也不负责材质。
//
// 为什么 L3 自带 VertexFormat 而不复用 L1 的：
//   L1 的 VertexLayout 是**管线输入描述**（bindings/attributes/locations），
//   带着"这个管线要什么"的语义；而 MeshData 是**数据本身**（紧凑的字节块）。
//   混用会让人误以为 MeshData 里的顶点布局可以随便改 —— 其实 L3 图元生成的
//   顶点布局必须和 shader_library 里 kStandardPbr 的管线布局严格一致，
//   否则是运行期静默错位。所以这里把"数据"和"管线要什么"分成两个类型，
//   由 mesh_vertex_layout() 显式桥接，改一边就必须回头核对另一边。
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "mb_internal/magicblocks/primitives/types.h"
#include "mb_internal/magicblocks/resources/buffer.h"
#include "mb_internal/magicblocks/resources/pipeline.h"

namespace magicblocks::primitives {

using device::u32;
using device::u64;
using resources::IBuffer;
using resources::VertexLayout;

// 网格句柄 —— MeshId / kInvalidMeshId 定义在 types.h（合批排序键也要用，
// 放在 types.h 才能让 batch.h 不必依赖 mesh.h，保持二者解耦）。

// ---------------------------------------------------------------- 顶点格式
//
// 刻意**只有一种** L3 顶点格式，而不是让每个图元自定义：
//   合批要求"同一次 draw 的所有实例共享同一条顶点流布局"，
//   布局一多，排序键就得加一维，管线缓存也会炸。
//   目前一种布局 + 实例属性，足够全部程序化图元。
//
// 48 字节/顶点，三段 16B 对齐块：
//   [0,16)  position float3(12) + _pad0(4)  → 可整块当 vec4 取
//   [16,32) normal   float3(12) + _pad1(4)  → 同上
//   [32,48) color ubyte4norm(4) + uv float2(8) + _pad2(4)
//
// 为什么位置放寄存器低位：GPU 端最常见的读取是 position 与 normal，
// 让它们各自落在 16B 边界上，驱动能让一次 128-bit load 取完一个属性。
// 把 color 和 uv 挤在同一块是刻意的 —— 它们一般只在片段着色器用，对对齐不敏感。
struct Vertex {
  float position[3] = {0.0f, 0.0f, 0.0f};
  float _pad0 = 0.0f;
  float normal[3] = {0.0f, 1.0f, 0.0f};
  float _pad1 = 0.0f;
  std::uint8_t color[4] = {255, 255, 255, 255};  // RGBA8 归一化
  float uv[2] = {0.0f, 0.0f};
  float _pad2 = 0.0f;
};
static_assert(sizeof(Vertex) == 48, "Vertex 必须是 48 字节,合批与实例化 stride 依赖它");
static_assert(alignof(Vertex) == 4, "Vertex 保持 4 字节对齐,避免 std::vector 过度对齐");

// L3 标准顶点布局 —— 必须与 shader_library 里 kStandardPbr 的管线一致。
//
// location 分配（跳号是移动端驱动的性能陷阱，spec L1 已明确禁止）：
//   0 = position(float3)  1 = normal(float3)  2 = color(ubyte4norm)  3 = uv(float2)
//
// binding 1 留给 **实例属性**（step rate = instance），
// 它的 stride 就是 InstanceAttribs（见 batch.h），由渲染器绑定，不在 mesh 里。
VertexLayout mesh_vertex_layout();

// ---------------------------------------------------------------- 数据 / 接口

// CPU 侧网格数据。图元生成器产出它，IMeshFactory 负责上传成 IMesh。
struct MeshData {
  std::vector<Vertex> vertices;
  std::vector<std::uint32_t> indices;
  Box bounds = {};  // 局部空间 AABB，供拾取与视锥剔除

  bool empty() const { return vertices.empty() || indices.empty(); }
  void clear() {
    vertices.clear();
    indices.clear();
    bounds = Box{};
  }
};

// GPU 侧网格。不可变（L3 图元都是静态几何）。
//
// 为什么不可变：
//   合批器按 MeshId 排序并假设"同一 MeshId 的几何在整帧内一致"。
//   可变网格得为每次修改分配新 MeshId，否则排序键失效。L3 场景里
//   没有变形需求，保持不可变更简单也更省。
class IMesh {
 public:
  IMesh() = default;
  IMesh(const IMesh&) = delete;
  IMesh& operator=(const IMesh&) = delete;
  virtual ~IMesh() = default;

  virtual MeshId id() const = 0;

  virtual const IBuffer* vertex_buffer() const = 0;
  virtual const IBuffer* index_buffer() const = 0;

  virtual u32 vertex_count() const = 0;
  virtual u32 index_count() const = 0;
  // 索引类型固定在 32 位，避免 16 位索引在 >65535 顶点网格上的切换分支。
  // 保留这个虚函数是为了将来支持 u16 压缩时不必改调用方。
  virtual bool indices_are_u32() const { return true; }

  virtual const Box& bounds() const = 0;

  // 网格在 GPU 上占的字节数（诊断 / 预算用）。
  virtual u64 gpu_bytes() const = 0;
};

// 创建 IMesh：上传 MeshData 到 device 无关的 staging→GPU 路径。
// device 参数用不透明指针避免 L3 头直接暴露 L1 的 IRenderDevice 具体实现细节。
// 注意：这个函数**不做**同步等待；需要立即可见请调用方在帧首 flush。
std::unique_ptr<IMesh> create_mesh(resources::IBuffer* vertex_buffer,
                                   resources::IBuffer* index_buffer, MeshId id,
                                   const MeshData& data);

}  // namespace magicblocks::primitives
