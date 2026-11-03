// MagicBlocks SDK —— 帧同步(D5)
//
// N = in_flight_frames(2 或 3)帧环形资源复用。
// 注意边界(A3):IFrameSync **不碰 present**,present 归 ISwapchain;
// 这里只有 帧序号 → 槽位 的映射与每槽 fence 的等待。
#pragma once

#include <cstdint>

#include "magicblocks/device/result.h"
#include "magicblocks/device/types.h"

namespace magicblocks::device {

class IFrameSync {
 public:
  virtual ~IFrameSync() = default;

  // 帧环大小,来自 DeviceDesc::in_flight_frames,必须是 2 或 3。
  virtual u32 in_flight_count() const = 0;

  // 帧序号 → 环形槽位(== frame_index % in_flight_count())。
  virtual u32 slot_for_frame(std::uint64_t frame_index) const = 0;

  // 等待该槽位的 fence 以便安全复用(资源真复用的关键)。
  virtual Status wait_slot(u32 slot) = 0;

  // 推进帧序号(同时是 L1 VMA 帧边界的挂点)。
  virtual void advance_frame_index() = 0;

  virtual std::uint64_t frame_index() const = 0;
};

}  // namespace magicblocks::device
