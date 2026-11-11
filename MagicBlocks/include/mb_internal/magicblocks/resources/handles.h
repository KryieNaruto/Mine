// MagicBlocks SDK —— L1 resources 层公共类型:资源句柄 / 内存用途 / 资源用途位
//
// 设计约束(spec §2.1 / §6.3 / plans-l1 §3):
//   - 本头属于 L2+ 可见的公共面,**不得出现任何 Vulkan 类型**(G3)。页面标识、偏移、
//     大小全部用整数表达,由 backend/vulkan 内部解释成真实内存。
//   - **M3「句柄 = 偏移」**:ResourceId 是 {page_id, offset, size, id} 的值语义句柄。
//     命令流记录的是它(而不是指针),回放时同一 page_id/offset 指向同一片内存,
//     从而绕开「记录指针 → 回放 dangling」的经典陷阱。本轮只落句柄与其序列化,
//     命令流本体属 L2。
//   - id 不是地址而是**逻辑槽位**:资源重建后页可能重新分配,但 id 不变,
//     复现时用它做一致性校验。
//   - 本头只用 magicblocks::device::u32/u64(device/types.h),不另起整数别名。
//
// 为什么用途枚举(BufferUsage / TextureUsage / ShaderStage)放在这里而不是各资源头里:
//   allocation_kind() 是 IBuffer / ITexture / IPipeline 共有的"挂到哪个内存桶"的查询
//   (M2 的桶 = 内存侧属性),把 ResourceKind ↔ MemoryKind 的映射集中在一处,
//   避免三个资源头各自定义一份又互相 include 成环。
#pragma once

#include <cstdint>
#include <type_traits>

#include "mb_internal/magicblocks/device/types.h"

namespace magicblocks::resources {

using device::u32;
using device::u64;

// ---------------------------------------------------------------- 句柄(M3)

// 资源句柄 = 页内偏移。字段顺序即语义:先定位页(id),再定位页内区间(offset/size),
// 最后才是逻辑槽位(id)。sizeof == 24、8 字节对齐 → 恰好 3 个 u64 字,
// 在内存里是平铺的,不需要逐字段拷贝即可整块序列化。
struct ResourceId {
  u32 page_id = kInvalidPage;  // 所属内存页(M2 的 64 MB / 8 MB 大页)
  u32 offset = 0;              // 页内偏移(按桶切分后的子块起点,已对齐)
  u64 size = 0;                // 子块字节数
  u64 id = 0;                  // 逻辑资源槽位号(跨页重建保持不变,复现校验用)

  // 判空用:页号恒从 0 开始,所以 kInvalidPage 不会与真实页冲突。
  static constexpr u32 kInvalidPage = 0xFFFFFFFFu;
  // 采样器专用页号(P0 / X1):ISampler 不占内存子块,但 bind_sampled_texture
  // 的签名要求传一个**有效**句柄。给采样器发这个专用 page_id(size != 0、
  // id = slot_index)让 valid() 通过,同时不占任何真实页号 —— 真实页号从 0
  // 递增且 validate() 有 "page_id == pages 下标" 不变量,永远到不了这个值。
  static constexpr u32 kSamplerPage = 0xFFFFFFFEu;

  bool valid() const { return page_id != kInvalidPage && size != 0; }
  // 页内闭开区间 [offset, offset+size)。
  u64 end() const { return static_cast<u64>(offset) + size; }

  // -------------------------------------------------------------- 紧凑打包

  // 打包成单个 u64 是「命令流记录偏移而非指针」的落地前提(spec §5.2 / §6.3):
  // 每条命令带一个整数就能完整表达资源身份,而不是只带低 32 位的偏移。
  //
  // 字位布局(LSB → MSB):
  //   [0,16)  page_id  —— 16 位,页号 < 65536
  //   [16,32) offset   —— 16 位,页内偏移以 kPackGranularity(64 KiB)为单位。
  //                      16 位 × 64 KiB = 4 GiB 寻址空间,足以覆盖 64 MB 设备页
  //                      与 8 MB Host 页(M2)整页,留 64× 余量。
  //   [32,64) 资源槽位 id —— 32 位
  // 返回 false 表示字段超出打包宽度(调用方必须当成明确错误处理,不允许静默截断)。
  //
  // **注意**:pack() 只承载「命令流标识资源」所需的身份信息,不含 size ——
  // size 由解包方通过资源表按 id 反查(命令流本身不重复携带可推导数据)。
  // 需要 4 GiB 以上偏移或 65536 页以上的分配器布局在解析阶段就会以 InvalidArgument
  // 拒绝,而不是让命令流悄悄记下错误的偏移。
  static constexpr u64 kPackGranularity = 1ull << 16;  // 64 KiB
  static constexpr u32 kPackMaxPageId = 0xFFFFu;
  static constexpr u32 kPackMaxOffsetUnits = 0xFFFFu;
  static constexpr u64 kPackMaxResourceId = 0xFFFFFFFFull;

  bool pack(u64* out) const {
    if (out == nullptr) return false;
    if (page_id > kPackMaxPageId) return false;
    if ((offset % kPackGranularity) != 0) return false;
    const u64 offset_units = static_cast<u64>(offset) / kPackGranularity;
    if (offset_units > kPackMaxOffsetUnits) return false;
    if (id > kPackMaxResourceId) return false;
    *out = static_cast<u64>(page_id) | (offset_units << 16) | (id << 32);
    return true;
  }

  // 打包的逆运算。size 不参与打包,解包后恒为 0 ——
  // 调用方要拿完整描述就按 id 查资源表(这正是资源表存在的意义)。
  static ResourceId unpack(u64 packed) {
    ResourceId rid{};
    rid.page_id = static_cast<u32>(packed & 0xFFFFull);
    rid.offset = static_cast<u32>(((packed >> 16) & 0xFFFFull) * kPackGranularity);
    rid.size = 0;
    rid.id = packed >> 32;
    return rid;
  }

  // 字段全等比较(page_id/offset/size/id 四个都算,size 的不等也应该是不等)。
  bool operator==(const ResourceId& o) const {
    return page_id == o.page_id && offset == o.offset && size == o.size && id == o.id;
  }
  bool operator!=(const ResourceId& o) const { return !(*this == o); }
};

static_assert(sizeof(ResourceId) == 24, "ResourceId 必须是 3 个 u64 字,序列化依赖此布局");
static_assert(std::is_trivially_copyable<ResourceId>::value,
              "ResourceId 必须是可平凡拷贝的值类型(命令流快照/回环读写)");

// ---------------------------------------------------------------- 内存用途(M2)

// 内存页性质:两套页尺寸差一个数量级,决定页从哪条 VMA 路径申请(spec §6.1)。
enum class PageKind : std::uint8_t {
  Device = 0,       // DEVICE_LOCAL,64 MB 大页(Vertex / Index / Uniform)
  HostVisible = 1,  // HOST_VISIBLE|HOST_COHERENT 且 MAPPED 常驻,8 MB 页(Texture / Staging)
};

// 分配桶(NOT 页类型):M2 要求「页内按用途分桶,桶内空闲链表切出偏移」。
// 同桶内的子块可在同一页内复用 → 同页 page_id 相同、offset 互不重叠。
// 说明:spec 未硬性规定 Texture / Staging 各归哪套页。本头按
// 「CPU 侧会上传/读回像素」处理,把它们放在 HostVisible 页(MAPPED 常驻),
// 使 buffer_texture_map() 这类映射视图对纹理恒可用;R5 的 staging 缓冲同理。
enum class MemoryKind : std::uint8_t {
  Vertex = 0,
  Index = 1,
  Uniform = 2,
  Texture = 3,
  Staging = 4,
};

// 桶 → 页类型。前端不需要知道页尺寸,只按 kind 要内存。
inline PageKind page_kind_of(MemoryKind kind) {
  switch (kind) {
    case MemoryKind::Vertex:
    case MemoryKind::Index:
    case MemoryKind::Uniform:
      return PageKind::Device;
    case MemoryKind::Texture:
    case MemoryKind::Staging:
      return PageKind::HostVisible;
  }
  return PageKind::Device;
}

// ---------------------------------------------------------------- 资源用途位

// IBuffer / ITexture / IPipeline 共有的"挂哪个桶"查询(M2)。
// 多用途资源(例如 UBO|Storage)取其中最"设备侧"的一支,保证设备内存优先。
enum class ResourceKind : std::uint8_t {
  Unknown = 0,
  Vertex = 1,
  Index = 2,
  Uniform = 3,
  Storage = 4,
  Texture = 5,
  Staging = 6,
};

inline MemoryKind memory_kind_of(ResourceKind kind) {
  switch (kind) {
    case ResourceKind::Vertex:
      return MemoryKind::Vertex;
    case ResourceKind::Index:
      return MemoryKind::Index;
    case ResourceKind::Uniform:
    case ResourceKind::Storage:
      return MemoryKind::Uniform;
    case ResourceKind::Texture:
      return MemoryKind::Texture;
    case ResourceKind::Staging:
    case ResourceKind::Unknown:
      break;
  }
  return MemoryKind::Staging;
}

// ---------------------------------------------------------------- buffer 用途

// R1:顶点 / 索引 / UBO / 存储(存储缓冲=SSBO,本轮描述符集不建,
// 用途位先备好,GPU 侧由 push constant + UBO 页承载,见 R4)。
enum class BufferUsage : std::uint8_t {
  Vertex = 0,
  Index = 1,
  Uniform = 2,
  Storage = 3,
  Staging = 4,  // R5:上传队列的 CPU 可见暂存
};

// 用途位 → 内存桶。Staging 走 MemoryKind::Staging 桶(plans-l1 用例
// Upload.StagingGoesThroughAllocator 就是查这条)。
inline MemoryKind memory_kind_of(BufferUsage usage) {
  switch (usage) {
    case BufferUsage::Vertex:
      return MemoryKind::Vertex;
    case BufferUsage::Index:
      return MemoryKind::Index;
    case BufferUsage::Uniform:
    case BufferUsage::Storage:
      return MemoryKind::Uniform;
    case BufferUsage::Staging:
      break;
  }
  return MemoryKind::Staging;
}

inline ResourceKind resource_kind_of(BufferUsage usage) {
  switch (usage) {
    case BufferUsage::Vertex:
      return ResourceKind::Vertex;
    case BufferUsage::Index:
      return ResourceKind::Index;
    case BufferUsage::Uniform:
      return ResourceKind::Uniform;
    case BufferUsage::Storage:
      return ResourceKind::Storage;
    case BufferUsage::Staging:
      break;
  }
  return ResourceKind::Staging;
}

// ---------------------------------------------------------------- texture 用途

// R1:颜色采样 / 渲染附件 / 深度附件 / CPU 上传 / CPU 读回 / Mipmap。
// 决定 backend 分配时的内存桶与布局(mip 链、深度格式必须走设备侧布局)。
enum class TextureUsage : std::uint8_t {
  Sampled = 0,     // 被着色器采样(含动态色表纹理,R4)
  ColorTarget = 1,
  DepthTarget = 2,
  TransferSrc = 3,  // CPU → GPU 上传目标
  TransferDst = 4,  // GPU → CPU 读回(Texture.ReadbackRgba8)
};

inline MemoryKind memory_kind_of(TextureUsage usage) {
  switch (usage) {
    case TextureUsage::ColorTarget:
    case TextureUsage::DepthTarget:
      return MemoryKind::Texture;  // 渲染附件:GPU 独占侧
    case TextureUsage::TransferSrc:
      return MemoryKind::Staging;  // 上传源:CPU 写
    case TextureUsage::Sampled:
    case TextureUsage::TransferDst:
      break;
  }
  return MemoryKind::Texture;
}

// ---------------------------------------------------------------- shader stage(G1)

// **C4 / G1 硬禁几何着色器 —— 结构性保证:枚举里根本没有 geometry 这一项**,
// 因此「传入几何着色器」在编译期就是不可能的,Pipeline.RejectsGeometryStage
// 这条用例不需要运行期校验分支。
// 另外只保留 Vertex/Fragment/Compute:没有 Tessellation,HW 能力探测与之配套。
// 声明位置:handles.h(资源用途的一支),command.h 的 push constant 可见性也用它。
enum class ShaderStage : std::uint8_t {
  Vertex = 0,
  Fragment = 1,
  Compute = 2,
};

// ---------------------------------------------------------------- 比较函数(共享)

// 深度比较(pipeline.h 的 DepthState)与采样器比较(texture.h 的 SamplerDesc)共用
// 同一套序;放在这里是为了让两个资源头各取所需、互不 include(避免成环)。
enum class CompareOp : std::uint8_t {
  Disabled = 0,  // 仅采样器用:不做比较采样
  Never = 1,
  Less = 2,
  Equal = 3,
  LessOrEqual = 4,
  Greater = 5,
  NotEqual = 6,
  GreaterOrEqual = 7,
  Always = 8,
};

}  // namespace magicblocks::resources
