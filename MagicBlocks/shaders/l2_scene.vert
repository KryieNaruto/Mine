#version 450
// MagicBlocks L2 —— 提交队列可视化顶点着色器
//
// C4/G2:禁止几何着色器。所有"造几何"走实例化绘制:
//   顶点着色器按 gl_InstanceIndex 推导本实例的格位与颜色,
//   只用 gl_VertexIndex 区分四边形 4 个角 —— 不依赖任何几何 stage。
//
// 与本着色器配套的管线拓扑是 **TRIANGLE_LIST**(L1 的 PrimitiveType 只有
// TriangleList / LineList,没有 strip),因此 4 个角按两个三角形发:
//   gl_VertexIndex 0,1,2 → 三角形 A(左下、右下、右上)
//   gl_VertexIndex 3,4,5 → 三角形 B(左下、右上、左上)
// 所以调用方必须传 vertex_count = 6、instance_count = 16。
//
// 顶点数据由程序化生成(unit quad),不读顶点缓冲 —— 顶点布局为空。

layout(location = 0) out vec3 vColor;
layout(location = 1) out vec2 vUv;
layout(location = 2) flat out uint vInstance;

// TRIANGLE_LIST 的 6 个顶点(两个三角形拼成 unit quad)。
const vec2 kCorners[6] = vec2[6](
  vec2(-1.0, -1.0), vec2( 1.0, -1.0), vec2( 1.0,  1.0),
  vec2(-1.0, -1.0), vec2( 1.0,  1.0), vec2(-1.0,  1.0)
);

void main() {
  uint inst = gl_InstanceIndex;

  // 网格布局:4 x 4 个色块(与 L0 自检画面同构,便于人眼对照)。
  const uint kCols = 4u;
  uint cx = inst % kCols;
  uint cy = inst / kCols;

  // 每个实例占视口 1/4 的格子,内缩留缝。
  // 实例 0 落在图像**左上角**(通道序标定点)。
  // NDC 的 y 向上为正,裁剪空间 y=+1 对应图像第 0 行,
  // 所以 cy=0 这一行要放到 NDC 的 +y 端 → 用 (kCols-1-cy)。
  vec2 cell = vec2(1.0 / float(kCols), 1.0 / float(kCols));
  vec2 center = vec2((float(cx) + 0.5) * cell.x,
                     (float(kCols - 1u - cy) + 0.5) * cell.y) * 2.0 - 1.0;
  vec2 half_extent = cell * 0.42;

  vec2 corner = kCorners[gl_VertexIndex];
  vec2 pos = center + corner * half_extent;

  gl_Position = vec4(pos, 0.0, 1.0);

  if (inst == 0u) {
    // 实例 0:纯绿标定点。L0 用纯红,RGBA/BGRA 写反会变蓝 ——
    // L2 改用纯绿,两套 golden 因此可以**独立判定**(plans-l2 §6.5)。
    vColor = vec3(0.0, 1.0, 0.0);
  } else {
    // 其余实例:确定性配色,由实例索引直接推导,不依赖任何时间/输入。
    float t = float(inst) / 15.0;
    vColor = vec3(0.10 + 0.85 * t, 0.20 + 0.70 * (1.0 - t), 0.45 + 0.45 * float(cy) / 3.0);
  }
  vUv = corner * 0.5 + 0.5;
  vInstance = inst;
}
