// MagicBlocks SDK —— L3 `IShaderLibrary`:Shader 模板登记与管线派生(spec §9.3)
//
// 职责边界(**这是 Material 与 Shader 联动的关键**,spec §9.4 规则 1/2):
//   - `IShaderLibrary` 只知道"shader 模板"(SPIR-V + 顶点布局 + 推送常量字段表),
//     以及"由模板 + 固定功能状态派生 IPipeline";
//   - `IMaterial` 只知道"一组参数值 + 一个 ShaderKey";
//   - **材质不含管线**:同样的 ShaderKey 在不同 pass 深度状态下会派生出不同管线,
//     这件事由 library 的 pipeline 缓存负责,材质不感知。
//
// 为什么这样切(spec §9.1/§9.2):
//   L1 只有 push constant(R4,无描述符集),移动端上限 128 字节。若把材质参数放进
//   push constant,一次材质切换就要一次 push + draw,材质数 = drawcall 数。
//   因此:**材质参数逐实例**(实例属性里的 StandardParams),push constant 只放
//   "每帧一次"的东西(view_proj 64B + 少量标量),同一 ShaderKey 下的所有材质
//   合成一次 draw_instanced。
//
// 顶点布局约定(L3 内部恒定,shader 必须逐 location 对上):
//   binding 0(逐顶点):location 0 = Float3 position
//                       location 1 = Float3 normal(可选,没有时 shader 用常量)
//                       location 2 = Float3 color   (可选,顶点色/线色)
//   binding 1(逐实例):location 3 = Float4 世界矩阵第 0 列(x 轴 + tx)
//                       location 4 = Float4 第 1 列
//                       location 5 = Float4 第 2 列
//                       location 6 = Float4 第 3 列(平移)
//                       location 7 = Float4 base_color(rgba)
//                       location 8 = Float4 params(roughness, metallic, emissive, unused)
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "mb_internal/magicblocks/device/render_device.h"
#include "mb_internal/magicblocks/device/result.h"
#include "mb_internal/magicblocks/primitives/types.h"
#include "mb_internal/magicblocks/resources/pipeline.h"

namespace magicblocks::primitives {

using resources::BlendState;
using resources::CullMode;
using resources::DepthState;
using resources::IPipeline;
using resources::PipelineDesc;
using resources::PrimitiveType;
using resources::RasterState;
using resources::ShaderModule;
using resources::ShaderStage;
using resources::VertexAttribute;
using resources::VertexFormat;
using resources::VertexInputBinding;
using resources::VertexLayout;

// Shader 模板名(L3 内置的固定集合。**新增玩法不得新增名字** —— 那说明抽象不够通用)
namespace shader_names {
inline constexpr const char* kStandardPbr = "std_pbr";        // 3D 实体(积木/印章)
inline constexpr const char* kStandardUnlit = "std_unlit";    // 2D/纯色(涂鸦/叠叠乐)
inline constexpr const char* kStrokeRibbon = "stroke_ribbon"; // 画笔条带
inline constexpr const char* kLine2d = "line2d";              // 屏幕恒宽线(gizmo/虚线/网格)
inline constexpr const char* kPickId = "pick_id";             // 拾取(输出实体 id)
}  // namespace shader_names

// 一个 shader 模板里可被推导的"开关位"。位含义由模板自己的 shader 决定,
// library 只做"相同 flags → 同一个管线"的去重。
enum : std::uint32_t {
  kShaderFlagVertexColor = 1u << 0,   // 顶点色(线色/条带色)
  kShaderFlagVertexNormal = 1u << 1,  // 有法线(算光照)
  kShaderFlagAlphaBlend = 1u << 2,    // 透明混合(与不透明用**不同的 ShaderKey**,D-L3-3)
  kShaderFlagScreenSpace = 1u << 3,   // 顶点已在像素空间(gizmo/覆盖层)
};

// Shader 选择键(管线派生 + 合批排序的第一关键字)。
struct ShaderKey {
  std::uint32_t name_hash = 0;   // FNV-1a(shader_names::*)
  std::uint32_t flags = 0;

  bool operator==(const ShaderKey& o) const {
    return name_hash == o.name_hash && flags == o.flags;
  }
  bool operator!=(const ShaderKey& o) const { return !(*this == o); }
  bool valid() const { return name_hash != 0; }
};

// 由名字构造(哈希函数固定,跨进程稳定 —— 命令流要按它复现)。
std::uint32_t shader_name_hash(const char* name);
ShaderKey make_shader_key(const char* name, std::uint32_t flags = 0);

// 固定功能状态:合批排序时作为管线派生的一部分。
// **它不进 MaterialDesc**(spec §9.4 规则 1:换材质不悄悄换深度状态)。
struct RasterStateL3 {
  CullMode cull = CullMode::None;
  PrimitiveType primitive = PrimitiveType::TriangleList;
  bool depth_test = true;
  bool depth_write = true;
  bool blend = false;

  bool operator==(const RasterStateL3& o) const {
    return cull == o.cull && primitive == o.primitive && depth_test == o.depth_test &&
           depth_write == o.depth_write && blend == o.blend;
  }
  bool operator!=(const RasterStateL3& o) const { return !(*this == o); }
};

// 渲染目标附件形状(管线与 pass 必须兼容)。与 L0 的离屏目标默认形状一致。
struct TargetShape {
  device::Format color_format = device::Format::RGBA8_UNORM;
  bool has_depth = false;
  std::uint32_t samples = 1;

  bool operator==(const TargetShape& o) const {
    return color_format == o.color_format && has_depth == o.has_depth && samples == o.samples;
  }
};

// 推送常量(src 侧统一布局,shader 侧用 push_constant block 逐字段对上):
//   offset 0  : mat4 view_proj         (64 字节)
//   offset 64 : vec4 viewport_size_xy_time_w(像素宽、像素高、保留、保留)
//   offset 80 : vec4 misc
// 合计 96 字节(<= 移动端 128 的硬上限,留出余量)。
inline constexpr std::uint32_t kPushConstantBytes = 96;
inline constexpr std::uint32_t kPushConstantViewProjOffset = 0;
inline constexpr std::uint32_t kPushConstantViewportOffset = 64;

struct PushConstantsFrame {
  float view_proj[16] = {};
  float viewport[4] = {1.0f, 1.0f, 0.0f, 0.0f};
  float misc[4] = {};
};

// ---------------------------------------------------------------- Shader 模板

// SPIR-V 字节码的只读视图(生命周期由内嵌表保证,不转移所有权)。
struct ShaderModuleRef {
  const std::uint8_t* spirv = nullptr;
  std::size_t size_bytes = 0;
  bool valid() const { return spirv != nullptr && size_bytes >= 4 && (size_bytes % 4) == 0; }
};

struct ShaderTemplateDesc {
  std::string name;                 // shader_names::*
  ShaderModuleRef vertex{};         // 由 create_shader_library 从内嵌 SPIR-V 填
  ShaderModuleRef fragment{};
  VertexLayout layout{};            // 见本头开头的顶点布局约定
  std::uint32_t push_constant_bytes = kPushConstantBytes;
};

// 管线条目(派生 + 缓存)。`IPipeline*` 由 library 持有,生命周期到库销毁。
struct PipelineEntry {
  ShaderKey key{};
  RasterStateL3 raster{};
  TargetShape shape{};
  IPipeline* pipeline = nullptr;
  std::uint64_t hash = 0;
  // 派生次数(性能断言:相同 key 再来一次必须命中缓存,不重复建管线)。
  std::uint64_t created = 1;
};

class IShaderLibrary {
 public:
  IShaderLibrary(const IShaderLibrary&) = delete;
  IShaderLibrary& operator=(const IShaderLibrary&) = delete;
  virtual ~IShaderLibrary() = default;

  // 登记一个模板(启动期一次)。重名 → Invalid。
  // 失败时**不**留下半登记项(全或无)。
  virtual Status register_template(const ShaderTemplateDesc& desc) = 0;

  // 是否登记过该名字。
  virtual bool has(const char* name) const = 0;
  virtual std::size_t template_count() const = 0;

  // 取模板的顶点布局(shader 与网格必须对上;mesh 校验用)。
  virtual const VertexLayout* layout_for(const ShaderKey& key) const = 0;

  // 派生(或命中缓存)一条管线。
  //   - key 未登记 → NotFound;
  //   - 设备不支持该组合(如 push constant 超限) → 原样透传设备错误。
  virtual Result<IPipeline*> acquire_pipeline(const ShaderKey& key, const RasterStateL3& raster,
                                              const TargetShape& shape) = 0;

  // 已建管线数 / 已建次数(未命中时 +1)—— 性能回归门控用它。
  virtual std::size_t pipeline_count() const = 0;
  virtual std::uint64_t pipeline_create_count() const = 0;

  // 清空全部派生的管线(设备重建/形状变化时用;模板保留)。
  virtual void clear_pipelines() = 0;

 protected:
  IShaderLibrary() = default;
};

// 工厂:把内嵌 SPIR-V(由 CMake 的 embed_shaders.py 生成)登记成 L3 的标准模板集。
// `device` 需要来真正建管线;纯软件态可传 nullptr(只做校验与哈希,不建 VkPipeline)。
// 全部内嵌 SPIR-V 由 `primitives/embedded_shaders.h` 给出(见该头的说明)。
std::unique_ptr<IShaderLibrary> create_shader_library(device::IRenderDevice* device);

}  // namespace magicblocks::primitives
