// MagicBlocks SDK —— L2 S3 渲染图实现(pass 声明序 / 附件读写 / 先写后读 / 幂等校验)
//
// 零 Vulkan(G3)、零时钟(D13)。只操作 IRenderTarget 的公共面,不碰句柄。
#include <algorithm>
#include <string>
#include <vector>

#include "scene/scene_internal.h"

namespace magicblocks::scene {

AttachmentRef AttachmentRef::make_external(IRenderTarget* target) {
  AttachmentRef ref;
  ref.source = Source::ExternalTarget;
  ref.external = target;
  ref.producer_pass = 0;
  return ref;
}

AttachmentRef AttachmentRef::make_pass_output(std::size_t producer_pass) {
  AttachmentRef ref;
  ref.source = Source::PassOutput;
  ref.external = nullptr;
  ref.producer_pass = producer_pass;
  return ref;
}

namespace {

class RenderGraphImpl final : public IRenderGraph {
 public:
  void add_pass(const PassDesc& desc) override { passes_.push_back(desc); }

  void clear() override { passes_.clear(); }

  std::size_t pass_count() const override { return passes_.size(); }

  const std::vector<PassDesc>& passes() const override { return passes_; }

  IRenderTarget* resolve(const AttachmentRef& ref) const override {
    switch (ref.source) {
      case AttachmentRef::Source::ExternalTarget:
        return ref.external;
      case AttachmentRef::Source::PassOutput:
        // N1:反向查 producer 的 own_target。找不到/为空 → nullptr(调用方必须当成错误)。
        if (ref.producer_pass >= passes_.size()) return nullptr;
        return passes_[ref.producer_pass].own_target;
    }
    return nullptr;
  }

  // 幂等:只读 passes_,不缓存也不修改任何成员。
  GraphValidation validate() const override {
    GraphValidation out;
    out.status = Status::Ok_();
    out.failing_pass = 0;
    out.message.clear();

    // 逐个 pass 校验。
    for (std::size_t k = 0; k < passes_.size(); ++k) {
      const PassDesc& pass = passes_[k];
      const std::string where = "pass[" + std::to_string(k) + "] \"" + pass.name + "\": ";

      if (pass.name.empty()) {
        return fail(out, k, where + "name 为空(pass 名要进命令流,必须显式给)");
      }

      // 规则 1:输出附件必须可解析。
      if (resolve(pass.color_output) == nullptr) {
        return fail(out, k, where + "color_output 无法解析(外部目标为空,或 producer 的 "
                                 "own_target 未设置)");
      }

      // 规则 2:输出 source == PassOutput 时,producer 必须**已经**在本 pass 之前声明
      //         (不允许前向引用),且 producer 自己必须有 own_target。
      if (pass.color_output.source == AttachmentRef::Source::PassOutput) {
        if (pass.color_output.producer_pass >= k) {
          return fail(out, k, where + "color_output 前向引用/自引用 producer[" +
                                   std::to_string(pass.color_output.producer_pass) +
                                   "](必须引用更早声明的 pass)");
        }
        if (passes_[pass.color_output.producer_pass].own_target == nullptr) {
          return fail(out, k, where + "color_output 的 producer[" +
                                   std::to_string(pass.color_output.producer_pass) +
                                   "] 没有 own_target,无处取值");
        }
      }

      // 规则 3:每个输入附件都必须可解析(先写后读)。
      for (std::size_t in = 0; in < pass.color_inputs.size(); ++in) {
        const AttachmentRef& ref = pass.color_inputs[in];
        if (resolve(ref) == nullptr) {
          return fail(out, k, where + "color_inputs[" + std::to_string(in) + "] 无法解析");
        }
        if (ref.source == AttachmentRef::Source::PassOutput) {
          if (ref.producer_pass >= k) {
            return fail(out, k, where + "color_inputs[" + std::to_string(in) + "] 读尚未写入的附件:"
                                     " producer[" + std::to_string(ref.producer_pass) +
                                     "] 不在本 pass 之前(先离屏再贴图要求 producer 先声明)");
          }
        }
      }
    }

    return out;
  }

 private:
  static GraphValidation fail(GraphValidation& out, std::size_t index,
                              const std::string& message) {
    out.status = Status::Invalid(message);
    out.failing_pass = index;
    out.message = message;
    return out;
  }

  std::vector<PassDesc> passes_;
};

}  // namespace

std::unique_ptr<IRenderGraph> create_render_graph() {
  return std::make_unique<RenderGraphImpl>();
}

}  // namespace magicblocks::scene
