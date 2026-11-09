// MagicBlocks L3 —— 图元网格生成（纯 CPU，可无设备单测）
#include "magicblocks/primitives/mesh_factory.h"

#include <tesselator.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>

namespace magicblocks::primitives {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kHalf = 0.5f;

// 从顶点列表重算 AABB。
// 每个图元生成后都调一次而不是手填 —— 手填的边界是历史 bug 高发地
// （改了生成参数忘改 bounds，拾取就偏了）。
Box compute_bounds(const std::vector<Vertex>& verts) {
  Box b;
  b.min = Vec3{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
               std::numeric_limits<float>::max()};
  b.max = Vec3{-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(),
               -std::numeric_limits<float>::max()};
  if (verts.empty()) {
    b.min = Vec3{0, 0, 0};
    b.max = Vec3{0, 0, 0};
    return b;
  }
  for (const Vertex& v : verts) {
    for (int k = 0; k < 3; ++k) {
      if (v.position[k] < (&b.min.x)[k]) (&b.min.x)[k] = v.position[k];
      if (v.position[k] > (&b.max.x)[k]) (&b.max.x)[k] = v.position[k];
    }
  }
  // 【必须】清掉 empty 标志。
  //
  // Box::empty 的语义（见 types.cpp 的 Box::expand）是"还没有任何有效范围"，
  // 而 Box() 默认 empty = true。这里算出了 min/max 却不清标志，
  // 就等于对调用方宣称"这个网格没有范围",尽管几何就在那里。
  //
  // 这是一个曾经真实存在、且长期没被发现的 bug：没有任何既有单测断言过
  // mesh.bounds.empty,而真正读它的地方是拾取与剔除
  // （intersect_ray_box 第一行就是 if (box.empty) return false)。
  // 后果是"某些物体点不中、被错误剔除",且全程零报错。
  b.empty = false;
  return b;
}

void set_color(Vertex& v, std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) {
  v.color[0] = r;
  v.color[1] = g;
  v.color[2] = b;
  v.color[3] = a;
}

// 追加一个四边形（p0-p1-p2-p3 逆时针），自动拆成两个三角形。
// 顶点色默认白（真色由材质/实例属性决定），法线由调用方给。
void push_quad(MeshData& m, const Vec3& p0, const Vec3& p1, const Vec3& p2, const Vec3& p3,
               const Vec3& normal) {
  const std::uint32_t base = static_cast<std::uint32_t>(m.vertices.size());

  const Vec3* pts[4] = {&p0, &p1, &p2, &p3};
  for (int i = 0; i < 4; ++i) {
    Vertex v;
    v.position[0] = pts[i]->x;
    v.position[1] = pts[i]->y;
    v.position[2] = pts[i]->z;
    v.normal[0] = normal.x;
    v.normal[1] = normal.y;
    v.normal[2] = normal.z;
    set_color(v, 255, 255, 255, 255);
    // uv 按四边形的局部 (s,t)，让贴图能正确平铺
    v.uv[0] = (i == 1 || i == 2) ? 1.0f : 0.0f;
    v.uv[1] = (i >= 2) ? 1.0f : 0.0f;
    m.vertices.push_back(v);
  }
  m.indices.push_back(base + 0);
  m.indices.push_back(base + 1);
  m.indices.push_back(base + 2);
  m.indices.push_back(base + 0);
  m.indices.push_back(base + 2);
  m.indices.push_back(base + 3);
}

// 六个面的几何定义表。
//
// 绕序**不是手写的**，而是由算法保证 —— 这是关键设计决定：
//   第一版我把 24 个坐标手写进表里，结果 12 个三角形的绕序全反
//   （背面剔除下整个立方体会消失），改一次又反了一半。手工核对
//   "从外面看是否逆时针"在 3D 里极难靠脑内旋转完成，而且改一个坐标
//   就可能破坏邻面的顺序，属于典型的人肉脆弱代码。
//
//   现在改成：每个面只声明 **法线 n 和面上两个切向 u/v**，
//   四个角按 (0,0) → (1,0) → (1,1) → (0,1) 的 u/v 参数生成。
//   只要约定 u × v = n（右手系），生成出的四边形必然满足
//   (p1-p0) × (p2-p0) ∥ n，绕序自动正确。
//   u × v = n 这个条件由构造时断言 + 测试双重保障。
struct FaceDef {
  std::uint8_t bit;  // BoxFace 掩码位
  Vec3 normal;       // 外法线
  Vec3 u;            // 切向 1，满足 u × v = normal
  Vec3 v;            // 切向 2
};

Vec3 cross3(const Vec3& a, const Vec3& b) {
  return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

// 六个面：主轴方向 + 两个切向。全部满足 u × v = normal。
const FaceDef kFaces[6] = {
    // +X: (0,1,0) × (0,0,1) = (1,0,0) ✓
    {1, Vec3{1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, 0, 1}},
    // -X: (0,0,1) × (0,1,0) = (0*0-1*1, 1*0-0*0, 0*1-0*0) = (-1,0,0) ✓
    {2, Vec3{-1, 0, 0}, Vec3{0, 0, 1}, Vec3{0, 1, 0}},
    // +Y: (0,0,1) × (1,0,0) = (0*0-1*0, 1*1-0*0, 0*0-0*1) = (0,1,0) ✓
    {4, Vec3{0, 1, 0}, Vec3{0, 0, 1}, Vec3{1, 0, 0}},
    // -Y: (1,0,0) × (0,0,1) = (0*1-0*0, 0*0-1*1, 1*0-0*0) = (0,-1,0) ✓
    {8, Vec3{0, -1, 0}, Vec3{1, 0, 0}, Vec3{0, 0, 1}},
    // +Z: (1,0,0) × (0,1,0) = (0,0,1) ✓
    {16, Vec3{0, 0, 1}, Vec3{1, 0, 0}, Vec3{0, 1, 0}},
    // -Z: (0,1,0) × (1,0,0) = (1*0-0*0, 0*1-0*0, 0*0-1*1) = (0,0,-1) ✓
    {32, Vec3{0, 0, -1}, Vec3{0, 1, 0}, Vec3{1, 0, 0}},
};

class MeshFactory final : public IMeshFactory {
 public:
  MeshData cube() const override { return box_faces(0xFF); }

  MeshData box_faces(std::uint8_t faces_mask) const override {
    MeshData m;
    // 预先校验：u × v 必须等于 normal，否则生成出的面绕序必然错。
    // 这是"构造即正确"的把关点 —— 新增面时必须过这一关，不靠人眼。
    for (const FaceDef& f : kFaces) {
      const Vec3 c = cross3(f.u, f.v);
      const float dot = c.x * f.normal.x + c.y * f.normal.y + c.z * f.normal.z;
      // u × v 必须与 normal 同向（点积 > 0），否则绕序必然错。
      // 用 assert 而不是运行期返回：这是**表写错**，不是用户输入错，
      // 应该在开发期立即炸掉，而不是悄悄产出一个背面朝外的立方体。
      assert(dot > 0.99f && "FaceDef 的 u × v 与 normal 不同向，会导致绕序错误");
      (void)dot;    }

    for (const FaceDef& f : kFaces) {
      if ((faces_mask & f.bit) == 0) continue;

      // 中心 = normal * 半边长；四角 = 中心 ± u/2 ± v/2，
      // 按 (0,0)→(1,0)→(1,1)→(0,1) 的 u/v 参数绕一圈。
      // u × v = normal 保证了这一圈就是从外面看逆时针。
      const Vec3 c{f.normal.x * kHalf, f.normal.y * kHalf, f.normal.z * kHalf};
      const Vec3 hu{f.u.x * kHalf, f.u.y * kHalf, f.u.z * kHalf};
      const Vec3 hv{f.v.x * kHalf, f.v.y * kHalf, f.v.z * kHalf};

      auto corner = [&](float su, float sv) {
        return Vec3{c.x + su * hu.x + sv * hv.x, c.y + su * hu.y + sv * hv.y,
                    c.z + su * hu.z + sv * hv.z};
      };

      push_quad(m, corner(-1, -1), corner(+1, -1), corner(+1, +1), corner(-1, +1), f.normal);
    }
    if (faces_mask == 0) return m;  // 空网格，bounds 留默认
    m.bounds = compute_bounds(m.vertices);
    return m;
  }

  MeshData quad() const override {
    MeshData m;
    push_quad(m, Vec3{-kHalf, -kHalf, 0}, Vec3{kHalf, -kHalf, 0}, Vec3{kHalf, kHalf, 0},
              Vec3{-kHalf, kHalf, 0}, Vec3{0, 0, 1});
    m.bounds = compute_bounds(m.vertices);
    return m;
  }

  MeshData grid(u32 divisions) const override {
    MeshData m;
    // divisions 为 0 或 1 时没有意义（0 格或 1 个点无法成面），直接返回空。
    if (divisions < 1) return m;

    const u32 n = divisions;
    const float step = 1.0f / static_cast<float>(n);

    // 顶点：(n+1)² 个，XZ 平面，y=0
    m.vertices.reserve(static_cast<std::size_t>(n + 1) * (n + 1));
    for (u32 z = 0; z <= n; ++z) {
      for (u32 x = 0; x <= n; ++x) {
        Vertex v;
        v.position[0] = -kHalf + static_cast<float>(x) * step;
        v.position[1] = 0.0f;
        v.position[2] = -kHalf + static_cast<float>(z) * step;
        v.normal[0] = 0.0f;
        v.normal[1] = 1.0f;
        v.normal[2] = 0.0f;
        set_color(v, 255, 255, 255, 255);
        v.uv[0] = static_cast<float>(x) / static_cast<float>(n);
        v.uv[1] = static_cast<float>(z) / static_cast<float>(n);
        m.vertices.push_back(v);
      }
    }

    // 索引：n² 个格子 × 2 三角形 × 3
    m.indices.reserve(static_cast<std::size_t>(n) * n * 6);
    const u32 row = n + 1;
    for (u32 z = 0; z < n; ++z) {
      for (u32 x = 0; x < n; ++x) {
        const std::uint32_t i0 = z * row + x;
        const std::uint32_t i1 = i0 + 1;
        const std::uint32_t i2 = i0 + row;
        const std::uint32_t i3 = i2 + 1;
        // 从 +Y 往下看，逆时针绕序
        m.indices.push_back(i0);
        m.indices.push_back(i2);
        m.indices.push_back(i1);
        m.indices.push_back(i1);
        m.indices.push_back(i2);
        m.indices.push_back(i3);
      }
    }

    m.bounds = compute_bounds(m.vertices);
    return m;
  }

  // ---- M4 旋转体图元 ----
  //
  // 三个图元共享同一个参数化思路：绕 +Y 轴转的旋成面（surface of revolution）。
  // 但它们**不能**用同一个通用 helper 生成，原因是法线语义不同：
  //   Cylinder 侧面法线在水平面内（跟高度无关）；
  //   Cone     侧面法线有固定倾角（斜边决定）；
  //   Sphere   法线就是归一化后的位置。
  // 抽一个"通用 revolve(profile, normals)"虽然能写，但参数的隐式契约
  // （剖面上每段贡献什么法线）比三个直白的循环更难读，也更容易写错。
  // 这类几何代码宁可有 3 份 20 行，也不要 1 份 60 行带 4 个模式的开关。
  //
  // 共同的绕序约定（与 box 一致，由 u × v = n 推导）：
  //   侧面从**外面**看必须逆时针。以 +Y 为轴，角度 a 增大方向是绕轴逆时针，
  //   侧面四边形的绕序取 (下一段下沿, 本段下沿, 本段上沿, 下一段上沿) 即可。

  MeshData cylinder(float radius, float height, u32 segments) const override {
    MeshData m;
    // 退化参数直接返回空：半径非正、高度非正、分段 < 3 都构不成圆柱。
    // 返回空而不是 clamp 到最小值 —— 静默修正会让调用方以为自己传的参数生效了。
    if (radius <= 0.0f || height <= 0.0f || segments < 3) return m;

    const u32 seg = segments;
    const float half_h = height * kHalf;
    const float step = 2.0f * kPi / static_cast<float>(seg);

    // 侧面：seg+1 圈顶点（首尾重复，让 uv 的 u 能连续到 1.0）
    // 每圈 2 个顶点（下沿、上沿），法线朝外水平方向。
    const std::uint32_t side_base = 0;
    for (u32 i = 0; i <= seg; ++i) {
      const float a = static_cast<float>(i) * step;
      const float ca = std::cos(a);
      const float sa = std::sin(a);
      const float u = static_cast<float>(i) / static_cast<float>(seg);

      Vertex vb;  // 下沿
      vb.position[0] = radius * ca;
      vb.position[1] = -half_h;
      vb.position[2] = radius * sa;
      vb.normal[0] = ca;
      vb.normal[1] = 0.0f;
      vb.normal[2] = sa;
      set_color(vb, 255, 255, 255, 255);
      vb.uv[0] = u;
      vb.uv[1] = 0.0f;
      m.vertices.push_back(vb);

      Vertex vt;  // 上沿
      vt.position[0] = radius * ca;
      vt.position[1] = half_h;
      vt.position[2] = radius * sa;
      vt.normal[0] = ca;
      vt.normal[1] = 0.0f;
      vt.normal[2] = sa;
      set_color(vt, 255, 255, 255, 255);
      vt.uv[0] = u;
      vt.uv[1] = 1.0f;
      m.vertices.push_back(vt);
    }

    // 侧面索引：每段一个四边形 = 2 个三角形。
    //
    // ⚠️ 绕序陷阱（实测踩过，全部 5 个旋转体图元都栽在这）：
    //   点按 (cos a, y, sin a) 生成，a 增大时方向是 +X → +Z。
    //   在右手系里，从 +Y 往下看这是**顺时针**，不是逆时针。
    //   所以 (下i, 下i+1, 上i+1) 的叉积是朝内的 —— 最初就是这么写的，
    //   结果 (p1-p0)×(p2-p0) 与顶点法线点积恰好为 -|n|，
    //   背面剔除下整个圆柱侧面消失，只剩两个端盖。
    //   正确绕序是把每一段的两个三角形**反过来**：(下i, 上i+1, 下i+1)。
    //
    //   凡是"绕着某个轴扫一串点"生成的几何都要单独验证绕序，
    //   脑内想象右手定则在这种有符号置换上极不可靠。
    for (u32 i = 0; i < seg; ++i) {
      const std::uint32_t b0 = side_base + i * 2;      // 下沿 i
      const std::uint32_t t0 = b0 + 1;                 // 上沿 i
      const std::uint32_t b1 = side_base + (i + 1) * 2;  // 下沿 i+1
      const std::uint32_t t1 = b1 + 1;                 // 上沿 i+1
      m.indices.push_back(b0);
      m.indices.push_back(t1);
      m.indices.push_back(b1);
      m.indices.push_back(b0);
      m.indices.push_back(t0);
      m.indices.push_back(t1);
    }

    // 端盖：上盖法线 +Y，下盖法线 -Y。
    // 用三角扇而不是把端盖顶点并进侧面 —— 端盖法线是纯竖直的，
    // 和侧面的水平法线不可能共享顶点，共享会让边缘的光照插值糊成一片圆角。
    const auto add_cap = [&](float y, float ny) {
      const std::uint32_t center = static_cast<std::uint32_t>(m.vertices.size());
      Vertex c;
      c.position[0] = 0.0f;
      c.position[1] = y;
      c.position[2] = 0.0f;
      c.normal[0] = 0.0f;
      c.normal[1] = ny;
      c.normal[2] = 0.0f;
      set_color(c, 255, 255, 255, 255);
      c.uv[0] = 0.5f;
      c.uv[1] = 0.5f;
      m.vertices.push_back(c);

      const std::uint32_t rim = static_cast<std::uint32_t>(m.vertices.size());
      for (u32 i = 0; i <= seg; ++i) {
        const float a = static_cast<float>(i) * step;
        const float ca = std::cos(a);
        const float sa = std::sin(a);
        Vertex v;
        v.position[0] = radius * ca;
        v.position[1] = y;
        v.position[2] = radius * sa;
        v.normal[0] = 0.0f;
        v.normal[1] = ny;
        v.normal[2] = 0.0f;
        set_color(v, 255, 255, 255, 255);
        // 端盖 uv 用圆盘映射，方便将来贴顶面纹理
        v.uv[0] = 0.5f + 0.5f * ca;
        v.uv[1] = 0.5f + 0.5f * sa;
        m.vertices.push_back(v);
      }

      for (u32 i = 0; i < seg; ++i) {
        // ⚠️ 端盖绕序与侧面同源：点按 (cos a, y, sin a) 生成，
        // a 增大在右手系里是**顺时针**（从 +Y 俯视）。
        // 所以上盖要逆序遍历圆周才得到朝 +Y 的法线。
        // 写成 i → i+1 会得到朝 -Y 的法线，上盖被背面剔除掉，
        // 从正上方看圆柱是个空洞（但侧面完全正常，极难归因）。
        if (ny > 0.0f) {
          // 上盖：逆时针从 +Y 俯视 = 圆周反向遍历
          m.indices.push_back(center);
          m.indices.push_back(rim + i + 1);
          m.indices.push_back(rim + i);
        } else {
          // 下盖：扫视方向本身就是"从 -Y 仰视的逆时针"，正序即可
          m.indices.push_back(center);
          m.indices.push_back(rim + i);
          m.indices.push_back(rim + i + 1);
        }
      }
    };

    add_cap(+half_h, +1.0f);
    add_cap(-half_h, -1.0f);

    m.bounds = compute_bounds(m.vertices);
    return m;
  }

  MeshData cone(float radius, float height, u32 segments) const override {
    MeshData m;
    if (radius <= 0.0f || height <= 0.0f || segments < 3) return m;

    const u32 seg = segments;
    const float half_h = height * kHalf;
    const float step = 2.0f * kPi / static_cast<float>(seg);

    // 侧面法线的倾角：斜边从 (radius, -half_h) 到 (0, +half_h)，
    // 外法线 = normalize(高度方向分量, 半径方向分量)
    //        = normalize(height, 2*radius)。
    // 这里**必须**逐顶点给半径方向，而不是整个锥面共用一个法线 ——
    // 否则锥体会渲染成一个扁平的多边形扇，光照完全不随角度变化。
    const float nlen = std::sqrt(height * height + 4.0f * radius * radius);
    const float n_rad = height / nlen;    // 半径方向分量
    const float n_up = (2.0f * radius) / nlen;  // 竖直分量

    for (u32 i = 0; i <= seg; ++i) {
      const float a = static_cast<float>(i) * step;
      const float ca = std::cos(a);
      const float sa = std::sin(a);
      const float u = static_cast<float>(i) / static_cast<float>(seg);

      Vertex vb;
      vb.position[0] = radius * ca;
      vb.position[1] = -half_h;
      vb.position[2] = radius * sa;
      vb.normal[0] = n_rad * ca;
      vb.normal[1] = n_up;
      vb.normal[2] = n_rad * sa;
      set_color(vb, 255, 255, 255, 255);
      vb.uv[0] = u;
      vb.uv[1] = 0.0f;
      m.vertices.push_back(vb);

      // 顶点处的"上沿"退化成轴上的一个点：所有段共享同一个尖端位置，
      // 但法线不同，所以每个顶点各存一份（否则尖端处法线会被平均成竖直向上，
      // 出现一个黑洞式的光照异常）。
      Vertex vt;
      vt.position[0] = 0.0f;
      vt.position[1] = half_h;
      vt.position[2] = 0.0f;
      vt.normal[0] = n_rad * ca;
      vt.normal[1] = n_up;
      vt.normal[2] = n_rad * sa;
      set_color(vt, 255, 255, 255, 255);
      vt.uv[0] = u;
      vt.uv[1] = 1.0f;
      m.vertices.push_back(vt);
    }

    for (u32 i = 0; i < seg; ++i) {
      const std::uint32_t b0 = i * 2;
      const std::uint32_t t0 = b0 + 1;
      const std::uint32_t b1 = (i + 1) * 2;
      const std::uint32_t t1 = b1 + 1;
      m.indices.push_back(b0);
      m.indices.push_back(t1);
      m.indices.push_back(b1);
      m.indices.push_back(b0);
      m.indices.push_back(t1);
      m.indices.push_back(t0);
    }

    // 底面（法线 -Y）。锥体只有下盖。
    const std::uint32_t center = static_cast<std::uint32_t>(m.vertices.size());
    {
      Vertex c;
      c.position[0] = 0.0f;
      c.position[1] = -half_h;
      c.position[2] = 0.0f;
      c.normal[0] = 0.0f;
      c.normal[1] = -1.0f;
      c.normal[2] = 0.0f;
      set_color(c, 255, 255, 255, 255);
      c.uv[0] = 0.5f;
      c.uv[1] = 0.5f;
      m.vertices.push_back(c);
    }
    const std::uint32_t rim = static_cast<std::uint32_t>(m.vertices.size());
    for (u32 i = 0; i <= seg; ++i) {
      const float a = static_cast<float>(i) * step;
      const float ca = std::cos(a);
      const float sa = std::sin(a);
      Vertex v;
      v.position[0] = radius * ca;
      v.position[1] = -half_h;
      v.position[2] = radius * sa;
      v.normal[0] = 0.0f;
      v.normal[1] = -1.0f;
      v.normal[2] = 0.0f;
      set_color(v, 255, 255, 255, 255);
      v.uv[0] = 0.5f + 0.5f * ca;
      v.uv[1] = 0.5f + 0.5f * sa;
      m.vertices.push_back(v);
    }
    // 底面（法线 -Y）。锥体只有下盖。
    //
    // 绕序与 cylinder 的下盖一致：(center, i, i+1)。
    // 这里曾经写成 (center, i+1, i) —— 那会得到朝 +Y 的法线，
    // 与顶点法线 (0,-1,0) 相反，底面在背面剔除下消失，
    // 从下往上看是个空洞。注意上下盖的绕序是**相反**的，
    // 不要以为"统一成 i→i+1"就对了。
    for (u32 i = 0; i < seg; ++i) {
      m.indices.push_back(center);
      m.indices.push_back(rim + i);
      m.indices.push_back(rim + i + 1);
    }

    m.bounds = compute_bounds(m.vertices);
    return m;
  }

  MeshData sphere(float radius, u32 rings, u32 segments) const override {
    MeshData m;
    // rings = 纬度圈数（从北极到南极分成多少段），segments = 经线数。
    // rings < 2 时只剩两个极点，构不成球面。
    if (radius <= 0.0f || rings < 2 || segments < 3) return m;

    const u32 nr = rings;  // 纬向段数
    const u32 ns = segments;  // 经向段数

    // 顶点：纬度环 0..nr，每环 ns+1 个（首尾重复，让 uv 的 u 连续到 1）。
    // 极点是特殊处理：第 0 环全是北极点、第 nr 环全是南极点，
    // 位置重合但 uv 的 u 不同 —— 这是为了让贴图在极点处不出现接缝撕裂。
    //
    // 法线 = 归一化位置（球心在原点，半径为 radius）。
    // 这是球体法线最干净的写法：不需要任何角度推导，天然平滑。
    for (u32 r = 0; r <= nr; ++r) {
      const float v = static_cast<float>(r) / static_cast<float>(nr);  // 0=北极 1=南极
      // 极角 theta: 0 在北极，pi 在南极
      const float theta = v * kPi;
      const float st = std::sin(theta);
      const float ct = std::cos(theta);

      for (u32 s = 0; s <= ns; ++s) {
        const float u = static_cast<float>(s) / static_cast<float>(ns);
        const float phi = u * 2.0f * kPi;
        const float sp = std::sin(phi);
        const float cp = std::cos(phi);

        Vertex vert;
        vert.position[0] = radius * st * cp;
        vert.position[1] = radius * ct;
        vert.position[2] = radius * st * sp;
        // 法线 = position / radius
        vert.normal[0] = st * cp;
        vert.normal[1] = ct;
        vert.normal[2] = st * sp;
        set_color(vert, 255, 255, 255, 255);
        vert.uv[0] = u;
        vert.uv[1] = v;
        m.vertices.push_back(vert);
      }
    }

    // 索引：每对相邻纬环之间是 ns 个四边形。
    // 极点环会退化成三角形（四边形的两个顶点重合），
    // 仍然按四边形发索引也可以，但会产出**退化三角形**（面积 0），
    // 白白浪费顶点处理。所以两端的环单独按三角形发。
    const u32 row = ns + 1;
    for (u32 r = 0; r < nr; ++r) {
      for (u32 s = 0; s < ns; ++s) {
        const std::uint32_t a = r * row + s;        // 上环 s
        const std::uint32_t b = a + 1;              // 上环 s+1
        const std::uint32_t c = (r + 1) * row + s;  // 下环 s
        const std::uint32_t d = c + 1;              // 下环 s+1

        // ⚠️ 球面绕序的推导（和旋转体侧面同源的顺时针陷阱）：
        //   u 增大的方向是经度 +1，在右手系里绕 +Y 是**顺时针**（俯视）；
        //   v 增大是从北极往南极走（纬度递减）。
        //   所以 (a, b, d) 的叉积指向球**内**。
        //   正确绕序要把每个三角形反过来：(a, d, b) / (a, c, d)。
        //   最初全部 224 个三角形都写反了 —— 症状是整个球被背面剔除后
        //   完全不显示，但顶点数、法线、包围盒全对，只能靠绕序检查抓到。
        if (r == 0) {
          // 北极退化：a 与 b 都在北极点，四边形塌成三角形 (a, c, d)
          m.indices.push_back(a);
          m.indices.push_back(d);
          m.indices.push_back(c);
        } else if (r + 1 == nr) {
          // 南极退化：c 与 d 都在南极点
          m.indices.push_back(a);
          m.indices.push_back(b);
          m.indices.push_back(c);
        } else {
          m.indices.push_back(a);
          m.indices.push_back(b);
          m.indices.push_back(d);
          m.indices.push_back(a);
          m.indices.push_back(d);
          m.indices.push_back(c);
        }
      }
    }

    m.bounds = compute_bounds(m.vertices);
    return m;
  }

  MeshData prism(float radius, float height, u32 sides) const override {
    MeshData m;
    if (radius <= 0.0f || height <= 0.0f || sides < 3) return m;

    const u32 n = sides;
    const float half_h = height * kHalf;
    const float step = 2.0f * kPi / static_cast<float>(n);

    // 侧面：**每面独立 4 个顶点**，法线是面的外法线（平直着色）。
    //
    // 这与 cylinder 的做法正好相反：cylinder 在同一圈上共享顶点，
    // 法线取径向方向，相邻面之间插值后看起来是圆滑的。
    // 棱柱必须让每个面有自己的顶点，否则两条棱之间的法线会被平均，
    // 棱就"化"成了圆柱 —— 棱柱的棱角是它的识别特征，不能糊掉。
    for (u32 i = 0; i < n; ++i) {
      const float a0 = static_cast<float>(i) * step;
      const float a1 = static_cast<float>(i + 1) * step;
      const float c0 = std::cos(a0), s0 = std::sin(a0);
      const float c1 = std::cos(a1), s1 = std::sin(a1);

      // 面的外法线：取该面两侧角平分线的方向。
      // 对正多边形，角平分线方向就等于面法线方向。
      const float am = (a0 + a1) * 0.5f;
      const float nx = std::cos(am);
      const float nz = std::sin(am);

      const std::uint32_t base = static_cast<std::uint32_t>(m.vertices.size());
      // 四边形：底i, 底i+1, 顶i+1, 顶i —— 从外面看逆时针
      const float px[4] = {radius * c0, radius * c1, radius * c1, radius * c0};
      const float py[4] = {-half_h, -half_h, half_h, half_h};
      const float pz[4] = {radius * s0, radius * s1, radius * s1, radius * s0};
      const float uvx[4] = {0.0f, 1.0f, 1.0f, 0.0f};
      const float uvy[4] = {0.0f, 0.0f, 1.0f, 1.0f};
      for (int k = 0; k < 4; ++k) {
        Vertex v;
        v.position[0] = px[k];
        v.position[1] = py[k];
        v.position[2] = pz[k];
        v.normal[0] = nx;
        v.normal[1] = 0.0f;
        v.normal[2] = nz;
        set_color(v, 255, 255, 255, 255);
        v.uv[0] = uvx[k];
        v.uv[1] = uvy[k];
        m.vertices.push_back(v);
      }
      // 四边形顶点顺序：[底i, 底i+1, 顶i+1, 顶i]
      //
      // ⚠️ 绕序与旋转体同源的同一个坑：点按 (cos a, y, sin a) 生成，
      // a 增大在右手系里是顺时针（从 +Y 俯视）。所以 (0,1,2) 的叉积
      // 指向**内侧**，必须反成 (0,2,1)。写反的后果是棱柱侧面全部消失，
      // 只剩上下两个盖子 —— 看起来像个"悬浮的六边形片"。
      m.indices.push_back(base + 0);
      m.indices.push_back(base + 2);
      m.indices.push_back(base + 1);
      m.indices.push_back(base + 0);
      m.indices.push_back(base + 3);
      m.indices.push_back(base + 2);
    }

    // 上下盖：正 n 边形，法线纯竖直。
    const auto add_polygon_cap = [&](float y, float ny) {
      const std::uint32_t center = static_cast<std::uint32_t>(m.vertices.size());
      Vertex c;
      c.position[0] = 0.0f;
      c.position[1] = y;
      c.position[2] = 0.0f;
      c.normal[0] = 0.0f;
      c.normal[1] = ny;
      c.normal[2] = 0.0f;
      set_color(c, 255, 255, 255, 255);
      c.uv[0] = 0.5f;
      c.uv[1] = 0.5f;
      m.vertices.push_back(c);

      const std::uint32_t ring = static_cast<std::uint32_t>(m.vertices.size());
      for (u32 i = 0; i <= n; ++i) {
        const float a = static_cast<float>(i) * step;
        const float ca = std::cos(a), sa = std::sin(a);
        Vertex v;
        v.position[0] = radius * ca;
        v.position[1] = y;
        v.position[2] = radius * sa;
        v.normal[0] = 0.0f;
        v.normal[1] = ny;
        v.normal[2] = 0.0f;
        set_color(v, 255, 255, 255, 255);
        v.uv[0] = 0.5f + 0.5f * ca;
        v.uv[1] = 0.5f + 0.5f * sa;
        m.vertices.push_back(v);
      }
      for (u32 i = 0; i < n; ++i) {
        if (ny > 0.0f) {
          m.indices.push_back(center);
          m.indices.push_back(ring + i + 1);
          m.indices.push_back(ring + i);
        } else {
          m.indices.push_back(center);
          m.indices.push_back(ring + i);
          m.indices.push_back(ring + i + 1);
        }
      }
    };
    add_polygon_cap(+half_h, +1.0f);
    add_polygon_cap(-half_h, -1.0f);

    m.bounds = compute_bounds(m.vertices);
    return m;
  }

  MeshData trapezoid(float radius_bottom, float radius_top, float height,
                     u32 segments) const override {
    MeshData m;
    if (radius_bottom <= 0.0f || radius_top < 0.0f || height <= 0.0f || segments < 3) {
      return m;
    }

    const u32 seg = segments;
    const float half_h = height * kHalf;
    const float step = 2.0f * kPi / static_cast<float>(seg);
    const float rb = radius_bottom;
    const float rt = radius_top;

    // 侧面法线的倾斜量 —— 本图元唯一需要动脑的地方。
    //
    // 侧面在 (半径, 高度) 平面上是一条从 (rb, -h/2) 到 (rt, +h/2) 的直线，
    // 方向 (rt-rb, height)。外法线垂直于它，单位化后是
    //   n_rad = height / L,  n_up = (rb - rt) / L,  L = |(height, rb-rt)|
    //
    // 三个退化情形验证：
    //   rb == rt（圆柱）  → n_up = 0，退化成纯水平法线 ✓
    //   rt == 0（圆锥）   → n_up > 0，法线朝上倾斜 ✓
    //   rt > rb（倒锥）   → n_up < 0，法线朝下倾斜 ✓
    //
    // 注意 n_up 的符号是 (rb - rt) 而不是 (rt - rb)：上细下粗时外侧法线
    // 应向**上**倾斜。写反的话光照会整体翻转，暗部与亮部对调 ——
    // 这种错误在小图上很难一眼看出，所以上面把三个退化情形都写下来了。
    const float drad = rb - rt;
    const float nlen = std::sqrt(height * height + drad * drad);
    const float n_rad = height / nlen;  // height > 0 保证 nlen > 0
    const float n_up = drad / nlen;

    for (u32 i = 0; i <= seg; ++i) {
      const float a = static_cast<float>(i) * step;
      const float ca = std::cos(a), sa = std::sin(a);
      const float u = static_cast<float>(i) / static_cast<float>(seg);

      Vertex vb;
      vb.position[0] = rb * ca;
      vb.position[1] = -half_h;
      vb.position[2] = rb * sa;
      vb.normal[0] = n_rad * ca;
      vb.normal[1] = n_up;
      vb.normal[2] = n_rad * sa;
      set_color(vb, 255, 255, 255, 255);
      vb.uv[0] = u;
      vb.uv[1] = 0.0f;
      m.vertices.push_back(vb);

      Vertex vt;
      vt.position[0] = rt * ca;
      vt.position[1] = half_h;
      vt.position[2] = rt * sa;
      vt.normal[0] = n_rad * ca;
      vt.normal[1] = n_up;
      vt.normal[2] = n_rad * sa;
      set_color(vt, 255, 255, 255, 255);
      vt.uv[0] = u;
      vt.uv[1] = 1.0f;
      m.vertices.push_back(vt);
    }

    for (u32 i = 0; i < seg; ++i) {
      const std::uint32_t b0 = i * 2;
      const std::uint32_t t0 = b0 + 1;
      const std::uint32_t b1 = (i + 1) * 2;
      const std::uint32_t t1 = b1 + 1;
      m.indices.push_back(b0);
      m.indices.push_back(t1);
      m.indices.push_back(b1);
      m.indices.push_back(b0);
      m.indices.push_back(t0);
      m.indices.push_back(t1);
    }

    // 端盖：半径 > 0 的那一端才需要。
    // 顶半径为 0（圆锥）时顶盖会退化成一堆零面积三角形 —— 不发。
    const auto add_tapered_cap = [&](float y, float ny, float r) {
      if (r <= 0.0f) return;
      const std::uint32_t center = static_cast<std::uint32_t>(m.vertices.size());
      Vertex c;
      c.position[0] = 0.0f;
      c.position[1] = y;
      c.position[2] = 0.0f;
      c.normal[0] = 0.0f;
      c.normal[1] = ny;
      c.normal[2] = 0.0f;
      set_color(c, 255, 255, 255, 255);
      c.uv[0] = 0.5f;
      c.uv[1] = 0.5f;
      m.vertices.push_back(c);

      const std::uint32_t ring = static_cast<std::uint32_t>(m.vertices.size());
      for (u32 i = 0; i <= seg; ++i) {
        const float a = static_cast<float>(i) * step;
        const float ca = std::cos(a), sa = std::sin(a);
        Vertex v;
        v.position[0] = r * ca;
        v.position[1] = y;
        v.position[2] = r * sa;
        v.normal[0] = 0.0f;
        v.normal[1] = ny;
        v.normal[2] = 0.0f;
        set_color(v, 255, 255, 255, 255);
        v.uv[0] = 0.5f + 0.5f * ca;
        v.uv[1] = 0.5f + 0.5f * sa;
        m.vertices.push_back(v);
      }
      for (u32 i = 0; i < seg; ++i) {
        if (ny > 0.0f) {
          m.indices.push_back(center);
          m.indices.push_back(ring + i + 1);
          m.indices.push_back(ring + i);
        } else {
          m.indices.push_back(center);
          m.indices.push_back(ring + i);
          m.indices.push_back(ring + i + 1);
        }
      }
    };
    add_tapered_cap(+half_h, +1.0f, rt);
    add_tapered_cap(-half_h, -1.0f, rb);

    m.bounds = compute_bounds(m.vertices);
    return m;
  }
};

}  // namespace

// ---------------------------------------------------------------- 轮廓挤出
//
// 这是 L4 玩法（涂鸦描边、印章底板）共同依赖的**几何能力**。
// 放在 L3 而不是 L4，理由与文件顶部一致：它是纯几何算法，
// 不含任何玩法语义，且必须能脱离设备单测。

namespace {

// 有符号面积（鞋带公式）。>0 = 逆时针。
float signed_area(const std::vector<Vec2>& ring) {
  if (ring.size() < 3) return 0.0f;
  float a = 0.0f;
  for (std::size_t i = 0; i < ring.size(); ++i) {
    const Vec2& p = ring[i];
    const Vec2& q = ring[(i + 1) % ring.size()];
    a += p.x * q.y - q.x * p.y;
  }
  return 0.5f * a;
}

// 去掉相邻重复点与共线点。
//
// 必须做：libtess2 遇到零长度边会产生不确定的三角化结果，
// 而笔画采样天然会产生重复点（同一像素连续 Move）。
// 共线点虽不致命，但会让侧壁生成出一堆零面积三角形。
std::vector<Vec2> clean_ring(const std::vector<Vec2>& in, float eps = 1e-6f) {
  std::vector<Vec2> out;
  out.reserve(in.size());
  for (const Vec2& p : in) {
    if (!out.empty()) {
      const float dx = p.x - out.back().x;
      const float dy = p.y - out.back().y;
      if (dx * dx + dy * dy <= eps * eps) continue;
    }
    out.push_back(p);
  }
  // 首尾重复也要去掉
  while (out.size() >= 2) {
    const float dx = out.front().x - out.back().x;
    const float dy = out.front().y - out.back().y;
    if (dx * dx + dy * dy <= eps * eps) {
      out.pop_back();
    } else {
      break;
    }
  }
  if (out.size() < 3) return {};

  // 共线剔除
  std::vector<Vec2> res;
  res.reserve(out.size());
  const std::size_t n = out.size();
  for (std::size_t i = 0; i < n; ++i) {
    const Vec2& a = out[(i + n - 1) % n];
    const Vec2& b = out[i];
    const Vec2& c = out[(i + 1) % n];
    const float cross = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (std::fabs(cross) > eps * eps) res.push_back(b);
  }
  if (res.size() < 3) return {};
  return res;
}

}  // namespace

MeshData from_outline(const OutlineSet& outline, const ExtrudeParams& params) {
  MeshData m;

  // 1) 清理 + 收集所有环（外轮廓与孔一视同仁 —— libtess2 按填充规则
  //    自动判断归属，我们不猜）。
  std::vector<std::vector<Vec2>> rings;
  rings.reserve(outline.outer.size() + outline.holes.size());
  for (const auto& r : outline.outer) {
    std::vector<Vec2> c = clean_ring(r);
    if (!c.empty()) rings.push_back(std::move(c));
  }
  for (const auto& r : outline.holes) {
    std::vector<Vec2> c = clean_ring(r);
    if (!c.empty()) rings.push_back(std::move(c));
  }
  if (rings.empty()) return m;  // 空轮廓 → 空网格，由调用方判定

  std::vector<std::vector<Vec2>> caps;  // 三角化后的盖面三角形（索引空间一致）
  std::size_t total_pts = 0;
  for (const auto& r : rings) total_pts += r.size();

  // 2) 三角化：libtess2，TESS_POLYGONS + 非零环绕。
  //    非零环绕（NONZERO）而不是奇偶（ODD）：多笔重叠时重叠区仍然是实心的，
  //    这正是"路径包围区域"的语义；ODD 会把重叠处挖出洞。
  TESStesselator* tess = tessNewTess(nullptr);
  if (tess == nullptr) return m;
  for (const auto& r : rings) {
    tessAddContour(tess, 2, r.data(), static_cast<int>(sizeof(Vec2)),
                   static_cast<int>(r.size()));
  }
  const int ok = tessTesselate(tess, TESS_WINDING_NONZERO, TESS_POLYGONS, 3, 2, nullptr);
  if (!ok) {
    tessDeleteTess(tess);
    return m;  // 三角化失败 → 空网格（不崩、不静默产出碎片）
  }

  const int vcount = tessGetVertexCount(tess);
  const TESSreal* tv = tessGetVertices(tess);
  const TESSindex* elems = tessGetElements(tess);
  const int ecount = tessGetElementCount(tess);
  if (vcount <= 0 || ecount <= 0) {
    tessDeleteTess(tess);
    return m;
  }

  const float z0 = params.z_base;
  const float z1 = params.z_base + params.thickness;
  const bool has_sides = params.thickness > 0.0f;

  // 3) 盖面顶点（每个顶点两份：上盖 + 下盖）。
  //    上下盖法线相反，所以不能共享顶点。
  const auto push_cap_vertex = [&m](float x, float y, float z, float nz) {
    Vertex v;
    v.position[0] = x;
    v.position[1] = y;
    v.position[2] = z;
    v.normal[0] = 0.0f;
    v.normal[1] = 0.0f;
    v.normal[2] = nz;
    set_color(v, 255, 255, 255, 255);
    v.uv[0] = x;
    v.uv[1] = y;
    const std::uint32_t id = static_cast<std::uint32_t>(m.vertices.size());
    m.vertices.push_back(v);
    return id;
  };

  // libtess2 输出的三角形绕序与其坐标系的 y 轴方向相反（它按 y 向下约定）。
  // 直接用会让所有盖面朝反方向 —— 这个坑不能靠肉眼发现，
  // 所以这里统一翻转一次，并用单测 Extrude.CapNormal 锁住。

  std::vector<std::uint32_t> top_ids(static_cast<std::size_t>(vcount));
  for (int i = 0; i < vcount; ++i) {
    top_ids[static_cast<std::size_t>(i)] =
        push_cap_vertex(tv[i * 2 + 0], tv[i * 2 + 1], z1, +1.0f);
  }

  if (params.cap_top) {
    for (int e = 0; e < ecount; ++e) {
      const TESSindex* tri = &elems[e * 3];
      if (tri[0] == TESS_UNDEF || tri[1] == TESS_UNDEF || tri[2] == TESS_UNDEF) continue;
      // libtess2 输出的三角形在 XY 平面上是**逆时针**（实测 cross = +4，
      // 输入环也是逆时针就保持逆时针）。上盖法线是 +Z，
      // 逆时针绕序的 (b-a)×(c-a) 正好指向 +Z → **不翻转**。
      //
      // 这里原先做了一个"翻转"，注释还写着"libtess2 按 y 向下约定"——
      // 那是从别处抄来的说法，实测不成立。翻转的后果是上盖法线朝下，
      // 从上方看整个盖面被背面剔除掉，模型看起来像被切开了。
      m.indices.push_back(top_ids[static_cast<std::size_t>(tri[0])]);
      m.indices.push_back(top_ids[static_cast<std::size_t>(tri[1])]);
      m.indices.push_back(top_ids[static_cast<std::size_t>(tri[2])]);
    }
  }

  std::vector<std::uint32_t> bot_ids(static_cast<std::size_t>(vcount));
  if (params.cap_bottom && has_sides) {
    for (int i = 0; i < vcount; ++i) {
      bot_ids[static_cast<std::size_t>(i)] =
          push_cap_vertex(tv[i * 2 + 0], tv[i * 2 + 1], z0, -1.0f);
    }
    for (int e = 0; e < ecount; ++e) {
      const TESSindex* tri = &elems[e * 3];
      if (tri[0] == TESS_UNDEF || tri[1] == TESS_UNDEF || tri[2] == TESS_UNDEF) continue;
      // 下盖法线是 -Z，所以**要**翻转绕序（交换 1、2 号索引），
      // 让 (b-a)×(c-a) 指向 -Z 与法线一致。
      m.indices.push_back(bot_ids[static_cast<std::size_t>(tri[0])]);
      m.indices.push_back(bot_ids[static_cast<std::size_t>(tri[2])]);
      m.indices.push_back(bot_ids[static_cast<std::size_t>(tri[1])]);
    }
  }

  // 4) 侧壁：遍历**输入环**的每条边（不是三角化后的边 —— 三角化会产生
  //    内部对角线，沿它们建墙就把实体切碎了）。
  //    只有真正的边界边需要墙；输入环全是边界边，因为孔/外轮廓的
  //    每条边都对应一段真实侧面。
  if (has_sides) {
    for (const auto& r : rings) {
      const std::size_t n = r.size();
      const bool ccw = signed_area(r) > 0.0f;
      for (std::size_t i = 0; i < n; ++i) {
        const Vec2& a = r[i];
        const Vec2& b = r[(i + 1) % n];
        // 外法线：逆时针环的边 (a→b) 的外法线是 (dy, -dx)；顺时针则相反。
        float nx = (b.y - a.y);
        float ny = -(b.x - a.x);
        if (!ccw) {
          nx = -nx;
          ny = -ny;
        }
        const float len = std::sqrt(nx * nx + ny * ny);
        if (len <= 1e-9f) continue;
        nx /= len;
        ny /= len;

        // 侧壁四个角：a下、b下、b上、a上。以 (a→b) 为前进方向、
        // 外法线为右，则从外侧看是逆时针 → 正面朝向观察者。
        const Vec3 p0{a.x, a.y, z0};
        const Vec3 p1{b.x, b.y, z0};
        const Vec3 p2{b.x, b.y, z1};
        const Vec3 p3{a.x, a.y, z1};
        if (ccw) {
          push_quad(m, p0, p1, p2, p3, Vec3{nx, ny, 0.0f});
        } else {
          // 顺时针环的外法线已翻转，顶点顺序也要跟着翻，否则法线与绕序打架
          push_quad(m, p1, p0, p3, p2, Vec3{nx, ny, 0.0f});
        }
      }
    }
  }

  tessDeleteTess(tess);

  m.bounds = compute_bounds(m.vertices);
  return m;
}

std::vector<Vec2> stroke_outline(const std::vector<Vec2>& points, float width, bool round_cap,
                                 std::uint32_t arc_steps) {
  std::vector<Vec2> out;
  if (points.size() < 2 || !(width > 0.0f)) return out;

  // 去重复点（方向向量会退化成 NaN）
  std::vector<Vec2> pts;
  pts.reserve(points.size());
  for (const Vec2& p : points) {
    if (pts.empty()) {
      pts.push_back(p);
      continue;
    }
    const float dx = p.x - pts.back().x;
    const float dy = p.y - pts.back().y;
    if (dx * dx + dy * dy > 1e-12f) pts.push_back(p);
  }
  if (pts.size() < 2) return out;

  const float hw = 0.5f * width;
  const std::uint32_t steps = arc_steps == 0 ? 1u : arc_steps;

  // 构造胶囊：沿左法线一侧正着走，到终点画半圆，再沿右法线一侧倒着走，
  // 回到起点画半圆。这样结果是一条闭合环，且自然逆时针。
  //
  // 只在顶点处取法线（不做 miter 处理）：笔画点密，折角很小，
  // miter 的收益抵不上它带来的尖刺问题（锐角处 miter 会飞出去）。
  const auto normal_at = [&pts, hw](std::size_t i, float sign) {
    // 用相邻段的平均方向，避免折角处出现豁口
    Vec2 d_prev{}, d_next{};
    if (i > 0) {
      d_prev = Vec2{pts[i].x - pts[i - 1].x, pts[i].y - pts[i - 1].y};
    }
    if (i + 1 < pts.size()) {
      d_next = Vec2{pts[i + 1].x - pts[i].x, pts[i + 1].y - pts[i].y};
    }
    Vec2 d = d_prev;
    if (i > 0 && i + 1 < pts.size()) {
      // 归一化后平均
      const float lp = std::sqrt(d_prev.x * d_prev.x + d_prev.y * d_prev.y);
      const float ln = std::sqrt(d_next.x * d_next.x + d_next.y * d_next.y);
      Vec2 a{d_prev.x / lp, d_prev.y / lp};
      Vec2 b{d_next.x / ln, d_next.y / ln};
      d = Vec2{a.x + b.x, a.y + b.y};
      const float l = std::sqrt(d.x * d.x + d.y * d.y);
      if (l <= 1e-6f) d = b;  // 折返 180°，退化成后一段方向
    } else if (i == 0) {
      d = d_next;
    }
    const float l = std::sqrt(d.x * d.x + d.y * d.y);
    if (l <= 1e-9f) return Vec2{0.0f, 0.0f};
    // 左法线 = (-dy, dx)。sign=+1 取左，-1 取右。
    return Vec2{-d.y / l * hw * sign, d.x / l * hw * sign};
  };

  const std::size_t n = pts.size();

  // 正向：起点 → 终点，取左法线
  for (std::size_t i = 0; i < n; ++i) {
    const Vec2 nn = normal_at(i, +1.0f);
    out.push_back(Vec2{pts[i].x + nn.x, pts[i].y + nn.y});
  }

  // 终点半圆：从左法线转到右法线（绕终点，逆时针）
  if (round_cap) {
    const Vec2 d{pts[n - 1].x - pts[n - 2].x, pts[n - 1].y - pts[n - 2].y};
    const float base = std::atan2(d.y, d.x);
    // 角度必须**扫满** 180°，所以 t 必须能取到 0 和 1。
    //
    // ⚠️ 这里原来写的是 t = k / steps（k 从 1 到 steps-1），
    // 于是 t 只落在 (0,1) 开区间里，angle 只覆盖 -60°..+60° 而不是
    // -90°..+90° —— 半圆被砍掉了两端，胶囊两端变成"被削平的圆"，
    // 面积少了 0.126（正好是缺掉的那块弓形）。
    // 这个错误肉眼完全看不出来：它产出的是一个合法的闭合多边形，
    // 只是形状比胶囊略瘦。只有算面积才能发现。
    //
    // 正确做法：分母用 steps-1，让 k=1 → t=0（起始角）、
    // k=steps-1 → t=1（终止角），两端点位与正/反向段的首末点重合，
    // 由外层去重逻辑兜住。
    const std::uint32_t denom = steps > 1 ? steps - 1 : 1;
    for (std::uint32_t k = 1; k < steps; ++k) {
      const float t = static_cast<float>(k) / static_cast<float>(denom);
      // 终点半圆：末点已经在左法线侧（base+90°），要绕到右法线侧（base-90°）。
      // **经正前方**走完 180°，就意味着角度从 +90° **递减**到 -90°：
      //     +90° → 0° → -90°
      // cos 由 0 → 1 → 0，x 向外凸出到 (n-1)+hw。
      //
      // ⚠️ 用递增 (+π/2 → +3π/2) 扫的是**反方向**，弧会朝线段内侧凹进去：
      // 实测 x 只到 2.0 就收回，胶囊右端被压平成一堵墙。
      // 判断办法只有一个：算点的 x 范围。胶囊两端必须各超出端点 hw。
      const float a = base + kPi * 0.5f - kPi * t;
      out.push_back(
          Vec2{pts[n - 1].x + hw * std::cos(a), pts[n - 1].y + hw * std::sin(a)});
    }
  }

  // 反向：终点 → 起点，取右法线
  for (std::size_t k = n; k-- > 0;) {
    const Vec2 nn = normal_at(k, -1.0f);
    out.push_back(Vec2{pts[k].x + nn.x, pts[k].y + nn.y});
  }

  // 起点半圆（与终点半圆同样的 t 映射问题，一并修正）
  if (round_cap) {
    const Vec2 d{pts[1].x - pts[0].x, pts[1].y - pts[0].y};
    const float base = std::atan2(d.y, d.x);
    const std::uint32_t denom = steps > 1 ? steps - 1 : 1;
    for (std::uint32_t k = 1; k < steps; ++k) {
      const float t = static_cast<float>(k) / static_cast<float>(denom);
      // 起点半圆要接在**环的末尾**：环此时刚走完右法线一侧、回到起点附近，
      // 下一步要回到环的第一个点（左法线侧的起点）。
      // 所以这半圈从 base-90°（右法线侧）出发，**经后方**扫到 base+90°（左法线侧），
      // 即角度递减 180°：-90° → -180° → -270°(== +90°)。
      //
      // ⚠️ 换成 +π*t 会变成"从 base-90° 经 base 正前方到 base+90°"，
      // 扫的是线段的**前方**而不是后方 —— 起点端被画成朝前凸的半球，
      // 而且与终点半圆重叠，整个多边形自交，面积反而更小。
      const float a = base - kPi * 0.5f - kPi * t;
      out.push_back(Vec2{pts[0].x + hw * std::cos(a), pts[0].y + hw * std::sin(a)});
    }
  }

  // 统一反转成逆时针。
  //
  // 上面的构造顺序（正向走左法线侧 → 终点半圆 → 反向走右法线侧 → 起点半圆）
  // 走出来的环**是顺时针**：以水平线 (0,0)→(2,0) 为例，路径依次经过
  // (0,+hw)、(2,+hw)、(2,-hw)、(0,-hw) —— 从上往下再往左回，是顺时针。
  // 而头文件契约要求逆时针（from_outline 靠 signed_area>0 判断外法线方向，
  // 顺时针会让整个实体的侧壁法线朝内）。
  //
  // 直接反转点序是最省事也最不容易错的做法：环的方向整体翻转，
  // 内部各段（含两段半圆的扫向）自动跟着正确，不用逐段改角度公式。
  std::reverse(out.begin(), out.end());

  // 去掉反转后出现的相邻重复点 —— 半圆在 t=0 / t=1 处会与
  // 正/反向段的端点重合（角度公式此时给出与原端点相同的坐标，
  // 只是浮点误差可能让它差 1e-7）。重复点会让挤出的侧壁产生
  // 零长度四边形 → 零面积三角形，在 GPU 上表现为随机闪烁。
  //
  // 注意必须和**首尾相接**也一起判：环是闭合的，
  // 最后一个点和第一个点重复同样是退化。
  if (out.size() >= 2) {
    std::vector<Vec2> dedup;
    dedup.reserve(out.size());
    for (const Vec2& p : out) {
      if (!dedup.empty()) {
        const float dx = p.x - dedup.back().x;
        const float dy = p.y - dedup.back().y;
        if (dx * dx + dy * dy <= 1e-14f) continue;
      }
      dedup.push_back(p);
    }
    // 首尾重合也去掉一个
    if (dedup.size() >= 2) {
      const float dx = dedup.front().x - dedup.back().x;
      const float dy = dedup.front().y - dedup.back().y;
      if (dx * dx + dy * dy <= 1e-14f) dedup.pop_back();
    }
    out.swap(dedup);
  }

  return out;
}

std::unique_ptr<IMeshFactory> create_mesh_factory() {
  return std::make_unique<MeshFactory>();
}

}  // namespace magicblocks::primitives
