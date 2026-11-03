// MagicBlocks SDK —— L1 纹理与采样器(R1 / R4)
//
// 设计约束(spec R1 / R4 / §6.3, plans-l1 §3):
//   - **R1 全部从内存页切偏移**:ITexture / ISampler 创建时向 IMemoryAllocator 要子块,
//     id() 是那个 ResourceId(M3),命令流只记偏移;不单独触发 VMA 分配。
//   - **格式不必新增枚举**:颜色/深度格式统一取自 device::Format(L0 的 types.h 已经
//     有 RGBA8_UNORM / R8_UNORM / D16 / D24S8 / D32_SFLOAT),不在这里再造一份
//     格式枚举 —— 两份格式表迟早会漂移。
//   - **R4 描述符策略**:push constant 优先 + 少量 UBO 页 + 一张动态色表纹理;
//     本轮**不建描述符集**,所以 ISampler 也不需要"绑定槽位"概念,
//     采样器只描述滤波/寻址/比较。
//   - **离屏读回是一等公民**(spec D4 / §5.4):readback_rgba8 的行序与
//     IRenderTarget::readback_rgba8 一致(自上而下,R,G,B,A 逐像素),
//     golden 比对可以两者混用。
#pragma once

#include <cstddef>
#include <cstdint>

#include "magicblocks/device/result.h"
#include "magicblocks/device/types.h"
#include "magicblocks/resources/handles.h"

namespace magicblocks::resources {

using device::Extent2D;
using device::Format;
using device::Result;
using device::Status;
using device::u32;
using device::u64;

// ---------------------------------------------------------------- 纹理(R1)

struct TextureDesc {
  Extent2D extent{};                       // 宽高,都必须 > 0
  Format format = Format::RGBA8_UNORM;
  u32 mip_levels = 1;                      // 1 = 不生成 mip 链
  TextureUsage usage = TextureUsage::Sampled;
  u32 samples = 1;                         // MSAA 采样数;> 1 只允许 ColorTarget/DepthTarget
};

class ITexture {
 public:
  ITexture(const ITexture&) = delete;
  ITexture& operator=(const ITexture&) = delete;
  virtual ~ITexture() = default;

  virtual ResourceId id() const = 0;
  virtual const TextureDesc& desc() const = 0;

  // 像素(RGBA8 等紧凑布局,行间无 padding);用于字节数断言与读回长度校验。
  virtual u64 size() const = 0;
  virtual MemoryKind allocation_kind() const = 0;

  // CPU → 纹理上传(走 staging 批处理,R5)。level 越界 → InvalidArgument。
  virtual Status upload(const void* data, u64 size_bytes, u32 level = 0) = 0;
  // 纹理 → CPU,行序自左上、逐像素 R,G,B,A(与 IRenderTarget 同口径)。
  // out 长度必须 >= width*height*4;深度格式或不可读回 → Unsupported(明确报错)。
  virtual Status readback_rgba8(std::uint8_t* out, std::size_t size_bytes) const = 0;

 protected:
  ITexture() = default;
};

// ---------------------------------------------------------------- 采样器(R1)

enum class FilterMode : std::uint8_t {
  Nearest = 0,
  Linear = 1,
};

enum class WrapMode : std::uint8_t {
  ClampToEdge = 0,
  Repeat = 1,
  MirroredRepeat = 2,
};

// 深度比较模式 CompareOp 定义在 handles.h(与 pipeline.h 的 DepthState 共用一套)。

struct SamplerDesc {
  FilterMode min_filter = FilterMode::Linear;
  FilterMode mag_filter = FilterMode::Linear;
  FilterMode mip_filter = FilterMode::Linear;
  WrapMode wrap_u = WrapMode::ClampToEdge;
  WrapMode wrap_v = WrapMode::ClampToEdge;
  CompareOp compare = CompareOp::Disabled;  // 非 Disabled 且纹理不是深度格式 → 创建报错
};

// 非法 filter / wrap / compare → InvalidArgument(plans-l1 用例 Sampler.CreateAndDestroy
// 的"非法 filter"分支就是这条),绝不做"就近取一个合法值"的静默降级。
class ISampler {
 public:
  ISampler(const ISampler&) = delete;
  ISampler& operator=(const ISampler&) = delete;
  virtual ~ISampler() = default;

  virtual ResourceId id() const = 0;
  virtual const SamplerDesc& desc() const = 0;
  // 采样器对象在描述符关闭时也占一个槽位号(动态色表纹理之类);
  // 用于命令流的稳定性日志与去重。
  virtual u32 slot_index() const = 0;

 protected:
  ISampler() = default;
};

}  // namespace magicblocks::resources
