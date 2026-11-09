// MagicBlocks L3 —— 画笔实现（spec §6.2.1）
//
// 核心算法：把一条中心线折线转成屏幕空间条带（ribbon）。
//
// 每个中心线点 P_i 采样一个切线方向，取其法线 N_i = perp(T_i)，
// 在 P_i ± N_i * (width/2) 处各放一个顶点，相邻两点之间用
// 两个三角形连接（P_iL, P_iR, P_{i+1}L / P_{i+1}L, P_iR, P_{i+1}R）。
//
// 为什么在 CPU 上做而不是用几何着色器展开：
//   G1 门控明确禁止 geometry shader（移动端支持差）。
//   而且 CPU 展开能精确控制增量更新 —— 见下面的 append_segment。
#include "magicblocks/primitives/painter.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace magicblocks::primitives {
namespace {

constexpr float kPi = 3.14159265358979323846f;

// 相邻两点距离小于这个值就当同一个点，不生成段。
// 触摸屏抖动极多，不去重会让一笔产生大量退化的零面积三角形，
// 白白消耗带宽和填充率。
constexpr float kMinSegmentPx = 0.35f;

// 平滑细分的目标步长（spec §6.2：≤2px）。
constexpr float kSmoothStepPx = 2.0f;

// 2D 向量小工具（Vec2 在 types.h，这里只补本模块要的两个操作）
inline float vlen(float x, float y) { return std::sqrt(x * x + y * y); }

inline std::uint64_t fnv_mix(std::uint64_t h, float v) {
  // 把 float 按位模式融进哈希 —— 不能用 std::hash<float>（实现无关但取值不保证跨平台稳定），
  // 也不能按数值转换（NaN/±0 会撞）。按位是最稳的。
  std::uint32_t bits = 0;
  std::memcpy(&bits, &v, sizeof(bits));
  h ^= bits;
  h *= 0x100000001b3ULL;
  return h;
}

class Painter final : public IPainter {
 public:
  void set_desc(const StrokeDesc& d) override { desc_ = d; }
  const StrokeDesc& desc() const override { return desc_; }

  void feed(const StrokeEvent& ev) override {
    switch (ev.kind) {
      case StrokeEventKind::Begin:
        begin_stroke(ev);
        break;
      case StrokeEventKind::Move:
        move_stroke(ev);
        break;
      case StrokeEventKind::End:
        end_stroke(ev);
        break;
      case StrokeEventKind::Cancel:
        cancel_stroke(ev);
        break;
    }
  }

  void feed_batch(const StrokeEvent* events, std::size_t count) override {
    if (events == nullptr) return;
    for (std::size_t i = 0; i < count; ++i) feed(events[i]);
  }

  const StrokeGeometry& geometry() const override {
    if (!dirty_) return cached_;
    rebuild_cache();
    return cached_;
  }

  u32 completed_stroke_count() const override { return completed_strokes_; }

  void cancel_current() override {
    if (!active_) return;
    // 整笔丢弃：把 active_stroke_ 起点之后追加的顶点/索引全部砍掉。
    // 这正是 Cancel 与 End 的差别 —— End 保留已画的部分，Cancel 一点都不留。
    const std::size_t vcount = active_vertex_start_ < all_vertices_.size() ? active_vertex_start_
                                                                         : all_vertices_.size();
    const std::size_t icount = active_index_start_ < all_indices_.size() ? active_index_start_
                                                                        : all_indices_.size();
    all_vertices_.resize(vcount);
    all_indices_.resize(icount);
    active_ = false;
    ++stats_.strokes_cancelled;
    dirty_ = true;
  }

  void clear() override {
    all_vertices_.clear();
    all_indices_.clear();
    active_ = false;
    completed_strokes_ = 0;
    stats_ = PainterStats{};
    dirty_ = true;
  }

  std::vector<std::vector<Vec2>> extract_outline() const override {
    std::vector<std::vector<Vec2>> out;
    // 按 stroke 分段还原中心线：几何里每个点都有它的 "point index"，
    // 这里用最简单可靠的来源 —— 每个中心线点贡献 2 个顶点（左右），
    // 段与段之间共享端点会导致顶点数不是简单的 2N，
    // 所以直接返回缓存的每条中心线点列（构建时就存了）。
    for (const auto& s : stroke_polylines_) {
      std::vector<Vec2> line;
      line.reserve(s.size());
      for (const auto& p : s) line.push_back(p);
      if (!line.empty()) out.push_back(std::move(line));
    }
    return out;
  }

  const PainterStats& stats() const override { return stats_; }

  std::uint64_t geometry_hash() const override {
    // 直接对顶点/索引做 FNV-1a。同样的输入 → 同样的顶点序列 → 同样的 hash。
    std::uint64_t h = 0xcbf29ce484222325ULL;
    for (const StrokeVertex& v : all_vertices_) {
      h = fnv_mix(h, v.position[0]);
      h = fnv_mix(h, v.position[1]);
      h ^= (static_cast<std::uint64_t>(v.color[0]) << 24) |
           (static_cast<std::uint64_t>(v.color[1]) << 16) |
           (static_cast<std::uint64_t>(v.color[2]) << 8) |
           static_cast<std::uint64_t>(v.color[3]);
      h *= 0x100000001b3ULL;
    }
    for (std::uint32_t idx : all_indices_) {
      h ^= idx;
      h *= 0x100000001b3ULL;
    }
    return h;
  }

  void reserve(u32 strokes, u32 points_per_stroke) override {
    // 每条线段的顶点数按 4 算（首段 4、后续每段 2 个新顶点，
    // 但为了简单统一按 4 预留 —— 多留点不影响正确性）。
    const std::size_t est = static_cast<std::size_t>(strokes) * points_per_stroke;
    all_vertices_.reserve(est * 4);
    all_indices_.reserve(est * 6);
    stroke_polylines_.reserve(strokes);
  }

 private:
  // ------------------------------------------------------------ 笔迹生命周期

  void begin_stroke(const StrokeEvent& ev) {
    // 上一笔没收到 End 就来了新的 Begin —— 视为隐式结束，
    // 不能直接覆盖（那样会丢几何且 stats 不准）。
    if (active_) {
      active_ = false;
      ++stats_.strokes_finished;
    }

    active_ = true;
    active_pointer_ = ev.pointer_id;
    active_stroke_id_ = ++next_stroke_id_;
    active_vertex_start_ = all_vertices_.size();
    active_index_start_ = all_indices_.size();
    active_points_.clear();
    active_points_.push_back(Vec2{ev.x, ev.y});
    stroke_polylines_.emplace_back();
    stroke_polylines_.back().push_back(Vec2{ev.x, ev.y});

    ++stats_.strokes_started;
    ++stats_.points_accepted;
    dirty_ = true;

    // 只有一个点时画不出条带（没有方向），先记下，等第一个 Move 再生成。
    // round_cap 的圆头也在这里之后补 —— 见 end_stroke。
  }

  void move_stroke(const StrokeEvent& ev) {
    if (!active_ || ev.pointer_id != active_pointer_) return;
    if (static_cast<std::int32_t>(active_points_.size()) >= desc_.max_points_per_stroke) {
      ++stats_.points_dropped;
      return;
    }

    const Vec2 prev = active_points_.back();
    const float dx = ev.x - prev.x;
    const float dy = ev.y - prev.y;
    if (vlen(dx, dy) < kMinSegmentPx) {
      // 距离太近，丢弃（去抖）。这不算"丢了用户数据"，
      // 所以单独不增加 points_dropped —— 那是给真正被 max 限制掉的点用的。
      return;
    }

    active_points_.push_back(Vec2{ev.x, ev.y});
    stroke_polylines_.back().push_back(Vec2{ev.x, ev.y});
    ++stats_.points_accepted;

    // ---- 增量：只生成这一段，不碰之前的几何 ----
    append_last_segment();
    dirty_ = true;
  }

  void end_stroke(const StrokeEvent& ev) {
    if (!active_ || ev.pointer_id != active_pointer_) return;
    // End 事件本身可能带一个最终位置，也要收进来（否则末段少一截）
    const Vec2 prev = active_points_.back();
    if (vlen(ev.x - prev.x, ev.y - prev.y) >= kMinSegmentPx &&
        static_cast<std::int32_t>(active_points_.size()) < desc_.max_points_per_stroke) {
      active_points_.push_back(Vec2{ev.x, ev.y});
      stroke_polylines_.back().push_back(Vec2{ev.x, ev.y});
      ++stats_.points_accepted;
      append_last_segment();
    }

    if (desc_.round_cap) append_caps();

    active_ = false;
    ++stats_.strokes_finished;
    ++completed_strokes_;
    dirty_ = true;
  }

  void cancel_stroke(const StrokeEvent&) {
    cancel_current();
  }

  // ------------------------------------------------------------ 几何生成

  // 生成 [n-2, n-1] 这一段（n = 点数）。n < 2 时什么都不做。
  //
  // 增量性：这个函数只 push_back 新顶点和索引，绝不修改已有顶点。
  // 已有段的法线在采样下一个点时可能"应该"变化（miter join 需要），
  // 但这里刻意用**每段独立法线**（不平滑），代价是拐角处有微小缝隙 ——
  // 对画笔来说这比"回退修改已有顶点"（破坏增量性、引发 GPU 缓冲重传）划算得多。
  void append_last_segment() {
    const std::size_t n = active_points_.size();
    if (n < 2) return;

    // 用**最近三个点**估计切线，让拐角看起来更顺（中心差分）。
    // 只有两个点时退化为段方向。
    const Vec2 p0 = (n >= 3) ? active_points_[n - 3] : active_points_[0];
    const Vec2 p1 = active_points_[n - 2];
    const Vec2 p2 = active_points_[n - 1];

    const float tx = p2.x - p0.x;
    const float ty = p2.y - p0.y;
    const float len = vlen(tx, ty);
    if (len < 1e-6f) return;

    // 切线单位向量
    const float ux = tx / len;
    const float uy = ty / len;
    // 法线 = 切线逆时针转 90°
    const float nx = -uy;
    const float ny = ux;
    const float hw = desc_.width_px * 0.5f;

    // 两端各两个顶点（左/右）
    const std::uint32_t base = static_cast<std::uint32_t>(all_vertices_.size());

    StrokeVertex vl0, vr0, vl1, vr1;
    set_color(vl0);
    set_color(vr0);
    set_color(vl1);
    set_color(vr1);

    vl0.position[0] = p1.x + nx * hw;
    vl0.position[1] = p1.y + ny * hw;
    vr0.position[0] = p1.x - nx * hw;
    vr0.position[1] = p1.y - ny * hw;
    vl1.position[0] = p2.x + nx * hw;
    vl1.position[1] = p2.y + ny * hw;
    vr1.position[0] = p2.x - nx * hw;
    vr1.position[1] = p2.y - ny * hw;

    all_vertices_.push_back(vl0);
    all_vertices_.push_back(vr0);
    all_vertices_.push_back(vl1);
    all_vertices_.push_back(vr1);

    // 两个三角形：屏幕坐标 y 向下，所以下面的绕序在 Vulkan 默认
    // front-face = counter-clockwise 下是正面。
    // 但笔迹管线用 TriangleList 且关闭背面剔除（笔迹是双面可见的），
    // 所以这里绕序不影响可见性，只影响将来若开启剔除时的正确性。
    all_indices_.push_back(base + 0);  // vl0
    all_indices_.push_back(base + 1);  // vr0
    all_indices_.push_back(base + 2);  // vl1

    all_indices_.push_back(base + 2);  // vl1
    all_indices_.push_back(base + 1);  // vr0
    all_indices_.push_back(base + 3);  // vr1

    ++stats_.segments_appended;
    stats_.geometry_vertices_added += 4;
  }

  // 圆头：在首尾各画一个三角形扇，半径 = width/2。
  //
  // 为什么用 8 边而不是 16 边：
  //   6px 宽的笔迹上，8 边形的弦高误差约 0.11px，肉眼不可见；
  //   顶点数减半。cap 只在一笔的首尾出现，对总量影响小，
  //   但对"每帧大量短笔迹"的场景能省下可观带宽。
  void append_caps() {
    const float hw = desc_.width_px * 0.5f;
    if (hw <= 0.0f || active_points_.empty()) return;

    constexpr int kSegments = 8;
    constexpr float kTwoPi = 2.0f * 3.14159265358979323846f;

    for (int end_idx = 0; end_idx < 2; ++end_idx) {
      const Vec2 c = (end_idx == 0) ? active_points_.front() : active_points_.back();
      // 只有 1 个点时不画 cap（还没有方向，画出来的圆是模糊的默认朝向，
      // 而且下一段会立刻盖住它）。两端的 cap 只在有至少 2 个点时才有意义。
      if (active_points_.size() < 2) break;

      const std::uint32_t center = static_cast<std::uint32_t>(all_vertices_.size());
      StrokeVertex cv;
      set_color(cv);
      cv.position[0] = c.x;
      cv.position[1] = c.y;
      all_vertices_.push_back(cv);

      for (int i = 0; i < kSegments; ++i) {
        const float a = kTwoPi * static_cast<float>(i) / static_cast<float>(kSegments);
        StrokeVertex v;
        set_color(v);
        v.position[0] = c.x + std::cos(a) * hw;
        v.position[1] = c.y + std::sin(a) * hw;
        all_vertices_.push_back(v);
      }

      for (int i = 0; i < kSegments; ++i) {
        const std::uint32_t a = center + 1 + static_cast<std::uint32_t>(i);
        const std::uint32_t b = center + 1 + static_cast<std::uint32_t>((i + 1) % kSegments);
        all_indices_.push_back(center);
        all_indices_.push_back(a);
        all_indices_.push_back(b);
      }
      stats_.geometry_vertices_added += static_cast<u32>(kSegments + 1);
    }
  }

  void set_color(StrokeVertex& v) {
    // Color4 是 [0,1] 浮点，转成 8 位归一化
    auto to_u8 = [](float f) {
      if (f <= 0.0f) return std::uint8_t{0};
      if (f >= 1.0f) return std::uint8_t{255};
      return static_cast<std::uint8_t>(f * 255.0f + 0.5f);
    };
    v.color[0] = to_u8(desc_.color.r);
    v.color[1] = to_u8(desc_.color.g);
    v.color[2] = to_u8(desc_.color.b);
    v.color[3] = to_u8(desc_.color.a);
  }

  void rebuild_cache() const {
    cached_.vertices = all_vertices_;
    cached_.indices = all_indices_;
    cached_.closed = !active_;
    dirty_ = false;
  }

  StrokeDesc desc_{};
  PainterStats stats_{};

  // 全部笔迹的累积几何（所有笔共享一个缓冲区，方便"一次 draw 画完所有笔迹"）
  std::vector<StrokeVertex> all_vertices_;
  std::vector<std::uint32_t> all_indices_;
  // 每条笔的中心线点列（供 extract_outline）
  std::vector<std::vector<Vec2>> stroke_polylines_;

  // 当前笔的状态
  bool active_ = false;
  std::uint32_t active_pointer_ = 0;
  u32 active_stroke_id_ = 0;
  u32 next_stroke_id_ = 0;
  std::vector<Vec2> active_points_;
  std::size_t active_vertex_start_ = 0;
  std::size_t active_index_start_ = 0;

  u32 completed_strokes_ = 0;
  mutable bool dirty_ = true;
  mutable StrokeGeometry cached_{};
};

}  // namespace

std::unique_ptr<IPainter> create_painter() { return std::make_unique<Painter>(); }

}  // namespace magicblocks::primitives
