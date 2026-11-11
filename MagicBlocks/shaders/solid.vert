#version 450
// MagicBlocks L3 验证工具 —— 不透明实体的顶点着色器(仅 sky_cli 用)
//
// 用途:在天空盒之上画几块**不透明实体**,用来证明"天空不写深度、实体能盖住它"。
// 这不是任何一层的 SDK 能力,只是一块可被判读的对照几何 —— 因此刻意极简:
//   binding 0(逐顶点):location 0 = vec3 position,location 1 = vec4 color
//
// 变换 = 相机 view_proj(push constant),与 L3 场景管线用**同一个**矩阵来源,
// 保证"实体和天空对齐的是同一台相机"。

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec4 inColor;

layout(location = 0) out vec4 vColor;

layout(push_constant) uniform PushConstants {
  mat4 view_proj;
} pc;

void main() {
  vColor = inColor;
  gl_Position = pc.view_proj * vec4(inPosition, 1.0);
}
