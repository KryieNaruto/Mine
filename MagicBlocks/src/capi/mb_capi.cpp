// MagicBlocks —— mb_capi.h 的实现(C ABI,新稳定边界)
//
// ============================================================================
// 这个文件在分层里的位置
// ============================================================================
//
//       壳层(Kotlin/JNI、未来 JNA、CLI、任何语言)
//              │  只依赖 include/magicblocks/mb_capi.h + 动态库
//              ▼
//   ┌──────────────────────┐
//   │  src/capi/mb_capi.cpp │  ← C ABI:不透明句柄 + 定长结构体 + 错误码
//   └──────────┬───────────┘
//              ▼  (只有这里能 include SDK 内部头)
//   src/android/scene_renderer.cpp (PIMPL) + device/ resources/ primitives/ backend/
//
// 三条硬规矩(违反即返工):
//   1. 本文件**不 include 任何 UI 侧文件**。SDK 不反向依赖 UI。
//   2. ABI 侧不留任何需要调用方释放的 SDK 内存:字符串一律"调用方缓冲区 + 返回所需长度";
//      `mb_model_list` 是唯一的例外,它有自己的 `mb_model_list_free`,规则写在头文件里。
//   3. 每个入口都要能吃住"无效句柄":返回 MB_E_INVALID_ARG,绝不 UB。句柄用 magic 校验,
//      这样把野指针传进来也不会解引用到随便一块内存(先读 magic 再判断,仍是 UB 的理论边界,
//      但句柄只由 create/destroy 产生,与旧 JNI 的裸指针 jlong 相比严格更安全)。
//
// 【为何不直接暴露 SceneRenderer 的 C++ 成员】
//   旧 JNI 每帧都穿过 C++ 类:JNI → SceneRenderer(带 6 个 unique_ptr) → SDK 内部。
//   只要 SDK 改一个虚类,`jni_bridge.cpp` 就要重编。现在 JNI 只看得到 mb_session*。
#include "magicblocks/mb_capi.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <vector>

#include "android/scene_renderer.h"

// SDK 内部头:仅用于模型目录扫描/错误模型映射。它们不会出现在本文件的导出面上。
#include "mb_internal/magicblocks/device/result.h"
#include "mb_internal/magicblocks/primitives/mesh_loader.h"
#include "mb_internal/magicblocks/primitives/model_catalog.h"

namespace {

// ============================================================================
// 1. 版本
// ============================================================================

std::atomic<std::uint32_t> g_abi_version{MB_ABI_VERSION};

// 模型列表文本线格式的版本(与 ABI 版本是两件事,理由见头文件 §10)。
// 改线格式(标记/分隔/字段顺序/sanitize 规则)就 +1,并同步改
// tools/capi_protocol/protocol.json 的 protocol_version —— --check 会盯着。
constexpr std::uint32_t MB_PROTOCOL_VERSION_NOW = 1;

// ============================================================================
// 2. 日志回调(替代旧的 JNI 反射调用 logNative)
//
// 旧实现:SceneRenderer 里直接 FindClass("com/magicblocks/pad/MbRenderer") +
// GetStaticMethodID("logNative", "(Ljava/lang/String;)V")。类改名/混淆即静默丢日志,
// 且 SDK 认识了一个 UI 类名 —— 反向依赖。
// 新实现:SDK 只认一个函数指针;JNI 层负责把它接到 Kotlin。
// ============================================================================

struct LogSink {
  std::mutex mutex;
  mb_log_cb cb = nullptr;
  void* user = nullptr;
};

LogSink& log_sink() {
  static LogSink s;
  return s;
}

void emit_log(mb_log_level level, const char* fmt, ...) {
  LogSink& sink = log_sink();
  mb_log_cb cb = nullptr;
  void* user = nullptr;
  {
    std::lock_guard<std::mutex> lk(sink.mutex);
    cb = sink.cb;
    user = sink.user;
  }
  if (cb == nullptr) return;  // 没注册 = 静默丢弃(与旧的"找不到类就跳过"一致,但不打印错误)

  char buf[1024];
  std::va_list args;
  va_start(args, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  buf[sizeof(buf) - 1] = '\0';
  // 回调里不允许再调 mb_session_*(见头文件) —— 这里也不持锁调用,避免互锁。
  cb(user, level, buf);
}

// ============================================================================
// 3. 句柄
// ============================================================================

// 句柄 magic:用来区分"有效句柄"和"随便一个非 NULL 指针"。
// 选值不讲究,只要不是 0/常见值即可。
constexpr std::uint64_t kSessionMagic = 0x4D4253455353494FULL;  // "MB SESSIO"
constexpr std::uint64_t kModelListMagic = 0x4D424D4F444C4953ULL;  // "MB MODLIS"

}  // namespace

// 不透明句柄的真实定义:调用方只看得到前向声明。
struct mb_session {
  std::uint64_t magic = kSessionMagic;
  std::unique_ptr<magicblocks::android::SceneRenderer> renderer;
  std::string status;  // 最近一次失败的可读原因(给 mb_session_status_text)
  std::mutex mutex;
  // ready 单独存,而不是从 status 字符串里"猜"。上一版写的是
  // `status.find("last_error=") == npos`,那是**字符串嗅探**,一处文案改动就静默失准。
  bool ready = false;
};

struct mb_model_list {
  std::uint64_t magic = kModelListMagic;
  std::vector<mb_model_entry> entries;
};

namespace {

// 句柄校验。对 NULL / 野指针 / 已销毁(free 后 magic 被改坏)都返回 nullptr。
mb_session* as_session(const mb_session* s) {
  if (s == nullptr) return nullptr;
  if (s->magic != kSessionMagic) return nullptr;
  return const_cast<mb_session*>(s);
}

mb_model_list* as_list(const mb_model_list* l) {
  if (l == nullptr) return nullptr;
  if (l->magic != kModelListMagic) return nullptr;
  return const_cast<mb_model_list*>(l);
}

// ---- 字符串:调用方缓冲区 + 返回所需长度 ----
size_t copy_out(const std::string& src, char* buf, size_t buf_size) {
  const size_t need = src.size() + 1;  // 含 NUL
  if (buf != nullptr && buf_size > 0) {
    const size_t n = need < buf_size ? need : buf_size;
    std::memcpy(buf, src.c_str(), n - 1);
    buf[n - 1] = '\0';
  }
  return need;
}

// Status → mb_err。**顺序敏感**:
//   - Status::Io 同时覆盖"目录不存在"和"真的读失败",用 message 细分(见 docs/UI_SDK_SPLIT.md §5.4);
//   - 未知码一律 MB_E_INTERNAL,绝不悄悄当成功。
mb_err map_status(const magicblocks::device::Status& st) {
  using magicblocks::device::StatusCode;
  switch (st.code) {
    case StatusCode::Ok:
      return MB_OK;
    case StatusCode::InvalidArgument:
      return MB_E_INVALID_ARG;
    case StatusCode::Unsupported:
    case StatusCode::ShaderCompileFailed:
      return MB_E_UNSUPPORTED;
    case StatusCode::OutOfMemory:
      return MB_E_OUT_OF_MEMORY;
    case StatusCode::DeviceLost:
      return MB_E_DEVICE_LOST;
    case StatusCode::InitializationFailed:
      return MB_E_NOT_INITIALIZED;
    case StatusCode::IoError:
      // scan_model_dir 对"目录不存在"返回 Status::Io("…目录不存在: …")。
      // 头文件要求"目录不存在"与"空目录"在**错误码**上可区分 —— 前者 MB_E_NOT_FOUND。
      if (st.message.find("不存在") != std::string::npos) return MB_E_NOT_FOUND;
      return MB_E_IO;
    default:
      return MB_E_INTERNAL;
  }
}

// 定长拷贝:保证 NUL 结尾,超长截断(截断在 UTF-8 字符中间时退到字符边界)。
void copy_fixed(char* dst, size_t cap, const std::string& src) {
  if (cap == 0) return;
  size_t n = src.size();
  if (n >= cap) {
    n = cap - 1;
    // 退回到 UTF-8 起始字节(0b10xxxxxx 是续字节),避免把汉字劈成两半。
    while (n > 0 && (static_cast<unsigned char>(src[n]) & 0xC0) == 0x80) --n;
  }
  std::memcpy(dst, src.data(), n);
  dst[n] = '\0';
}

// 记录"最近一次失败原因"。壳层通过 mb_session_status_text 读。
void set_status(mb_session* s, const std::string& text) {
  if (s == nullptr) return;
  std::lock_guard<std::mutex> lk(s->mutex);
  s->status = text;
}

}  // namespace

// ============================================================================
// 4. 错误码 / 版本 / 日志 —— extern "C" 开始
// ============================================================================

extern "C" {

const char* MB_CALL mb_err_str(mb_err e) {
  switch (e) {
    case MB_OK: return "MB_OK";
    case MB_E_INVALID_ARG: return "MB_E_INVALID_ARG";
    case MB_E_NOT_INITIALIZED: return "MB_E_NOT_INITIALIZED";
    case MB_E_NO_SURFACE: return "MB_E_NO_SURFACE";
    case MB_E_SURFACE_LOST: return "MB_E_SURFACE_LOST";
    case MB_E_OUT_OF_DATE: return "MB_E_OUT_OF_DATE";
    case MB_E_DEVICE_LOST: return "MB_E_DEVICE_LOST";
    case MB_E_UNSUPPORTED: return "MB_E_UNSUPPORTED";
    case MB_E_OUT_OF_MEMORY: return "MB_E_OUT_OF_MEMORY";
    case MB_E_IO: return "MB_E_IO";
    case MB_E_NOT_FOUND: return "MB_E_NOT_FOUND";
    case MB_E_BUSY: return "MB_E_BUSY";
    case MB_E_VERSION_MISMATCH: return "MB_E_VERSION_MISMATCH";
    case MB_E_INTERNAL: return "MB_E_INTERNAL";
    default: return "MB_E_UNKNOWN";
  }
}

std::uint32_t MB_CALL mb_abi_version(void) { return g_abi_version.load(); }

size_t MB_CALL mb_abi_version_string(char* buf, size_t buf_size) {
  const std::uint32_t v = g_abi_version.load();
  char tmp[32];
  std::snprintf(tmp, sizeof(tmp), "%u.%u.%u", (v / 10000u) % 100u, (v / 100u) % 100u, v % 100u);
  return copy_out(tmp, buf, buf_size);
}

mb_err MB_CALL mb_set_log_callback(const mb_log_cb_desc* desc) {
  if (desc != nullptr) {
    // 只认"至少覆盖到我们认识的字段"的描述符;未来尾部扩展不影响这里。
    if (desc->struct_size < sizeof(mb_log_cb_desc)) return MB_E_INVALID_ARG;
  }
  LogSink& sink = log_sink();
  std::lock_guard<std::mutex> lk(sink.mutex);
  // 注意:只有在调用方给 desc 时才更新 user。传 NULL 注销的含义是"清掉 cb",
  // **不要**顺手把 user 也置空(否则"先注册带 user 的回调、再临时不带 user 地
  // 探一次状态"就会把回调的上下文悄悄清掉 —— 那类 bug 只在日志里表现为段错误)。
  sink.cb = desc != nullptr ? desc->cb : nullptr;
  if (desc != nullptr) sink.user = desc->user;
  return MB_OK;
}

mb_session_desc MB_CALL mb_session_desc_default(void) {
  mb_session_desc d{};
  d.struct_size = sizeof(mb_session_desc);
  d.msaa = MB_MSAA_AUTO;
  d.app_name = nullptr;
  return d;
}

std::uint8_t MB_CALL mb_session_is_valid(const mb_session* s) {
  return as_session(s) != nullptr ? 1u : 0u;
}

std::uint8_t MB_CALL mb_model_list_is_valid(const mb_model_list* l) {
  return as_list(l) != nullptr ? 1u : 0u;
}

// ============================================================================
// 5. 会话生命周期
// ============================================================================

mb_err MB_CALL mb_session_create(const mb_session_desc* desc, mb_session** out) {
  if (out == nullptr) return MB_E_INVALID_ARG;
  *out = nullptr;
  if (desc == nullptr || desc->struct_size < sizeof(mb_session_desc)) return MB_E_INVALID_ARG;

  std::uint32_t samples = 1;
  switch (desc->msaa) {
    case MB_MSAA_AUTO: samples = 4; break;  // 库自选:当前实现请求 4x,不支持则降级
    case MB_MSAA_OFF: samples = 1; break;
    case MB_MSAA_4X: samples = 4; break;
    case MB_MSAA_8X: samples = 8; break;
    default: return MB_E_INVALID_ARG;  // 越界值不静默改写
  }

  auto* s = new (std::nothrow) mb_session();
  if (s == nullptr) return MB_E_OUT_OF_MEMORY;

  s->renderer = std::make_unique<magicblocks::android::SceneRenderer>();
  s->renderer->set_msaa_samples(samples);

  std::string err;
  if (!s->renderer->init(&err)) {
    // 与旧 nativeCreate 的关键差异:**明确报错**,而不是"失败也返回非 0 句柄让调用方去猜"。
    // 但句柄仍然交出去(见头文件 §6 的 @return 说明):创建失败时设备/资源可能是半成品,
    // 壳层可以用 mb_session_status_text 读到原因,然后 destroy。
    emit_log(MB_LOG_ERROR, "mb_session_create: init 失败: %s", err.c_str());
    set_status(s, "init 失败: " + err);
    *out = s;
    return MB_E_NOT_INITIALIZED;
  }
  s->ready = true;

  emit_log(MB_LOG_INFO, "mb_session_create: ok app=%s samples=%u",
           desc->app_name != nullptr ? desc->app_name : "MagicBlocks", samples);
  *out = s;
  return MB_OK;
}

mb_err MB_CALL mb_session_destroy(mb_session* s) {
  auto* sess = as_session(s);
  if (sess == nullptr) {
    // 对 NULL 是 no-op(幂等);对非法句柄报参数错。
    return s == nullptr ? MB_OK : MB_E_INVALID_ARG;
  }
  sess->magic = 0;  // 先失效:之后任何入口传进来都只会得到 MB_E_INVALID_ARG
  sess->renderer.reset();
  delete sess;
  return MB_OK;
}

mb_err MB_CALL mb_session_ready(const mb_session* s) {
  auto* sess = as_session(s);
  if (sess == nullptr) return MB_E_INVALID_ARG;
  // 就绪 = 设备与默认资源建好 = create 成功过。surface 另行判断。
  return (sess->ready && sess->renderer != nullptr) ? MB_OK : MB_E_NOT_INITIALIZED;
}

// ============================================================================
// 6. surface
// ============================================================================

mb_err MB_CALL mb_session_attach_surface(mb_session* s, std::uint32_t kind, void* native_window,
                                         std::uint32_t width, std::uint32_t height) {
  auto* sess = as_session(s);
  if (sess == nullptr || sess->renderer == nullptr) return MB_E_INVALID_ARG;
  if (native_window == nullptr || width == 0 || height == 0) return MB_E_INVALID_ARG;
  if (kind != MB_SURFACE_ANDROID && kind != MB_SURFACE_WIN32) {
    // 本库这一版只建立了 Android/Win32 两条 surface 路径;XCB/HEADLESS 未接线。
    return MB_E_UNSUPPORTED;
  }

  std::string err;
  if (!sess->renderer->attach_surface(native_window, static_cast<int>(width),
                                      static_cast<int>(height), &err)) {
    set_status(sess, "attach_surface 失败: " + err);
    emit_log(MB_LOG_ERROR, "mb_session_attach_surface: %s", err.c_str());
    return MB_E_INTERNAL;
  }
  set_status(sess, "");
  return MB_OK;
}

mb_err MB_CALL mb_session_detach_surface(mb_session* s) {
  auto* sess = as_session(s);
  if (sess == nullptr || sess->renderer == nullptr) return MB_E_INVALID_ARG;
  sess->renderer->detach_surface();
  return MB_OK;
}

mb_err MB_CALL mb_session_surface_resized(mb_session* s, std::uint32_t width, std::uint32_t height) {
  auto* sess = as_session(s);
  if (sess == nullptr || sess->renderer == nullptr) return MB_E_INVALID_ARG;
  if (width == 0 || height == 0) return MB_E_INVALID_ARG;
  // 这一版:重建交换链要求尺寸,而 SceneRenderer 的重建入口是 render_frame 内部的
  // OutOfDate 分支(它用 swapchain 自己的 extent)。这里不谎称已处理 ——
  // Android 的 surfaceChanged 直接走 attach_surface(先 detach 再 attach),见
  // docs/UI_SDK_SPLIT.md §5.6 的迁移说明。
  return MB_E_UNSUPPORTED;
}

// ============================================================================
// 7. 渲染
// ============================================================================

mb_pose MB_CALL mb_pose_default(void) {
  mb_pose p{};
  p.q[0] = 0.f;
  p.q[1] = 0.f;
  p.q[2] = 0.f;
  p.q[3] = 1.f;
  p.scale = 1.f;
  p.pan[0] = 0.f;
  p.pan[1] = 0.f;
  return p;
}

mb_err MB_CALL mb_session_render(mb_session* s, mb_pose pose) {
  auto* sess = as_session(s);
  if (sess == nullptr || sess->renderer == nullptr) return MB_E_INVALID_ARG;

  // 全 0 四元数是非法姿态(没有方向),库内部会退化成 1,那是"悄悄改参数"——
  // 头文件约定这里是 MB_E_INVALID_ARG。
  const float q2 = pose.q[0] * pose.q[0] + pose.q[1] * pose.q[1] + pose.q[2] * pose.q[2] +
                   pose.q[3] * pose.q[3];
  if (!(q2 > 1e-12f)) return MB_E_INVALID_ARG;

  if (!sess->renderer->surface_ready()) return MB_E_NO_SURFACE;

  const float scale = pose.scale > 0.f ? pose.scale : 1.f;  // <=0 视为 1(头文件约定)
  std::string err;
  if (!sess->renderer->render_frame(pose.q[0], pose.q[1], pose.q[2], pose.q[3], scale, pose.pan[0],
                                    pose.pan[1], &err)) {
    set_status(sess, "render 失败: " + err);
    emit_log(MB_LOG_WARN, "mb_session_render: %s", err.c_str());
    // 细节诊断:没有 surface 与其它错误分开报。
    if (err.find("没有 Surface") != std::string::npos) return MB_E_NO_SURFACE;
    if (err.find("交换链重建失败") != std::string::npos) return MB_E_OUT_OF_DATE;
    return MB_E_INTERNAL;
  }
  return MB_OK;
}

// ============================================================================
// 8. MSAA
// ============================================================================

mb_err MB_CALL mb_session_set_msaa(mb_session* s, mb_msaa msaa) {
  auto* sess = as_session(s);
  if (sess == nullptr || sess->renderer == nullptr) return MB_E_INVALID_ARG;
  std::uint32_t n = 1;
  switch (msaa) {
    case MB_MSAA_AUTO: n = 4; break;
    case MB_MSAA_OFF: n = 1; break;
    case MB_MSAA_4X: n = 4; break;
    case MB_MSAA_8X: n = 8; break;
    default: return MB_E_INVALID_ARG;
  }
  sess->renderer->set_msaa_samples(n);
  return MB_OK;
}

mb_err MB_CALL mb_session_msaa_effective(const mb_session* s, std::uint32_t* out_samples) {
  auto* sess = as_session(s);
  if (sess == nullptr || sess->renderer == nullptr || out_samples == nullptr) return MB_E_INVALID_ARG;
  *out_samples = sess->renderer->effective_msaa_samples();
  return MB_OK;
}

mb_err MB_CALL mb_session_msaa_supported(const mb_session* s, std::uint32_t* out_samples,
                                         size_t capacity, size_t* out_count) {
  auto* sess = as_session(s);
  if (sess == nullptr || sess->renderer == nullptr || out_count == nullptr) return MB_E_INVALID_ARG;
  const std::vector<std::uint32_t> lv = sess->renderer->supported_msaa_levels();
  const size_t need = lv.empty() ? 1 : lv.size();
  if (out_samples == nullptr || capacity == 0) {
    *out_count = need;  // 只查询所需个数
    return MB_OK;
  }
  const size_t n = need < capacity ? need : capacity;
  for (size_t i = 0; i < n; ++i) out_samples[i] = lv.empty() ? 1u : lv[i];
  *out_count = n;
  return n < need ? MB_E_BUSY : MB_OK;  // 缓冲区不够:BUSY = 扩容重试
}

// ============================================================================
// 9. 模型
// ============================================================================

mb_err MB_CALL mb_session_scan_models(const mb_session* s, const char* directory,
                                      mb_model_list** out) {
  if (out == nullptr) return MB_E_INVALID_ARG;
  *out = nullptr;
  const mb_session* sess = s;  // 允许 NULL:纯目录扫描,不依赖会话
  if (sess != nullptr && as_session(sess) == nullptr) return MB_E_INVALID_ARG;
  if (directory == nullptr) return MB_E_INVALID_ARG;

  auto scanned = magicblocks::primitives::scan_model_dir(std::string(directory));
  if (!scanned.ok()) {
    return map_status(scanned.status());  // 目录不存在 → MB_E_NOT_FOUND(不是空列表)
  }

  auto l = std::make_unique<mb_model_list>();
  for (const auto& m : scanned.value()) {
    mb_model_entry e{};
    copy_fixed(e.name, sizeof(e.name), m.name);
    copy_fixed(e.path, sizeof(e.path), m.path);
    copy_fixed(e.message, sizeof(e.message), m.message);
    e.bytes = m.bytes;
    e.ok = m.ok ? 1u : 0u;
    e.reserved0 = 0;
    l->entries.push_back(e);
  }
  *out = l.release();
  return MB_OK;
}

size_t MB_CALL mb_model_list_count(const mb_model_list* l) {
  auto* list = as_list(l);
  return list != nullptr ? list->entries.size() : 0;
}

mb_err MB_CALL mb_model_list_get(const mb_model_list* l, size_t index, mb_model_entry* out) {
  auto* list = as_list(l);
  if (list == nullptr || out == nullptr) return MB_E_INVALID_ARG;
  if (index >= list->entries.size()) return MB_E_NOT_FOUND;
  *out = list->entries[index];
  return MB_OK;
}

mb_err MB_CALL mb_model_list_free(mb_model_list* l) {
  auto* list = as_list(l);
  if (l == nullptr) return MB_OK;  // no-op,幂等
  if (list == nullptr) return MB_E_INVALID_ARG;
  list->magic = 0;
  delete list;
  return MB_OK;
}

std::uint32_t MB_CALL mb_model_list_protocol_version(void) { return MB_PROTOCOL_VERSION_NOW; }

mb_err MB_CALL mb_session_load_model(mb_session* s, const char* path, const char* name) {
  auto* sess = as_session(s);
  if (sess == nullptr || sess->renderer == nullptr || path == nullptr) return MB_E_INVALID_ARG;
  const std::string p(path);
  if (p.empty()) return MB_E_INVALID_ARG;
  const std::string n = name != nullptr ? std::string(name) : std::string();

  std::string err;
  // 失败时 SceneRenderer 保证**保持原状**(不回落立方体、不留半成品)。
  if (!sess->renderer->load_model(p, n, &err)) {
    set_status(sess, "load_model 失败: " + err);
    emit_log(MB_LOG_ERROR, "mb_session_load_model: %s", err.c_str());
    if (err.find("路径为空") != std::string::npos) return MB_E_INVALID_ARG;
    if (err.find("设备未初始化") != std::string::npos) return MB_E_NOT_INITIALIZED;
    return MB_E_IO;
  }
  set_status(sess, "");
  return MB_OK;
}

mb_err MB_CALL mb_session_clear_model(mb_session* s) {
  auto* sess = as_session(s);
  if (sess == nullptr || sess->renderer == nullptr) return MB_E_INVALID_ARG;
  sess->renderer->clear_model();
  return MB_OK;
}

// ============================================================================
// 10. 状态与性能
// ============================================================================

size_t MB_CALL mb_session_status_text(const mb_session* s, char* buf, size_t buf_size) {
  auto* sess = as_session(s);
  if (sess == nullptr || sess->renderer == nullptr) return 0;
  const std::string text = sess->renderer->status_text();
  return copy_out(text, buf, buf_size);
}

mb_perf_snapshot MB_CALL mb_session_perf(const mb_session* s) {
  mb_perf_snapshot p{};
  p.struct_size = sizeof(mb_perf_snapshot);
  auto* sess = as_session(s);
  if (sess == nullptr || sess->renderer == nullptr) return p;
  p.msaa_effective = sess->renderer->effective_msaa_samples();
  // frames_total 等统计目前只有 status_text 里的 frames=;/perf 的完整实现见
  // docs/UI_SDK_SPLIT.md §8 的后续项。**不编造数字**:拿不到就留 0。
  return p;
}

}  // extern "C"
