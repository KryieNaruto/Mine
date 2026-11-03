#version 450
// MagicBlocks L2 —— 提交队列可视化片元着色器
//
// 输出:实例色 + 轻微 UV 渐变。左上角第一个实例(index 0)刻意画成
// 高饱和绿,作为「通道序 + L2 golden 身份」的标定点。
// L0 的 golden 用纯红标定点,L2 用纯绿 —— 两套基线可以**独立判定**
// (plans-l2 §6.5:把 golden_l2.png 换成 golden_l0.png 时,断言必须失败)。

layout(location = 0) in vec3 vColor;
layout(location = 1) in vec2 vUv;
layout(location = 2) flat in uint vInstance;

layout(location = 0) out vec4 outColor;

void main() {
  if (vInstance == 0u) {
    // 标定点:纯绿(R≈0,G=1,B≈0)
    outColor = vec4(0.0, 1.0, 0.0, 1.0);
    return;
  }
  vec3 c = vColor * (0.85 + 0.15 * vUv.y);
  outColor = vec4(c, 1.0);
}
