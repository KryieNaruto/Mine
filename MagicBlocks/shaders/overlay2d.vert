#version 450
// MagicBlocks L3 —— IOverlay2D 顶点着色器（spec §13.13）
//
// 输入是**像素坐标**（原点左上，+y 向下），这里一步转到 NDC。
//
// 为什么在 shader 里转而不是 CPU 里转：
//   视口尺寸会变（窗口 resize），如果 CPU 侧就把像素转成 NDC，
//   每次 resize 都得重建全部覆盖层几何。放 shader 里则只需改一个
//   push constant，几何完全不用动。
//
// 屏幕坐标约定（与 IRenderTarget::readback_rgba8 的行序契约一致）：
//   像素 (0,0) = 左上；NDC y = +1 对应图像第 0 行，所以 y 必须翻一次。
//   只在这一处翻 —— 别处再翻就是"双翻行"，L0 踩过这个坑。

layout(location = 0) in vec2 inPosition;  // 像素坐标
layout(location = 1) in vec4 inColor;     // RGBA8 归一化

layout(location = 0) out vec4 vColor;

// push constant：只有视口尺寸（8 字节，远低于 128 上限）
layout(push_constant) uniform OverlayPush {
  vec2 viewport_size;
} pc;

void main() {
  // 像素 → NDC：x 线性映射到 [-1,1]；y **翻转**（像素 y 向下，NDC y 向上）
  vec2 ndc = vec2(inPosition.x / pc.viewport_size.x * 2.0 - 1.0,
                  1.0 - inPosition.y / pc.viewport_size.y * 2.0);
  vColor = inColor;
  gl_Position = vec4(ndc, 0.0, 1.0);
}
