#version 450
// MagicBlocks L0 —— 实例化自检画面的片元着色器
//
// 输出:实例色 + 轻微 UV 渐变。左上角第一个实例(index 0)刻意画成
// 高饱和红,作为「通道序断言」的标定点(RGBA/BGRA 互换时该点会变蓝)。

layout(location = 0) in vec3 vColor;
layout(location = 1) in vec2 vUv;
layout(location = 2) flat in uint vInstance;

layout(location = 0) out vec4 outColor;

void main() {
  if (vInstance == 0u) {
    // 标定点:纯红(R=1,G≈0,B=0)
    outColor = vec4(1.0, 0.0, 0.0, 1.0);
    return;
  }
  vec3 c = vColor * (0.85 + 0.15 * vUv.y);
  outColor = vec4(c, 1.0);
}
