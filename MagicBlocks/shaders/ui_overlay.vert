#version 450
// MagicBlocks UI 消费层 —— 覆盖层实例化顶点着色器(spec §8 UI 画进 Vulkan)
//
// G2/C4:禁止几何着色器。UI 图元(矩形/描边/线段)全部走**实例化绘制**:
//   - 程序化生成一个单位四边形(gl_VertexIndex 0..3),
//   - 每个实例带一个矩形(像素空间 left/top/right/bottom)+ 颜色,
//   - 顶点着色器把像素矩形映射到 NDC。
//
// 屏幕坐标约定(与 IRenderTarget::readback_rgba8 的行序契约一致):
//   像素 (0,0) = 左上;NDC y = +1 对应图像第 0 行,因此 y 需要翻一次。
//   只在这一处翻,别处不再翻 —— 否则会重演 L0 踩过的"双翻行"坑。
//
// 实例数据走 **storage buffer**(SSBO),每帧一次绑定:
//   instance = { vec4 rect(x0,y0,x1,y1); vec4 color; }
// 不读顶点缓冲、不用描述符更新,稳态帧 0 次分配。
#extension GL_ARB_separate_shader_objects : enable

layout(location = 0) out vec4 vColor;

layout(std430, binding = 0) readonly buffer UiInstances {
  vec4 kInstances[];  // 每实例 2 个 vec4:rect,color
};

// 屏幕尺寸(像素),由 push constant 传入(避免 UBO 与描述符更新)
layout(push_constant) uniform UiPush {
  vec2 viewport;
} pc;

// 单位四边形(TRIANGLE_STRIP):(0,0) (1,0) (0,1) (1,1)
const vec2 kCorners[4] = vec2[4](
  vec2(0.0, 0.0), vec2(1.0, 0.0),
  vec2(0.0, 1.0), vec2(1.0, 1.0)
);

void main() {
  uint base = uint(gl_InstanceIndex) * 2u;
  vec4 rect = kInstances[base];
  vec4 color = kInstances[base + 1u];

  vec2 c = kCorners[gl_VertexIndex];
  vec2 px = mix(rect.xy, rect.zw, c);          // 像素空间
  vec2 ndc = vec2(px.x / pc.viewport.x * 2.0 - 1.0,
                  1.0 - px.y / pc.viewport.y * 2.0);  // 唯一一次 y 翻转
  gl_Position = vec4(ndc, 0.0, 1.0);
  vColor = color;
}
