#version 450
// MagicBlocks L3 —— 画笔条带片段着色器
//
// 只做一件事：输出顶点色。笔迹是纯色（StrokeDesc::color）的带 alpha 的条带，
// 混合由管线的 blend state 负责（预乘 alpha，见 primitive_renderer 的管线描述）。
//
// 为什么不做抗锯齿：
//   绘制 MSAA 条带需要额外采样或几何加宽，而画笔在移动端首先要保证的是
//   "跟手"——延迟比边缘质量重要。边缘质量留给上层决定（L4 可以换成
//   带 signed-distance 的版本）。

layout(location = 0) in vec4 vColor;

layout(location = 0) out vec4 outColor;

void main() {
  outColor = vColor;
}
