#version 450
// MagicBlocks L3 —— 方向背景(sky)顶点着色器
//
// 用途:画"只随相机**朝向**变化、不随平移变化"的背景。几何是一个铺满屏幕的
// 全屏三角形(3 顶点、无顶点缓冲),每个像素的方向由片元着色器用逆视图投影
// 反算 —— 它只含旋转信息,于是相机平移时画面**逐像素不变**。
//
// 深度:z_ndc = 1.0(near=0 → far=1 的 Vulkan 约定,即最远),同时
// `gl_Position.xy = z_ndc * pos.xy` 把三角形**压到 z=1 平面上**(w 正好等于 z),
//   这样即使在 z∈[0,1] 的 Vulkan 裁剪空间里也不会被 near 平面裁掉。
//   场景画在它之后:本 pass 关闭深度写,场景凡是覆到的像素都会覆盖它。
//
// 确定性:不使用时间/抖动/随机,同一相机 + 同一贴图 → 逐字节相同的输出。
// G1:只有 VS/FS 两个 stage,没有几何着色器。

layout(location = 0) out vec2 vNdc;

void main() {
  // 全屏三角形:覆盖 [-1,1]^2 的超集。index = 0..2
  //   (-1,-1) (3,-1) (-1,3)
  // 注意 Vulkan 的 +y 朝下(像素坐标原点左上),三角形朝向由管线
  // cull=None 保证不受影响(见 pipeline: CullMode::None)。
  vec2 pos = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2) * 2.0 - 1.0;
  vNdc = pos;
  gl_Position = vec4(pos, 1.0, 1.0);
}
