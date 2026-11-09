#version 450
// MagicBlocks L3 —— 带纹理采样的实例化 3D 图元片段着色器
//
// 与 l3_scene.frag 的区别只有一处：基色乘以贴图的 R 通道（颜色）。
// 光照模型**完全照搬** l3_scene.frag（半兰伯特 + Blinn-Phong），
// 这样同一个物体在"有贴图/无贴图"两种管线下的亮度观感一致，
// 也让既有 golden 不受影响（那是另一条管线）。
//
// 为什么颜色在片元采、位移在顶点做：
//   - 顶点位移需要深度在**细分后**的网格上采样才平滑，所以放 VS；
//   - 颜色是低频细节，片元采就够，且 UV 在本文件里可用 gl 插值得到。
//
// 确定性：不读时间、不读相机位置，光照方向是常量 —— 命令回放逐字节可复现。

layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec3 vWorldPos;
layout(location = 2) in vec4 vColor;
layout(location = 3) in vec2 vUv;
layout(location = 4) flat in float vRoughness;
layout(location = 5) flat in float vMetallic;
layout(location = 6) in vec3 vEmissive;

layout(location = 0) out vec4 outColor;

// 与 l3_scene_textured.vert 保持同一套 set/binding 约定
layout(set = 0, binding = 0) uniform sampler2D uStampTex;

// 固定光照方向（世界空间），与 l3_scene.frag 完全一致
const vec3 kLightDir = normalize(vec3(0.45, 0.85, 0.30));
const vec3 kFillDir  = normalize(vec3(-0.60, -0.25, -0.50));

void main() {
  vec3 n = normalize(vNormal);

  float ndl = dot(n, kLightDir);
  float diffuse = ndl * 0.5 + 0.5;
  diffuse = diffuse * 0.85 + 0.15;

  float fill = max(dot(n, kFillDir), 0.0) * 0.25;

  vec3 half_dir = normalize(kLightDir + vec3(0.0, 0.0, 1.0));
  float spec_power = mix(96.0, 4.0, clamp(vRoughness, 0.0, 1.0));
  float spec = pow(max(dot(n, half_dir), 0.0), spec_power);
  spec *= mix(0.45, 0.05, clamp(vRoughness, 0.0, 1.0));

  // 贴图 R 通道 = 印面颜色；G 通道已在 VS 用于位移，这里不再参与着色。
  // 贴图 R 直接**乘**实例基色：贴图为白时行为退化成无贴图的 l3_scene.frag。
  float stampR = texture(uStampTex, vUv).r;
  vec3 base = vColor.rgb * vec3(stampR);

  vec3 diffuse_color = base * (1.0 - clamp(vMetallic, 0.0, 1.0) * 0.7);
  vec3 spec_color = mix(vec3(1.0), base, clamp(vMetallic, 0.0, 1.0));

  vec3 lit = diffuse_color * (diffuse + fill) + spec_color * spec + vEmissive;
  outColor = vec4(lit * vColor.a, vColor.a);
}
