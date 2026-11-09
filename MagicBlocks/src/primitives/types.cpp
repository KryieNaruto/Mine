// MagicBlocks SDK —— L3 primitives 基础值类型实现
//
// 矩阵数学、包围盒、射线求交的**唯一实现**。任何别处再写一份
// look_at / perspective / ray-AABB 都是 bug 温床(已经是 L0~L2 的教训:
// 双份翻行导致 golden 对不上)。
#include "magicblocks/primitives/types.h"

#include <algorithm>
#include <limits>

namespace magicblocks::primitives {

// ---------------------------------------------------------------- Mat4

Mat4 Mat4::inverse(bool* ok) const {
  // 高斯-约当消元(部分主元)。选它而不是手抄 4x4 余子式展开的原因:
  //   1. 余子式公式是**行主序**写法,而本类存储是**列主序**,
  //      照抄极易在"写回时要不要转置"上出错(本文件第一版就错在这里,
  //      表现为 inverse 结果量级完全离谱,靠 unit test 才抓到);
  //   2. 消元的索引在 [r][c] 上闭环,与存储顺序无关;
  //   3. 数值上比直接展开更稳(部分主元避免小除数放大误差)。
  // 性能:相机每帧只求一次逆,64 次乘加的开销可忽略,正确性优先。
  float a[4][8];
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      a[r][c] = at(c, r);          // at(col,row) → 行主序工作区
      a[r][4 + c] = (c == r) ? 1.0f : 0.0f;
    }
  }

  for (int col = 0; col < 4; ++col) {
    // 部分主元:在这一列里挑绝对值最大的行。
    int pivot = col;
    float best = std::fabs(a[col][col]);
    for (int r = col + 1; r < 4; ++r) {
      const float v = std::fabs(a[r][col]);
      if (v > best) {
        best = v;
        pivot = r;
      }
    }
    if (best < 1e-12f) {   // 奇异
      if (ok != nullptr) *ok = false;
      return Mat4::identity();
    }
    if (pivot != col) {
      for (int c = 0; c < 8; ++c) std::swap(a[col][c], a[pivot][c]);
    }
    // 归一化主元行。
    const float inv_p = 1.0f / a[col][col];
    for (int c = 0; c < 8; ++c) a[col][c] *= inv_p;
    // 消去其它行的该列。
    for (int r = 0; r < 4; ++r) {
      if (r == col) continue;
      const float f = a[r][col];
      if (f == 0.0f) continue;
      for (int c = 0; c < 8; ++c) a[r][c] -= f * a[col][c];
    }
  }

  Mat4 out{};
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) out.at(c, r) = a[r][4 + c];   // 行主序工作区 → 列主序存储
  }
  if (ok != nullptr) *ok = true;
  return out;
}

Mat4 make_translation(const Vec3& t) {
  Mat4 r;
  r.at(3, 0) = t.x;
  r.at(3, 1) = t.y;
  r.at(3, 2) = t.z;
  return r;
}

Mat4 make_scale(const Vec3& s) {
  Mat4 r;
  r.at(0, 0) = s.x;
  r.at(1, 1) = s.y;
  r.at(2, 2) = s.z;
  return r;
}

Mat4 make_rotation_quat(float x, float y, float z, float w) {
  // 不要求归一化(与 scene::Transform 同口径),这里按 length^2 归一化一次;
  // 长度为 0 时退化为单位阵(而不是产出 NaN 矩阵)。
  const float len2 = x * x + y * y + z * z + w * w;
  if (len2 < 1e-12f) return Mat4::identity();
  const float inv = 1.0f / std::sqrt(len2);
  x *= inv;
  y *= inv;
  z *= inv;
  w *= inv;
  Mat4 r;
  const float xx = x * x, yy = y * y, zz = z * z;
  const float xy = x * y, xz = x * z, yz = y * z;
  const float wx = w * x, wy = w * y, wz = w * z;
  // 标准 quaternion → 4x4(列主);与 glm::mat4_cast 逐元素一致。
  r.at(0, 0) = 1.0f - 2.0f * (yy + zz);
  r.at(1, 0) = 2.0f * (xy - wz);
  r.at(2, 0) = 2.0f * (xz + wy);
  r.at(0, 1) = 2.0f * (xy + wz);
  r.at(1, 1) = 1.0f - 2.0f * (xx + zz);
  r.at(2, 1) = 2.0f * (yz - wx);
  r.at(0, 2) = 2.0f * (xz - wy);
  r.at(1, 2) = 2.0f * (yz + wx);
  r.at(2, 2) = 1.0f - 2.0f * (xx + yy);
  return r;
}

Mat4 make_trs(const float t[3], const float q[4], const float s[3]) {
  return make_translation({t[0], t[1], t[2]}) * make_rotation_quat(q[0], q[1], q[2], q[3]) *
         make_scale({s[0], s[1], s[2]});
}

Mat4 make_look_at(const Vec3& eye, const Vec3& target, const Vec3& up) {
  const Vec3 f = normalize(target - eye);   // 前
  Vec3 s = cross(f, up);                    // 右
  if (length(s) < 1e-6f) s = Vec3{1.0f, 0.0f, 0.0f};  // 视线与 up 平行时的兜底
  s = normalize(s);
  const Vec3 u = cross(s, f);               // 重新正交化后的上

  Mat4 r;
  r.at(0, 0) = s.x;
  r.at(0, 1) = u.x;
  r.at(0, 2) = -f.x;
  r.at(1, 0) = s.y;
  r.at(1, 1) = u.y;
  r.at(1, 2) = -f.y;
  r.at(2, 0) = s.z;
  r.at(2, 1) = u.z;
  r.at(2, 2) = -f.z;
  r.at(3, 0) = -dot(s, eye);
  r.at(3, 1) = -dot(u, eye);
  r.at(3, 2) = dot(f, eye);
  return r;
}

Mat4 make_perspective(float fov_y_radians, float aspect, float near_z, float far_z,
                      bool reverse_z) {
  Mat4 r;
  for (float& v : r.m) v = 0.0f;
  // ---------------------------------------------------------------- Y 轴约定(重要)
  //
  // Vulkan 的 NDC 与 OpenGL 不同:**x 向右、y 向 (屏幕) 下**,z ∈ [0,1]。
  // 而 L3 的世界是 **+y 向上**。若不处理,画面会上下颠倒。
  //
  // 做法:在投影矩阵里对 y 取负(at(1,1) 为负),把"世界 +y 向上"翻成
  // "NDC +y 向下"。这样之后:
  //     ndc_y = -1  → 屏幕顶部(像素 y = 0)
  //     ndc_y = +1  → 屏幕底部(像素 y = height)
  // 于是 `world_to_screen` 里 **不能再二次翻转**,直接用
  //     py = (ndc_y * 0.5 + 0.5) * h
  // 即可与"像素原点左上、+y 向下"的 InputEvent 口径对齐(D-L3-2)。
  //
  // 历史教训:第一版在这里取了负号、又在 world_to_screen 里取了负号,
  // 两次翻转互相抵消 → 画面仍然颠倒,靠 unit test 才抓到。
  // **翻转只允许发生在这一个地方**,相机侧视为"NDC y 向下"。
  const float tan_half = std::tan(fov_y_radians * 0.5f);
  r.at(0, 0) = 1.0f / (aspect * tan_half);
  r.at(1, 1) = -1.0f / tan_half;
  if (reverse_z) {
    // 反向 z:近平面映射到 1,远平面映射到 0(深度精度翻倍提升)。
    r.at(2, 2) = near_z / (far_z - near_z);
    r.at(3, 2) = (far_z * near_z) / (far_z - near_z);
  } else {
    r.at(2, 2) = far_z / (near_z - far_z);
    r.at(3, 2) = (far_z * near_z) / (near_z - far_z);
  }
  r.at(2, 3) = -1.0f;
  return r;
}

Mat4 make_orthographic(float left, float right, float bottom, float top, float near_z,
                       float far_z, bool reverse_z) {
  Mat4 r;
  for (float& v : r.m) v = 0.0f;
  r.at(0, 0) = 2.0f / (right - left);
  // 与 make_perspective 同一个 Y 约定:世界 +y 向上 → Vulkan NDC +y 向下。
  // 注意 bottom/top 仍是"世界/视图空间"的 y(通常 bottom<top=+y 向上),
  // 因此 y 缩放取负即完成翻转,与透视路径一致。
  r.at(1, 1) = -2.0f / (top - bottom);
  r.at(3, 0) = -(right + left) / (right - left);
  r.at(3, 1) = (top + bottom) / (top - bottom);
  if (reverse_z) {
    r.at(2, 2) = 1.0f / (far_z - near_z);
    r.at(3, 2) = far_z / (far_z - near_z);
  } else {
    r.at(2, 2) = 1.0f / (near_z - far_z);
    r.at(3, 2) = near_z / (near_z - far_z);
  }
  r.at(3, 3) = 1.0f;
  return r;
}

// ---------------------------------------------------------------- Box

void Box::expand(const Vec3& p) {
  if (empty) {
    min = p;
    max = p;
    empty = false;
    return;
  }
  min.x = std::min(min.x, p.x);
  min.y = std::min(min.y, p.y);
  min.z = std::min(min.z, p.z);
  max.x = std::max(max.x, p.x);
  max.y = std::max(max.y, p.y);
  max.z = std::max(max.z, p.z);
}

void Box::expand(const Box& other) {
  if (other.empty) return;
  expand(other.min);
  expand(other.max);
}

bool Box::operator==(const Box& o) const {
  if (empty != o.empty) return false;
  if (empty) return true;
  return min == o.min && max == o.max;
}

// ---------------------------------------------------------------- 求交

bool intersect_ray_box(const Ray& ray, const Box& box, float* t_out, float t_min) {
  if (box.empty) return false;
  // slab 法。对方向分量为 0 的轴:起点不在 slab 内则必然不相交;
  // 用 1/d 会得到 ±inf,0*inf = NaN,因此逐轴显式处理。
  float t_near = -std::numeric_limits<float>::infinity();
  float t_far = std::numeric_limits<float>::infinity();
  for (int axis = 0; axis < 3; ++axis) {
    const float o = ray.origin[axis];
    const float d = ray.direction[axis];
    const float lo = box.min[axis];
    const float hi = box.max[axis];
    if (std::fabs(d) < 1e-9f) {
      if (o < lo || o > hi) return false;
      continue;
    }
    const float inv = 1.0f / d;
    float t0 = (lo - o) * inv;
    float t1 = (hi - o) * inv;
    if (t0 > t1) std::swap(t0, t1);
    t_near = std::max(t_near, t0);
    t_far = std::min(t_far, t1);
    if (t_near > t_far) return false;
  }
  const float t = t_near >= t_min ? t_near : t_far;
  if (t < t_min || t > t_far) return false;
  if (t_out != nullptr) *t_out = t;
  return true;
}

bool intersect_ray_triangle(const Ray& ray, const Vec3& a, const Vec3& b, const Vec3& c,
                            float* t_out, float t_min) {
  // Möller–Trumbore(双面)。
  const Vec3 e1 = b - a;
  const Vec3 e2 = c - a;
  const Vec3 p = cross(ray.direction, e2);
  const float det = dot(e1, p);
  if (std::fabs(det) < 1e-9f) return false;  // 平行
  const float inv_det = 1.0f / det;
  const Vec3 s = ray.origin - a;
  const float u = dot(s, p) * inv_det;
  if (u < 0.0f || u > 1.0f) return false;
  const Vec3 q = cross(s, e1);
  const float v = dot(ray.direction, q) * inv_det;
  if (v < 0.0f || u + v > 1.0f) return false;
  const float t = dot(e2, q) * inv_det;
  if (t < t_min) return false;
  if (t_out != nullptr) *t_out = t;
  return true;
}

bool intersect_ray_plane(const Ray& ray, const Plane& plane, float* t_out, float t_min) {
  const float denom = dot(plane.normal, ray.direction);
  if (std::fabs(denom) < 1e-9f) return false;  // 平行
  const float t = -(dot(plane.normal, ray.origin) + plane.d) / denom;
  if (t < t_min) return false;
  if (t_out != nullptr) *t_out = t;
  return true;
}

bool intersect_ray_sphere(const Ray& ray, const Vec3& center, float radius, float* t_out,
                          float t_min) {
  const Vec3 oc = ray.origin - center;
  const float a = dot(ray.direction, ray.direction);
  if (a < 1e-12f) return false;
  const float b = 2.0f * dot(oc, ray.direction);
  const float c = dot(oc, oc) - radius * radius;
  const float disc = b * b - 4.0f * a * c;
  if (disc < 0.0f) return false;
  const float sq = std::sqrt(disc);
  const float t0 = (-b - sq) / (2.0f * a);
  const float t1 = (-b + sq) / (2.0f * a);
  const float t = t0 >= t_min ? t0 : t1;
  if (t < t_min) return false;
  if (t_out != nullptr) *t_out = t;
  return true;
}

float distance_point_segment(const Vec3& p, const Vec3& a, const Vec3& b) {
  const Vec3 ab = b - a;
  const float len2 = dot(ab, ab);
  if (len2 < 1e-12f) return length(p - a);
  float t = dot(p - a, ab) / len2;
  t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
  return length(p - (a + ab * t));
}

float distance_ray_segment(const Ray& ray, const Vec3& a, const Vec3& b, float* t_out) {
  // 两线最近点(标准闭式解,见 Ericson《Real-Time Collision Detection》5.1.9)。
  const Vec3 u = ray.direction;
  const Vec3 v = b - a;
  const Vec3 w0 = ray.origin - a;
  const float aa = dot(u, u);
  const float bb = dot(u, v);
  const float cc = dot(v, v);
  const float dd = dot(u, w0);
  const float ee = dot(v, w0);
  const float denom = aa * cc - bb * bb;
  float sc = 0.0f;
  float tc = 0.0f;
  if (std::fabs(denom) < 1e-9f) {
    // 平行:退化为"射线起点到线段"的距离近似(t 取 -1 表示未定义)。
    if (t_out != nullptr) *t_out = -1.0f;
    return distance_point_segment(ray.origin, a, b);
  } else {
    sc = (bb * ee - cc * dd) / denom;   // 射线参数
    tc = (aa * ee - bb * dd) / denom;   // 线段参数
  }
  tc = tc < 0.0f ? 0.0f : (tc > 1.0f ? 1.0f : tc);
  if (sc < 0.0f) {
    sc = 0.0f;
    tc = dot(v, w0) / (cc > 1e-12f ? cc : 1.0f);
    tc = tc < 0.0f ? 0.0f : (tc > 1.0f ? 1.0f : tc);
  }
  if (t_out != nullptr) *t_out = sc;
  const Vec3 pr = ray.origin + u * sc;
  const Vec3 ps = a + v * tc;
  return length(pr - ps);
}

}  // namespace magicblocks::primitives
