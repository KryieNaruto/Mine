// MagicBlocks SDK —— L2 S9 渲染特性注册表实现(注册序即录制序 / 移除不重排)
//
// 零 Vulkan、零时钟。特性只能通过上下文拿到提交队列,不能碰命令列表(spec §5.4)。
#include <algorithm>
#include <string>
#include <vector>

#include "scene/scene_internal.h"

namespace magicblocks::scene {
namespace {

class FeatureRegistryImpl final : public IRenderFeatureRegistry {
 public:
  void add(IRenderFeature* feature) override {
    // nullptr 不是"合法特性",静默接受会让录制期崩。
    if (feature == nullptr) return;
    if (std::find(features_.begin(), features_.end(), feature) != features_.end()) return;
    features_.push_back(feature);
  }

  bool remove(IRenderFeature* feature) override {
    auto it = std::find(features_.begin(), features_.end(), feature);
    if (it == features_.end()) return false;
    // erase 保序:后面的元素相对顺序不变(RemoveKeepsOrder)。
    features_.erase(it);
    return true;
  }

  std::size_t size() const override { return features_.size(); }

  const std::vector<IRenderFeature*>& features() const override { return features_; }

  Status record_layer(FeatureLayer layer, FeatureRecordContext& ctx) override {
    if (ctx.queue == nullptr) {
      return Status::Invalid("IRenderFeatureRegistry::record_layer: 上下文的 queue 为空");
    }
    // 严格按注册序;layer 不匹配的跳过。
    for (IRenderFeature* feature : features_) {
      if (feature == nullptr) continue;
      if (feature->layer() != layer) continue;
      Status s = feature->record(ctx);
      if (!s.ok()) return s;
    }
    return Status::Ok_();
  }

 private:
  std::vector<IRenderFeature*> features_;
};

}  // namespace

std::unique_ptr<IRenderFeatureRegistry> create_feature_registry() {
  return std::make_unique<FeatureRegistryImpl>();
}

}  // namespace magicblocks::scene
