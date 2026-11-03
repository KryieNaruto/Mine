// MagicBlocks SDK —— L1 管线(R2 / R4)
//
// 设计约束(spec R2 / R4 / C4 / G1 / G2 / P12, plans-l1 §3):
//   - **无 geometry stage 字段(R2 硬性要求)**:PipelineDesc 里根本没有 geometry
//     着色器槽,ShaderStage 枚举也只有 Vertex/Fragment/Compute(见 handles.h)。
//     这是**结构性保证**:想接几何着色器连字段都写不出来,不需要运行期校验分支。
//   - **不支持线框多边形模式(R2 明文)**:RasterState 里没有 polygon_mode 字段。
//     「无深度虚线/网格指示」这类需求由 L3 的 IOverlay2D(屏幕空间线)承担,
//     不靠光栅器线框 —— 线框在移动端 TBR 上代价高且行为有差异。
//   - **G2 无 GS 的替代方案**:一切"造几何"都走实例化绘制(顶点着色器按
//     gl_InstanceIndex 取参数)+ push constant/UBO 参数表。因此 PipelineDesc 里
//     实例化相关只体现在顶点布局的逐实例属性(VertexInputBinding::instanced),
//     没有"每实例几何展开"之类的字段。
//   - **P12 通用状态**:DepthState{depthTest, depthWrite, depthOp, depthConstWrite}。
//     叠叠乐"永远显示内面" = depthTest=false + depthWrite=true(depthConst=near)
//     + CullMode::None,必须能用这一个结构体原样表达出来。
//   - 默认值按 plans-l1 用例 Pipeline.DepthStateDefaults:
//     depthTest=true, depthWrite=true, depthOp=Less。
//   - **R4 描述符策略**:push constant 优先 + 少量 UBO 页 + 一张动态色表纹理;
//     本轮不建描述符集 → IPipeline::descriptor_set_count() 必须为 0
//     (Pipeline.UsesPushConstantsOnly)。
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "magicblocks/device/result.h"
#include "magicblocks/device/types.h"
#include "magicblocks/resources/handles.h"

namespace magicblocks::resources {

using device::Format;
using device::Result;
using device::Status;
using device::u32;
using device::u64;

// ---------------------------------------------------------------- 顶点布局

// 顶点属性格式。故意**不覆盖整张 GPU 格式表**:只列程序化几何(图元/挤出/实例化)
// 实际用得到的,新增格式必须同时改 backend 的映射,不会"悄悄支持但没人测过"。
enum class VertexFormat : std::uint8_t {
  Float = 0,
  Float2 = 1,
  Float3 = 2,
  Float4 = 3,
  UByte4Norm = 4,
  Short2 = 5,
  Short4 = 6,
};

// VertexFormat → 字节数(布局校验与 stride 计算用)。
inline u32 vertex_format_size(VertexFormat f) {
  switch (f) {
    case VertexFormat::Float:
      return 4;
    case VertexFormat::Float2:
      return 8;
    case VertexFormat::Float3:
      return 12;
    case VertexFormat::Float4:
      return 16;
    case VertexFormat::UByte4Norm:
      return 4;
    case VertexFormat::Short2:
      return 4;
    case VertexFormat::Short4:
      return 8;
  }
  return 0;
}

// 一个属性。location 与 GLSL 的 layout(location = N) 一一对应,
// **不允许跳号**(跳号在移动端驱动上是性能陷阱,创建时直接 InvalidArgument)。
struct VertexAttribute {
  u32 location = 0;
  u32 binding = 0;                       // 属于哪个 VertexInputBinding
  VertexFormat format = VertexFormat::Float3;
  u32 offset = 0;                        // 绑定的字节偏移
};

// 一个顶点/实例流。instanced=true 即 G2 的实例化属性来源
// (gl_InstanceIndex 取参数),而**不是**每实例几何展开。
struct VertexInputBinding {
  u32 binding = 0;
  u32 stride = 0;          // 每顶点/每实例步进字节数,必 > 0
  bool instanced = false;
};

struct VertexLayout {
  std::vector<VertexInputBinding> bindings;
  std::vector<VertexAttribute> attributes;

  bool empty() const { return bindings.empty() || attributes.empty(); }
};

// ---------------------------------------------------------------- 着色器(R6)

// 一份 SPIR-V 字节码(由 GLSL → glslc 内嵌生成,R6;运行期不读文件)。
struct ShaderModule {
  ShaderStage stage = ShaderStage::Vertex;
  const std::uint8_t* spirv = nullptr;  // 指向引擎内嵌的静态数据,不转移所有权
  std::size_t size_bytes = 0;           // 必须是 4 的倍数(SPIR-V 字对齐)

  bool valid() const {
    return spirv != nullptr && size_bytes >= 4 && (size_bytes % 4) == 0;
  }
};

// ---------------------------------------------------------------- 混合 / 深度 / 光栅

enum class BlendFactor : std::uint8_t {
  Zero = 0,
  One = 1,
  SrcAlpha = 2,
  OneMinusSrcAlpha = 3,
  DstAlpha = 4,
  OneMinusDstAlpha = 5,
  SrcColor = 6,
  OneMinusSrcColor = 7,
};

enum class BlendOp : std::uint8_t {
  Add = 0,
  Subtract = 1,
  ReverseSubtract = 2,
  Min = 3,
  Max = 4,
};

struct BlendState {
  bool enabled = false;                 // 默认关闭:纯色积木/离屏合成走不混合路径
  BlendFactor src_color = BlendFactor::SrcAlpha;
  BlendFactor dst_color = BlendFactor::OneMinusSrcAlpha;
  BlendOp color_op = BlendOp::Add;
  BlendFactor src_alpha = BlendFactor::One;
  BlendFactor dst_alpha = BlendFactor::OneMinusSrcAlpha;
  BlendOp alpha_op = BlendOp::Add;
};

// P12:带常量深度写的深度状态(叠叠乐内面渲染的关键能力)。
// 语义:depthTest=false 时不比较;depthWrite=true 且 depthConstWrite=true 时
// 把整片区域写成 depth_const 值,而不是写片元自身深度。
struct DepthState {
  bool depthTest = true;
  bool depthWrite = true;
  CompareOp depthOp = CompareOp::Less;
  bool depthConstWrite = false;
  float depth_const = 1.0f;   // depthConstWrite=true 时写入的深度值(near=0,far=1)
};

enum class CullMode : std::uint8_t {
  None = 0,      // 叠叠乐"背面可见"
  Front = 1,
  Back = 2,
};

enum class FrontFace : std::uint8_t {
  CounterClockwise = 0,
  Clockwise = 1,
};

// R2 的光栅状态:**只有**剔除与深度偏移。
// 此处**不要**加 polygon_mode:spec R2 明文「不支持线框多边形模式」,
// 加一个 "Wireframe" 枚举值就等于把这条禁令架空了。
struct RasterState {
  CullMode cull = CullMode::Back;
  FrontFace front_face = FrontFace::CounterClockwise;
  bool depth_bias_enabled = false;
  float depth_bias_constant = 0.0f;
  float depth_bias_slope = 0.0f;
  float depth_bias_clamp = 0.0f;
};

// ---------------------------------------------------------------- 图元(R2)

// R2:primitive(Rect/Line)。Rect 用于 2D 画布/印章底板的全屏矩形,
// Line 用于线框盒/网格指示(配合 Instanced 顶点布局,G2)。
enum class PrimitiveType : std::uint8_t {
  TriangleList = 0,
  LineList = 1,
};

// ---------------------------------------------------------------- 描述(R2)

struct PipelineDesc {
  VertexLayout vertexLayout{};
  std::vector<ShaderModule> stages;   // 必须含 Vertex(或 Compute);见下方校验规则
  BlendState blend{};
  DepthState depth{};
  RasterState raster{};
  u32 samples = 1;                          // MSAA 采样数,须在 DeviceCaps 支持下
  PrimitiveType primitive = PrimitiveType::TriangleList;
  bool compute = false;                     // true → 走计算管线(stages 只允许 Compute)
  // 资源布局:(kind, 字节数)。本轮只做 push constant 与 UBO 页,
  // 不建描述符集(R4),因此这里只声明需要多少 push constant 空间。
  u32 push_constant_bytes = 0;              // 必须 <= DeviceCaps.max_push_constant_size
};

// 校验规则(由 backend 实现,创建时调用):
//   - stages 为空 → InvalidArgument(Pipeline.RejectsEmptyStages)
//   - 图形管线缺 Vertex 或 Fragment → InvalidArgument
//   - 图形管线出现 Compute stage → InvalidArgument
//   - 计算管线出现 Vertex/Fragment → InvalidArgument
//   - push_constant_bytes 超 DeviceCaps.max_push_constant_size → Unsupported
//   - samples 不在 DeviceCaps.supported_msaa_levels → Unsupported
// 注:stage 枚举里没有几何/细分,所以"拒绝几何着色器"不需要运行期分支。

// ---------------------------------------------------------------- 接口(R2)

class IPipeline {
 public:
  IPipeline(const IPipeline&) = delete;
  IPipeline& operator=(const IPipeline&) = delete;
  virtual ~IPipeline() = default;

  virtual const PipelineDesc& desc() const = 0;
  virtual bool is_compute() const = 0;

  // R4:本轮描述符策略为 push constant 优先,**不建描述符集** →
  // 恒返回 0,这是 Pipeline.UsesPushConstantsOnly 的断言点。
  virtual u32 descriptor_set_count() const = 0;
  // 实际生效的 push constant 字节数(<= desc().push_constant_bytes)。
  virtual u32 push_constant_bytes() const = 0;

  // 管线内部缓存句柄(供 backend 侧命令流快照;公共层不解释它)。
  virtual u64 hash() const = 0;

 protected:
  IPipeline() = default;
};

}  // namespace magicblocks::resources
