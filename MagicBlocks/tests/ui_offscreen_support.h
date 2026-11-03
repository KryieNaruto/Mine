// 测试辅助:一个"只存在、不做任何 GPU 工作"的离屏目标。
//
// 为什么单独成文件:`tools/check_ui_no_readback.py`(UIG1 门)禁止 UI 层与 UI 测试
// 出现"读回"能力,只有路径/文件名含 `offscreen` 的文件豁免(spec §5.5 允许离屏
// 路径读回)。Presenter 的 Offscreen 分支确实需要 IRenderTarget 接口(它继承出
// readback_rgba8),所以这个 fake 必须待在豁免范围内的文件里。
//
// 本目标不会被真的读回:用它的用例只验证**状态机**(Ok/Skipped/帧计数),
// 不验证像素。像素由 SwiftShader 的真实离屏目标和 golden 负责。
#pragma once

#include <cstddef>
#include <cstdint>

#include "magicblocks/device/render_target.h"

namespace magicblocks::ui::test {

class FakeOffscreenTarget final : public device::IRenderTarget {
 public:
  explicit FakeOffscreenTarget(device::Extent2D e) : extent_(e) {}

  device::TargetDesc::Kind kind() const override { return device::TargetDesc::Kind::Offscreen; }
  device::Extent2D extent() const override { return extent_; }
  device::Format color_format() const override { return device::Format::RGBA8_UNORM; }
  device::u32 samples() const override { return 1; }
  device::AttachmentView color_attachment() const override { return {}; }
  device::AttachmentView depth_attachment() const override { return {}; }
  device::Status readback_rgba8(std::uint8_t*, std::size_t) const override {
    return device::Status::Unsupported("FakeOffscreenTarget 不参与像素验证");
  }

 private:
  device::Extent2D extent_;
};

}  // namespace magicblocks::ui::test
