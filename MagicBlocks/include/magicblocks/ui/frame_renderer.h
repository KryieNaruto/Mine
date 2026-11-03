// MagicBlocks UI 消费层 —— 帧渲染接口(spec §5 Presenter 上屏链路、§5.5 离屏分流)
//
// 为什么 UI 层要自己声明这个接口(而不是直接用 SDK 头):
//
//   spec §5.2 主路径要求 UI 与场景**同帧、同一 CommandList、同一次 submit** 渲染进
//   swapchain image。但 SDK 目前的 L0 只到 `IRenderTarget`(附件视图 + 离屏读回),
//   还没有 L1 `ICommandList` / L2 `IRenderGraph` —— 也就是说,**"渲染一帧并上屏"
//   这条入口在 SDK 里还不存在**(ISwapchain 只有 `current_target()`,没有 acquire /
//   submit / present)。
//
//   为了让 UI 层的 Presenter 今天就能定型并被测试覆盖,这里声明 `IFrameRenderer`
//   作为"帧渲染的唯一出口",由 SDK 落地后**用真实现替换**(签名不变):
//     - `render_into_device_image()` = 在调用方给定的同一 CommandBuffer 内录制
//       场景 pass + UI pass,并 signal 一个信号量给 present 用;
//     - `render_to_memory()` = 离屏路径(§5.5),golden/CLI/录制专用。
//
// **两条路径不共用函数**:主路径上没有任何 CPU 回读,回读只可能出现在
// `render_to_memory()` 里(spec §10 验收门 1 由静态门控脚本兜底)。
//
// 关键点:这个接口**没有** `readback` / `map` 成员,主路径在类型层面就够不着 CPU。
#pragma once

#include <cstdint>

#include "magicblocks/ui/types.h"

namespace magicblocks::ui {

// 一帧的渲染产物句柄(由实现定义;**不透明**,UI 层不得解释其内部)。
// Swapchain 模式下它与 swapchain image 绑定;Offscreen 模式下它是离屏附件。
struct FrameImage {
  std::uint64_t id = 0;
  u32 width = 0;
  u32 height = 0;
  bool valid = false;
};

// 主路径:把场景 + UI 录进**调用方已有的** command buffer,并返回"present 前需要
// 等待的信号量"。合约(spec §5.4,实现方必须遵守,门控脚本静态兜底):
//   - 不得发起任何 submit(整帧只有 Presenter 那一次);
//   - 不得 readback / map;
//   - 不得创建/销毁 swapchain 或 render pass(走缓存)。
class IFrameRenderer {
 public:
  virtual ~IFrameRenderer() = default;

  // 主路径:场景 pass → UI pass → image layout 转 PRESENT。
  // `cmd` 是 SDK 侧不透明的命令缓冲句柄(0 = 由实现自建)。返回 false = 录制失败。
  virtual bool render_into_device_image(std::uint64_t cmd, FrameImage* out_image,
                                        std::uint64_t* out_present_semaphore) = 0;

  // 离屏路径(spec §5.5):场景 + UI 渲染到离屏 image,再交回主机内存。
  // **只有这条路径允许 CPU 回读**,且只用于 CLI / CI / golden / 命令回放。
  virtual bool render_to_memory(u32 width, u32 height, std::uint8_t* out_rgba8,
                                u32 row_bytes) = 0;
};

}  // namespace magicblocks::ui
