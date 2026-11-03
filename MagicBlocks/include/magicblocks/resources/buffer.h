// MagicBlocks SDK —— L1 缓冲资源(R1 / R5)
//
// 设计约束(spec R1 / R5 / §6.3, plans-l1 §3):
//   - **全部从内存页切偏移**,不单独触发 VMA 分配:IBuffer 创建时向
//     IMemoryAllocator 要一个子块,id() 原样返回该 ResourceId(M3「句柄 = 偏移」)。
//     于是 Buffer.HandleIsOffset 可以用「两个 buffer 的 page_id/offset 互不重叠」
//     直接判定,而不必去看内部指针。
//   - **CPU 写永远走 mapped()**:映射首址由分配器给出(Host 页 MAPPED 常驻),
//     本接口不提供 vkMapMemory 的等价入口(G4)。设备页桶(Vertex/Index/Uniform)
//     在 CPU 侧不可映射,mapped() 返回 nullptr —— 要写数据请用 Staging 缓冲
//     + upload(R5),而不是让调用方去猜映射语义。
//   - **R5 上传队列**:staging → GPU 副本按帧批处理,staging 也走分页分配器
//     (MemoryKind::Staging 桶),这是 Upload.StagingGoesThroughAllocator 的依据。
//   - 由 IRenderDevice::create_buffer 创建、由调用方持有 unique_ptr;
//     销毁即子块归还本页(M4),不立即退 VMA。
//
// 硬约束:零 Vk* 类型、零 #include <vulkan/...>(G3);错误复用 device::Status/Result。
#pragma once

#include <cstdint>

#include "magicblocks/device/result.h"
#include "magicblocks/device/types.h"
#include "magicblocks/resources/handles.h"

namespace magicblocks::resources {

using device::Result;
using device::Status;
using device::u32;
using device::u64;

// ---------------------------------------------------------------- 描述(R1)

struct BufferDesc {
  u64 size = 0;                  // 字节数,必 > 0(否则 InvalidArgument)
  BufferUsage usage = BufferUsage::Vertex;
  u64 alignment = 0;             // 0 = 用桶的默认对齐;非 0 必须是 2 的幂
  bool persistent_map = false;   // 期望常驻映射(Host 页恒满足;设备页不可满足)
};

// ---------------------------------------------------------------- 接口(R1)

class IBuffer {
 public:
  IBuffer(const IBuffer&) = delete;
  IBuffer& operator=(const IBuffer&) = delete;
  virtual ~IBuffer() = default;

  // 句柄 = 偏移(M3):命令流记录它,而不是指针。
  virtual ResourceId id() const = 0;
  virtual const BufferDesc& desc() const = 0;

  // 实际分配到的字节数(可能因对齐大于 desc().size)。
  virtual u64 size() const = 0;
  // 本缓冲所在内存桶(M2),必须与 BufferDesc::usage 对应(memory_kind_of)。
  virtual MemoryKind allocation_kind() const = 0;

  // CPU 可写映射首址;不可映射(设备页)返回 nullptr。
  virtual void* mapped() = 0;
  virtual const void* mapped() const = 0;

  // CPU → 缓冲写入(单队列单线程录制,不做并发写保护)。
  // offset + size 越界 → InvalidArgument;缓冲不可映射 → Unsupported(明确报错)。
  virtual Status write(const void* data, u64 offset, u64 size) = 0;
  virtual Status read(void* out, u64 offset, u64 size) const = 0;

  // R5:上传请求。数据进入 staging 批处理,由帧边界统一提交到 GPU 副本。
  // 本轮语义 = 立即完成本次请求的入队并返回;真 GPU 拷贝在命令提交时发生。
  virtual Status upload(const void* data, u64 size, u64 dst_offset = 0) = 0;
  // 丢弃尚未提交的上传批(例如整帧被取消)。
  virtual void flush_pending() = 0;
  // 尚未提交的上传字节数。
  virtual u64 pending_upload_bytes() const = 0;

 protected:
  IBuffer() = default;
};

}  // namespace magicblocks::resources
