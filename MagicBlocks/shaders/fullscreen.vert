#version 450
// MagicBlocks L0 —— 实例化自检画面的顶点着色器
//
// C4/G2:禁止几何着色器。所有"造几何"都走实例化绘制:
// 顶点着色器按 gl_InstanceIndex 推导本实例的格位与颜色,
// 只用 gl_VertexIndex 区分四边形的 6 个顶点 —— 不依赖任何几何 stage。
//
// 顶点数据由程序化生成(unit quad),不读顶点缓冲:
//   gl_VertexIndex 0..5 → TRIANGLE_LIST 覆盖一个四边形(两个三角形)
//   gl_InstanceIndex 0..15 → 4x4 色块
//
// L0 不声明 push constant:自检画面的全部参数都由实例索引程序化推导,
// 因此 pipeline layout 为空(见 vulkan_draw.cpp)。

layout(location = 0) out vec3 vColor;
layout(location = 1) out vec2 vUv;
layout(location = 2) flat out uint vInstance;

// TRIANGLE_LIST 的 6 个顶点(两个三角形):(左下, 右下, 右上) + (左下, 右上, 左上)
//
// 必须是 6 个顶点,**不能**沿用 TRIANGLE_STRIP 的 4 个角:
//   resources::PipelineDesc 的图元只有 TriangleList / LineList
//   (include/magicblocks/resources/pipeline.h 的 PrimitiveType),
//   而 to_vk_topology(vulkan_pipeline.cpp) 对非 LineList 一律给 TRIANGLE_LIST。
//   4 个 STRIP 角配 TRIANGLE_LIST 时只会画出三角形 (0,1,2):
//   四边形缺一半、实例 0 的纯红标定点也落不进去,而且不报任何错。
//   l2_scene.vert 用的就是下面这套 6 顶点排布,两者保持同构。
const vec2 kCorners[6] = vec2[6](
  vec2(-1.0, -1.0), vec2( 1.0, -1.0), vec2( 1.0,  1.0),
  vec2(-1.0, -1.0), vec2( 1.0,  1.0), vec2(-1.0,  1.0)
);

void main() {
  uint inst = gl_InstanceIndex;

  // 网格布局:4 x 4 个色块
  const uint kCols = 4u;
  uint cx = inst % kCols;
  uint cy = inst / kCols;

  // 每个实例占视口 1/4 的格子,内缩留缝。
  // 实例 0 必须落在图像**左上角**(通道序标定点)。
  // NDC 的 y 向上为正,graphics pipeline 的裁剪空间 y=+1 对应图像第 0 行,
  // 所以 cy=0 这一行要放到 NDC 的 +y 端 → 用 (kCols-1-cy)。
  // (readback_rgba8 按 vkCmdCopyImageToBuffer 的原生行序 1:1 拷出,不再翻行。)
  vec2 cell = vec2(1.0 / float(kCols), 1.0 / float(kCols));
  vec2 center = vec2((float(cx) + 0.5) * cell.x,
                     (float(kCols - 1u - cy) + 0.5) * cell.y) * 2.0 - 1.0;
  vec2 half_extent = cell * 0.42;

  vec2 corner = kCorners[gl_VertexIndex];
  vec2 pos = center + corner * half_extent;

  gl_Position = vec4(pos, 0.0, 1.0);

  if (inst == 0u) {
    // 实例 0:纯红标定点。输出通道必须原样落到 R,
    // 一旦 RGBA/BGRA 写反这里就会变成蓝色 —— 可判定的通道序断言。
    vColor = vec3(1.0, 0.0, 0.0);
  } else {
    // 其余实例:确定性配色,由实例索引直接推导,不依赖任何时间/输入
    float t = float(inst) / 15.0;
    vColor = vec3(0.15 + 0.8 * t, 0.25 + 0.6 * (1.0 - t), 0.35 + 0.5 * float(cx) / 3.0);
  }
  vUv = corner * 0.5 + 0.5;
  vInstance = inst;
}
