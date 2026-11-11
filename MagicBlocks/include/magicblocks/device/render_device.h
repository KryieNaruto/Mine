// MagicBlocks SDK —— 设备入口(D1/D2/D7/D8)
#pragma once

#include <memory>

#include "magicblocks/device/frame_sync.h"
#include "magicblocks/device/lifecycle.h"
#include "magicblocks/device/render_target.h"
#include "magicblocks/device/result.h"
#include "magicblocks/device/swapchain.h"
#include "magicblocks/device/types.h"
#include "magicblocks/resources/buffer.h"
#include "magicblocks/resources/command.h"
#include "magicblocks/resources/memory.h"
#include "magicblocks/resources/pipeline.h"
#include "magicblocks/resources/texture.h"

namespace magicblocks::device {

class IRenderDevice {
 public:
  virtual ~IRenderDevice() = default;

  // 按目标附件形状派生管线的工厂(G8 / t13,2026-11-11)。
  //
  // 为什么放在 L0 抽象上:IShaderLibrary 属于 L3,必须能按目标形状(离屏带深度 /
  // 交换链 BGRA8 / MSAA 采样数)派生管线;但注册工厂表与设备生命周期一体,
  // 只有设备自己知道"当前 device 对应哪个实现对"。
  // 约定:**不支持按形状派生的后端返回 nullptr**,调用方必须显式回落到
  // create_pipeline(desc),不得静默假定形状已生效。
  virtual resources::IPipelineFactory* pipeline_factory() { return nullptr; }

  virtual BackendType backend() const = 0;
  virtual const DeviceCaps& caps() const = 0;

  // ------------------------------------------------------ 校验层消息(L0)
  //
  // 校验层消息是默认能力:DeviceDesc::enable_validation 默认 true,
  // 层挂上后 ERROR/WARNING 会打印到 stderr。
  //
  // 判据用 validation_enabled():它直接回答「层在不在」。
  // 不要用 message_count>0 做判据 —— 那回答的是「现在有没有问题」,
  // 代码干净时一条消息都没有,用 >0 会反过来逐次拒掉正确的修复。
  virtual bool validation_enabled() const = 0;
  virtual u32 validation_message_count() const = 0;   // 回调总次数
  virtual u32 validation_error_count() const = 0;     // ERROR 级次数,健康跑批应为 0

  // 创建离屏渲染目标(CI/golden/录制回放都走这条,无需窗口)。
  virtual Result<std::unique_ptr<IRenderTarget>> create_offscreen_target(const TargetDesc& desc) = 0;

  // 创建窗口渲染目标;surface 无效/平台不支持 → Unsupported。
  virtual Result<std::unique_ptr<IRenderTarget>> create_window_target(const TargetDesc& desc) = 0;

  // 创建交换链;present 语义归 swapchain,不归 IFrameSync。
  virtual Result<std::unique_ptr<ISwapchain>> create_swapchain(const NativeSurface& surface,
                                                               Extent2D extent) = 0;

  // 帧同步(环形槽复用)。
  virtual IFrameSync* frame_sync() = 0;

  // ---- L1 资源层(spec §4 R1-R6 / M1-M6)----
  // 显存分配器。全仓唯一的显存申请入口(G4),返回受管的分页分配器。
  virtual resources::IMemoryAllocator* memory_allocator() = 0;

  virtual Result<std::unique_ptr<resources::IBuffer>> create_buffer(
      const resources::BufferDesc& desc) = 0;
  virtual Result<std::unique_ptr<resources::ITexture>> create_texture(
      const resources::TextureDesc& desc) = 0;
  virtual Result<std::unique_ptr<resources::ISampler>> create_sampler(
      const resources::SamplerDesc& desc) = 0;
  // 立方体贴图(6 个面的方向查表纹理)。与 create_texture 分开见 texture.h 的说明:
  // 立方体只需要"建 + 整面上传",没有 readback / mip / 采样数这些 2D 才有的形状。
  virtual Result<std::unique_ptr<resources::ICubeMap>> create_cube_map(
      const resources::CubeMapDesc& desc) = 0;
  // R4:图元管线不暴露几何着色阶段(ShaderStage 枚举只有 VS/FS/CS)。
  virtual Result<std::unique_ptr<resources::IPipeline>> create_pipeline(
      const resources::PipelineDesc& desc) = 0;
  virtual Result<std::unique_ptr<resources::ICommandList>> create_command_list() = 0;

  // 把一条**已录制完成**的命令列表真正下发执行(设备帧提交)。
  //
  // 为什么需要这个入口:resources::ICommandList 的 draw 系列在设备态只做
  // "录制进命令流",并把命令编码到它自己的 VkCommandBuffer 的做法只在
  // backend 内的 submit_device_frame() 里成立。没有这条公开通路时,
  // 上层(L2/L3/demo)即使把 11 类命令全录对了,像素也不会出现在目标上 ——
  // 只能借用 OffscreenTarget::draw_selftest() 那条写死几何的旁路。
  //
  // 契约:
  //   - list 必须由**本设备**的 create_command_list() 产生,且已 end_pass
  //     (in_pass() == false)。
  //   - 成功后目标附件内容即为该命令流的执行结果;配合
  //     IRenderTarget::readback_rgba8 取出像素。
  //   - 失败即整帧不提交(D8:不静默降级),返回带原因的非 Ok Status。
  virtual Status submit_command_list(resources::ICommandList& list) = 0;

  // 生命周期事件(D7):SurfaceChanged → 重建,SurfaceDestroyed → 失效。
  virtual Status notify_lifecycle(const AppLifecycleEvent& ev) = 0;

  virtual Status wait_idle() = 0;
};

// 工厂:按 DeviceDesc 创建设备;失败返回带 code/message 的 Status(D8)。
Result<std::unique_ptr<IRenderDevice>> create_render_device(const DeviceDesc& desc);

}  // namespace magicblocks::device
