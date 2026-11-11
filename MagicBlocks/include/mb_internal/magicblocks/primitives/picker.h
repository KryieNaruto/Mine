#pragma once
// MagicBlocks L3 —— IPicker（spec §13.14 / M3 第 14 项）
//
// 射线拾取的**纯 CPU** 实现。为什么放 L3 而不是让玩法自己写：
//   1. 射线与各种图元的求交（AABB / 三角形 / 平面 / 球）是纯数学，
//      但每个玩法都要用，写五遍就有五种"边界算不算命中"的约定；
//   2. **GPU 拾取（读像素）需要 readback —— 那会 stall 整帧**（C-L3-c 明令
//      禁止在渲染热路径读回）。所以默认路径必须是 CPU 射线；
//   3. GPU 拾取仅作为**可选**的离线/低频路径提供，显式标注它的代价。
//
// 坐标系：`screen_to_ray` 来自 ICamera，像素原点左上（与 Picker 一致）。

#include <cstdint>
#include <memory>
#include <vector>

#include "mb_internal/magicblocks/primitives/alloc.h"
#include "mb_internal/magicblocks/primitives/camera.h"
#include "mb_internal/magicblocks/primitives/types.h"

namespace magicblocks::primitives {

// ---------------------------------------------------------------- 结果

struct PickHit {
  // 命中的实体。0 表示"命中了纯几何"（没有归属实体）。
  std::uint32_t entity = 0;
  // 从射线起点算起的距离（世界单位）。沿 `Ray::direction` 的 t。
  //
  // ⚠️ 默认值必须是 **-1**，不能是 0。
  // 原先是 0.0f，而 `valid()` 判断 `distance >= 0`，
  // 于是"默认构造的 PickHit"天然有效 —— 拾取失败时调用方若用
  // `hit.valid()` 判断，会把一个从未被写过的对象当成命中，
  // 拿到 distance=0、point=(0,0,0) 的假结果。这类 bug 不会崩，
  // 只会在"点空白处选中了原点物体"时表现为灵异。
  float distance = -1.0f;
  // 命中点与世界法线。
  Vec3 point{0.0f, 0.0f, 0.0f};
  Vec3 normal{0.0f, 0.0f, 1.0f};
  // 命中的三角形在网格里的索引（`kNoTriangle` = 非三角网格命中）。
  std::uint32_t triangle = 0;
  bool valid() const { return distance >= 0.0f; }
};
inline constexpr std::uint32_t kNoTriangle = 0xFFFFFFFFu;

struct PickStats {
  std::uint32_t entities_tested = 0;
  std::uint32_t aabb_tests = 0;
  std::uint32_t aabb_hits = 0;
  std::uint32_t triangle_tests = 0;
  std::uint32_t triangle_hits = 0;
  // 被"最近优先"提前剪枝掉的实体数（宽相位 AABB 已排序时 > 0）。
  std::uint32_t pruned = 0;
};

// ---------------------------------------------------------------- 接口

class IPicker {
 public:
  IPicker() = default;
  IPicker(const IPicker&) = delete;
  IPicker& operator=(const IPicker&) = delete;
  virtual ~IPicker() = default;

  // ---- 单次求交（无状态，工具函数）----

  // 射线 vs 轴对齐包围盒（slab 法）。返回 true 并写 `out_t`（进入点距离）。
  // `direction` 必须已归一化。起点在盒内时 `out_t = 0`。
  virtual bool intersect_box(const Ray& ray, const Box& box, float* out_t) const = 0;

  // 射线 vs 三角形（Möller–Trumbore）。双面命中（不做背面剔除）——
  // 拾取要"看得见的都点得到"，而薄壁网格常常是单面建模的。
  virtual bool intersect_triangle(const Ray& ray, const Vec3& a, const Vec3& b, const Vec3& c,
                                  float* out_t, Vec3* out_normal) const = 0;

  // 射线 vs 平面。`plane` 用 Plane 的 (normal, d) 表示。
  virtual bool intersect_plane(const Ray& ray, const Plane& plane, float* out_t) const = 0;

  // 射线 vs 球。
  virtual bool intersect_sphere(const Ray& ray, const Vec3& center, float radius,
                                float* out_t, Vec3* out_normal) const = 0;

  // ---- 屏幕空间（玩法最常用的入口）----

  // 射线 vs **单个** AABB，返回是否命中。屏幕坐标进来，内部走 camera 求射线。
  virtual bool pick_aabb_screen(const ICamera& camera, float px, float py, const Box& box,
                                float* out_t) const = 0;

  // 射线 vs 三角形网格（顶点 + 索引），返回最近命中。
  virtual bool pick_mesh_screen(const ICamera& camera, float px, float py, const Vec3* verts,
                                std::uint32_t vertex_count, const std::uint32_t* indices,
                                std::uint32_t index_count, PickHit* out) const = 0;

  // ---- 场景级 ----

  // 清空已登记的拾取对象。每帧重建（场景变化频繁，增量维护反而更贵）。
  virtual void begin_scene() = 0;

  // 登记一个可拾取对象。`box` 是世界空间 AABB（宽相位）。
  virtual void add_object(std::uint32_t entity, const Box& box) = 0;

  // 用屏幕点拾取场景里最近的 object。返回 false = 什么也没点到。
  //
  // 宽相位用登记的 AABB；窄相位只能由调用方提供（picker 不认识网格格式）。
  // 传 nullptr 的 `narrow` 就退化成"AABB 命中即算命中" —— 足够用于
  // gizmo 的粗拾取与"选中某个积木"这类不要求精度的场景。
  using NarrowPhase = bool (*)(std::uint32_t entity, const Ray& ray, float* out_t);
  virtual bool pick_scene(const ICamera& camera, float px, float py, PickHit* out,
                          NarrowPhase narrow = nullptr) = 0;

  virtual const PickStats& stats() const = 0;

  // 帧内缓冲来源。
  virtual void set_frame_arena(IFrameArena*) = 0;
};

// 相机求射线的适配：picker 只依赖 ICamera 的 `screen_to_ray`，
// 不依赖相机内部（这是 U6 解耦的前提 —— 拾取不关心投影类型）。

std::unique_ptr<IPicker> create_picker();

}  // namespace magicblocks::primitives
