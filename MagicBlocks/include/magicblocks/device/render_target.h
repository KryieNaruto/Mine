// MagicBlocks SDK —— 渲染目标(D4)
//
// spec D4:WindowTarget 与 OffscreenTarget 是**同一种对象**——同一个 IRenderTarget
// 抽象承载两种 kind,上层不需要为窗口/离屏写两套代码。离屏是一等公民:
// 所有 CI/golden/录制回放都跑在 OffscreenTarget 上,不需要窗口或显示服务。
#pragma once

#include <memory>

#include "magicblocks/device/result.h"
#include "magicblocks/device/types.h"

namespace magicblocks::device {

class IRenderDevice;

// 颜色/深度附件视图。L0 只暴露尺寸与格式,GPU 句柄留在 backend。
struct AttachmentView {
  Extent2D extent{};
  Format format = Format::Unknown;
  bool valid = false;
};

class IRenderTarget {
 public:
  virtual ~IRenderTarget() = default;

  virtual TargetDesc::Kind kind() const = 0;
  virtual Extent2D extent() const = 0;
  virtual Format color_format() const = 0;
  virtual u32 samples() const = 0;

  virtual AttachmentView color_attachment() const = 0;
  virtual AttachmentView depth_attachment() const = 0;  // with_depth=false 时 valid=false

  // 读回颜色附件为 RGBA8(行序自左上,逐像素 R,G,B,A)。
  // out 长度必须 >= width*height*4;成功时填满并返回 Ok。
  virtual Status readback_rgba8(std::uint8_t* out, std::size_t size_bytes) const = 0;
};

}  // namespace magicblocks::device
