#version 450
// MagicBlocks —— 立方体 demo 片元着色器
//
// 一个半兰伯特 + 环境光,足以让立方体的 6 个面有明确的明暗差异 ——
// 人眼(以及像素统计)能立刻分辨"这是立体的",而不是一张平面色块。

layout(location = 0) in vec3 vColor;
layout(location = 1) in vec3 vNormal;

layout(location = 0) out vec4 outColor;

void main() {
  // 固定方向光(世界空间),不随时间变化 → 渲染结果可复现。
  const vec3 kLightDir = normalize(vec3(0.45, 0.80, 0.40));

  float ndotl = max(dot(normalize(vNormal), kLightDir), 0.0);
  // 半兰伯特:让暗面也有层次,避免纯黑。
  float diffuse = ndotl * 0.75 + 0.25;

  outColor = vec4(vColor * diffuse, 1.0);
}
