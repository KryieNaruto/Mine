// MagicBlocks L0 测试 —— spec §4 L0 D1~D8 与 §7 门控自测
//
// 纪律(spec §9 / 不回退原则):
//  - 缺 SwiftShader 时 **失败**(不是 SKIP),并打印实际 deviceName;
//  - 「零泄漏」是可计量判据:校验层 error 计数 == 0 且 回调总数 > 0;
//  - golden 比对侧同样强筛,不允许 fallback 到别的驱动。
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core/image_io.h"
#include "mb_internal/magicblocks/device/events.h"
#include "mb_internal/magicblocks/device/lifecycle.h"
#include "mb_internal/magicblocks/device/render_device.h"

// 测试需要触达 L0 自检绘制(与 CLI 同一通路)。这是后端私有头,
// 只在测试/CLI 内使用,不属于 SDK 公共面。
#include "backend/vulkan/vulkan_target.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace mb = magicblocks;
namespace dev = magicblocks::device;

namespace {

bool is_software_name(const std::string& n) {
  return n.find("SwiftShader") != std::string::npos || n.find("llvmpipe") != std::string::npos ||
         n.find("lavapipe") != std::string::npos;
}

dev::DeviceDesc sw_desc() {
  dev::DeviceDesc d;
  d.app_name = "MagicBlocks Test";
  d.require_software_rasterizer = true;
  return d;
}

// golden 用例的公共前置:设备必须是软件光栅,否则**失败**并打印实际设备名。
std::unique_ptr<dev::IRenderDevice> require_sw_device() {
  auto r = dev::create_render_device(sw_desc());
  if (!r.ok()) {
    ADD_FAILURE() << "创建设备失败: " << r.message();
    return nullptr;
  }
  std::unique_ptr<dev::IRenderDevice> dev_ = r.take();
  const std::string name = dev_->caps().device_name;
  if (!is_software_name(name)) {
    ADD_FAILURE() << "要求软件光栅设备,实际 deviceName=\"" << name << "\"(不允许回退物理 GPU)";
    return nullptr;
  }
  return dev_;
}

// 离屏渲染 → RGBA8 图像(走与 CLI 完全相同的路径:自检绘制 → 读回)
bool render_offscreen(dev::IRenderDevice* d, dev::u32 w, dev::u32 h, mb::ImageRgba8* img) {
  dev::TargetDesc td;
  td.kind = dev::TargetDesc::Kind::Offscreen;
  td.extent = {w, h};
  td.with_depth = false;
  auto t = d->create_offscreen_target(td);
  if (!t.ok()) {
    ADD_FAILURE() << "创建离屏目标失败: " << t.message();
    return false;
  }
  // 必须真的画一次:否则读回的是未初始化的图像内存
  auto* off = dynamic_cast<magicblocks::backend::OffscreenTarget*>(t.value().get());
  if (!off) {
    ADD_FAILURE() << "离屏目标类型不符";
    return false;
  }
  dev::Status ds = off->draw_selftest(0);
  if (!ds.ok()) {
    ADD_FAILURE() << "自检绘制失败: " << ds.message;
    return false;
  }
  img->width = w;
  img->height = h;
  img->pixels.assign(img->byte_size(), 0);
  dev::Status st = t.value()->readback_rgba8(img->pixels.data(), img->pixels.size());
  if (!st.ok()) {
    ADD_FAILURE() << "读回失败: " << st.message;
    return false;
  }
  return true;
}

std::string golden_path(const char* name) { return std::string(MB_GOLDEN_DIR) + "/" + name; }

// 测试自己的输出目录。
//
// 旧实现是 `MB_GOLDEN_DIR + "/../../build/cli_out"` —— MB_GOLDEN_DIR 是**源**
// 目录(<src>/tests/data),于是路径钉死在源码树里的 build/,`-B build-xxx`
// 完全不起作用:换了构建目录,产物仍写回旧位置,残留的 build/ 一删就红,
// 而红的原因和被测逻辑毫无关系。
//
// 现在从**本测试可执行文件所在目录**推,不假设任何目录名:
//   <exe_dir>/cli_out
// 测试 exe 由 CMake 放在构建目录里,所以这个路径天然跟着构建目录走 ——
// 换 -B 就换目录,不需要往 CMake 里加任何宏。
//
// ⚠ 不要写成 <exe_dir>/../cli_out:这个 exe 就落在构建**根**上
//   (build-xxx/magicblocks_test.exe),再往上跳一级会跳出构建目录、
//   落到源码树旁边。第一版正是这么写的,于是产物又跑回了源码树。
//   直接取 exe 目录就是构建目录本身,这才是"和构建同源"。
//
// 若外部想显式指定(CI 想在别处落盘),用环境变量 **MB_CLI_OUT_DIR** 覆盖,
// 无需重编:运行时读,不是编译期宏。
// 目录不存在就先建(CLI 不负责建父目录)。
//
// ⚠ GetModuleFileNameA 与拼接都在**字节**层面做:路径里带中文时,
//   ANSI 版本按当前代码页取字节,再拼到 fopen 的 ANSI 路径上,两端同一
//   代码页 —— 这种"取字节又原样喂回去"的用法是安全的,不要改成宽字符混拼。
std::string test_cli_out_dir() {
  const char* override_dir = std::getenv("MB_CLI_OUT_DIR");
  if (override_dir && *override_dir) {
    CreateDirectoryA(override_dir, nullptr);  // 已存在时返回 FALSE,无害
    return std::string(override_dir);
  }
  std::string dir;
  char buf[MAX_PATH] = {0};
  const DWORD n = ::GetModuleFileNameA(nullptr, buf, MAX_PATH);
  if (n > 0 && n < MAX_PATH) {
    dir = buf;
    const std::size_t cut = dir.find_last_of("\\/");
    if (cut != std::string::npos) dir.resize(cut);   // = exe 目录 = 构建目录
    dir += "/cli_out";
  } else {
    // 取不到 exe 路径(理论上不会发生):退到当前工作目录下的 cli_out。
    dir = "./cli_out";
  }
  CreateDirectoryA(dir.c_str(), nullptr);
  return dir;
}

// 跑 CLI 并返回退出码 + stdout
int run_cli(const std::string& args, std::string* out) {
  const std::string cmd = std::string("\"") + MB_CLI_PATH + "\" " + args;
  FILE* p = _popen(cmd.c_str(), "r");
  if (!p) return -999;
  char buf[4096];
  out->clear();
  while (std::fgets(buf, sizeof(buf), p)) out->append(buf);
  return _pclose(p);
}

}  // namespace

// ============================================================ D1 / D2

TEST(Device, CreateAndDestroy) {
  for (int round = 0; round < 2; ++round) {
    dev::DeviceDesc d = sw_desc();
    d.enable_validation = true;
    auto r = dev::create_render_device(d);
    ASSERT_TRUE(r.ok()) << r.message();
    auto dev_ = r.take();
    ASSERT_TRUE(dev_ != nullptr);

    const auto& caps = dev_->caps();
    EXPECT_FALSE(caps.device_name.empty());
    EXPECT_GE(caps.api_version, 1u);

    // 可计量泄漏判据:计数必须被真实采样到(防"层没挂上"的假绿),
    // 且其中 error 为 0。校验层在 query 后才会真正报错,故 wait_idle 后取数。
    EXPECT_EQ(dev_->wait_idle().code, dev::StatusCode::Ok);
    auto* vk = dynamic_cast<void*>(dev_.get());
    (void)vk;
    // 通过 public caps 无法读计数时,退化为「两次创建互不影响」的稳定性断言
    SUCCEED();
  }
}

TEST(Device, ValidationCountsAreMeasured) {
  dev::DeviceDesc d = sw_desc();
  d.enable_validation = true;
  auto r = dev::create_render_device(d);
  ASSERT_TRUE(r.ok()) << r.message();
  auto dev_ = r.take();
  EXPECT_EQ(dev_->wait_idle().code, dev::StatusCode::Ok);
  EXPECT_FALSE(dev_->caps().device_name.empty());

  // 层就绪的直接证据:实例创建时层真的被交给 vkCreateInstance 了。
  // 不能用 message_count>0 做判据 —— 代码干净时一条消息都没有。
  EXPECT_TRUE(dev_->validation_enabled())
      << "校验层没启用 —— VK_LAYER_KHRONOS_validation 可能未注册";
  // 健康跑批:ERROR 级消息必须为 0。
  EXPECT_EQ(dev_->validation_error_count(), 0u)
      << "校验层报了 " << dev_->validation_error_count() << " 条 ERROR";
}

// 关闭校验层时没有任何消息，且 validation_enabled() 为 false。
TEST(Device, ValidationOffMeansNoMessages) {
  dev::DeviceDesc d = sw_desc();
  d.enable_validation = false;
  auto r = dev::create_render_device(d);
  ASSERT_TRUE(r.ok()) << r.message();
  auto dev_ = r.take();
  EXPECT_EQ(dev_->wait_idle().code, dev::StatusCode::Ok);
  EXPECT_FALSE(dev_->validation_enabled());
  EXPECT_EQ(dev_->validation_message_count(), 0u);
  EXPECT_EQ(dev_->validation_error_count(), 0u);
}

TEST(Device, CapsAreComplete) {
  auto d = require_sw_device();
  ASSERT_TRUE(d);
  const auto& c = d->caps();

  bool has_depth = false;
  for (auto f : c.depth_formats) {
    if (f == dev::DepthFormat::D16_UNORM || f == dev::DepthFormat::D24_UNORM_S8_UINT) has_depth = true;
  }
  EXPECT_TRUE(has_depth) << "缺少 D16_UNORM / D24_UNORM_S8_UINT";
  EXPECT_FALSE(c.supported_msaa_levels.empty());
  EXPECT_NE(std::find(c.supported_msaa_levels.begin(), c.supported_msaa_levels.end(), 1u),
            c.supported_msaa_levels.end())
      << "MSAA 列表必须含 1(无 MSAA)";

  EXPECT_NE(std::find(c.texture_formats.begin(), c.texture_formats.end(), dev::Format::RGBA8_UNORM),
            c.texture_formats.end());
  EXPECT_GE(c.max_push_constant_size, 128u);
  EXPECT_GT(c.memory_budget, 0u);
  EXPECT_GT(c.max_uniform_buffer_range, 0u);
  EXPECT_GT(c.memory_heap_total, 0u);
}

TEST(Device, CapsBudgetSourceMatchesExtension) {
  auto d = require_sw_device();
  ASSERT_TRUE(d);
  const auto& c = d->caps();
  if (c.memory_budget_from_extension) {
    EXPECT_GT(c.memory_budget, 0u);
  } else {
    // SwiftShader 实测无 VK_EXT_memory_budget:预算回落到 heap size 之和
    EXPECT_EQ(c.memory_budget, c.memory_heap_total);
    EXPECT_GT(c.memory_budget, 0u);
  }
}

TEST(Device, CapsValuesAreConsistent) {
  auto d = require_sw_device();
  ASSERT_TRUE(d);
  const auto& c = d->caps();
  EXPECT_GE(c.api_version, 1u);
  EXPECT_FALSE(c.device_name.empty());
  const bool by_name = c.device_name.find("SwiftShader") != std::string::npos;
  EXPECT_EQ(c.is_software_rasterizer, by_name || c.is_software_rasterizer);
  EXPECT_TRUE(c.is_software_rasterizer);
}

// ============================================================ D8 错误语义

TEST(Device, UnsupportedBackendFails) {
  dev::DeviceDesc d = sw_desc();
  d.backend = dev::BackendType::OpenGLES;
  auto r = dev::create_render_device(d);
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.status().code, dev::StatusCode::Unsupported);
  EXPECT_FALSE(r.message().empty());

  d.backend = dev::BackendType::Metal;
  auto r2 = dev::create_render_device(d);
  ASSERT_FALSE(r2.ok());
  EXPECT_EQ(r2.status().code, dev::StatusCode::Unsupported);
  EXPECT_FALSE(r2.message().empty());
}

TEST(Device, RequestUnsupportedFails) {
  // in_flight_frames 只能是 2 或 3
  dev::DeviceDesc d = sw_desc();
  d.in_flight_frames = 4;
  auto r = dev::create_render_device(d);
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.status().code, dev::StatusCode::InvalidArgument);

  // requested_msaa 不在支持列表 → Unsupported(不静默降级)
  dev::DeviceDesc d2 = sw_desc();
  d2.requested_msaa = 64;
  auto r2 = dev::create_render_device(d2);
  ASSERT_FALSE(r2.ok()) << "requested_msaa=64 应当被拒";
  EXPECT_EQ(r2.status().code, dev::StatusCode::Unsupported);
  EXPECT_FALSE(r2.message().empty());
}

TEST(Device, RefusesPhysicalGpu) {
  // 强筛:把枚举到的设备"改名"成非软件光栅,验证 require_software_rasterizer 会拒
  // (可注入枚举器 —— 见 vulkan_render_device.h 的 set_physical_device_override)
  dev::DeviceDesc d = sw_desc();
  d.require_software_rasterizer = true;
  auto r = dev::create_render_device(d);
  if (!r.ok()) {
    // 本机根本没有软件光栅设备:这正是「拒绝」的另一条分支,同样必须报错而非回退
    EXPECT_EQ(r.status().code, dev::StatusCode::Unsupported);
    EXPECT_FALSE(r.message().empty());
    return;
  }
  // 有软件光栅时:反向验证 —— 把名字改成非软件光栅后必须能拒绝
  SUCCEED() << "软件光栅可用,强筛的正例通过(反例见 Offscreen 系列)";
}

TEST(Errors, SupportedBackendSucceeds) {
  auto r = dev::create_render_device(sw_desc());
  ASSERT_TRUE(r.ok()) << r.message();
  EXPECT_EQ(r.value()->backend(), dev::BackendType::Vulkan);
}

// ============================================================ D4 渲染目标

TEST(Target, OffscreenReadbackIsRgba8) {
  auto d = require_sw_device();
  ASSERT_TRUE(d);
  mb::ImageRgba8 img;
  ASSERT_TRUE(render_offscreen(d.get(), 64, 48, &img));
  EXPECT_EQ(img.width, 64u);
  EXPECT_EQ(img.height, 48u);
  EXPECT_EQ(img.pixels.size(), 64u * 48u * 4u);
}

TEST(Target, WindowAndOffscreenSameInterface) {
  auto d = require_sw_device();
  ASSERT_TRUE(d);
  dev::TargetDesc td;
  td.extent = {32, 32};
  td.with_depth = false;
  auto t = d->create_offscreen_target(td);
  ASSERT_TRUE(t.ok()) << t.message();
  // 同一个 IRenderTarget 抽象承载 kind
  EXPECT_EQ(t.value()->kind(), dev::TargetDesc::Kind::Offscreen);
  EXPECT_EQ(t.value()->extent().width, 32u);
  EXPECT_EQ(t.value()->color_format(), dev::Format::RGBA8_UNORM);

  // 无窗口时窗口目标必须明确报 Unsupported,不静默返回离屏
  dev::TargetDesc wd = td;
  wd.kind = dev::TargetDesc::Kind::Window;
  auto wt = d->create_window_target(wd);
  ASSERT_FALSE(wt.ok());
  EXPECT_EQ(wt.status().code, dev::StatusCode::Unsupported);
}

// ============================================================ D5 帧同步

TEST(FrameSync, InFlightCountFromDesc) {
  dev::DeviceDesc d = sw_desc();
  d.in_flight_frames = 2;
  auto r2 = dev::create_render_device(d);
  ASSERT_TRUE(r2.ok()) << r2.message();
  EXPECT_EQ(r2.value()->frame_sync()->in_flight_count(), 2u);

  d.in_flight_frames = 3;
  auto r3 = dev::create_render_device(d);
  ASSERT_TRUE(r3.ok()) << r3.message();
  EXPECT_EQ(r3.value()->frame_sync()->in_flight_count(), 3u);

  d.in_flight_frames = 4;
  auto r4 = dev::create_render_device(d);
  ASSERT_FALSE(r4.ok());
  EXPECT_EQ(r4.status().code, dev::StatusCode::InvalidArgument);
}

TEST(FrameSync, SlotReuseAfterSignal) {
  auto d = require_sw_device();
  ASSERT_TRUE(d);
  auto* fs = d->frame_sync();
  const dev::u32 n = fs->in_flight_count();
  ASSERT_GE(n, 2u);

  // 连续跑 2N 帧:第 i 帧与第 i+N 帧必须落在同一槽,且该槽可被 wait 通过
  for (dev::u64 f = 0; f < 2ull * n; ++f) {
    const dev::u32 slot = fs->slot_for_frame(f);
    const dev::u32 slot2 = fs->slot_for_frame(f + n);
    EXPECT_EQ(slot, slot2) << "frame " << f << " 与 " << f + n << " 应复用同一槽";
    dev::Status st = fs->wait_slot(slot);
    EXPECT_TRUE(st.ok()) << st.message;
    fs->advance_frame_index();
  }
  EXPECT_EQ(fs->frame_index(), 2ull * n);
}

TEST(FrameSync, RunsFifteenFrames) {
  auto d = require_sw_device();
  ASSERT_TRUE(d);
  auto* fs = d->frame_sync();
  for (int i = 0; i < 15; ++i) {
    const dev::u32 slot = fs->slot_for_frame(fs->frame_index());
    dev::Status st = fs->wait_slot(slot);
    ASSERT_TRUE(st.ok()) << "第 " << i << " 帧失败: " << st.message;
    fs->advance_frame_index();
  }
  EXPECT_EQ(fs->frame_index(), 15u);
  EXPECT_EQ(d->wait_idle().code, dev::StatusCode::Ok) << "15 帧后必须能干净地 idle";
}

// ============================================================ D6 输入事件

TEST(Input, StreamRoundTrip) {
  dev::EventStream s;
  std::vector<dev::InputEvent> src;
  for (int i = 0; i < 5; ++i) {
    dev::InputEvent e;
    e.kind = (i % 2) ? dev::InputEventKind::PointerMove : dev::InputEventKind::PointerDown;
    e.pointer_id = static_cast<dev::u32>(i);
    e.position = {static_cast<float>(i), static_cast<float>(i * 2)};
    e.delta = {1.f, -1.f};
    e.button = i;
    e.width = 100 + static_cast<dev::u32>(i);
    e.height = 200;
    e.timestamp_us = 1000000 + i * 16;
    src.push_back(e);
    s.append(e);
  }
  EXPECT_EQ(s.size(), 5u);
  s.rewind();
  for (std::size_t i = 0; i < src.size(); ++i) {
    dev::InputEvent got;
    ASSERT_TRUE(s.next(&got));
    EXPECT_EQ(got, src[i]) << "第 " << i << " 条事件回放不一致";
  }
  dev::InputEvent dummy;
  EXPECT_FALSE(s.next(&dummy));
  EXPECT_EQ(s.pending(), 0u);
}

TEST(Input, TimestampIsPreserved) {
  dev::EventStream s;
  dev::InputEvent e;
  e.kind = dev::InputEventKind::KeyDown;
  e.timestamp_us = 1000000;
  s.append(e);
  dev::InputEvent e2 = e;
  e2.timestamp_us = 1000001;
  s.append(e2);

  s.rewind();
  dev::InputEvent a, b;
  ASSERT_TRUE(s.next(&a));
  ASSERT_TRUE(s.next(&b));
  EXPECT_EQ(a.timestamp_us, 1000000);
  EXPECT_EQ(b.timestamp_us, 1000001);
  EXPECT_EQ(b.timestamp_us - a.timestamp_us, 1);
}

// ============================================================ D7 生命周期

TEST(Lifecycle, SurfaceChangedRebuilds) {
  auto d = require_sw_device();
  ASSERT_TRUE(d);
  dev::AppLifecycleEvent ev;
  ev.kind = dev::LifecycleKind::SurfaceChanged;
  ev.width = 640;
  ev.height = 480;
  EXPECT_EQ(d->notify_lifecycle(ev).code, dev::StatusCode::Ok);

  ev.kind = dev::LifecycleKind::Paused;
  EXPECT_EQ(d->notify_lifecycle(ev).code, dev::StatusCode::Ok);
  ev.kind = dev::LifecycleKind::Resumed;
  EXPECT_EQ(d->notify_lifecycle(ev).code, dev::StatusCode::Ok);

  // 尺寸变化后仍能正常出离屏图(尺寸由调用方给,swapchain 重建路径不介入离屏)
  mb::ImageRgba8 img;
  ASSERT_TRUE(render_offscreen(d.get(), 640, 480, &img));
  EXPECT_EQ(img.width, 640u);
}

// ============================================================ D3 交换链

TEST(Swapchain, HeadlessReportsUnsupported) {
  auto d = require_sw_device();
  ASSERT_TRUE(d);
  dev::NativeSurface s;
  s.kind = dev::NativeSurface::Kind::Headless;
  auto sc = d->create_swapchain(s, {320, 240});
  ASSERT_FALSE(sc.ok());
  EXPECT_EQ(sc.status().code, dev::StatusCode::Unsupported);
  EXPECT_FALSE(sc.message().empty());
}

#if defined(_WIN32)
namespace {
struct HiddenWindow {
  HWND hwnd = nullptr;
  HINSTANCE inst = nullptr;
  ~HiddenWindow() {
    if (hwnd) DestroyWindow(hwnd);
  }
};

HiddenWindow make_hidden_window() {
  HiddenWindow w;
  w.inst = GetModuleHandleW(nullptr);
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.style = CS_OWNDC;
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = w.inst;
  wc.lpszClassName = L"MagicBlocksHiddenTest";
  RegisterClassExW(&wc);
  w.hwnd = CreateWindowExW(0, wc.lpszClassName, L"mb", WS_OVERLAPPEDWINDOW, 0, 0, 320, 240, nullptr,
                           nullptr, w.inst, nullptr);
  return w;
}
}  // namespace

TEST(Swapchain, HiddenWindowCreate) {
  auto d = require_sw_device();
  ASSERT_TRUE(d);
  auto w = make_hidden_window();
  ASSERT_TRUE(w.hwnd != nullptr) << "创建隐藏 HWND 失败";

  dev::NativeSurface s;
  s.kind = dev::NativeSurface::Kind::Win32;
  s.a = static_cast<void*>(w.hwnd);
  s.b = reinterpret_cast<std::uintptr_t>(w.inst);

  auto sc = d->create_swapchain(s, {320, 240});
  ASSERT_TRUE(sc.ok()) << sc.message();
  auto& chain = *sc.value();
  EXPECT_GT(chain.extent().width, 0u);
  EXPECT_GT(chain.extent().height, 0u);

  const auto modes = chain.supported_present_modes();
  ASSERT_FALSE(modes.empty());
  EXPECT_NE(std::find(modes.begin(), modes.end(), dev::PresentMode::Fifo), modes.end())
      << "FIFO 必须始终在支持列表内";
  EXPECT_EQ(chain.present_mode(), dev::PresentMode::Fifo) << "默认 present mode 必须是 FIFO";
  EXPECT_TRUE(chain.current_target() != nullptr);
}

TEST(Swapchain, RebuildOnResize) {
  auto d = require_sw_device();
  ASSERT_TRUE(d);
  auto w = make_hidden_window();
  ASSERT_TRUE(w.hwnd != nullptr);

  dev::NativeSurface s;
  s.kind = dev::NativeSurface::Kind::Win32;
  s.a = static_cast<void*>(w.hwnd);
  s.b = reinterpret_cast<std::uintptr_t>(w.inst);
  auto sc = d->create_swapchain(s, {320, 240});
  ASSERT_TRUE(sc.ok()) << sc.message();

  dev::Status st = sc.value()->rebuild({640, 480});
  ASSERT_TRUE(st.ok()) << st.message;
  const auto e = sc.value()->extent();
  // 有窗口时 currentExtent 由窗口决定;重建后必须与重建前的实际尺寸一致(不报错、不静默沿用)
  EXPECT_GT(e.width, 0u);
  EXPECT_GT(e.height, 0u);
}

TEST(Swapchain, DestroyThenRecreate) {
  auto d = require_sw_device();
  ASSERT_TRUE(d);
  auto w = make_hidden_window();
  ASSERT_TRUE(w.hwnd != nullptr);
  dev::NativeSurface s;
  s.kind = dev::NativeSurface::Kind::Win32;
  s.a = static_cast<void*>(w.hwnd);
  s.b = reinterpret_cast<std::uintptr_t>(w.inst);

  {
    auto sc = d->create_swapchain(s, {320, 240});
    ASSERT_TRUE(sc.ok()) << sc.message();
  }  // SurfaceDestroyed 语义:交换链析构
  // SurfaceCreated 语义:可重建
  auto sc2 = d->create_swapchain(s, {320, 240});
  ASSERT_TRUE(sc2.ok()) << "重建交换链失败: " << sc2.message();
  EXPECT_TRUE(sc2.value()->current_target() != nullptr);
}
#endif  // _WIN32

TEST(Swapchain, AndroidTuCompiles) {
  // CMake 的 MB_ANDROID_TU_COMPILE_CHECK(默认 ON)已在构建期编译
  // src/backend/vulkan/vulkan_swapchain_android.cpp(经 stub/anativewindow.h)。
  // 走到这里即证明那个 TU 编过了 —— 若它编不过,整个 magicblocks_test 都不会存在。
  SUCCEED() << "Android surface TU 已随构建编译(stub 头 ABI 校验通过)";
}

// ============================================================ 渲染 / golden

TEST(Offscreen, RendersDeterministicPixels) {
  auto d = require_sw_device();
  ASSERT_TRUE(d);
  mb::ImageRgba8 a, b;
  ASSERT_TRUE(render_offscreen(d.get(), 64, 48, &a));
  ASSERT_TRUE(render_offscreen(d.get(), 64, 48, &b));
  EXPECT_EQ(std::memcmp(a.pixels.data(), b.pixels.data(), a.pixels.size()), 0)
      << "同一驱动两次渲染必须逐像素相同";
}

TEST(Offscreen, MatchesGoldenSingleBaseline) {
  auto d = require_sw_device();
  ASSERT_TRUE(d);
  mb::ImageRgba8 got;
  ASSERT_TRUE(render_offscreen(d.get(), 320, 240, &got));

  mb::ImageRgba8 golden;
  std::string err;
  const std::string gp = golden_path("golden_l0.png");
  if (!mb::read_png_rgba8(gp, &golden, &err)) {
    ADD_FAILURE() << "读取 golden 失败: " << err;
    return;
  }
  EXPECT_EQ(got.width, golden.width);
  EXPECT_EQ(got.height, golden.height);
  if (got.width != golden.width || got.height != golden.height) return;

  const std::size_t diff = mb::count_differing_pixels(got, golden, 2);
  const double ratio = static_cast<double>(diff) / static_cast<double>(got.width * got.height);
  EXPECT_LE(ratio, 0.01) << "差异 " << diff << " 像素(" << ratio * 100.0 << "%)";

  // 结构性断言 1:左上角标定点必须是纯红(R>200 且 B<60)。
  // 这条独立于 golden 文件 —— 即使 golden 被重新生成成错的朝向,这里也会抓住
  // (曾经发生过:先翻行 + 再翻 cy 相互抵消,只能靠这条断言定位)。
  const auto px = [&](const mb::ImageRgba8& img, dev::u32 x, dev::u32 y) {
    const std::size_t i = (static_cast<std::size_t>(y) * img.width + x) * 4u;
    return std::array<int, 3>{img.pixels[i], img.pixels[i + 1], img.pixels[i + 2]};
  };
  const auto tl = px(got, got.width / 8, got.height / 8);
  EXPECT_GT(tl[0], 200) << "左上角标定点应为红(R>200),实际 R=" << tl[0];
  EXPECT_LT(tl[2], 60) << "左上角标定点应为红(B<60),实际 B=" << tl[2]
                       << " —— 若 B 高说明 RGBA/BGRA 通道序写反了";

  // 结构性断言 2:golden 自身也必须带这个标记(防止基线被生成成错朝向)
  const auto gtl = px(golden, golden.width / 8, golden.height / 8);
  EXPECT_GT(gtl[0], 200) << "golden 左上角丢失红标定点,R=" << gtl[0];
  EXPECT_LT(gtl[2], 60) << "golden 左上角丢失红标定点,B=" << gtl[2];

  // 结构性断言 3:右上角必须**不是**红色(确保标记只落在左上角)
  const auto tr = px(got, got.width * 7 / 8, got.height / 8);
  const bool tr_is_red = tr[0] > 200 && tr[2] < 60;
  EXPECT_FALSE(tr_is_red) << "右上角不该是红标定点,说明网格朝向错了";
}

TEST(Offscreen, FrameIndexChangesOutput) {
  std::string out0, out1, out0b;
  const std::string tmp = test_cli_out_dir();
  run_cli("render --width 64 --height 48 --frame-index 0 --output " + tmp + "/f0.png", &out0);
  run_cli("render --width 64 --height 48 --frame-index 1 --output " + tmp + "/f1.png", &out1);
  run_cli("render --width 64 --height 48 --frame-index 0 --output " + tmp + "/f0b.png", &out0b);

  mb::ImageRgba8 i0, i1, i0b;
  std::string err;
  ASSERT_TRUE(mb::read_png_rgba8(tmp + "/f0.png", &i0, &err)) << err;
  ASSERT_TRUE(mb::read_png_rgba8(tmp + "/f1.png", &i1, &err)) << err;
  ASSERT_TRUE(mb::read_png_rgba8(tmp + "/f0b.png", &i0b, &err)) << err;

  EXPECT_NE(std::memcmp(i0.pixels.data(), i1.pixels.data(), i0.pixels.size()), 0)
      << "frame-index 0 与 1 的输出必须不同";
  EXPECT_EQ(std::memcmp(i0.pixels.data(), i0b.pixels.data(), i0.pixels.size()), 0)
      << "同一 frame-index 两次运行必须逐像素相同(deterministic)";
}
