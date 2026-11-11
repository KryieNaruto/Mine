// MagicBlocks SDK —— L3 primitives 基础值类型
//
// 设计依据:docs/superpowers/specs/2026-11-04-magicblocks-l3-primitives-design.md
//
// 约束(spec §2.3 / §3 / D-L3-1):
//   - **公共头零第三方依赖**:不用 glm::mat4 等类型出现在签名里(那是 L3 内部实现细节),
//     公共面用 POD(可直接进命令流 payload、可直接 memcpy、跨编译器布局稳定)。
//   - **零 Vk* 类型(G3)**:本头只依赖 L0/L1 的公共值类型。
//   - 浮点一律 float(移动端),不用 double:双精度既慢又与 GPU 精度不一致。
//   - 命名空间 `magicblocks::primitives`。
//
// 坐标约定(spec §5.2 / D-L3-2,全 L3 统一,不要再各写一套):
//   - **屏幕/像素坐标**:原点左上,+x 向右,+y 向下(与 device::InputEvent::position 一致)。
//   - **世界坐标**:右手系,+y 向上(与常见建模工具一致)。
//   - **NDC**:Vulkan 惯例,z ∈ [0,1](不是 GL 的 [-1,1]),y 向上。
//   - 视图矩阵 look-at,near/far 正向。
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

#include "mb_internal/magicblocks/device/result.h"
#include "mb_internal/magicblocks/device/types.h"

namespace magicblocks::primitives {

using device::Extent2D;
using device::Status;
using device::Result;
using device::u32;
using device::u64;
using device::i32;

// ---------------------------------------------------------------- 向量

struct Vec2 {
  float x = 0.0f;
  float y = 0.0f;

  Vec2() = default;
  Vec2(float x_, float y_) : x(x_), y(y_) {}

  Vec2 operator+(const Vec2& o) const { return {x + o.x, y + o.y}; }
  Vec2 operator-(const Vec2& o) const { return {x - o.x, y - o.y}; }
  Vec2 operator*(float s) const { return {x * s, y * s}; }
  Vec2 operator/(float s) const { return {x / s, y / s}; }
  Vec2& operator+=(const Vec2& o) { x += o.x; y += o.y; return *this; }
  bool operator==(const Vec2& o) const { return x == o.x && y == o.y; }
  bool operator!=(const Vec2& o) const { return !(*this == o); }
};

struct Vec3 {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;

  Vec3() = default;
  Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

  Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
  Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
  Vec3 operator-() const { return {-x, -y, -z}; }
  Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
  Vec3 operator/(float s) const { return {x / s, y / s, z / s}; }
  Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
  Vec3& operator-=(const Vec3& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
  Vec3& operator*=(float s) { x *= s; y *= s; z *= s; return *this; }
  bool operator==(const Vec3& o) const { return x == o.x && y == o.y && z == o.z; }
  bool operator!=(const Vec3& o) const { return !(*this == o); }

  float operator[](int i) const { return i == 0 ? x : (i == 1 ? y : z); }
  float& operator[](int i) { return i == 0 ? x : (i == 1 ? y : z); }
};

struct Vec4 {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float w = 0.0f;

  Vec4() = default;
  Vec4(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}

  Vec4 operator+(const Vec4& o) const { return {x + o.x, y + o.y, z + o.z, w + o.w}; }
  Vec4 operator-(const Vec4& o) const { return {x - o.x, y - o.y, z - o.z, w - o.w}; }
  Vec4 operator*(float s) const { return {x * s, y * s, z * s, w * s}; }
  bool operator==(const Vec4& o) const { return x == o.x && y == o.y && z == o.z && w == o.w; }
  bool operator!=(const Vec4& o) const { return !(*this == o); }
};

inline float dot(const Vec2& a, const Vec2& b) { return a.x * b.x + a.y * b.y; }
inline float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(const Vec3& a, const Vec3& b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float length(const Vec3& v) { return std::sqrt(dot(v, v)); }
inline float length(const Vec2& v) { return std::sqrt(dot(v, v)); }
inline Vec3 normalize(const Vec3& v) {
  const float len = length(v);
  return len > 1e-8f ? v / len : Vec3{0.0f, 0.0f, 0.0f};
}
inline Vec2 normalize(const Vec2& v) {
  const float len = length(v);
  return len > 1e-8f ? v / len : Vec2{0.0f, 0.0f};
}
inline Vec3 lerp(const Vec3& a, const Vec3& b, float t) { return a + (b - a) * t; }

// ---------------------------------------------------------------- 矩阵(列主,与 glm/Vulkan 一致)
//
// m[col * 4 + row]。为什么列主:Vulkan/GLSL 与 glm 默认都是列主,
// 命令流里按 16 个 float 顺序存,GPU 端 mat4 直接 memcpy 就能用,不需要转置。
struct Mat4 {
  float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

  static Mat4 identity() { return Mat4{}; }

  float& at(int col, int row) { return m[col * 4 + row]; }
  float at(int col, int row) const { return m[col * 4 + row]; }

  Vec4 column(int c) const { return {m[c * 4 + 0], m[c * 4 + 1], m[c * 4 + 2], m[c * 4 + 3]}; }

  Mat4 operator*(const Mat4& o) const {
    Mat4 r;
    for (int c = 0; c < 4; ++c) {
      for (int row = 0; row < 4; ++row) {
        float s = 0.0f;
        for (int k = 0; k < 4; ++k) s += at(k, row) * o.at(c, k);
        r.at(c, row) = s;
      }
    }
    return r;
  }

  Vec4 operator*(const Vec4& v) const {
    return {at(0, 0) * v.x + at(1, 0) * v.y + at(2, 0) * v.z + at(3, 0) * v.w,
            at(0, 1) * v.x + at(1, 1) * v.y + at(2, 1) * v.z + at(3, 1) * v.w,
            at(0, 2) * v.x + at(1, 2) * v.y + at(2, 2) * v.z + at(3, 2) * v.w,
            at(0, 3) * v.x + at(1, 3) * v.y + at(2, 3) * v.z + at(3, 3) * v.w};
  }

  Vec3 transform_point(const Vec3& p) const {
    const Vec4 r = (*this) * Vec4{p.x, p.y, p.z, 1.0f};
    if (r.w != 0.0f && r.w != 1.0f) return {r.x / r.w, r.y / r.w, r.z / r.w};
    return {r.x, r.y, r.z};
  }
  Vec3 transform_direction(const Vec3& d) const {
    const Vec4 r = (*this) * Vec4{d.x, d.y, d.z, 0.0f};
    return {r.x, r.y, r.z};
  }

  // 一般 4x4 求逆(余子式展开)。失败(奇异)→ 返回单位阵并置 *ok=false。
  Mat4 inverse(bool* ok = nullptr) const;
};

// 常用构造(实现见 types.cpp;矩阵数学只有一份,别在别处再写)。
Mat4 make_translation(const Vec3& t);
Mat4 make_scale(const Vec3& s);
Mat4 make_rotation_quat(float x, float y, float z, float w);
// TRS:先缩放、再旋转、再平移(与 scene::Transform 的语义一致)。
Mat4 make_trs(const float t[3], const float q[4], const float s[3]);
Mat4 make_look_at(const Vec3& eye, const Vec3& target, const Vec3& up);
// Vulkan 惯例:z ∈ [0,1],y 向上。fov_y_radians 为垂直视角。
Mat4 make_perspective(float fov_y_radians, float aspect, float near_z, float far_z,
                      bool reverse_z = false);
Mat4 make_orthographic(float left, float right, float bottom, float top, float near_z,
                       float far_z, bool reverse_z = false);

// ---------------------------------------------------------------- 颜色 / 包围盒 / 射线 / 平面

struct Color4 {
  float r = 1.0f;
  float g = 1.0f;
  float b = 1.0f;
  float a = 1.0f;

  Color4() = default;
  Color4(float r_, float g_, float b_, float a_ = 1.0f) : r(r_), g(g_), b(b_), a(a_) {}
  Vec4 to_vec4() const { return {r, g, b, a}; }
  bool operator==(const Color4& o) const { return r == o.r && g == o.g && b == o.b && a == o.a; }
  bool operator!=(const Color4& o) const { return !(*this == o); }
};

// 轴对齐包围盒。**empty 是显式字段**(与 scene::Aabb 同口径):min/max 全 0 时
// 无法区分"空盒"与"原点处的退化盒",序列化必须显式带上。
struct Box {
  Vec3 min;
  Vec3 max;
  bool empty = true;

  static Box of(const Vec3& lo, const Vec3& hi) { return {lo, hi, false}; }
  Vec3 center() const { return (min + max) * 0.5f; }
  Vec3 extent() const { return max - min; }
  float radius() const { return length(max - min) * 0.5f; }
  bool contains(const Vec3& p) const {
    return p.x >= min.x && p.y >= min.y && p.z >= min.z && p.x <= max.x && p.y <= max.y &&
           p.z <= max.z;
  }
  // 并进一个点(空盒时直接初始化)。
  void expand(const Vec3& p);
  void expand(const Box& other);
  bool operator==(const Box& o) const;
};

struct Ray {
  Vec3 origin;
  Vec3 direction;   // 不要求归一化;求交时按参数 t 使用
  bool operator==(const Ray& o) const { return origin == o.origin && direction == o.direction; }
};

// 平面:n·p + d = 0(n 归一化时 d = -n·p0)。
struct Plane {
  Vec3 normal{0.0f, 1.0f, 0.0f};
  float d = 0.0f;
};

struct RayHit {
  bool hit = false;
  float t = 0.0f;     // 沿 ray.direction 的参数
  Vec3 point;
  // 首次命中的面序号(网格拾取用;非网格类型为 0)
  u32 primitive_index = 0;
};

// ---------------------------------------------------------------- 求交(spec §5 IPicker / M3)
//
// 全部是纯函数,**只在这里实现一份**。t_min 默认 0(允许起点即命中);
// 命中须满足 t >= t_min;out 只在 hit 时被写。

bool intersect_ray_box(const Ray& ray, const Box& box, float* t_out, float t_min = 0.0f);
// Möller–Trumbore。两个朝向都算命中(不做背面剔除 —— 拾取要"点到就算")。
bool intersect_ray_triangle(const Ray& ray, const Vec3& a, const Vec3& b, const Vec3& c,
                            float* t_out, float t_min = 0.0f);
bool intersect_ray_plane(const Ray& ray, const Plane& plane, float* t_out,
                         float t_min = 0.0f);
// 线与球(用于 gizmo 屏幕空间拾取的距离判定)。
bool intersect_ray_sphere(const Ray& ray, const Vec3& center, float radius, float* t_out,
                          float t_min = 0.0f);
// 点到线段的最近距离(gizmo 轴线段命中阈値用)。
float distance_point_segment(const Vec3& p, const Vec3& a, const Vec3& b);
// 射线与线段(3D)的最近距离,以及对应的射线参数 t(屏幕空间 gizmo 拾取的核心)。
// 返回最近距离;若射线参数 < 0 或两线近似平行,t_out 填 -1。
float distance_ray_segment(const Ray& ray, const Vec3& a, const Vec3& b, float* t_out);

// ---------------------------------------------------------------- 句柄

// 网格句柄(MeshTable 槽位)。0 = 无效。
using MeshId = u32;
inline constexpr MeshId kInvalidMeshId = 0;

// 材质槽位(MaterialTable 槽位)。0 是保留的默认材质。
using MaterialSlot = u32;
inline constexpr MaterialSlot kDefaultMaterialSlot = 0;

}  // namespace magicblocks::primitives
