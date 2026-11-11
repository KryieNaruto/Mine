#version 450
// MagicBlocks L3 —— 方向背景(sky)片元着色器
//
// 输入:立方体贴图(6 面 2D 纹理的硬件立方体视图,samplerCube)。
// 方向 = 逆视图投影把该像素反算到世界空间,再减相机位置 —— 但**只取旋转**:
// 逆图投影(逆投影 × 逆视图旋转)由 CPU 按当前相机算好推上来,
// 因此相机平移不会改变任何像素(这正是 sky 的正确性判据)。
//
// 采样:`texture()` 让硬件按立方体贴图规则选面 + 面内双线性插值(采样器 Linear /
// ClampToEdge)。贴图已是 tonemapped 的 8bit,直接输出,不做 HDR 处理。
//
// G1:只有 VS/FS,无几何着色器;不使用导数/时间/随机 → 输出确定。

layout(location = 0) in vec2 vNdc;

layout(location = 0) out vec4 outColor;

// set 0 / binding 0:立方体贴图(P0 采样纹理通路,slot 0)。
layout(set = 0, binding = 0) uniform samplerCube uSkyCube;

// push constant:逆图投影(裁到面的 4x4 矩阵块,列主序)
layout(push_constant) uniform PushConstants {
  mat4 inv_proj_rot;  // 逆投影 × 视图旋转的逆(不含平移)
} pc;

void main() {
  // NDC → 世界方向。z 取 1(far 平面上的点),w = 1 → 得到的方向只依赖像素位置,
  // 与相机位置无关。
  vec4 far_point = pc.inv_proj_rot * vec4(vNdc, 1.0, 1.0);
  vec3 dir = normalize(far_point.xyz / far_point.w);
  outColor = vec4(texture(uSkyCube, dir).rgb, 1.0);
}
