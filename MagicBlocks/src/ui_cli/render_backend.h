// MagicBlocks UI 消费层 —— CLI 侧的"缩略图 + 天空盒 + 场景"渲染后端(UI 布局 v2)
//
// ============================================================================
// 这个文件为什么在那里
// ============================================================================
//
// UI 库本体(`src/ui/**`、`include/magicblocks/ui/**`)**不许**出现 `Vk*`(G3)、
// **不许**做 CPU 回读(UIG1),也不该认识"模型/天空盒"这些渲染概念 —— 它只认
// `ListItem` 与 `Control`。所以 v2 的全部渲染接线都放在 **CLI 侧**(本文件),
// 由 CLI 这个"外部装配方"去调 L3 的 `model_catalog` / `sky` 接口。
//
// UI 层只拿到两样东西:
//   1. 条目文案(模型名,由本文件从 catalog 扫出来再喂给 `SidebarConfig`);
//   2. 一个**已渲染好的缩略图 RGBA8 缓冲**,由本文件读回后交给 CLI 合成。
//
// ============================================================================
// 为什么缩略图是"CPU 合成"而不是往 overlay 里塞纹理
// ============================================================================
//
// `IOverlay2D::blit()` 存在,但 `TextureHandle` 目前**只有 `kNoTexture`**,
// 纹理图集还没落地(spec §9)。若强行给 overlay 塞一张"离屏纹理",就得在
// backend 里新增纹理绑定路径 —— 那是 skybox 的文件,不该由 UI 任务改。
//
// 所以走的是**离屏路径**(spec §5.5:golden/录制专用,唯一允许 CPU 回读的口子):
//   渲染缩略图 → `readback_rgba8` → 按条目区域贴到最终帧的像素缓冲上。
// 这条口子在 `src/ui/scene_offscreen.cpp` 已经是既有做法(UIG1 的豁免路径
// 就叫 `offscreen`),本文件沿用同一口径。
//
// ============================================================================
// v2 资源纪律:一次装配、多次使用
// ============================================================================
//
// 缩略图与场景渲染共用同一条实例化 3D 管线、同一张实例属性缓冲、同一个相机。
// 这不是"省事":分配器**不自动扩页**(`reserve_pages` 之后桶里才有槽位),
// 每个缩略图各建一条管线/一张缓冲会在第 5 张模型上撞 64 KiB 槽位耗尽 —— 实测
// 见本任务报告。因此所有 GPU 资源都由 `SceneResources::create()` 在装配期建一次。
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "magicblocks/device/render_device.h"
#include "magicblocks/primitives/camera.h"
#include "magicblocks/primitives/mesh.h"
#include "magicblocks/primitives/sky.h"
#include "magicblocks/resources/buffer.h"
#include "magicblocks/resources/command.h"
#include "magicblocks/resources/pipeline.h"
#include "magicblocks/resources/texture.h"

namespace magicblocks::ui_cli {

// 一张已渲染好的缩略图(RGBA8,行主序,无 padding)。
struct Thumbnail {
  std::string name;   // 模型名(与 UI 条目文案同源,便于对齐)
  std::string path;   // 源文件路径(诊断)
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::vector<std::uint8_t> rgba;
  // 渲染失败的条目**照样保留**(带错误原因):列表要显示"这个模型用不了",
  // 而不是让它凭空消失(catalog 的纪律,见 model_catalog.h 第 2 条)。
  bool ok = false;
  std::string error;

  bool usable() const {
    return ok && width > 0 && height > 0 &&
           rgba.size() == static_cast<std::size_t>(width) * height * 4u;
  }
};

// 缩略图渲染配置。
struct ThumbnailConfig {
  std::uint32_t width = 48;
  std::uint32_t height = 36;
  // 归一化后的包围盒对角线(与 model_catalog 的 ModelNormalize 一致)
  float target_diagonal = 1.0f;
  // 取景留边倍数(>1)。越小模型越满。
  float framing_margin = 1.15f;
  // 背景清屏色(与 UI 调色板 kPanelFill 接近,让缩略图"融进"条目)
  std::uint8_t clear_rgb[3] = {32, 36, 44};
  // 固定视角的方位角/仰角(度)。不读时钟 —— 缩略图必须逐字节可复现。
  float yaw_degrees = 24.f;
  float pitch_degrees = 18.f;
};

// 场景渲染配置(左列表选中模型的预览大图)。
struct SceneRenderConfig {
  std::uint32_t width = 480;
  std::uint32_t height = 360;
  // 画面区域:只有这块矩形被写(其余像素保持调用方传入的内容)。
  // UI 层把这块让出来给场景,两侧列表仍然画在自己的区域上。
  std::uint32_t area_x = 0;
  std::uint32_t area_y = 0;
  std::uint32_t area_width = 0;   // 0 = 整宽
  std::uint32_t area_height = 0;  // 0 = 整高
  float target_diagonal = 1.0f;
  float framing_margin = 1.15f;
  float yaw_degrees = 24.f;
  float pitch_degrees = 18.f;
  // 没有天空盒时的清屏色(app 背景色)。
  std::uint8_t clear_rgb[3] = {16, 18, 22};
  // 天空盒是否参与(由 SkySetup 成功与否决定)
  bool draw_sky = false;
};

// ------------------------------------------------------------------ 场景资源
//
// 装配期建一次:实例化管线(带深度)+ 实例属性缓冲 + 相机。
// 失败时 `ok=false` 并带原因,不产出半成品。
class SceneResources {
 public:
  SceneResources();
  ~SceneResources();
  SceneResources(SceneResources&&) noexcept;
  SceneResources& operator=(SceneResources&&) noexcept;
  SceneResources(const SceneResources&) = delete;
  SceneResources& operator=(const SceneResources&) = delete;

  // 建管线与实例缓冲。`has_depth` 决定管线是否声明深度附件:
  // 缩略图与场景目标都带深度,所以传 true(与 model_cli 同口径)。
  static std::unique_ptr<SceneResources> create(magicblocks::device::IRenderDevice& device,
                                                std::uint32_t samples, bool has_depth);

  bool ok() const { return ok_; }
  const std::string& error() const { return error_; }

  magicblocks::resources::IPipeline* pipeline() const { return pipeline_; }
  magicblocks::resources::IBuffer* instance_buffer() const { return instance_buffer_; }

 private:
  bool ok_ = false;
  std::string error_;
  magicblocks::resources::IPipeline* pipeline_ = nullptr;
  magicblocks::resources::IBuffer* instance_buffer_ = nullptr;

  struct Owned;
  std::unique_ptr<Owned> owned_;
};

// 把一个模型渲染成缩略图。**任何一步失败都不崩**:返回 ok=false 并带原因。
//
// 内部自建离屏 target(带深度),一帧一次 `submit_command_list`(UIG2)。
// 模型加载/上传同样在这里(每次调用独立,失败不影响别的条目)。
Thumbnail render_thumbnail(magicblocks::device::IRenderDevice& device,
                           SceneResources& res, const std::string& path,
                           const std::string& name, const ThumbnailConfig& cfg);

// 批量渲染一组模型(名字与路径成对给出,长度取较短的那个)。
// 顺序与传入顺序一致 —— UI 列表顺序必须稳定(golden 才能复现)。
std::vector<Thumbnail> render_thumbnails(magicblocks::device::IRenderDevice& device,
                                         SceneResources& res,
                                         const std::vector<std::string>& names,
                                         const std::vector<std::string>& paths,
                                         const ThumbnailConfig& cfg);

// 把场景(天空盒 + 选中模型)渲染进 `pixels` 的指定区域。
//
// 只在 `cfg.area_*` 这块矩形里写像素(区域外不动),调用方可以先把 UI 画好
// 再调它 —— 顺序反过来也行(区域互不重叠)。像素格式 RGBA8 行主序。
//
// `sky_cube` 是天空盒的立方体贴图(setup_sky() 建好的那份,所有权仍在调用方)。
// 必须显式传:天空管线要在**本次真实目标形状**下建立,那一刻才知道"有没有深度",
// 所以 setup_sky() 刻意不提前 prepare。
bool render_scene_area(magicblocks::device::IRenderDevice& device, SceneResources& res,
                       magicblocks::primitives::ISkyPass* sky,
                       const magicblocks::resources::ICubeMap* sky_cube,
                       const std::string& model_path, const std::string& model_name,
                       const SceneRenderConfig& cfg, std::uint8_t* pixels,
                       std::size_t row_pitch_bytes, std::string* error);

// ---------------------------------------------------------------- 天空盒

// 天空盒装配结果。**失败时明确说明原因**,绝不退化成纯色(那是 sky.h 契约 5)。
struct SkySetup {
  std::unique_ptr<magicblocks::resources::ICubeMap> cube;
  std::unique_ptr<magicblocks::primitives::ISkyPass> pass;
  bool ok = false;
  std::string error;
  std::string scene;  // 实际用的场景名(诊断/报告用)
};

// 从 `asset_root/cubeMap/<scene>_*.png` 建立方体贴图 + 天空通道。
//
// 资源缺失时返回 ok=false 且 error 写明路径 —— CLI 据此 **exit 2**,
// 不返回"一个空天空"让调用方以为天空关了。
SkySetup setup_sky(magicblocks::device::IRenderDevice& device,
                   const std::string& asset_root, const std::string& scene);

}  // namespace magicblocks::ui_cli
