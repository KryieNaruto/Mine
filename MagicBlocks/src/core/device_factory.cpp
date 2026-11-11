// MagicBlocks —— 后端工厂入口(非 backend 文件,只做转发)
//
// G3 要求:公共面不出现 Vk*。本文件只把 create_render_device 转发给实现,
// 里面没有任何 Vulkan 类型。
//
// 2026-11-11(t3):转发的**实现**搬到了 core/device_ops.cpp —— 这个文件原本写着
// `return backend::create_vulkan_device(desc);`,即 backend/ 之外出现了
// `backend::` 命名空间,G3 门控(tools/check_layer_purity.py)判红。
// 现在放行的那**一个**翻译层是 core/device_ops.cpp(理由见其文件头),
// 本文件退回纯委托,一行分支都没有。
#include "mb_internal/magicblocks/device/render_device.h"

#include "core/device_ops.h"

namespace magicblocks::device {

Result<std::unique_ptr<IRenderDevice>> create_render_device(const DeviceDesc& desc) {
  return core::create_render_device(desc);
}

}  // namespace magicblocks::device
