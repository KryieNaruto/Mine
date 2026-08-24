#version 450
// 笔画片元着色器:单色(红色)描边。
layout(location = 0) out vec4 outColor;

void main() {
  outColor = vec4(1.0, 0.0, 0.0, 1.0);  // 红色
}
