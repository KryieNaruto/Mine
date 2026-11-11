// MagicBlocks L3 primitives —— 方向背景(sky)通道
//
// ============================================================================
// 它是什么 / 为什么在 L3
// ============================================================================
//
// 「只随相机**朝向**变化、不随平移变化的着色背景」是渲染器的通用原语(和
// 相机/网格/材质同一级别),不是某个玩法的内容 —— 所以它住 L3,由上层
// (UI 离屏合成 / 未来的场景渲染图)自己决定要不要用、贴哪组图。
// **L0–L2 不引用本头**(G3/G7 的门控口径不变:本文件里没有任何玩法名)。
//
// 硬件形态由 L1 的立方体贴图承担(resources/texture.h 的 ICubeMap):
// 六张同尺寸 2D 面 = 一个 6 层的 cube 视图,片元着色器用 samplerCube 采样。
//
// ============================================================================
// 绘制契约(每条都要能在像素上验出来)
// ============================================================================
//
//   1. **先画**:调用方在最前(清屏之后、场景之前)调 record();本体只在
//      `desc.clear_color` 为真时清屏,并把清屏色记为"它认得的底色"。
//   2. **不写深度**:管线 depthTest=false / depthWrite=false —— 场景任意几何都
//      覆盖天空,天空自己不会遮挡任何东西。
//   3. **方向只含旋转**:片元用 `inv_proj_rot * vec4(ndc, 1, 1)` 求方向,矩阵
//      由 CPU 按当前相机算成「逆投影 × 视图旋转的逆」(**不含平移**)。
//      于是:相机**旋转** → 画面变;相机**平移** → 画面逐字节不变。
//   4. **确定性**:不读时间/不抖动/无随机;同一相机 + 同一贴图两次录制 →
//      命令流与输出逐字节相同。
//   5. **失败即报错**:贴图缺失 / 面未上传 / 视图投影矩阵非法 → 返回带原因的
//      Status(NotFound / InvalidArgument / Unsupported),**绝不**退化成纯色清屏
//      —— 那会让"天空盒没加载"看起来像"天空是纯色"。
//
// ============================================================================
// 典型用法
// ============================================================================
//
//   auto tex = create_cube_map_from_faces(device, asset_root + "/cubeMap", "qwantani_dusk_2_puresky_1k");
//   auto sky = create_sky_pass(*device);
//   SkyPassDesc d; d.target_shape = ...; d.cube = tex.value().get();
//   sky->prepare(d);
//   sky->record(*cmd, camera, {.clear_color=false});   // 与场景同一 pass
//   cmd->end_pass();
//   device->submit_command_list(*cmd);
#if !defined(NOMINMAX)
#define NOMINMAX
#endif

#pragma once

#include <memory>
#include <string>

#include "mb_internal/magicblocks/device/render_device.h"
#include "mb_internal/magicblocks/primitives/camera.h"
#include "mb_internal/magicblocks/primitives/types.h"
#include "mb_internal/magicblocks/resources/buffer.h"
#include "mb_internal/magicblocks/resources/command.h"
#include "mb_internal/magicblocks/resources/pipeline.h"
#include "mb_internal/magicblocks/resources/texture.h"

namespace magicblocks::primitives {

using device::Status;
using device::u32;
using device::u64;

// ---------------------------------------------------------------- 面文件命名

// 六张面文件的后缀(与资源目录里的命名约定一致,**不做任何亮度/朝向猜测**):
//   <name>_right.png  → +X    <name>_left.png   → -X
//   <name>_top.png    → +Y    <name>_bottom.png → -Y
//   <name>_front.png  → +Z    <name>_back.png   → -Z
// 该映射与参考着色器 Function.slang 的 CubeUVToDir 逐项一致(面序 = 硬件层序)。
const char* cube_face_suffix(resources::CubeFace face);

// 一个场景的六面文件路径(按 CubeFace 顺序)。dir 是 cubeMap 目录,base 是场景名
// (例如 "qwantani_dusk_2_puresky_1k",不含后缀)。
struct CubeFacePaths {
  std::string dir;
  std::string base;
  std::string path(resources::CubeFace face) const;
};

// 六面 PNG → L1 立方体贴图(RGBA8_UNORM,1024² 量级,mip_levels = 1)。
//
// 行为约定(失败即报错,不降级):
//   - 任一面缺失 → NotFound,message 带**期望的绝对路径**;
//   - 六面尺寸/格式不一致 → InvalidArgument(带每面的实际值);
//   - 不是 8bit RGBA/RGB PNG → Unsupported(带读图器的原始错误);
//   - 任一 upload_face / flush 失败 → 原样透传该错误。
// `read_png_rgba8` 是引擎自带的零依赖解码器(src/core/image_io.h)。
Result<std::unique_ptr<resources::ICubeMap>> create_cube_map_from_faces(
    device::IRenderDevice& device, const std::string& cube_map_dir, const std::string& base_name);

// 同上,但从内存里的六张 RGBA8 面建(便于单测与"不碰文件系统"的调用方)。
// faces[i] 对应 CubeFace 的顺序,必须是 width*height*4 字节。
Result<std::unique_ptr<resources::ICubeMap>> create_cube_map_from_pixels(
    device::IRenderDevice& device, u32 width, u32 height,
    const std::uint8_t* const faces[resources::kCubeFaceCount]);

// ---------------------------------------------------------------- 相机 → 天空矩阵

// 「逆投影 × 视图旋转的逆」:把 NDC(xy ∈ [-1,1], z=1)映射到世界**方向**。
//
// 关键点:视图矩阵的平移被**整列丢掉**再重新正交化 —— 逆矩阵只承担旋转,
// 因此结果对相机位置不敏感。这正是"天空盒随旋转、不随平移"的数学来源。
// 失败(view/proj 不可逆,或相机 up 与视线共线)→ 返回 false 且不写 out。
bool make_sky_view_rotation_inverse(const CameraSnapshot& camera, Mat4* out);

// 同上,但从已算好的矩阵来(调用方有自己的投影实现时用)。
bool make_sky_view_rotation_inverse(const Mat4& view, const Mat4& proj, Mat4* out);

// ---------------------------------------------------------------- 通道描述

// 渲染目标形状:管线与 pass 必须逐项兼容(颜色格式 / 有无深度 / 采样数),
// 与 L0 的 IRenderTarget 一一对应。
struct SkyTargetShape {
  device::Format color_format = device::Format::RGBA8_UNORM;
  bool has_depth = false;
  u32 samples = 1;
};

struct SkyPassDesc {
  SkyTargetShape target_shape{};
  // 立方体贴图(**不转移所有权**;nullptr → prepare 返回 InvalidArgument)。
  const resources::ICubeMap* cube = nullptr;
  // 采样方式:天空盒要求 Linear + ClampToEdge(面内回绕会把边缘拉成条纹)。
  resources::SamplerDesc sampler{};
};

// 一次 record() 的选项。
struct SkyRecordOptions {
  // true(默认)→ 本通道自己清屏;false → 与场景共用一个 pass,由场景清屏。
  bool clear_color = true;
  // clear_color=true 时使用的清屏色(alpha 也清,离屏目标需要确定的 alpha)。
  float clear_color_rgba[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  bool clear_depth = true;
  float clear_depth_value = 1.0f;
  // 只画目标的一个矩形(默认整图;与 RenderPassDesc 的 area 语义一致)。
  u32 area_x = 0;
  u32 area_y = 0;
  u32 area_width = 0;   // 0 = 整宽
  u32 area_height = 0;  // 0 = 整高
};

// 录制统计 —— 用于断言"这一通道没有偷偷多发 draw / 多开 pass"。
struct SkyRecordStats {
  u32 draws = 0;
  u32 pipeline_binds = 0;
  u32 passes = 0;          // 本通道自己开的 pass 数(共用场景 pass 时为 0)
  u32 texture_binds = 0;
  u32 push_constant_sets = 0;
  // 实际用到的视图旋转逆矩阵(列主序 16 个 float),便于在测试里直接比对。
  float inv_proj_rot[16] = {};
};

class ISkyPass {
 public:
  ISkyPass() = default;
  ISkyPass(const ISkyPass&) = delete;
  ISkyPass& operator=(const ISkyPass&) = delete;
  virtual ~ISkyPass() = default;

  // 建管线/采样器(幂等:同 desc 重复调用不重建)。
  //   cube 为空 / 面未上传 → InvalidArgument / Unsupported(明确原因)。
  virtual Status prepare(const SkyPassDesc& desc) = 0;

  // 本通道要画到哪个目标。**默认 nullptr**:
  //   - record() 里 clear_color=false 且 clear_depth=false(与场景共用 pass)时不需要它;
  //   - 需要开 pass 却没设目标 → InvalidArgument(明确报错,不静默画到别处)。
  // 目标生命周期由调用方持有(本对象不拥有、不销毁)。
  virtual void set_target(device::IRenderTarget* target) = 0;

  // 录制:按 options 开一个 pass(需要时)、绑管线/贴图、推矩阵、一次 draw(3 顶点)。
  // **不** end_pass —— 由调用方决定这一通道是独立 pass 还是与场景共用。
  virtual Status record(resources::ICommandList& cmd, const CameraSnapshot& camera,
                        const SkyRecordOptions& options) = 0;

  // 半分辨率等降级档位:0 = 全分辨率(默认);n>0 时按 2^n 缩小。本轮只支持 0 ——
  // 非 0 明确返回 Unsupported(不做"看起来能用"的伪降级)。
  virtual Status set_downscale(u32 level) = 0;

  virtual const SkyPassDesc& desc() const = 0;
  virtual const SkyRecordStats& stats() const = 0;
  virtual void reset_stats() = 0;
};

std::unique_ptr<ISkyPass> create_sky_pass(device::IRenderDevice& device);

}  // namespace magicblocks::primitives
