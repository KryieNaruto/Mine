#version 450
// 笔画顶点着色器:输入归一化 [0,1] 坐标点,经 push constant 缩放/平移后映射到裁剪空间。
layout(location = 0) in vec2 inPos;

layout(push_constant) uniform PushConsts {
  vec2 uScale;   // 坐标 → 归一化 缩放(两个分量通常相同)
  vec2 uOffset;  // 归一化平移
} pc;

void main() {
  vec2 p = inPos * pc.uScale + pc.uOffset;
  // 输入按图像/窗口坐标(原点左上,y 向下):p.y=0 在图像顶部。
  // Vulkan 视口变换把 NDC y=-1 映到 framebuffer 顶部行,与 OpenGL 相反;
  // 故此处不再翻转,直接 p*2-1 即可让 y=0 → 顶部。
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
