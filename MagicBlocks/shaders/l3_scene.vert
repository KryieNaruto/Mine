#version 450
// MagicBlocks L3 —— 实例化 3D 图元顶点着色器（spec §4.3 合批 / §9.2 实例属性）
//
// 一个 draw 画 N 个实例：几何来自 binding 0（每顶点的 position/normal/color/uv），
// 每实例的世界矩阵与材质参数来自 binding 1（step rate = instance）。
//
// G2:禁止实例化之外的"几何展开"—— 顶点着色器只按 gl_VertexIndex 取顶点，
// 不做任何"生成新几何"的事。

// ---- binding 0：每顶点（与 mesh.h 的 Vertex 布局严格一致，48 字节）----
layout(location = 0) in vec3 inPosition;   // offset 0
layout(location = 1) in vec3 inNormal;     // offset 16
layout(location = 2) in vec4 inColor;      // offset 32 (ubyte4 归一化)
layout(location = 3) in vec2 inUv;         // offset 36

// ---- binding 1：每实例（与 batch.h 的 InstanceAttribs 严格一致）----
// InstanceAttribs 是 48 字节：model float[16] (64B) + base_color(16B) + emissive(12B)
//   + roughness(4B) + metallic(4B) + pad(12B) = 112B
// 注意：矩阵占 4 个 vec4 location（每列一个），这是 Vulkan 的标准做法。
layout(location = 4) in vec4 inModel0;     // 列 0
layout(location = 5) in vec4 inModel1;     // 列 1
layout(location = 6) in vec4 inModel2;     // 列 2
layout(location = 7) in vec4 inModel3;     // 列 3（平移在 xyz）
layout(location = 8) in vec4 inBaseColor;
layout(location = 9) in vec4 inMaterial;   // xyz = emissive, w = roughness
layout(location = 10) in vec4 inMaterial2; // x = metallic

layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec3 vWorldPos;
layout(location = 2) out vec4 vColor;
layout(location = 3) out vec2 vUv;
layout(location = 4) flat out float vRoughness;
layout(location = 5) flat out float vMetallic;
layout(location = 6) out vec3 vEmissive;

// R4 描述符策略：push constant 优先。128B 上限里只放两个矩阵（正好用满）。
layout(push_constant) uniform PushConstants {
  mat4 view_proj;   // 相机视图投影（已含 Y 翻转，见 ICamera）
  mat4 normal_matrix;  // 法线矩阵（model 的逆转置）
} pc;

void main() {
  // 列主序重建世界矩阵（Mat4 的 m[col*4+row] 与这里逐列传入的顺序一致）
  mat4 model = mat4(inModel0, inModel1, inModel2, inModel3);

  vec4 world = model * vec4(inPosition, 1.0);
  vWorldPos = world.xyz;

  // 法线用逆转置矩阵变换：非等比缩放时直接用 model 会让法线歪掉
  vNormal = mat3(pc.normal_matrix) * inNormal;

  // 顶点色（通常为白）× 实例基色 = 最终基色。
  // 这样同一个 Cube 网格可以用不同颜色实例化，仍然合成一个 draw。
  vColor = inColor * inBaseColor;

  vUv = inUv;
  vRoughness = inMaterial.w;
  vMetallic = inMaterial2.x;
  vEmissive = inMaterial.xyz;

  gl_Position = pc.view_proj * world;
}
