// MagicBlocks —— 场景渲染器的**薄壳头**(PIMPL)
//
// ============================================================================
// 这个文件里为什么一个 SDK 头都没有
// ============================================================================
//
// 2026-11-11 t3 之前,这个头 include 了 7 个 SDK 内部头(device/render_device.h、
// device/swapchain.h、resources/{buffer,command,pipeline}.h、primitives/{model_catalog,types}.h),
// 并且成员里直接摆着 6 个 unique_ptr 指向 SDK 的内部虚类:
//
//     std::unique_ptr<dev::IRenderDevice>  device_;
//     std::unique_ptr<dev::ISwapchain>     swapchain_;
//     std::unique_ptr<res::IBuffer>        vertex_buffer_, index_buffer_;
//     std::unique_ptr<res::IPipeline>      pipeline_;
//     std::unique_ptr<prim::ILoadedModel>  model_;
//
// 后果(这才是"拆不干净"的**物理**原因,不是 include 好不好看):
//   - 这个类的**尺寸**由 6 个虚类的布局决定;只要任何一个内部虚表加/删一个函数,
//     SceneRenderer 的大小与析构路径就变 → 所有 include 它的 TU 必须全量重编;
//   - include 传染:包含 scene_renderer.h 的人被迫看见 SDK 内部类型,于是"UI 只依赖
//     一个头"永远只是口头约定。
//
// 现在改成 PIMPL:头里只有一个前向声明 `struct Impl;` 和一个 `std::unique_ptr<Impl>`。
// 尺寸固定 = 一个指针。SDK 内部怎么重构,这个头一行不用改,用它的文件二进制不用重编。
//
// 生命周期由壳层驱动(Kotlin/JNI 或未来的 JNA),经 **mb_capi.h** 的 C ABI 调用:
//   mb_session_create            → 建设备/管线/缓冲
//   mb_session_attach_surface    → 建 swapchain(拿到 ANativeWindow*/HWND)
//   mb_session_render            → 取图 → 录制 → 提交 → 呈现
//   mb_session_detach_surface    → 拆 swapchain
//   mb_session_destroy           → 拆设备
//
// 相机约定见 mb_capi.h 的 mb_pose(单位四元数、绕相机自身轴累积)。
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace magicblocks::android {

class SceneRenderer {
 public:
  SceneRenderer();
  ~SceneRenderer();

  SceneRenderer(const SceneRenderer&) = delete;
  SceneRenderer& operator=(const SceneRenderer&) = delete;

  // 建设备与资源。失败时 err 里有原因,且**对象仍然存在**(status_text 可读)。
  bool init(std::string* err);

  // 壳层 Surface 就绪：建 swapchain。window 是 ANativeWindow*(Android)/HWND(Win32)。
  bool attach_surface(void* window, int width, int height, std::string* err);
  void detach_surface();

  // 渲染一帧并直接呈现。四元数 (qx,qy,qz,qw) 为相机姿态,scale 为缩放,
  // pan_x/pan_y 为世界空间平移(壳层已按可视宽度换算好)。
  bool render_frame(float qx, float qy, float qz, float qw, float scale, float pan_x, float pan_y,
                    std::string* err);

  bool surface_ready() const;

  // 切到 `path` 指定的 OBJ。失败返回 false 且**保持原状**(不回落立方体、不留半成品)。
  // `name` 只用于日志/状态文本。
  bool load_model(const std::string& path, const std::string& name, std::string* err);
  void clear_model();

  // MSAA:必须在 attach_surface **之前**设定(决定交换链建几张 MS 图)。
  void set_msaa_samples(std::uint32_t n);
  // 实际生效的采样数(设备不支持时低于请求值)。UI 必须显示这个值。
  std::uint32_t effective_msaa_samples() const;
  // 设备报告的受支持档位(升序,至少含 1)。
  std::vector<std::uint32_t> supported_msaa_levels() const;

  // 调试信息(壳层打日志用)。
  std::string status_text() const;

  // 供 mb_capi.cpp 的内部实现访问 Impl(只有它需要,壳层不要用)。
  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace magicblocks::android
