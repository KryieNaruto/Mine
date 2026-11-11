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
#include <memory>
#include <vector>

#include "mb_internal/magicblocks/device/result.h"
#include "mb_internal/magicblocks/device/types.h"
#include "mb_internal/magicblocks/resources/handles.h"

// IPipelineFactory 用到 device::IRenderDevice&,这里只声明指针/引用,前向声明即可,
// 避免 resources/* → device/render_device.h → resources/* 的循环包含。
namespace magicblocks::device {
class IRenderDevice;
}

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

// ---------------------------------------------------------------- 附件形状(G8)
//
// 2026-11-11 (t13/g8): the attachment shape now lives here as a plain value.
// 为什么形状要住进公共头:
//   L3 的 IShaderLibrary 需要**按目标形状**派生管线(离屏带深度 / 交换链 BGRA8 /
//   MSAA 采样数),否则"带深度的目标永远与管线 pass 不兼容"。此前 L3 的做法是
//   #include "backend/vulkan/vulkan_pipeline.h" 拿 backend::AttachmentShape ——
//   那是 L3 反向依赖 L0 实现,G8 门控直接判红。
//   解法:形状降级成**纯值**(device::Format + bool + u32,零 Vk*),住在公共头;
//   真正建管线的后端入口经 resources::PipelineFactoryTag 间接调用(由后端注册)。
struct AttachmentShape {
  // 默认 = 不带深度的 RGBA8 单采样:与公共 create_pipeline(desc) 的既有行为一致,
  // 因此 PipelineDesc 默认构造出来的 desc.attachment 不会改变任何既有管线的形状。
  Format color_format = Format::RGBA8_UNORM;
  bool has_depth = false;
  Format depth_format = Format::D16_UNORM;
  u32 samples = 1;
  // true = 颜色附件是交换链图像:pass 的 initial/finalLayout 必须是 PRESENT_SRC_KHR。
  bool from_swapchain = false;
  // 本次 pass 是否清颜色 / 清深度。管线自带 pass 恒为 CLEAR —— 它们**参与**
  // 形状兼容性比较(vulkan_device_frame.cpp 的 pass_compatible),否则"不清"会被
  // 静默忽略(F-VX 的双 pass 就是靠这个)。
  bool clear_color = true;
  bool clear_depth = true;

  // 值语义比较:后端按形状做 pass 兼容性判定(逐项比较),所以必须可等值比较。
  // 字段顺序与 backend 侧的同名结构**逐项一致**,便于两侧直接互相转换。
  bool operator==(const AttachmentShape& o) const {
    return color_format == o.color_format && has_depth == o.has_depth &&
           depth_format == o.depth_format && samples == o.samples &&
           from_swapchain == o.from_swapchain && clear_color == o.clear_color &&
           clear_depth == o.clear_depth;
  }
  bool operator!=(const AttachmentShape& o) const { return !(*this == o); }
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
  // 资源布局:(kind, 字节数)。默认只做 push constant 与 UBO 页,不建描述符集(R4):
  // sampled_texture_slots == 0 时这里只声明需要多少 push constant 空间。
  u32 push_constant_bytes = 0;              // 必须 <= DeviceCaps.max_push_constant_size
  // P0 纹理采样(X1 裁定):声明多少个可采样 2D 纹理槽。
  //   0(默认)→ **不建描述符集**,descriptor_set_count() 返回 0,既有管线与
  //             5 张 golden 逐字节不变(这是 P0-1 的验收口径)。
  //   >0     → 建**恰好 1 个** descriptor set layout(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
  //             descriptorCount = sampled_texture_slots),descriptor_set_count() 返回 1。
  //             固定 set=0, binding=0..slots-1。**不做** bindless / 纹理数组 / 多集。
  // 绑定入口是 ICommandList::bind_sampled_texture(texture, sampler, slot)。
  u32 sampled_texture_slots = 0;
  // 目标附件形状(G8,2026-11-11)。由调用方按"管线将被录进哪个目标"填;
  // L3 的 IShaderLibrary 会把它原样透传给按形状派生的后端入口。
  // 默认值 = 不带深度的 RGBA8 单采样 ⇒ 既有行为零变化。
  AttachmentShape attachment{};
};

// ---------------------------------------------------------------- 按形状派生(G8)

// 后端"按形状建管线"能力的**不透明标签**。
//
// 分层方向:L1 头里**只**声明这些入口,后端在初始化时把函数指针注册进来
// (L0 实现 → L1 抽象,单向)。L3 只认 resources::IPipelineFactory 与公共值类型
// AttachmentShape,永远看不到任何 backend:: / Vk* 类型。
//
// IPipeline 的完整定义在本文件更靠后(见下方 class IPipeline),工厂接口只需要
// unique_ptr 的参数类型,前向声明即可 —— 不能为了它把整个类提前,那会和
// PipelineDesc 的依赖顺序打架。
class IPipeline;

class IPipelineFactory {
 public:
  IPipelineFactory(const IPipelineFactory&) = delete;
  IPipelineFactory& operator=(const IPipelineFactory&) = delete;
  virtual ~IPipelineFactory() = default;

  // 按目标附件形状建管线。失败必须显式返回(不降级为"忽略形状")。
  virtual device::Result<std::unique_ptr<IPipeline>> create_shaped_pipeline(
      device::IRenderDevice& device, const PipelineDesc& desc,
      const AttachmentShape& shape) = 0;

 protected:
  IPipelineFactory() = default;
};

// 后端工厂表的两个入口(无状态,按 device 动态解析)。
//
//   - PipelineFactoryResolveFn:接收 device,返回该 device 对应的工厂(或 nullptr)。
//   - register_pipeline_factory:把上面这个**解析器函数**交给 L1。
//
// 注意:早先 PipelineFactoryRegisterFn 被写成 `void(*)(PipelineFactoryResolveFn)`,
// 即"接收注册器的函数",这多了一层毫无必要的间接,且导致实现侧
// `g_resolver_fn(device)` 编译期 C2664(把 IRenderDevice* 当成解析器传进去)。
// 现在直接按"注册=交出一个解析器"的语义定义,只有一层。
using PipelineFactoryResolveFn = IPipelineFactory* (*)(device::IRenderDevice*);
using PipelineFactoryRegisterFn = PipelineFactoryResolveFn;

// 注册后端工厂解析器。只在后端初始化时调用一次(L0 → L1 的单向注入)。
// 传 nullptr 表示撤销(设备销毁前)。
void register_pipeline_factory(PipelineFactoryRegisterFn fn);

// 按当前已注册的后端解析器取工厂;没有后端接管 → nullptr。
// 调用方拿到 nullptr 时必须显式回落(走 create_pipeline(desc)),
// 不得静默假定形状已生效。
IPipelineFactory* resolve_pipeline_factory(device::IRenderDevice* device);

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

  // R4 默认策略是 push constant 优先,**默认不建描述符集**;
  // 只有 PipelineDesc::sampled_texture_slots > 0(P0 纹理采样)时建 1 个。
  //   返回 0  ⇔ sampled_texture_slots == 0(既有行为,Pipeline.UsesPushConstantsOnly)
  //   返回 1  ⇔ sampled_texture_slots > 0
  // 恒不超过 1:本轮不做多集/bindless(裁定 X1)。
  virtual u32 descriptor_set_count() const = 0;
  // 该管线声明了多少个可采样纹理槽(0 = 未声明,任何 bind_sampled_texture 都会被拒)。
  virtual u32 sampled_texture_slots() const = 0;
  // 实际生效的 push constant 字节数(<= desc().push_constant_bytes)。
  virtual u32 push_constant_bytes() const = 0;

  // 管线内部缓存句柄(供 backend 侧命令流快照;公共层不解释它)。
  virtual u64 hash() const = 0;

 protected:
  IPipeline() = default;
};

}  // namespace magicblocks::resources
