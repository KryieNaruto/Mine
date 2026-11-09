// MagicBlocks L3 —— 图元网格工厂（spec §13 M2 第 9 项；M4 扩充其余图元）
//
// 设计取舍：工厂只**产出 MeshData**（纯 CPU 顶点/索引），不直接接触 GPU。
//   - 一切几何算法（挤出、轮廓、球面细分）都能脱离设备单元测试，
//     不需要 SwiftShader 就能验证拓扑；
//   - 上传策略（staging、缓冲池、是否共享）由调用方决定，
//     L3 的 IMeshFactory 不替上层做内存决策。
//
// 每个图元都提供**单位尺寸**版本（Cube 边长 1，Cylinder 半径 .5 高 1），
// 由实例矩阵缩放。这样不同的长宽高共享同一个 MeshId，
// 合批时不会被拆开 —— 这是 §4.1"drawcall ≤ 300"的关键：
// 一万个不同大小的积木只用 1 个 Cube 网格。
//
// ⚠️ 但"只用单位尺寸"是**错的**，这里曾经自相矛盾过：
//   Cube 通过非均匀缩放就能得到任意立方体，所以单位尺寸够用；
//   而一个圆柱的"半径 : 高度"比一旦不同，**非均匀缩放会把圆截面压成椭圆**
//   —— 缩放矩阵作用在圆上不能保圆。所以半径/高度比不同的圆柱**必须是
//   不同的 MeshId**，无法靠实例矩阵节省。
//   结论：单位尺寸是"能用就用的默认"，不是"唯一允许的形态"。
//   带 radius/height 显式参数的版本正是为此存在 —— 它们让调用方可以
//   烘焙一个特定比例的圆柱，之后同样靠实例矩阵复用它的一万个实例。
//   两条路都保留：调 create_*_unit() 走共享单位网格；调 cylinder(r,h,s)
//   走烘焙。哪条更省，取决于场景里实际的尺寸分布，不该由 L3 替上层拍板。
#pragma once

#include <memory>
#include <vector>

#include "magicblocks/primitives/mesh.h"

namespace magicblocks::primitives {

// 立方体/盒子的面。用于生成**只有部分面**的网格（优化封闭体内部面）。
enum class BoxFace : std::uint8_t {
  kPosX = 0,
  kNegX = 1,
  kPosY = 2,
  kNegY = 3,
  kPosZ = 4,
  kNegZ = 5,
  kAll = 0xFF,
};

// 程序化图元生成器。全部是**纯函数**，无状态、无分配依赖，可直接单测。
// 轮廓挤出参数（**L4 玩法共用能力**，不是某个玩法的专用通道）。
//
// 用途：把任意 2D 闭合轮廓（可带孔）沿 +Z 挤成一个**水密实体**。
// 典型客户：涂鸦的笔画轮廓、印章的底板轮廓。玩法差异只在参数，不新增接口。
struct ExtrudeParams {
  float thickness = 0.08f;        // 沿 +Z 的厚度；<=0 → 只出上盖（薄片）
  bool cap_bottom = true;         // 是否要下盖（有下盖才是封闭实体）
  bool cap_top = true;
  float z_base = 0.0f;            // 下盖所在 z；上盖在 z_base + thickness
  Color4 color{1.0f, 1.0f, 1.0f, 1.0f};
};

// 轮廓集合：外轮廓 + 孔。每条轮廓是闭合环（首尾**不**重复存同一个点）。
// 孔与外的区分交给三角化器按绕序/填充规则处理，这里不做归属判断。
struct OutlineSet {
  std::vector<std::vector<Vec2>> outer;
  std::vector<std::vector<Vec2>> holes;
  bool empty() const { return outer.empty(); }
};

// 把轮廓挤出成网格（纯 CPU，无设备即可单测）。
//
// 三角化由 libtess2 完成（支持带孔、自动处理退化），侧壁由每条轮廓边逐条生成。
// 有上下盖时结果是一个**封闭实体**：每条无向边恰好被 2 个三角形共享 ——
// 这是"挤出体是实体"的机械判据，也是 3D 遮挡正确的前提（单测 Extrude.Watertight）。
//
// 轮廓为空 / 全部退化 / tess 失败 → 返回空 MeshData（不崩，由调用方判定处理）。
MeshData from_outline(const OutlineSet& outline, const ExtrudeParams& params);

// 把中心线按笔宽偏移成闭合轮廓（圆头/方头）。
//
// 这是"路径"语义的核心：一笔折线 → 一个胶囊状闭合外轮廓。
// 结果是一条闭合环（首尾不重复），逆时针；点数不足 2 或 width<=0 → 返回空。
// 多条中心线的**合并策略**属于 L4（玩法手感），L3 只保证单条几何正确。
std::vector<Vec2> stroke_outline(const std::vector<Vec2>& points, float width, bool round_cap,
                                 std::uint32_t arc_steps = 6);

class IMeshFactory {
 public:
  IMeshFactory() = default;
  IMeshFactory(const IMeshFactory&) = delete;
  IMeshFactory& operator=(const IMeshFactory&) = delete;
  virtual ~IMeshFactory() = default;

  // 单位立方体（边长 1，中心在原点，AABB = [-0.5, 0.5]^3）。
  // 24 顶点 / 36 索引 —— 每面独立顶点，避免法线与 uv 在角上被平均
  // （积木要的是硬边，不是圆角）。
  virtual MeshData cube() const = 0;

  // 同上，但只生成 faces 位掩码选中的面。
  // 用途：一万个积木紧密堆叠时，被完全包住的面不可见，
  // 按邻接关系剔掉能把顶点数降到 1/3 —— 但那是 L4 玩法层的优化，
  // L3 只提供这个能力，不替调用方判断。
  virtual MeshData box_faces(std::uint8_t faces_mask) const = 0;

  // 单位四边形：XY 平面，法线 +Z，范围 [-0.5, 0.5]。
  // 4 顶点 / 6 索引（两个三角形共享对角线顶点）。
  virtual MeshData quad() const = 0;

  // 地面网格：XZ 平面，法线 +Y，总共 divisions×divisions 格。
  // uv 覆盖 [0,1]²，方便贴棋盘格/网格线。索引数为 divisions²×6。
  virtual MeshData grid(u32 divisions) const = 0;

  // ---- 旋转体（M4）----
  //
  // 用显式方法而不是一个 enum 参数：图元的参数集差别太大
  // （Cylinder 要半径+高度+分段，Prism 要边数），挤进一个签名会变成
  // 一堆没人用的默认值，调用点也读不出在造什么。
  //
  // 全部按**绕 +Y 轴**生成，底面在 -h/2、顶面在 +h/2，中心在原点。
  // 参数非法（半径/高度非正、分段 < 3、rings < 2）时返回**空 MeshData**，
  // 而不是 clamp 到最小值 —— 静默修正会让调用方以为自己的参数生效了。
  virtual MeshData cylinder(float radius, float height, u32 segments) const = 0;
  virtual MeshData cone(float radius, float height, u32 segments) const = 0;
  virtual MeshData sphere(float radius, u32 rings, u32 segments) const = 0;

  // 正棱柱：横截面是正 n 边形，外接圆半径 radius。
  //
  // 与 cylinder 的区别不只是分段少：**法线是每面一个（平直着色）**，
  // 而 cylinder 是逐顶点径向法线（平滑着色）。
  // 这是 spec 未规定、但语义上必须区分的点：把 Prism 做成"分段少的圆柱"
  // 会让它看起来像个圆角柱，而不是棱柱。棱柱的棱角是它的**识别特征**。
  virtual MeshData prism(float radius, float height, u32 sides) const = 0;

  // 棱台/圆台：上下底半径可以不同。
  //
  // 这个图元是为了"圆台"而加的 —— 印章把手（总 spec P4）需要上细下粗。
  // 用 cylinder 的非均匀缩放**做不出来**：缩放会把上下半径同比缩放，
  // 两端的比例差是缩放矩阵无法表达的（那是投影变换，不是线性变换）。
  //
  // 参数：
  //   radius_bottom 底面外接圆半径；radius_top 顶面外接圆半径。
  //   segments 横截面分段数：=4 就是梯形柱，>=3 任意正多边形台。
  //   radius_top == 0 时退化成圆锥（此时与 cone() 等价，但法线策略不同，
  //   见下）；两个半径相等时退化成 cylinder。
  //
  // 法线策略：**平滑**（与 cylinder 一致，逐顶点径向 + 倾斜）。
  // 想要平直着色的棱台请自己按 segments 生成后处理法线 —— L3 不做。
  virtual MeshData trapezoid(float radius_bottom, float radius_top, float height,
                             u32 segments) const = 0;

  // ---- 单位尺寸快捷方式 ----
  //
  // 这几个是**便利函数**，不是新的图元：等价于传入固定的半径/高度。
  // 存在的理由见文件顶部：Cube 那类可以用实例的非均匀缩放得到任意尺寸，
  // 而圆/球不行，所以"单位网格 + 实例缩放"这条路只对一部分图元成立。
  // 显式列出，免得每个调用点自己记"单位圆柱到底是半径 .5 还是 1"。
  MeshData cylinder_unit(u32 segments = 24) const {
    return cylinder(0.5f, 1.0f, segments);
  }
  MeshData cone_unit(u32 segments = 24) const { return cone(0.5f, 1.0f, segments); }
  MeshData sphere_unit(u32 rings = 16, u32 segments = 24) const {
    return sphere(0.5f, rings, segments);
  }
  MeshData prism_unit(u32 sides = 6) const { return prism(0.5f, 1.0f, sides); }
};

std::unique_ptr<IMeshFactory> create_mesh_factory();

}  // namespace magicblocks::primitives
