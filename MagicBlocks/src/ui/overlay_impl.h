// MagicBlocks UI 消费层 —— 覆盖层绘制实现的私有声明(只给 src/ui/ 内部用)
//
// 公共面对上层只有 `magicblocks/ui/overlay.h` 的 `stub::IOverlay2D`
// (追加图元 → 后端着色)。本头暴露两件公共面不该有、但离屏路径与测试需要的东西:
//   1. 图元的**结构化序列**(不透明句柄 → 可断言的图元列表);
//   2. 合批统计与本帧图元的只读访问(golden 比对与 G3 门槛都要它)。
//
// 放在 `src/ui/` 而不是 `include/`:上层拿不到,也就不会有人拿它当第二套绘制 API。
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "mb_internal/magicblocks/ui/overlay.h"
#include "mb_internal/magicblocks/ui/types.h"

namespace magicblocks::ui::detail {

// overlay.h 里的桩实体都在 ui::stub 名下,这里只增别名。
using stub::IOverlay2D;
using stub::kNoTexture;
using stub::StrokeStyle;
using stub::TextureHandle;

enum class PrimitiveKind : std::uint8_t {
  Fill = 0,
  Stroke = 1,
  Line = 2,
};

// 一个图元的完整描述(顺序即绘制顺序)。数值自包含 → 可 diff、可复现。
struct Primitive {
  PrimitiveKind kind = PrimitiveKind::Fill;
  Rgba8 color{};
  Vec2 a{};
  Vec2 b{};
  f32 width = 1.f;
  bool dashed = false;
  f32 dash_on = 6.f;
  f32 dash_off = 4.f;
};

// 该图元在 GPU 侧展开成多少顶点(合批与 G3 顶点上限的判据)。
u32 prim_vertex_count(const Primitive& p);

// `stub::IOverlay2D` 的落地实现。**不拥有任何 GPU 资源**:图元先进本对象,
// 由调用方(IFrameRenderer / 离屏后端)在帧末取走。
class Overlay2D final : public stub::IOverlay2D {
 public:
  Overlay2D();
  ~Overlay2D() override;

  Overlay2D(const Overlay2D&) = delete;
  Overlay2D& operator=(const Overlay2D&) = delete;

  void begin(Rect viewport_px) override;
  void fill_rect(Rect r, Rgba8 color) override;
  void stroke_rect(Rect r, Rgba8 color) override;
  void line(Vec2 a, Vec2 b, Rgba8 color, const StrokeStyle& style) override;
  void blit(Rect dst, TextureHandle texture, Rect src_uv, Rgba8 tint) override;
  void end() override;
  stub::IOverlay2D::Stats stats() const override;

  // 合批统计(按 spec §6 G3 的"按纹理/管线合批"口径计算;每帧调用一次)。
  stub::IOverlay2D::Stats compute_stats() const;

  std::size_t primitive_count() const;
  const std::vector<Primitive>& primitives() const;
  Rect viewport() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// 离屏路径的软件光栅化(spec §5.5)。`out_rgba8` 必须是 width*height*4 的 RGBA8,
// `row_bytes` = width*4(测试允许更大 stride)。不透明背景先由调用方填好。
void rasterize_to_rgba8(Rect viewport, const std::vector<Primitive>& prims, std::uint8_t* out,
                        u32 width, u32 height, u32 row_bytes);

}  // namespace magicblocks::ui::detail
