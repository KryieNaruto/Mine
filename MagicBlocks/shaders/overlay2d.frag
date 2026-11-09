#version 450
// MagicBlocks L3 —— IOverlay2D 片段着色器（spec §13.13）
//
// 覆盖层是**纯色**的：线、框、环、三角都不需要光照或纹理。
// 颜色直接来自顶点属性，因此多色覆盖层依然是**一个 draw**。

layout(location = 0) in vec4 vColor;
layout(location = 0) out vec4 outColor;

void main() {
  // 预乘 alpha：与引擎其余部分的混合约定一致
  outColor = vec4(vColor.rgb * vColor.a, vColor.a);
}
