#version 450
// MagicBlocks UI 消费层 —— 覆盖层片元着色器
//
// 只做 alpha 混合,不做任何采样(spec §8:本轮无纹理/字体图集;
// 图集落地后在 IOverlay2D::blit 里扩展,不在这里加分支)。
// 输出**非预乘** alpha,由 blend state 的 SRC_ALPHA / ONE_MINUS_SRC_ALPHA 混合。
#extension GL_ARB_separate_shader_objects : enable

layout(location = 0) in vec4 vColor;
layout(location = 0) out vec4 outColor;

void main() { outColor = vColor; }
