#version 450
// MagicBlocks L3 —— 像素级回归测试专用顶点着色器
//
// 只做一件事：像素坐标 → NDC，然后用固定颜色填满。
// 刻意极简（无常量、无实例、无光照），这样这条测试失败时，
// 可疑范围只剩下「上传 / 提交 / render pass 兼容性」，
// 不会和 L3 业务逻辑的任何细节混在一起。
//
// 屏幕坐标约定（全引擎唯一）：
//   像素 (0,0) = 左上；NDC y = +1 对应图像第 0 行 → y 必须翻一次。
// 视口尺寸通过 hardcode 的 uniform 传入不合适，所以这里用
// `inPosition` 直接给出 **归一化设备坐标**，绕开像素→NDC 的换算，
// 让这条测试只验证"命令提交链路"，不验证坐标系（坐标系由 L2 golden 覆盖）。

layout(location = 0) in vec2 inNdc;

void main() { gl_Position = vec4(inNdc, 0.0, 1.0); }
