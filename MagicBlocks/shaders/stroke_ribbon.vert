#version 450
// MagicBlocks L3 —— 画笔条带顶点着色器（spec §6.2.1）
//
// 输入是 **屏幕像素坐标**（原点左上、y 向下，与 InputEvent 同口径，D-L3-2）。
//
// ============================ Y 到底翻不翻（重要） ============================
//
// 引擎的**唯一约定**（见 ui_overlay.vert 与 IRenderTarget::readback_rgba8 行序契约）：
//
//     像素 (0,0) = 图像左上角
//     NDC y = +1 对应图像第 0 行
//
// 因此像素 → NDC 必须翻一次 y：
//
//     NDC.x =  px.x / width  * 2 - 1
//     NDC.y = 1 - py.y / height * 2      ← 翻转
//
// 这里**只能翻一次**。曾经写成 `py/height*2-1`（不翻）的理由是"Vulkan NDC 的 y
// 朝下、像素 y 也朝下，方向一致"，这个推理是错的：Vulkan 视口的 y 轴虽然朝下，
// 但 NDC 的 +1 仍映射到视口 **下边缘**（VkViewport height > 0 时），而我们的
// 行序契约要求 +1 落在第 0 行（上边缘）。两次"看起来朝下"的方向叠加后是**反的**，
// 表现为整幅画跑到屏幕外/上下颠倒 —— 而由于清屏色正常，看上去就像"一笔都没画"。
//
// C4/G1：禁止几何着色器。条带在 CPU 侧（IPainter）展开成三角形列表，
// 这里只做坐标变换 —— 顶点数由 IPainter 决定，着色器不造几何。

layout(location = 0) in vec2 inPosition;   // 像素坐标，原点左上
layout(location = 1) in vec4 inColor;      // RGBA8 归一化

layout(location = 0) out vec4 vColor;

layout(push_constant) uniform PushConstants {
  vec2 viewport_size;   // 像素宽高
  vec2 pad;
} pc;

void main() {
  vec2 ndc = vec2(inPosition.x / pc.viewport_size.x * 2.0 - 1.0,
                  1.0 - inPosition.y / pc.viewport_size.y * 2.0);

  // 笔迹永远画在最前面：z = 0 且在管线里已关闭深度测试/写入。
  gl_Position = vec4(ndc, 0.0, 1.0);
  vColor = inColor;
}
