#version 450
// MagicBlocks L3 —— 实例化 3D 图元片段着色器
//
// L3 不需要真的 PBR：图元（积木/方块/网格）的首要目标是"看得出形状与颜色"，
// 而不是材质真实感。所以这里用**半兰伯特 + 一点高光**：
//   - 半兰伯特（wrapped diffuse）让背光面不至于全黑，积木堆叠时邻面仍可辨；
//   - 一点点 Blinn-Phong 高光让立方体的棱角在视觉上立得住；
//   - emissive 直接加，用于 gizmo 高亮这类"自发光"效果。
//
// 真正的 PBR（IBL / BRDF 积分）留给 L4 的材质功能层 —— L3 的 IMaterial
// 已经有 roughness/metallic 字段，届时只需替换这个文件。

layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec3 vWorldPos;
layout(location = 2) in vec4 vColor;
layout(location = 3) in vec2 vUv;
layout(location = 4) flat in float vRoughness;
layout(location = 5) flat in float vMetallic;
layout(location = 6) in vec3 vEmissive;

layout(location = 0) out vec4 outColor;

// 固定光照方向（世界空间）。刻意不随时间变 —— 命令回放要求同样的输入
// 产出同样的像素，任何"跟时间/相机走"的光照都会破坏确定性。
const vec3 kLightDir = normalize(vec3(0.45, 0.85, 0.30));
const vec3 kFillDir  = normalize(vec3(-0.60, -0.25, -0.50));

void main() {
  vec3 n = normalize(vNormal);

  // 半兰伯特：dot 映射到 [0,1] 而不是 [-1,1]，背光面保留 30% 亮度
  float ndl = dot(n, kLightDir);
  float diffuse = ndl * 0.5 + 0.5;
  diffuse = diffuse * 0.85 + 0.15;   // 环境项，避免死黑

  // 补光：从反方向来一点，让侧面不至于同时过暗
  float fill = max(dot(n, kFillDir), 0.0) * 0.25;

  // Blinn-Phong 高光（用相机方向近似 = 光照方向，L3 不做视线相关计算）
  vec3 half_dir = normalize(kLightDir + vec3(0.0, 0.0, 1.0));
  float spec_power = mix(96.0, 4.0, clamp(vRoughness, 0.0, 1.0));
  float spec = pow(max(dot(n, half_dir), 0.0), spec_power);
  spec *= mix(0.45, 0.05, clamp(vRoughness, 0.0, 1.0));

  vec3 base = vColor.rgb;
  // 金属度让漫反射变暗、高光带基色（简化的金属响应）
  vec3 diffuse_color = base * (1.0 - clamp(vMetallic, 0.0, 1.0) * 0.7);
  vec3 spec_color = mix(vec3(1.0), base, clamp(vMetallic, 0.0, 1.0));

  vec3 lit = diffuse_color * (diffuse + fill) + spec_color * spec + vEmissive;
  outColor = vec4(lit * vColor.a, vColor.a);
}
