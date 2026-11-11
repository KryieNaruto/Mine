// MagicBlocks L3 —— IOverlay2D 实现（spec §13.13）
//
// 设计要点：
//   1. 收集期只往 vector 里 push 图元描述，**不做三角化**；
//      三角化推迟到 build()，这样同一帧内的图元可以连续追加、缓存友好。
//   2. 所有图元最终都化归成"带像素宽度的线段"或"实心多边形"两种，
//      三角化规则各一条，避免每种图元各写一份易错的顶点缠绕逻辑。
//   3. 线段三角化用**无对齐的薄四边形**（不按像素网格吸附）：
//      对齐会让线在不同角度下粗细跳变，反而不如不吸附。

#include "mb_internal/magicblocks/primitives/overlay2d.h"

#include <algorithm>
#include <cmath>

namespace magicblocks::primitives {
namespace {

constexpr float kPi = 3.14159265358979323846f;

std::uint8_t to_u8(float v) {
  if (v <= 0.0f) return 0;
  if (v >= 1.0f) return 255;
  return static_cast<std::uint8_t>(v * 255.0f + 0.5f);
}

void pack_color(OverlayVertex* v, const Color4& c) {
  v->color[0] = to_u8(c.r);
  v->color[1] = to_u8(c.g);
  v->color[2] = to_u8(c.b);
  v->color[3] = to_u8(c.a);
}

class Overlay2D final : public IOverlay2D {
 public:
  // 基类删了拷贝构造，隐式默认构造因此也被删除 —— 显式写出来。
  Overlay2D() = default;

  void begin_frame() override {
    lines_.clear();
    rects_.clear();
    rings_.clear();
    tris_.clear();
    geo_.vertices.clear();
    geo_.indices.clear();
    stats_ = OverlayStats{};
  }

  void build() override {
    geo_.vertices.clear();
    geo_.indices.clear();
    stats_.reallocations = 0;
    stats_.line_count = static_cast<std::uint32_t>(lines_.size());
    stats_.rect_count = static_cast<std::uint32_t>(rects_.size());
    stats_.ring_count = static_cast<std::uint32_t>(rings_.size());
    stats_.tri_count = static_cast<std::uint32_t>(tris_.size());

    const std::size_t v0 = geo_.vertices.capacity();
    const std::size_t i0 = geo_.indices.capacity();

    for (const LineDesc& l : lines_) emit_line(l.from, l.to, l.width_px, l.color);
    for (const RectDesc& r : rects_) emit_rect(r);
    for (const RingDesc& g : rings_) emit_ring(g);
    for (const TriDesc& t : tris_) emit_tri(t);

    if (geo_.vertices.capacity() != v0 || geo_.indices.capacity() != i0) {
      ++stats_.reallocations;
    }
    stats_.vertex_count = static_cast<std::uint32_t>(geo_.vertices.size());
    stats_.index_count = static_cast<std::uint32_t>(geo_.indices.size());
    built_ = true;
  }

  void set_viewport(std::uint32_t w, std::uint32_t h) override {
    vp_w_ = w;
    vp_h_ = h;
  }
  std::uint32_t viewport_width() const override { return vp_w_; }
  std::uint32_t viewport_height() const override { return vp_h_; }

  void push_line(const LineDesc& d) override { lines_.push_back(d); }
  void push_dash(const DashDesc& d) override {
    // 虚线在收集期就切成实线段：切分只依赖像素长度，
    // 不需要 GPU 状态，早切早省（build 时不用再算）。
    const float dx = d.to.x - d.from.x;
    const float dy = d.to.y - d.from.y;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len <= 1e-6f) return;
    const float dash = std::max(d.dash_px, 0.5f);
    const float gap = std::max(d.gap_px, 0.5f);
    const float period = dash + gap;
    const float ux = dx / len;
    const float uy = dy / len;

    float t = 0.0f;
    while (t < len) {
      const float t1 = std::min(t + dash, len);
      LineDesc seg;
      seg.from = {d.from.x + ux * t, d.from.y + uy * t};
      seg.to = {d.from.x + ux * t1, d.from.y + uy * t1};
      seg.width_px = d.width_px;
      seg.color = d.color;
      lines_.push_back(seg);
      t += period;
    }
  }

  void push_rect(const RectDesc& d) override { rects_.push_back(d); }
  void push_ring(const RingDesc& d) override { rings_.push_back(d); }
  void push_tri(const TriDesc& d) override { tris_.push_back(d); }

  const OverlayGeometry& geometry() const override { return geo_; }
  const OverlayStats& stats() const override { return stats_; }

  void reserve(std::uint32_t lines, std::uint32_t rects) override {
    // 每条线 4 顶点 6 索引；每条虚线段也按线算（保守按 8 段/条估）。
    const std::size_t lv = static_cast<std::size_t>(lines) * 4u * 8u;
    if (geo_.vertices.capacity() < lv) geo_.vertices.reserve(lv);
    if (geo_.indices.capacity() < lv / 2u) geo_.indices.reserve(lv / 2u);
    lines_.reserve(lines);
    rects_.reserve(rects);
  }

  void set_frame_arena(IFrameArena* arena) override { (void)arena; }

 private:
  void emit_vertex(float x, float y, const Color4& c) {
    OverlayVertex v;
    v.position[0] = x;
    v.position[1] = y;
    pack_color(&v, c);
    geo_.vertices.push_back(v);
  }

  // 一条有宽度的线段 → 一个四边形（4 顶点 6 索引）。
  // 做法：沿法线方向各偏移半个线宽。法线取 (-uy, ux)。
  void emit_line(const Vec2& from, const Vec2& to, float width, const Color4& c) {
    const float dx = to.x - from.x;
    const float dy = to.y - from.y;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len <= 1e-6f) {
      // 退化成一个点：画一个方块，避免"点状图元"整个消失。
      const float hw = std::max(width, 1.0f) * 0.5f;
      emit_quad(from.x - hw, from.y - hw, from.x + hw, from.y + hw, c);
      return;
    }
    const float ux = dx / len;
    const float uy = dy / len;
    const float hw = std::max(width, 0.5f) * 0.5f;
    const float nx = -uy * hw;
    const float ny = ux * hw;

    const std::uint32_t base = static_cast<std::uint32_t>(geo_.vertices.size());
    emit_vertex(from.x + nx, from.y + ny, c);
    emit_vertex(from.x - nx, from.y - ny, c);
    emit_vertex(to.x - nx, to.y - ny, c);
    emit_vertex(to.x + nx, to.y + ny, c);
    push_quad_indices(base);
  }

  void emit_quad(float x0, float y0, float x1, float y1, const Color4& c) {
    const std::uint32_t base = static_cast<std::uint32_t>(geo_.vertices.size());
    emit_vertex(x0, y0, c);
    emit_vertex(x1, y0, c);
    emit_vertex(x1, y1, c);
    emit_vertex(x0, y1, c);
    push_quad_indices(base);
  }

  void push_quad_indices(std::uint32_t base) {
    geo_.indices.push_back(base + 0);
    geo_.indices.push_back(base + 1);
    geo_.indices.push_back(base + 2);
    geo_.indices.push_back(base + 0);
    geo_.indices.push_back(base + 2);
    geo_.indices.push_back(base + 3);
  }

  void emit_rect(const RectDesc& r) {
    const float x0 = std::min(r.min.x, r.max.x);
    const float y0 = std::min(r.min.y, r.max.y);
    const float x1 = std::max(r.min.x, r.max.x);
    const float y1 = std::max(r.min.y, r.max.y);

    if (r.border_px <= 0.0f) {
      emit_quad(x0, y0, x1, y1, r.color);
      return;
    }
    // 空心框：四条边各一条线。边宽是像素。
    const float w = r.border_px;
    emit_line({x0, y0}, {x1, y0}, w, r.color);  // 上
    emit_line({x1, y0}, {x1, y1}, w, r.color);  // 右
    emit_line({x1, y1}, {x0, y1}, w, r.color);  // 下
    emit_line({x0, y1}, {x0, y0}, w, r.color);  // 左
  }

  void emit_ring(const RingDesc& g) {
    const std::uint32_t seg = std::max<std::uint32_t>(g.segments, 3u);
    const float step = 2.0f * kPi / static_cast<float>(seg);
    Vec2 prev{g.center.x + g.radius_px, g.center.y};
    for (std::uint32_t i = 1; i <= seg; ++i) {
      const float a = step * static_cast<float>(i);
      Vec2 cur{g.center.x + g.radius_px * std::cos(a),
               g.center.y + g.radius_px * std::sin(a)};
      emit_line(prev, cur, g.width_px, g.color);
      prev = cur;
    }
  }

  void emit_tri(const TriDesc& t) {
    const std::uint32_t base = static_cast<std::uint32_t>(geo_.vertices.size());
    emit_vertex(t.a.x, t.a.y, t.color);
    emit_vertex(t.b.x, t.b.y, t.color);
    emit_vertex(t.c.x, t.c.y, t.color);
    geo_.indices.push_back(base + 0);
    geo_.indices.push_back(base + 1);
    geo_.indices.push_back(base + 2);
  }

  std::vector<LineDesc> lines_;
  std::vector<RectDesc> rects_;
  std::vector<RingDesc> rings_;
  std::vector<TriDesc> tris_;

  OverlayGeometry geo_;
  OverlayStats stats_;
  std::uint32_t vp_w_ = 1;
  std::uint32_t vp_h_ = 1;
  bool built_ = false;
};

}  // namespace

std::unique_ptr<IOverlay2D> create_overlay2d() { return std::make_unique<Overlay2D>(); }

}  // namespace magicblocks::primitives
