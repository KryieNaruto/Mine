// MagicBlocks SDK —— L2 S3 `IRenderGraph`:渲染图与附件依赖
//
// 设计约束(spec §4 L2 S3 / plans-l2 §4.4):
//   - **pass 按声明序执行**:`passes()` 的顺序恒等于声明顺序
//     (RenderGraph.PassesAreInDeclarationOrder)。渲染图不做自动重排 ——
//     重排会让命令流哈希随实现漂移。
//   - **附件读写构成依赖图**:每个 pass 用 `AttachmentRef` 声明它**读**(输入)与
//     **写**(输出)的附件。所以"先离屏再贴图"(B4)能表达成:
//     pass A 写一块离屏 target → pass B 把它当输入。
//   - **N1 裁决**:`AttachmentRef::PassOutput` 只记 producer 下标还不够 ——
//     回放时得拿到那个 `IRenderTarget*` 才能录 `begin_pass`。因此:
//       * `PassDesc::own_target` 让一个 pass 声明"我输出的那块 target 是谁";
//       * `IRenderGraph::resolve(ref)` 把任意 `AttachmentRef` 解析成**具体指针**。
//     `validate()` 检查 `resolve()` 结果非空;`record()` 阶段只信任 `validate()` 过。
//   - **validate() 幂等**:连续两次都 Ok,且不改任何可观测状态
//     (RenderGraph.ValidateIsIdempotent)。
//   - **先写后读**:读一个还没被任何 pass 写过的附件 = 非法(RejectsReadBeforeWrite);
//     引用不存在的 producer、前向引用(读后面的 pass 的输出)同样非法。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "magicblocks/device/render_target.h"
#include "magicblocks/device/result.h"
#include "magicblocks/scene/types.h"

namespace magicblocks::scene {

// 附件引用。
//
// `ExternalTarget`:`external` 指向场景外部的目标(比如最终的离屏输出),
//                   由调用方在 `add_pass` 时就给好指针。
// `PassOutput`    :引用**另一个 pass 的输出**,用 `producer_pass` 记下标。
//                   实际指针由 `IRenderGraph::resolve()` 按 producer 的
//                   `own_target` 反查得到(N1)。
struct AttachmentRef {
  enum class Source : std::uint8_t {
    ExternalTarget = 0,
    PassOutput = 1,
  };

  Source source = Source::ExternalTarget;
  IRenderTarget* external = nullptr;        // source == ExternalTarget 时必填
  std::size_t producer_pass = 0;            // source == PassOutput 时有效

  static AttachmentRef make_external(IRenderTarget* target);
  static AttachmentRef make_pass_output(std::size_t producer_pass);
};

// 一个 pass 的声明。
struct PassDesc {
  std::string name;                 // 必填;命令流里逐字节保存(空名 → validate 失败)
  std::uint8_t layer = 0;           // 供 IRenderFeature 的 layer 匹配用(见 feature 头)
  std::uint8_t channel = 0;         // 通道号(同一 layer 内可多 pass)
  bool clear_color = true;
  float clear_color_rgba[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  bool clear_depth = true;
  float clear_depth_value = 1.0f;
  // render area 的**原始调用值**(0 = 整图)。注意这里**不做** 0→extent 的解析:
  // L1 会在入流时解析掉"0 表示整图"的语义(U2),L2 必须保存原始值,
  // 否则二进制往返会把 0 抹成具体宽度(plans-l2 §4.6.1.1)。
  std::uint32_t area_x = 0;
  std::uint32_t area_y = 0;
  std::uint32_t area_width = 0;    // 0 = 整图
  std::uint32_t area_height = 0;   // 0 = 整图

  // 本 pass **写入**的附件。source == PassOutput 时表示"我转发别人的输出"。
  AttachmentRef color_output{};
  // 本 pass **读入**的附件(作为输入纹理被采样)。可以是别的 pass 的输出(B4)。
  std::vector<AttachmentRef> color_inputs;

  // **N1**:本 pass 自己的输出 target。当其它 pass 用 `make_pass_output(idx)`
  // 引用本 pass 时,`resolve()` 就返回这个指针。为空则无法被引用。
  IRenderTarget* own_target = nullptr;
};

// 校验结果:Ok 或 Invalid + 说明(带 pass 下标与名字)。
struct GraphValidation {
  Status status{};              // status.ok() 才通过
  std::size_t failing_pass = 0; // 失败时的 pass 下标(Ok 时为 0)
  std::string message;          // 人读说明
};

class IRenderGraph {
 public:
  IRenderGraph(const IRenderGraph&) = delete;
  IRenderGraph& operator=(const IRenderGraph&) = delete;
  virtual ~IRenderGraph() = default;

  // 按声明序追加一个 pass。
  virtual void add_pass(const PassDesc& desc) = 0;

  // 清空全部 pass(含声明序)。
  virtual void clear() = 0;

  virtual std::size_t pass_count() const = 0;

  // 按声明序的 pass 列表。
  virtual const std::vector<PassDesc>& passes() const = 0;

  // 把附件引用解析成具体目标指针。无法解析时返回 nullptr ——
  // 调用方**不得**把 nullptr 传给 begin_pass(L1 会拒,但那是晚失败)。
  virtual IRenderTarget* resolve(const AttachmentRef& ref) const = 0;

  // 校验整张图。幂等、不改状态。
  virtual GraphValidation validate() const = 0;

 protected:
  IRenderGraph() = default;
};

}  // namespace magicblocks::scene
