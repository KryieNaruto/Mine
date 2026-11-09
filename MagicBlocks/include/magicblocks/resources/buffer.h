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

  // R5:上传请求。数据只进入 staging 批处理,**不会**立刻出现在 GPU 侧副本里。
  // 真正的 GPU 拷贝发生在 flush_pending()。
  //
  // 踩过的坑(务必看清):`upload()` 返回 Ok **不代表数据已经可用**。漏掉
  // flush_pending() 时不会报任何错,GPU 读到的仍是全 0 内存 —— 顶点缓冲全零
  // 会让所有三角形退化到原点,最终"画面上一个像素都没有,但清屏色正常",
  // 极难定位。凡是建完缓冲立刻要拿它画的代码,都必须紧跟一次 flush_pending()。
  virtual Status upload(const void* data, u64 size, u64 dst_offset = 0) = 0;
  // 把 upload() 入队的全部字节**提交**到 GPU 侧副本(staging → vkCmdCopyBuffer)。
  // 帧边界调用一次即可;析构时也会兜底调用。返回值语义见 commit 结果查询。
  virtual void flush_pending() = 0;
  // 尚未提交的上传字节数。**建完缓冲、画之前应当断言它为 0**,
  // 这是"忘了 flush"唯一可靠的观测点。
  virtual u64 pending_upload_bytes() const = 0;

 protected:
  IBuffer() = default;
};

}  // namespace magicblocks::resources
