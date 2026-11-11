// MagicBlocks SDK —— 应用生命周期(D7)
//
// Android: onPause/onResume/onSurfaceChanged;
// Windows/Linux: 窗口创建/销毁/尺寸变化;
// iOS: CAMetalLayer 变更走同一事件,接口不出现平台宏(spec §9)。
#pragma once

#include <cstdint>

namespace magicblocks::device {

struct Extent2D;

enum class LifecycleKind : std::uint8_t {
  Unknown = 0,
  SurfaceCreated,    // Android: APP_CMD_INIT_WINDOW
  SurfaceDestroyed,  // Android: APP_CMD_DESTROY_WINDOW
  SurfaceChanged,    // 旋转/缩放
  Paused,            // 前后台
  Resumed,
};

struct AppLifecycleEvent {
  LifecycleKind kind = LifecycleKind::Unknown;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
};

}  // namespace magicblocks::device
