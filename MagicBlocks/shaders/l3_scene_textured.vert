#version 450
// MagicBlocks L3 —— 带纹理采样的实例化 3D 图元顶点着色器
//
// 用途（F-ST 印章）：印面 = 颜色 + 深度双贴图。
//   贴图通道约定（RGBA8 打包，用户裁定不加 R8G8_UNORM）：
//     R = 颜色（印油/釉色）
//     G = 深度（这一点凸起多少：五官/纹样高度）
//     B, A = 0（保留）
//
// 本文件相对 l3_scene.vert 只多两件事：
//   1. 顶点着色器采样贴图的 G 通道，沿 +Z 做位移（清单 §3.3 路径 A：顶点位移）。
//      —— 这样"五官在透视下被凸起挡住"由深度附件自然完成，不需要视差近似。
//   2. 把贴图的 R 通道（颜色）串给片元。
//
// Z 位移在世界空间做（用 model 矩阵的第三列做方向），所以缩放过的印章
// 位移也跟着缩放，观感一致。
//
// 确定性约束（同 l3_scene.vert）：位移只依赖顶点 uv 与贴图内容，
// 不依赖时间/相机，命令回放逐字节可复现。

// ---- binding 0：每顶点（与 mesh.h 的 Vertex 布局严格一致，48 字节）----
layout(location = 0) in vec3 inPosition;   // offset 0
layout(location = 1) in vec3 inNormal;     // offset 16
layout(location = 2) in vec4 inColor;      // offset 32 (ubyte4 归一化)
layout(location = 3) in vec2 inUv;         // offset 36

// ---- binding 1：每实例（与 batch.h 的 InstanceAttribs 严格一致）----
layout(location = 4) in vec4 inModel0;
layout(location = 5) in vec4 inModel1;
layout(location = 6) in vec4 inModel2;
layout(location = 7) in vec4 inModel3;
layout(location = 8) in vec4 inBaseColor;
layout(location = 9) in vec4 inMaterial;   // xyz = emissive, w = roughness
layout(location = 10) in vec4 inMaterial2; // x = metallic, y = max_relief

layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec3 vWorldPos;
layout(location = 2) out vec4 vColor;
layout(location = 3) out vec2 vUv;
layout(location = 4) flat out float vRoughness;
layout(location = 5) flat out float vMetallic;
layout(location = 6) out vec3 vEmissive;

// ---- set 0：采样纹理（P0 唯一新增的描述符位）----
// 固定约定：binding 0 = 颜色+深度双通道贴图。
layout(set = 0, binding = 0) uniform sampler2D uStampTex;

// R4 描述符策略不变：push constant 只放矩阵，材质/位移幅度走实例属性。
layout(push_constant) uniform PushConstants {
  mat4 view_proj;      // 相机视图投影（已含 Y 翻转，见 ICamera）
  mat4 normal_matrix;  // 法线矩阵（model 的逆转置）
} pc;

void main() {
  // 列主序重建世界矩阵（Mat4 的 m[col*4+row] 与这里逐列传入的顺序一致）
  mat4 model = mat4(inModel0, inModel1, inModel2, inModel3);

  // 采样深度通道（G）。max_relief 走实例属性 inMaterial2.y，
  // 让同一个网格、同一张贴图可以用不同凸起幅度实例化。
  float relief = texture(uStampTex, inUv).g * max(inMaterial2.y, 0.0);

  // 位移方向 = 世界空间 +Z（model 的第三列，归一化掉缩放）。
  // 若 model 退化（零缩放）则方向无意义，退化为不位移。
  vec3 up = model[2].xyz;
  float up_len = length(up);
  vec3 relief_dir = up_len > 1e-6 ? up / up_len : vec3(0.0, 0.0, 1.0);

  vec4 world = model * vec4(inPosition, 1.0);
  world.xyz += relief_dir * relief;
  vWorldPos = world.xyz;

  // 法线用逆转置矩阵变换：非等比缩放时直接用 model 会让法线歪掉
  vNormal = mat3(pc.normal_matrix) * inNormal;

  // 顶点色 × 实例基色。贴图颜色在片元里再乘上去（见 .frag），
  // 这样不着色的印面（干净贴图）也能靠基色区分子印章。
  vColor = inColor * inBaseColor;

  vUv = inUv;
  vRoughness = inMaterial.w;
  vMetallic = inMaterial2.x;
  vEmissive = inMaterial.xyz;

  gl_Position = pc.view_proj * world;
}
