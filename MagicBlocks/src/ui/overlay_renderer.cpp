// MagicBlocks UI 消费层 —— 覆盖层绘制实现(桩 IOverlay2D 的落地 + 合批)
//
// 本文件是 UI 层**唯一**把图元变成像素的地方,同一份图元序列同时服务于两条路径:
//   - 主路径:合批成 drawcall,由 IFRameRenderer 录进 swapchain 帧(spec §5.2)
//             (GPU 侧真正画三角形;本文件的软件光栅器**不参与**主路径);
//   - 离屏路径:图元直接软件光栅化进离屏 image(golden/CLI,spec §5.5)。
//
// 为什么离屏路径用软件光栅而不是 GPU:离屏 golden 的目的是**布局与图元序列的
// 稳定性**回归(layer-ui golden),软件光栅逐像素确定、零驱动差异,换驱动不会假红。
// GPU 侧的绘制正确性由 SDK 自身的离屏渲染用例覆盖。
//
// 合批规则(spec §6 G3「按纹理/管线合批」):
//   - 实心矩形 / 描边矩形 / 线段 都退化成"带颜色的三角形",同一管线 → 同一 batch;
//   - 一个 batch 的顶点数超过 4096 就切下一个 batch → 一次 drawcall;
//   - 图元容器按帧 clear 复用,稳态帧 0 次分配(spec §6 G5)。
//
// 光栅化刻意**不加抗锯齿**、坐标四舍五入到整数像素 —— 同一输入逐像素同输出。
#include "overlay_impl.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace magicblocks::ui::detail {

namespace {

inline std::uint8_t clamp_u8(f32 v) {
  if (v <= 0.f) return 0;
  if (v >= 255.f) return 255;
  return static_cast<std::uint8_t>(v + 0.5f);
}

// 源色 over 目标色(非预乘 alpha):dst = src*a + dst*(1-a)。
// 目标是不透明背景,因此 alpha 结果恒为 255(不累积透明度)。
inline void blend_px(std::uint8_t* dst, Rgba8 src) {
  const f32 a = static_cast<f32>(src.a) / 255.f;
  if (a <= 0.f) return;
  const f32 ia = 1.f - a;
  dst[0] = clamp_u8(static_cast<f32>(src.r) * a + static_cast<f32>(dst[0]) * ia);
  dst[1] = clamp_u8(static_cast<f32>(src.g) * a + static_cast<f32>(dst[1]) * ia);
  dst[2] = clamp_u8(static_cast<f32>(src.b) * a + static_cast<f32>(dst[2]) * ia);
  dst[3] = 255;
}

inline void put(std::uint8_t* base, u32 row_bytes, u32 width, u32 height, int x, int y,
                Rgba8 color) {
  if (x < 0 || y < 0 || x >= static_cast<int>(width) || y >= static_cast<int>(height)) return;
  blend_px(base + static_cast<std::size_t>(y) * row_bytes + static_cast<std::size_t>(x) * 4, color);
}

u32 line_vertex_count(const Primitive& p) {
  if (!p.dashed) return 4;  // 一个线段四边形
  // 虚线按段数估顶点(每段 4 顶点);只用于合批判界,不参与实际绘制
  const f32 dx = p.b.x - p.a.x, dy = p.b.y - p.a.y;
  const f32 len = std::sqrt(dx * dx + dy * dy);
  const f32 period = std::max(1.f, p.dash_on + p.dash_off);
  const u32 segs = static_cast<u32>(std::max(1.f, std::floor(len / period)));
  return 4u * segs;
}

}  // namespace

u32 prim_vertex_count(const Primitive& p) {
  switch (p.kind) {
    case PrimitiveKind::Fill: return 6;    // 两个三角形
    case PrimitiveKind::Stroke: return 24; // 四边各一个四边形(4x4) → 保守 24
    case PrimitiveKind::Line: return line_vertex_count(p);
  }
  return 0;
}

// ---------------------------------------------------------------- Overlay2D::Impl

struct Overlay2D::Impl {
  Rect viewport{};
  std::vector<Primitive> prims;
  bool open = false;

  static constexpr std::size_t kReserve = 4096;

  Impl() { prims.reserve(kReserve); }

  void reset_frame() {
    prims.clear();
    open = false;
  }
};

Overlay2D::Overlay2D() : impl_(std::make_unique<Impl>()) {}
Overlay2D::~Overlay2D() = default;

void Overlay2D::begin(Rect viewport_px) {
  impl_->reset_frame();
  impl_->viewport = viewport_px;
  impl_->open = true;
}

void Overlay2D::fill_rect(Rect r, Rgba8 color) {
  if (r.empty() || color.a == 0) return;
  Primitive p{};
  p.kind = PrimitiveKind::Fill;
  p.color = color;
  p.a = {r.x, r.y};
  p.b = {r.right(), r.bottom()};
  impl_->prims.push_back(p);
}

void Overlay2D::stroke_rect(Rect r, Rgba8 color) {
  if (r.empty() || color.a == 0) return;
  Primitive p{};
  p.kind = PrimitiveKind::Stroke;
  p.color = color;
  p.a = {r.x, r.y};
  p.b = {r.right(), r.bottom()};
  impl_->prims.push_back(p);
}

void Overlay2D::line(Vec2 a, Vec2 b, Rgba8 color, const StrokeStyle& style) {
  if (color.a == 0) return;
  Primitive p{};
  p.kind = PrimitiveKind::Line;
  p.color = color;
  p.a = a;
  p.b = b;
  p.width = style.width_px;
  p.dashed = style.dashed;
  p.dash_on = style.dash_on_px;
  p.dash_off = style.dash_off_px;
  impl_->prims.push_back(p);
}

void Overlay2D::blit(Rect, TextureHandle, Rect, Rgba8) {
  // 本轮没有纹理图元(无字体图集/色表);接口保留,调用忽略。
}

void Overlay2D::end() { impl_->open = false; }

stub::IOverlay2D::Stats Overlay2D::compute_stats() const {
  stub::IOverlay2D::Stats s{};
  constexpr u32 kMaxVertsPerBatch = 4096;
  u32 batch_verts = 0;
  bool have_batch = false;
  for (const auto& p : impl_->prims) {
    const u32 v = prim_vertex_count(p);
    if (have_batch && batch_verts + v > kMaxVertsPerBatch) {
      ++s.batches;
      ++s.draw_calls;
      batch_verts = 0;
    }
    have_batch = true;
    batch_verts += v;
    s.vertices += v;
  }
  if (have_batch) {
    ++s.batches;
    ++s.draw_calls;
  }
  return s;
}

stub::IOverlay2D::Stats Overlay2D::stats() const { return compute_stats(); }

std::size_t Overlay2D::primitive_count() const { return impl_->prims.size(); }
const std::vector<Primitive>& Overlay2D::primitives() const { return impl_->prims; }
Rect Overlay2D::viewport() const { return impl_->viewport; }

// ---------------------------------------------------------------- 软件光栅化(离屏)

void rasterize_to_rgba8(Rect viewport, const std::vector<Primitive>& prims, std::uint8_t* out,
                        u32 width, u32 height, u32 row_bytes) {
  if (!out) return;
  (void)viewport;
  for (const auto& p : prims) {
    switch (p.kind) {
      case PrimitiveKind::Fill: {
        const int x0 = std::max(0, static_cast<int>(std::floor(p.a.x)));
        const int y0 = std::max(0, static_cast<int>(std::floor(p.a.y)));
        const int x1 = std::min(static_cast<int>(width), static_cast<int>(std::ceil(p.b.x)));
        const int y1 = std::min(static_cast<int>(height), static_cast<int>(std::ceil(p.b.y)));
        for (int y = y0; y < y1; ++y) {
          std::uint8_t* row = out + static_cast<std::size_t>(y) * row_bytes;
          for (int x = x0; x < x1; ++x) blend_px(row + static_cast<std::size_t>(x) * 4, p.color);
        }
        break;
      }
      case PrimitiveKind::Stroke: {
        const int x0 = static_cast<int>(std::floor(p.a.x));
        const int y0 = static_cast<int>(std::floor(p.a.y));
        const int x1 = static_cast<int>(std::ceil(p.b.x)) - 1;
        const int y1 = static_cast<int>(std::ceil(p.b.y)) - 1;
        for (int x = x0; x <= x1; ++x) {
          put(out, row_bytes, width, height, x, y0, p.color);
          if (y1 != y0) put(out, row_bytes, width, height, x, y1, p.color);
        }
        for (int y = y0; y <= y1; ++y) {
          put(out, row_bytes, width, height, x0, y, p.color);
          if (x1 != x0) put(out, row_bytes, width, height, x1, y, p.color);
        }
        break;
      }
      case PrimitiveKind::Line: {
        const f32 dx = p.b.x - p.a.x, dy = p.b.y - p.a.y;
        const f32 len = std::sqrt(dx * dx + dy * dy);
        if (len <= 0.f) {
          put(out, row_bytes, width, height, static_cast<int>(std::lround(p.a.x)),
              static_cast<int>(std::lround(p.a.y)), p.color);
          break;
        }
        if (p.dashed) {
          const f32 ux = dx / len, uy = dy / len;
          const f32 period = std::max(1.f, p.dash_on + p.dash_off);
          for (f32 t = 0.f; t < len; t += period) {
            const f32 seg_end = std::min(len, t + p.dash_on);
            for (f32 s = t; s < seg_end; s += 1.f) {
              put(out, row_bytes, width, height,
                  static_cast<int>(std::lround(p.a.x + ux * s)),
                  static_cast<int>(std::lround(p.a.y + uy * s)), p.color);
            }
          }
        } else {
          // Bresenham:纯整数、无浮点累积,确定性最强
          int x0 = static_cast<int>(std::lround(p.a.x));
          int y0 = static_cast<int>(std::lround(p.a.y));
          const int x1 = static_cast<int>(std::lround(p.b.x));
          const int y1 = static_cast<int>(std::lround(p.b.y));
          int ddx = std::abs(x1 - x0);
          const int sx = x0 < x1 ? 1 : -1;
          int ddy = -std::abs(y1 - y0);
          const int sy = y0 < y1 ? 1 : -1;
          int err = ddx + ddy;
          while (true) {
            put(out, row_bytes, width, height, x0, y0, p.color);
            if (x0 == x1 && y0 == y1) break;
            const int e2 = 2 * err;
            if (e2 >= ddy) {
              err += ddy;
              x0 += sx;
            }
            if (e2 <= ddx) {
              err += ddx;
              y0 += sy;
            }
          }
        }
        break;
      }
    }
  }
}

}  // namespace magicblocks::ui::detail
