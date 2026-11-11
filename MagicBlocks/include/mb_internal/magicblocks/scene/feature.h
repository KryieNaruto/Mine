// MagicBlocks SDK —— L2 S9 `IRenderFeatureRegistry`:渲染特性注册表
//
// 设计约束(spec §4 L2 S9 / §5.3 / plans-l2):
//   - **UI pass 只是"又一个注册项"**:特性按 layer 归类,由渲染图统一录制。
//   - **注册顺序即录制顺序**(FeatureRegistry.RecordsInRegistrationOrder):
//     同一 layer 内严格按注册序 —— 顺序可变会让命令流哈希漂移。
//   - **移除不重排**(FeatureRegistry.RemoveKeepsOrder):移除中间项后其余相对序不变。
//   - **特性被录进其 layer 对应的 pass**(FeatureRegistry.RecordsIntoGraphPass)。
//   - 与 `ui::stub::IRenderFeatureRegistry`(L0/UI 那颗桩)**并存但不互通**:
//     那颗桩的方法是 `add`/`remove`,特性是 `record()` 无参;本层的是
//     `record(FeatureRecordContext&)`。两套名字相同、签名不同,各自在各自命名空间
//     (§5.3.1 / R11 —— 本轮不做转发)。
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "mb_internal/magicblocks/device/result.h"
#include "mb_internal/magicblocks/scene/command_stream.h"
#include "mb_internal/magicblocks/scene/submit_queue.h"
#include "mb_internal/magicblocks/scene/types.h"

namespace magicblocks::scene {

using device::Status;

// 渲染层。取值与 spec §5.2 的两个 pass 对应:先场景、后覆盖层。
enum class FeatureLayer : std::uint8_t {
  Scene = 0,
  Overlay = 1,
};

// 特性录制期拿到的上下文。**刻意只给这几样**(spec §5.4):
// 不给 command list、不给 swapchain、不给读回入口 —— 那条路只有 Presenter 有。
// 特性只能把绘制项塞进提交队列,由渲染图统一录制。
struct FeatureRecordContext {
  class ISubmitQueue* queue = nullptr;
  std::uint64_t frame_index = 0;
};

// 一个可注册进渲染图的渲染特性。
class IRenderFeature {
 public:
  virtual ~IRenderFeature() = default;

  // 归属的渲染层。决定它被录进哪个 pass。
  virtual FeatureLayer layer() const = 0;

  // 在渲染图录制期被调用。**不得**发起单独提交(spec §5.4)。
  virtual Status record(FeatureRecordContext& ctx) = 0;
};

class IRenderFeatureRegistry {
 public:
  IRenderFeatureRegistry(const IRenderFeatureRegistry&) = delete;
  IRenderFeatureRegistry& operator=(const IRenderFeatureRegistry&) = delete;
  virtual ~IRenderFeatureRegistry() = default;

  // 稳定顺序注册:同一 layer 内按注册序录制。
  virtual void add(IRenderFeature* feature) = 0;

  // 移除指定特性(不影响其余项的相对顺序)。返回 false = 未注册过。
  virtual bool remove(IRenderFeature* feature) = 0;

  virtual std::size_t size() const = 0;

  // 按注册序的全部特性。录制时按 layer 过滤。
  virtual const std::vector<IRenderFeature*>& features() const = 0;

  // 按注册序调用的录制入口(layer 匹配的才录)。
  virtual Status record_layer(FeatureLayer layer, FeatureRecordContext& ctx) = 0;

 protected:
  IRenderFeatureRegistry() = default;
};

}  // namespace magicblocks::scene
