#version 450
// MagicBlocks —— 立方体 demo 顶点着色器
//
// 顶点布局(与 src/cube_demo/main.cpp 的 Vertex 结构逐字节对应):
//   location 0 : vec3 位置
//   location 1 : vec3 法线
//   location 2 : vec3 颜色
//
// push constant 共 128 字节(设备上限,SwiftShader 也只给 128):
//   offset 0  : mat4 view_proj
//   offset 64 : mat4 world

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inColor;

layout(location = 0) out vec3 vColor;
layout(location = 1) out vec3 vNormal;

layout(push_constant) uniform Push {
  mat4 view_proj;
  mat4 world;
} pc;

void main() {
  vec4 world_pos = pc.world * vec4(inPosition, 1.0);
  gl_Position = pc.view_proj * world_pos;

  // 只有旋转/等比缩放时可直接用 mat3(world) 变换法线;
  // 本 demo 不做非等比缩放,故不需要逆转置矩阵。
  vNormal = mat3(pc.world) * inNormal;
  vColor = inColor;
}
