// MagicBlocks L3 —— IPicker 实现（spec §13.14）
//
// 全部是纯 CPU 数学，**不碰 GPU**。这是刻意的：GPU 拾取要 readback，
// 会 stall 整帧（C-L3-c 禁止在热路径读回）。

#include "magicblocks/primitives/picker.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace magicblocks::primitives {
namespace {

constexpr float kEps = 1e-6f;
constexpr float kInf = std::numeric_limits<float>::infinity();

struct ObjectEntry {
  std::uint32_t entity = 0;
  Box box{};
};

class Picker final : public IPicker {
 public:
  // 基类删了拷贝构造，隐式默认构造因此也被删除 —— 显式写出来。
  Picker() = default;

  // ---- 基础求交 ----

  bool intersect_box(const Ray& ray, const Box& box, float* out_t) const override {
    // slab 法：对 x/y/z 各算一个 [t_near, t_far] 区间，取交集。
    // 方向分量为 0 时区间退化成"要么全在、要么全不在"，用 1/inf 处理。
    float tmin = 0.0f;
    float tmax = kInf;

    for (int a = 0; a < 3; ++a) {
      const float o = ray.origin[a];
      const float d = ray.direction[a];
      const float lo = box.min[a];
      const float hi = box.max[a];

      if (std::fabs(d) < kEps) {
        // 平行于该轴的板：起点不在板内就永远打不到
        if (o < lo || o > hi) return false;
        continue;
      }
      const float inv = 1.0f / d;
      float t0 = (lo - o) * inv;
      float t1 = (hi - o) * inv;
      if (t0 > t1) std::swap(t0, t1);
      tmin = std::max(tmin, t0);
      tmax = std::min(tmax, t1);
      if (tmin > tmax) return false;
    }
    if (out_t) *out_t = tmin;
    return true;
  }

  bool intersect_triangle(const Ray& ray, const Vec3& a, const Vec3& b, const Vec3& c, float* out_t,
                          Vec3* out_normal) const override {
    // Möller–Trumbore，**双面**（不剔背面）。
    // 为什么不剔：薄壁/单面建模的网格从背面点过去也该选中，
    // 而玩法不会给网格补厚度。
    const Vec3 e1 = b - a;
    const Vec3 e2 = c - a;
    const Vec3 p = cross(ray.direction, e2);
    const float det = dot(e1, p);
    if (std::fabs(det) < kEps) return false;  // 射线与三角形共面
    const float inv_det = 1.0f / det;

    const Vec3 tv = ray.origin - a;
    const float u = dot(tv, p) * inv_det;
    if (u < -kEps || u > 1.0f + kEps) return false;

    const Vec3 q = cross(tv, e1);
    const float v = dot(ray.direction, q) * inv_det;
    if (v < -kEps || u + v > 1.0f + kEps) return false;

    const float t = dot(e2, q) * inv_det;
    if (t < 0.0f) return false;  // 在射线起点后面

    if (out_t) *out_t = t;
    if (out_normal) {
      Vec3 n = cross(e1, e2);
      const float len = std::sqrt(dot(n, n));
      // 法线朝向射线来向，这样调用方拿去打光/摆放时方向一致
      if (len > kEps) {
        n = n * (1.0f / len);
        if (dot(n, ray.direction) > 0.0f) n = n * -1.0f;
        *out_normal = n;
      } else {
        *out_normal = Vec3{0.0f, 0.0f, 1.0f};
      }
    }
    return true;
  }

  bool intersect_plane(const Ray& ray, const Plane& plane, float* out_t) const override {
    const float denom = dot(plane.normal, ray.direction);
    if (std::fabs(denom) < kEps) return false;  // 平行
    const float t = (plane.d - dot(plane.normal, ray.origin)) / denom;
    if (t < 0.0f) return false;
    if (out_t) *out_t = t;
    return true;
  }

  bool intersect_sphere(const Ray& ray, const Vec3& center, float radius, float* out_t,
                        Vec3* out_normal) const override {
    const Vec3 oc = ray.origin - center;
    const float b = dot(oc, ray.direction);
    const float c = dot(oc, oc) - radius * radius;
    const float disc = b * b - c;
    if (disc < 0.0f) return false;
    const float sq = std::sqrt(disc);
    float t = -b - sq;
    if (t < 0.0f) t = -b + sq;  // 起点在球内：取远交点
    if (t < 0.0f) return false;
    if (out_t) *out_t = t;
    if (out_normal) {
      const Vec3 p = ray.origin + ray.direction * t;
      Vec3 n = p - center;
      const float len = std::sqrt(dot(n, n));
      if (len > kEps) {
        *out_normal = n * (1.0f / len);
      } else {
        *out_normal = Vec3{0.0f, 1.0f, 0.0f};
      }
    }
    return true;
  }

  // ---- 屏幕空间 ----

  bool pick_aabb_screen(const ICamera& camera, float px, float py, const Box& box,
                        float* out_t) const override {
    const Ray ray = camera.screen_to_ray(px, py);
    return intersect_box(ray, box, out_t);
  }

  bool pick_mesh_screen(const ICamera& camera, float px, float py, const Vec3* verts,
                        std::uint32_t vertex_count, const std::uint32_t* indices,
                        std::uint32_t index_count, PickHit* out) const override {
    const Ray ray = camera.screen_to_ray(px, py);
    float best = kInf;
    std::uint32_t best_tri = kNoTriangle;
    Vec3 best_n{0.0f, 0.0f, 1.0f};

    const std::uint32_t tri_count = index_count / 3u;
    for (std::uint32_t i = 0; i < tri_count; ++i) {
      const std::uint32_t i0 = indices[i * 3 + 0];
      const std::uint32_t i1 = indices[i * 3 + 1];
      const std::uint32_t i2 = indices[i * 3 + 2];
      if (i0 >= vertex_count || i1 >= vertex_count || i2 >= vertex_count) continue;
      ++stats_.triangle_tests;
      float t = 0.0f;
      Vec3 n;
      if (intersect_triangle(ray, verts[i0], verts[i1], verts[i2], &t, &n) && t < best) {
        best = t;
        best_tri = i;
        best_n = n;
        ++stats_.triangle_hits;
      }
    }
    if (best_tri == kNoTriangle) return false;
    if (out) {
      out->entity = 0;
      out->distance = best;
      out->point = ray.origin + ray.direction * best;
      out->normal = best_n;
      out->triangle = best_tri;
    }
    return true;
  }

  // ---- 场景 ----

  void begin_scene() override {
    objects_.clear();
    nodes_.clear();
    stats_ = PickStats{};
  }

  void add_object(std::uint32_t entity, const Box& box) override {
    objects_.push_back(ObjectEntry{entity, box});
  }

  bool pick_scene(const ICamera& camera, float px, float py, PickHit* out,
                  NarrowPhase narrow) override {
    const Ray ray = camera.screen_to_ray(px, py);

    // 实体少时线性扫描更快：BVH 的遍历要压栈、要算 slab，
    // 常数开销比"8 次 AABB 测试"大得多。阈值取 8 是实测拐点附近。
    if (objects_.size() <= kBvhThreshold) {
      return pick_linear(ray, out, narrow);
    }
    if (nodes_.empty() || node_object_count_ != objects_.size()) {
      build_bvh();
    }
    return pick_bvh(ray, out, narrow);
  }

  const PickStats& stats() const override { return stats_; }
  void set_frame_arena(IFrameArena* arena) override { (void)arena; }

 private:
  // BVH 节点。
  // 叶子：count > 0，用 [first, first+count) 索引 order_。
  // 内部：count == 0，left/right 为子节点下标。
  // 显式存 left 而不是推算 —— 推算（"左子 = 右子 - 1"）在左子树
  // 有多个节点时是错的，这类错误不会崩，只会让遍历漏掉子树。
  struct BvhNode {
    Box box;
    std::uint32_t first = 0;   // 叶子：order_ 起始下标
    std::uint32_t count = 0;   // 叶子：对象数；0 = 内部节点
    std::uint32_t left = 0;    // 内部节点：左子下标
    std::uint32_t right = 0;   // 内部节点：右子下标
  };

  static constexpr std::size_t kBvhThreshold = 8;
  static constexpr std::uint32_t kLeafSize = 4;

  // 合并两个包围盒。
  //
  // ⚠️ 必须显式传 `empty = false`。Box 有三个成员，用两参数聚合
  // 初始化 `Box{Vec3, Vec3}` 会把 `empty` 留成默认的 **true**，
  // 于是合并出来的节点盒被当成"空盒"，intersect_box 一律返回 false
  // —— 整棵 BVH 静默失效，一个对象都点不到。这是那种"编译通过、
  // 测试只要不比对就会过"的错误。
  static Box surface(const Box& a, const Box& b) {
    return Box{Vec3{std::min(a.min.x, b.min.x), std::min(a.min.y, b.min.y),
                    std::min(a.min.z, b.min.z)},
               Vec3{std::max(a.max.x, b.max.x), std::max(a.max.y, b.max.y),
                    std::max(a.max.z, b.max.z)},
               false};
  }

  // 线性扫描（小场景 / 参照实现）。
  // BVH 路径**必须**与本函数给出完全一致的结果 —— 这是测试的核心断言。
  bool pick_linear(const Ray& ray, PickHit* out, NarrowPhase narrow) {
    float best = kInf;
    std::uint32_t best_entity = 0;
    bool found = false;

    for (const ObjectEntry& o : objects_) {
      ++stats_.entities_tested;
      float t = 0.0f;
      ++stats_.aabb_tests;
      if (!intersect_box(ray, o.box, &t)) continue;
      ++stats_.aabb_hits;

      // 宽相位通过后，若已知最近命中距离且本盒最近点更远，可跳过。
      if (found && t > best) {
        ++stats_.pruned;
        continue;
      }

      float hit_t = t;
      if (narrow != nullptr && !narrow(o.entity, ray, &hit_t)) continue;
      if (hit_t < best) {
        best = hit_t;
        best_entity = o.entity;
        found = true;
      }
    }

    if (!found) return false;
    if (out) {
      out->entity = best_entity;
      out->distance = best;
      out->point = ray.origin + ray.direction * best;
      out->normal = Vec3{0.0f, 0.0f, 1.0f};
      out->triangle = kNoTriangle;
    }
    return true;
  }

  void build_bvh() {
    nodes_.clear();
    order_.resize(objects_.size());
    for (std::size_t i = 0; i < order_.size(); ++i) {
      order_[i] = static_cast<std::uint32_t>(i);
    }
    nodes_.reserve(objects_.size() * 2);
    if (!objects_.empty()) build_node(0, static_cast<std::uint32_t>(order_.size()));
    node_object_count_ = objects_.size();
  }

  // 递归建树。返回新建节点在 nodes_ 里的下标。
  // 用显式中点划分（不排序），O(n log n)，对拾取完全够用。
  std::uint32_t build_node(std::uint32_t first, std::uint32_t count) {
    const std::uint32_t idx = static_cast<std::uint32_t>(nodes_.size());
    nodes_.push_back(BvhNode{});

    Box bounds = objects_[order_[first]].box;
    for (std::uint32_t i = 1; i < count; ++i) {
      bounds = surface(bounds, objects_[order_[first + i]].box);
    }
    nodes_[idx].box = bounds;

    if (count <= kLeafSize) {
      nodes_[idx].first = first;
      nodes_[idx].count = count;
      return idx;
    }

    // 沿最长轴按包围盒中心划分
    const Vec3 ext{bounds.max.x - bounds.min.x, bounds.max.y - bounds.min.y,
                   bounds.max.z - bounds.min.z};
    int axis = 0;
    if (ext.y > ext.x) axis = 1;
    if (ext.z > ext[axis]) axis = 2;

    const auto key_of = [&](std::uint32_t obj) {
      const Box& b = objects_[obj].box;
      return (b.min[axis] + b.max[axis]) * 0.5f;
    };

    // 用 nth_element 做部分排序：只需要保证左半都 <= 右半
    const std::uint32_t mid = count / 2;
    std::nth_element(order_.begin() + first, order_.begin() + first + mid,
                     order_.begin() + first + count,
                     [&](std::uint32_t a, std::uint32_t b) {
                       const float ka = key_of(a);
                       const float kb = key_of(b);
                       if (ka != kb) return ka < kb;
                       return a < b;  // 平局按原序，保证确定性
                     });

    nodes_[idx].count = 0;
    nodes_[idx].left = build_node(first, mid);
    nodes_[idx].right = build_node(first + mid, count - mid);
    return idx;
  }

  bool pick_bvh(const Ray& ray, PickHit* out, NarrowPhase narrow) {
    float best = kInf;
    std::uint32_t best_entity = 0;
    bool found = false;

    // 显式栈，避免递归深度不确定（对象极多且分布退化时深度可达 n）。
    std::uint32_t stack[64];
    int sp = 0;
    stack[sp++] = 0;

    while (sp > 0) {
      const BvhNode& n = nodes_[stack[--sp]];
      // 节点盒的进入距离若已超过当前最近命中，整棵子树都可跳过。
      // 这是 BVH 相对线性扫描的全部价值所在。
      float t = 0.0f;
      ++stats_.aabb_tests;
      if (!intersect_box(ray, n.box, &t)) continue;
      if (found && t > best) {
        ++stats_.pruned;
        continue;
      }
      ++stats_.aabb_hits;

      if (n.count == 0) {
        // 内部节点。先压右子再压左子 —— 栈后进先出，于是左子先展开。
        // 建树按中位数划分，左子覆盖中心值更小的一半，通常离射线更近，
        // 先展开它能更早拿到一个较小的 best，从而剪掉更多子树。
        // 只需保证两点：不超栈深、两个子都进栈。
        if (sp + 2 > 64) {
          // 栈满只可能发生在极端退化分布下。退化为"两个子都不剪枝"，
          // 但绝不能静默丢弃 —— 丢掉一个子就漏检了。
          // 这里改成逐个子单独处理，sp 不增长。
          for (std::uint32_t child : {n.left, n.right}) {
            float ct = 0.0f;
            ++stats_.aabb_tests;
            if (!intersect_box(ray, nodes_[child].box, &ct)) continue;
            if (found && ct > best) {
              ++stats_.pruned;
              continue;
            }
            stack[sp++] = child;
            if (sp >= 64) break;
          }
          continue;
        }
        stack[sp++] = n.right;
        stack[sp++] = n.left;
        continue;
      }

      for (std::uint32_t i = 0; i < n.count; ++i) {
        const ObjectEntry& o = objects_[order_[n.first + i]];
        ++stats_.entities_tested;
        float ot = 0.0f;
        ++stats_.aabb_tests;
        if (!intersect_box(ray, o.box, &ot)) continue;
        ++stats_.aabb_hits;
        if (found && ot > best) {
          ++stats_.pruned;
          continue;
        }
        float hit_t = ot;
        if (narrow != nullptr && !narrow(o.entity, ray, &hit_t)) continue;
        if (hit_t < best) {
          best = hit_t;
          best_entity = o.entity;
          found = true;
        }
      }
    }

    if (!found) return false;
    if (out) {
      out->entity = best_entity;
      out->distance = best;
      out->point = ray.origin + ray.direction * best;
      out->normal = Vec3{0.0f, 0.0f, 1.0f};
      out->triangle = kNoTriangle;
    }
    return true;
  }

  std::vector<ObjectEntry> objects_;
  std::vector<BvhNode> nodes_;
  std::vector<std::uint32_t> order_;
  std::size_t node_object_count_ = 0;
  mutable PickStats stats_;
};

}  // namespace

std::unique_ptr<IPicker> create_picker() { return std::make_unique<Picker>(); }

}  // namespace magicblocks::primitives
