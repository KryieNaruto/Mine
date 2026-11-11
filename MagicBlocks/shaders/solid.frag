#version 450
// MagicBlocks L3 验证工具 —— 不透明实体的片元着色器(仅 sky_cli 用)
// 纯顶点色输出,不做光照/混合/时间相关任何运算 → 输出确定。
// G1:只有 VS/FS 两个 stage。

layout(location = 0) in vec4 vColor;
layout(location = 0) out vec4 outColor;

void main() { outColor = vColor; }
