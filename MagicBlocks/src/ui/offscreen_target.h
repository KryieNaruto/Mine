// MagicBlocks UI 消费层 —— 离屏呈现后端的私有声明(只给 src/ui/ 内部用)
//
// spec §5.5:离屏路径 = 场景 + UI 渲染到离屏 image → 读回。它**只**用于
// CLI / CI / golden / 命令回放,上屏路径不经过它(spec §5.5 原文)。
//
// 这里把"UI 层产出的图元"交给后端桩(`backend::VulkanFrameRenderer` /
// `UiOverlayPass`)去渲染并读回。图元的顺序即绘制顺序,后端内部做合批。
#pragma once

#include <cstdint>

#include "magicblocks/ui/types.h"

namespace magicblocks::backend {
class VulkanFrameRenderer;
}

namespace magicblocks::ui {

class UiApp;

class OffscreenTargetRenderer {
 public:
  OffscreenTargetRenderer();
  ~OffscreenTargetRenderer();

  OffscreenTargetRenderer(const OffscreenTargetRenderer&) = delete;
  OffscreenTargetRenderer& operator=(const OffscreenTargetRenderer&) = delete;

  // 绑定后端渲染器(外部持有;后端在 SDK 侧,UI 层不构造它)。
  bool attach(backend::VulkanFrameRenderer* renderer);
  bool attached() const { return renderer_ != nullptr; }

  // 一帧:让 UI 层画图元 → 图元转后端实例 → 渲染 → 读回 RGBA8。
  // `row_bytes` 必须 >= width*4(允许更大 stride)。
  bool present(UiApp& app, u32 width, u32 height, std::uint8_t* out_rgba8, u32 row_bytes);

 private:
  backend::VulkanFrameRenderer* renderer_ = nullptr;
};

}  // namespace magicblocks::ui
