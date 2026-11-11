// MagicBlocks —— Android 场景渲染器（JNI 层的 C++ 实现）
//
// 职责：把一个立方体渲染进**壳层给的 Surface**，全程零 CPU 回读。
//
// 与 cube_demo 的关键区别：
//   cube_demo：离屏目标 + readback_rgba8  → 有回读（那是 CI/golden 专用）
//   本文件  ：swapchain image + vkQueuePresentKHR → 无回读
//
// 生命周期由 Kotlin 侧驱动：
//   nativeCreate()             → 建设备/管线/缓冲
//   nativeSurfaceCreated(w)    → 建 swapchain（拿到 ANativeWindow*）
//   nativeRenderFrame(yaw,pitch,scale) → 取图 → 录制 → 提交 → 呈现
//   nativeSurfaceDestroyed()   → 拆 swapchain
//   nativeDestroy()            → 拆设备
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "magicblocks/device/render_device.h"
#include "magicblocks/device/swapchain.h"
#include "magicblocks/primitives/model_catalog.h"
#include "magicblocks/primitives/types.h"
#include "magicblocks/resources/buffer.h"
#include "magicblocks/resources/command.h"
#include "magicblocks/resources/pipeline.h"

namespace magicblocks::android {

namespace dev = magicblocks::device;
namespace res = magicblocks::resources;
namespace prim = magicblocks::primitives;

// 与 shaders/cube.vert 的 push_constant 块一一对应（128 字节 = 两个 mat4）。
struct PushConstants {
  float view_proj[16];  // offset 0
  float world[16];      // offset 64
};

// 一个面 4 个顶点，6 面共 24 个；每面独立颜色/法线 → 六面明暗不同。
struct Vertex {
  float px, py, pz;
  float nx, ny, nz;
  float r, g, b;
};

class SceneRenderer {
 public:
  SceneRenderer() = default;
  ~SceneRenderer();

  SceneRenderer(const SceneRenderer&) = delete;
  SceneRenderer& operator=(const SceneRenderer&) = delete;

  // 建设备与资源。device 用 SwiftShader 之外的**真实 GPU**（pad 上的 Mali）。
  bool init(std::string* err);

  // 壳层 Surface 就绪：建 swapchain。window 是 ANativeWindow*。
  bool attach_surface(void* window, int width, int height, std::string* err);
  void detach_surface();

  // 渲染一帧并直接呈现到 Surface。全程无 CPU 回读。
  //
  // 轴向约定（见 record_into 里的说明，别再改回去）：
  //   yaw   : 绕世界 Y 轴（视觉上的**左右**转）  ← 屏幕左右滑
  //   pitch : 绕世界 X 轴（视觉上的**上下**转，夹在 ±85°）← 屏幕上下滑
  // scale     : 缩放（双指捏合），本质是相机拉近拉远。
  // pan_x/y   : 平移（双指拖动），单位是**世界空间**的偏移量 ——
  //             由 Kotlin 侧按"屏幕像素 / 当前可视宽度"换算好再传进来，
  //             这样平移速度与缩放级别无关（贴着手指走）。
  bool render_frame(float yaw, float pitch, float scale, float pan_x, float pan_y,
                    std::string* err);

  bool surface_ready() const { return swapchain_ != nullptr; }

  // ---------------------------------------------------------------- 预设模型
  //
  // 场景里的物体默认是内置立方体；Kotlin 侧从列表里选中某个预设 OBJ 后，
  // 后续每一帧都画它（覆盖内置立方体）。"画哪一层"由这里决定，Kotlin 只给路径。

  // 切到 `path` 指定的 OBJ。成功返回 true；失败返回 false 且**保持原状**
  // （不静默切回立方体，也不留半成品网格）。`name` 只用于日志/状态文本。
  //
  // 必须在 UI 线程调用（与 render_frame 同一个线程）：内部要建 GPU 缓冲。
  bool load_model(const std::string& path, const std::string& name, std::string* err);

  // 切回内置立方体（释放已加载的模型）。
  void clear_model();

  bool has_model() const { return model_ != nullptr; }

  // MSAA 采样数（1=关闭）。必须在 attach_window **之前**设置 ——
  // 采样数决定交换链建几张什么规格的 MS 图，也决定管线按几采样建。
  // 设备不支持时不会失败，会退回 1x 并打日志说明原因。
  void set_msaa_samples(std::uint32_t n) { msaa_samples_ = n == 0 ? 1u : n; }
  std::uint32_t msaa_samples() const { return msaa_samples_; }
  // 实际生效的采样数（可能因设备不支持而低于请求值）。
  std::uint32_t effective_msaa_samples() const { return effective_msaa_samples_; }
  // 设备**报告支持**的采样数列表（升序，例如 {1,4,8}）。
  //
  // 为什么要问设备而不是在 UI 里写死 {1,4,8}：不同手机的 GPU 支持面差异很大
  // （有的只到 4x，有的到 16x），把 8x 写死在按钮上会出现"点了 8x 却悄悄退回
  // 4x"的假象 —— 这台 pad 上就真的验证过这种静默降级，所以必须由设备说了算。
  std::vector<std::uint32_t> supported_msaa_levels() const;

  // 调试信息（Kotlin 侧打日志用）
  std::string status_text() const;

 private:
  bool create_resources(std::string* err);
  void release_window();
  bool rebuild_pipeline_for_swapchain(std::string* err);
  bool record_into(dev::IRenderTarget* target, float yaw, float pitch, float scale, float pan_x,
                   float pan_y, std::string* err);

  std::unique_ptr<dev::IRenderDevice> device_;
  std::unique_ptr<dev::ISwapchain> swapchain_;
  std::uint32_t msaa_samples_ = 1;
  std::uint32_t effective_msaa_samples_ = 1;
  std::unique_ptr<res::IBuffer> vertex_buffer_;
  std::unique_ptr<res::IBuffer> index_buffer_;
  std::unique_ptr<res::IPipeline> pipeline_;
  std::uint64_t pipeline_hash_ = 0;
  std::uint32_t index_count_ = 0;

  // 当前选中的预设模型（null = 画内置立方体）。所有权归这里：加载成功才替换，
  // 失败时旧模型原样保留 —— 用户点了列表里一个坏文件，画面不该跟着变黑。
  std::unique_ptr<prim::ILoadedModel> model_;
  std::string model_name_;

  // ANativeWindow 引用：attach 时持有，detach 时释放（引用由 JNI 层的
  // ANativeWindow_fromSurface 产生，见 jni_bridge.cpp）。
  void* native_window_ = nullptr;

  int width_ = 0;
  int height_ = 0;

  // 屏幕相对 swapchain image 的旋转（度：0/90/180/270）。
  //
  // pad 是横屏应用但面板物理竖置，SurfaceFlinger 合成时把 image 旋转 90° 铺满屏幕。
  // 这带来两个后果，必须一起处理：
  //   1) 宽高比：屏幕上看到的是 image 高/宽，投影必须取倒数，否则正方体被拉成 1.5:1；
  //   2) 手势方向：手指在屏幕上左右滑，对应的其实是 image 空间里的上下，
  //      不补偿就会出现"左右拖像上下转"。
  // 取值来自 swapchain 的 VkSurfaceTransformFlagBitsKHR；取不到时为 0。
  std::uint32_t pre_transform_ = 0;
  int rotation_deg_ = 0;

  std::uint64_t frame_count_ = 0;
  std::string last_error_;
  std::mutex mutex_;  // Kotlin 的 UI 线程只有一条，加锁是为了防 life-cycle 竞态
};

}  // namespace magicblocks::android
