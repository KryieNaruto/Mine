// MagicBlocks —— JNI 桥:**一层薄翻译**
//
// ============================================================================
// 这个文件现在只做三件事
// ============================================================================
//   1. jlong ↔ mb_session*           (不透明句柄,不解引用)
//   2. jstring/jfloat/... ↔ C 标量   (UTF-8 进、UTF-8 出)
//   3. 把 mb_log_cb 接到 Kotlin 的 MbRenderer.logNative
//
// **不再** include 任何 SDK 内部头,也不再直接摸任何 C++ 类 —— 全部经 mb_capi.h。
// 这样做的收益不是"好看",而是可验证的:
//   - 这里任何一个签名改动都只依赖 mb_capi.h 的稳定性;
//   - 换成 JNA 时,Kotlin 侧语义不变(JNA 直接调同一组 mb_capi_*,这个文件整个可以删掉);
//   - 想审"UI 到底碰了 SDK 什么",只需读一个头文件。
//
// 历史上这里的问题(2026-11-11 t3 之前):
//   - FindClass("com/magicblocks/pad/MbRenderer") + GetStaticMethodID("logNative", "...")
//     —— 类改名/混淆即静默丢日志,且 SDK 反向认识了一个 UI 类名;
//   - nativeListModels 用一个 '\t' 分隔的字符串协议,字段分隔符与模型名里的制表符
//     没有任何转义 —— 名字里带 '\t' 就把整行拆错;
//   - 每帧都穿过 C++ 类,SDK 改虚类就得重编这个 TU。
//
// 关于 Kotlin 侧的**签名兼容**:原有 13 个 JNI 入口的符号名与参数表**一字未变**
// (nativeCreate/nativeSurfaceCreated/.../nativeWindowFromSurface + logNative 回调),
// 所以任何时刻的 Kotlin 侧都能和这个 .so 对上。t4 新增的只有两个**只读**入口
// (nativeAbiVersion / nativeModelListProtocolVersion),老 Kotlin 不调它们就不受影响。
//
// 一致性怎么守(桌面上可跑,不靠人眼):tools/capi_protocol/protocol.json 里那份
// `jni_entries` 表登记了每个入口的返回类型与逐位参数表,`gen_abi_table.py --check`
// 直接解析本文件的 JNIEXPORT 与之比对;Kotlin 侧的真实 JVM 描述符则由
// MagicBlocks/tests/capi_modellist 用 JVM 反射读编译器生成的 @Metadata 后一起比。
#include <jni.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(MB_ANDROID_USE_NDK)
#include <android/native_window.h>
#include <android/native_window_jni.h>
#endif

#include "magicblocks/mb_capi.h"

namespace {

// 句柄:jlong 里装的是 mb_session*。不解引用 —— 有效性由 mb_session_is_valid 判。
inline jlong pack_session(mb_session* s) { return static_cast<jlong>(reinterpret_cast<std::intptr_t>(s)); }
inline mb_session* unpack_session(jlong h) {
  return reinterpret_cast<mb_session*>(static_cast<std::intptr_t>(h));
}

std::string to_utf8(JNIEnv* env, jstring js) {
  if (js == nullptr) return {};
  const char* raw = env->GetStringUTFChars(js, nullptr);
  if (raw == nullptr) return {};  // OOM:JNI 已挂异常,设为空串
  std::string out(raw);
  env->ReleaseStringUTFChars(js, raw);
  return out;
}

// ============================================================================
// 日志:C ABI 回调 → Kotlin MbRenderer.logNative(String)
//
// 反射部分被**收拢到这一个函数**里,而且它现在只服务于 UI 自己 ——
// SDK 不知道 MbRenderer 这个类存在(SDK 只认识 mb_log_cb)。
// 找不到类/方法时不抛异常(JNI 里抛异常会让后续调用全部失效)。
// ============================================================================
void JNICALL on_native_log(void* user, mb_log_level level, const char* msg) {
  auto* env = static_cast<JNIEnv*>(user);
  if (env == nullptr || msg == nullptr) return;
  jclass cls = env->FindClass("com/magicblocks/pad/MbRenderer");
  if (cls == nullptr) {
    env->ExceptionClear();
    return;
  }
  jmethodID mid = env->GetStaticMethodID(cls, "logNative", "(Ljava/lang/String;)V");
  if (mid == nullptr) {
    env->ExceptionClear();
    env->DeleteLocalRef(cls);
    return;
  }
  // 级别前缀保留在消息里,免得 t4 之前 Kotlin 侧丢信息。
  const char* tag = level == MB_LOG_ERROR ? "[E] "
                    : level == MB_LOG_WARN ? "[W] "
                    : level == MB_LOG_INFO ? "[I] "
                                           : "[D] ";
  std::string text(tag);
  text += msg;
  jstring js = env->NewStringUTF(text.c_str());
  env->CallStaticVoidMethod(cls, mid, js);
  if (env->ExceptionCheck()) env->ExceptionClear();
  env->DeleteLocalRef(js);
  env->DeleteLocalRef(cls);
}

// 每个 JNI 入口都先注册(幂等)一次日志回调:回调的 user 需要当前线程的 JNIEnv,
// 而 JNIEnv 是线程本地且只在调用期间有效 —— 所以只能在有 env 的时候绑定。
// 渲染线程上的日志因此不会回调(详见 docs/UI_SDK_SPLIT.md §5.6 的线程说明)。
//
// ============================================================================
// 这里有一个**踩过的坑**，别改回去
// ============================================================================
// 第一版写的是 `d.user = env;`，直接把当前调用的 JNIEnv* 存进回调。
// 看起来"每次调用都会重绑一次，所以总是新鲜的"——错:
//   - JNIEnv* 是**线程本地且只在本次 JNI 调用期间有效**；
//   - 回调是在**另一次**调用(或渲染线程)里触发的，那时 user 已经是个野指针；
//   - 表现:mb_capi_smoke 里 nativeCreate 一进来就在日志回调上 C0000005，
//     而崩溃点离"user 过期"这件事实隔着好几层。
//
// 正确做法是 JNI 层的标准写法:每个线程一个 TLS 槽,槽里的 JNIEnv 在**每次 JNI 调用
// 期间**刷新，并在调用结束(bind_log_scope 析构)时置空 —— 这样"离开调用之后就没有
// 有效 env"是**显式**的，而不是靠指针恰好还没被复用。渲染线程(Android 上由
// EngineSurfaceView 持有的那一条)需要在那边自己 AttachCurrentThread 后调一次
// `bind_log`(见 docs/UI_SDK_SPLIT.md §5.6)。
thread_local JNIEnv* tls_env = nullptr;

void bind_log(JNIEnv* env) {
  tls_env = env;
  std::fflush(stdout);
  mb_log_cb_desc d{};
  d.struct_size = sizeof(d);
  d.cb = on_native_log;
  d.user = reinterpret_cast<void*>(env);
  mb_set_log_callback(&d);
}

// RAII:进入 JNI 入口时绑定本线程 env,离开时清掉"当前线程的 env"。
// 注意只清**本线程**的:别的线程若已 Attach，它们的槽各自有效。
struct BindLogScope {
  explicit BindLogScope(JNIEnv* env) { bind_log(env); }
  ~BindLogScope() { tls_env = nullptr; }
};

}  // namespace

extern "C" {

// ============================================================================
// 版本入口 —— Kotlin 侧在 loadLibrary 之后**第一个**调的就是这两个
// ============================================================================
//
// 一次 JNI 调用只回一个值,而 UI 需要知道两件**不同**的事:ABI 版本(26 个 C 入口的
// 名字/签名/结构体布局)与模型列表文本协议版本。把它们编码进一个整数(高位 ABI、
// 低位协议)看着省一次调用,实际是把"两个独立演化的版本"绑成一条曲线:以后协议
// 单独 bump 也要假装动了 ABI —— 那正是本次 bug 的同一种思路。所以给两个入口。
//
// 为什么不用 `nativeEcho(String)` 那种通用反射桥:那等于把"UI 认识 SDK 内部"的门
// 重新打开一扇(UI 可以拿它探任意函数名),而且 C ABI 的 26 个入口不再是被审的
// 唯一面。多写 6 行 JNI 换"UI 只碰白名单入口",值。
JNIEXPORT jint JNICALL
Java_com_magicblocks_pad_MbRenderer_nativeAbiVersion(JNIEnv*, jclass) {
  return static_cast<jint>(mb_abi_version());
}

JNIEXPORT jint JNICALL
Java_com_magicblocks_pad_MbRenderer_nativeModelListProtocolVersion(JNIEnv*, jclass) {
  return static_cast<jint>(mb_model_list_protocol_version());
}

JNIEXPORT jlong JNICALL
Java_com_magicblocks_pad_MbRenderer_nativeCreate(JNIEnv* env, jclass) {
  BindLogScope log_scope(env);
  mb_session_desc d = mb_session_desc_default();
  mb_session* s = nullptr;
  const mb_err e = mb_session_create(&d, &s);
  // 语义保持:失败也返回非 0 句柄(nativeStatus 能读原因)。区别是失败现在**可知**了,
  // 而且状态文本由 mb_capi 在 create 内部写好,这里不用再抄一遍。
  if (e != MB_OK) {
    // 走注册的回调把错误名打进 logcat;具体原因用 nativeStatus 读。
    on_native_log(env, MB_LOG_ERROR, mb_err_str(e));
  }
  return pack_session(s);
}

JNIEXPORT jboolean JNICALL
Java_com_magicblocks_pad_MbRenderer_nativeSurfaceCreated(JNIEnv* env, jclass, jlong handle,
                                                         jlong window, jint width, jint height) {
  BindLogScope log_scope(env);
  mb_session* s = unpack_session(handle);
  if (s == nullptr) return JNI_FALSE;
  void* win = reinterpret_cast<void*>(static_cast<std::intptr_t>(window));
  const mb_err e = mb_session_attach_surface(s, MB_SURFACE_ANDROID, win,
                                             static_cast<std::uint32_t>(width),
                                             static_cast<std::uint32_t>(height));
  return e == MB_OK ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_magicblocks_pad_MbRenderer_nativeSurfaceDestroyed(JNIEnv*, jclass, jlong handle) {
  mb_session* s = unpack_session(handle);
  if (s != nullptr) (void)mb_session_detach_surface(s);
}

JNIEXPORT jboolean JNICALL
Java_com_magicblocks_pad_MbRenderer_nativeRenderFrame(JNIEnv* env, jclass, jlong handle, jfloat qx,
                                                      jfloat qy, jfloat qz, jfloat qw, jfloat scale,
                                                      jfloat pan_x, jfloat pan_y) {
  BindLogScope log_scope(env);
  mb_session* s = unpack_session(handle);
  if (s == nullptr) return JNI_FALSE;
  mb_pose pose{};
  pose.q[0] = qx;
  pose.q[1] = qy;
  pose.q[2] = qz;
  pose.q[3] = qw;
  pose.scale = scale;
  pose.pan[0] = pan_x;
  pose.pan[1] = pan_y;
  const mb_err e = mb_session_render(s, pose);
  // 语义保持:旧 Kotlin 只认 true/false;"这帧丢弃"仍算 false(下一帧就好)。
  return e == MB_OK ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jstring JNICALL
Java_com_magicblocks_pad_MbRenderer_nativeStatus(JNIEnv* env, jclass, jlong handle) {
  mb_session* s = unpack_session(handle);
  if (s == nullptr) return env->NewStringUTF("handle=null");
  const size_t need = mb_session_status_text(s, nullptr, 0);
  if (need == 0) return env->NewStringUTF("handle=null");
  std::string text(need, '\0');
  mb_session_status_text(s, text.data(), text.size());
  if (!text.empty() && text.back() == '\0') text.pop_back();
  return env->NewStringUTF(text.c_str());
}

JNIEXPORT void JNICALL
Java_com_magicblocks_pad_MbRenderer_nativeDestroy(JNIEnv*, jclass, jlong handle) {
  mb_session* s = unpack_session(handle);
  (void)mb_session_destroy(s);
}

// ---- MSAA ----

JNIEXPORT void JNICALL
Java_com_magicblocks_pad_MbRenderer_nativeSetMsaa(JNIEnv*, jclass, jlong handle, jint samples) {
  mb_session* s = unpack_session(handle);
  if (s == nullptr) return;
  mb_msaa m = MB_MSAA_OFF;
  switch (samples) {
    case 1: m = MB_MSAA_OFF; break;
    case 4: m = MB_MSAA_4X; break;
    case 8: m = MB_MSAA_8X; break;
    default: m = samples <= 1 ? MB_MSAA_OFF : MB_MSAA_AUTO; break;
  }
  (void)mb_session_set_msaa(s, m);
}

JNIEXPORT jint JNICALL
Java_com_magicblocks_pad_MbRenderer_nativeMsaaSamples(JNIEnv*, jclass, jlong handle) {
  mb_session* s = unpack_session(handle);
  std::uint32_t n = 1;
  if (s != nullptr && mb_session_msaa_effective(s, &n) != MB_OK) n = 1;
  return static_cast<jint>(n);
}

// 兼容旧格式:"1,4,8"。t4 改为 mb_session_msaa_supported 的数组形式后可以删掉。
JNIEXPORT jstring JNICALL
Java_com_magicblocks_pad_MbRenderer_nativeSupportedMsaa(JNIEnv* env, jclass, jlong handle) {
  mb_session* s = unpack_session(handle);
  std::vector<std::uint32_t> levels;
  size_t count = 0;
  if (s != nullptr && mb_session_msaa_supported(s, nullptr, 0, &count) == MB_OK && count > 0) {
    levels.assign(count, 1u);
    if (mb_session_msaa_supported(s, levels.data(), levels.size(), &count) != MB_OK) count = 0;
    levels.resize(count);
  }
  std::string out;
  if (levels.empty()) {
    out = "1";
  } else {
    for (size_t i = 0; i < levels.size(); ++i) {
      if (i) out += ',';
      out += std::to_string(levels[i]);
    }
  }
  return env->NewStringUTF(out.c_str());
}

// ---- 预设模型列表 ----
//
// ============================================================================
// 文本线格式 —— 这里**不写任何字面量**,全部取自 mb_capi.h §10 的协议常量
// ============================================================================
//   第一行:  MB_PROTOCOL_STATUS_PREFIX + <错误码名>          （恒有,成功时是 MB_OK）//   此后:    每个模型 MB_PROTOCOL_TOKENS_PER_MODEL 行,一行一个字段,
//            行序 = mb_protocol_field（name/path/bytes/ok/message）;
//            字段里的 MB_PROTOCOL_SANITIZE_CHARS 换成空格。
//
// 为什么字段各占一行而不是用 '\t' 拼成一行:t3 之前的旧格式里,模型名或原因
// 一旦含制表符/换行,整行就被拆错,而且没有任何转义 —— 那是一个"数据里带分隔符
// 就静默错"的协议。一行一个字段之后,唯一还需要防的就是**换行**本身,而它被
// sanitize 规则明确换掉了。
//
// 与 Kotlin 侧的一致性由 tools/capi_protocol/ 守住:
//   protocol.json 是唯一事实来源 → 生成 golden 语料 → 桌面程序把同一批字节
//   同时喂给 C++ 拼装与**真实 Kotlin 编译产物**(JVM 反射调 MbAbiProtocol.parseModelList)。
// 改这里任何一个字符,capi_protocol 的 golden 与 Kotlin 解析会同时变红。
//
// 历史事故（t3 引入、t4 修掉，别改回去）：
//   t3 把首行从 "ok" 改成 "err=MB_E_*"，Kotlin 侧仍在比 "ok"，于是每次扫描
//   **无条件**走 Failed，界面上还显示字面量 "err=MB_OK" —— 宣称失败却写着成功。
//   桌面全绿，因为没有任何一层在验 Kotlin 侧解析。
JNIEXPORT jstring JNICALL
Java_com_magicblocks_pad_MbRenderer_nativeListModels(JNIEnv* env, jclass, jstring directory) {
  if (directory == nullptr) {
    // 错误码名也走 mb_err_str(),不手写 —— 手写就等于又开了一份"错误码名"副本。
    return env->NewStringUTF((std::string(MB_PROTOCOL_STATUS_PREFIX) + mb_err_str(MB_E_INVALID_ARG)).c_str());
  }
  const std::string dir = to_utf8(env, directory);

  mb_model_list* list = nullptr;
  const mb_err e = mb_session_scan_models(nullptr, dir.c_str(), &list);

  std::string out = MB_PROTOCOL_STATUS_PREFIX;
  out += mb_err_str(e);
  out += MB_PROTOCOL_LINE_SEP_STR;
  if (e == MB_OK && list != nullptr) {
    const size_t n = mb_model_list_count(list);
    auto sanitize = [](const char* src, size_t cap) {
      // 定长字段可能没有 NUL,自己找边界(不用 strnlen:那在 C++ 里不在 <cstring> 的保证内)。
      size_t len = 0;
      while (len < cap && src[len] != '\0') ++len;
      std::string v(src, len);
      for (char& c : v) {
        if (std::strchr(MB_PROTOCOL_SANITIZE_CHARS, c) != nullptr && c != '\0') {
          c = MB_PROTOCOL_SANITIZE_CHAR;
        }
      }
      return v;
    };
    auto put = [&out](const std::string& field) {
      out += field;
      out += MB_PROTOCOL_LINE_SEP_STR;
    };
    for (size_t i = 0; i < n; ++i) {
      mb_model_entry entry{};
      if (mb_model_list_get(list, i, &entry) != MB_OK) continue;
      // 顺序必须与 mb_protocol_field 一致:name / path / bytes / ok / message。
      put(sanitize(entry.name, sizeof(entry.name)));                  // FIELD_NAME
      put(sanitize(entry.path, sizeof(entry.path)));                  // FIELD_PATH
      put(std::to_string(entry.bytes));                               // FIELD_BYTES
      put(entry.ok != 0 ? std::string("1") : std::string("0"));       // FIELD_OK
      put(sanitize(entry.message, sizeof(entry.message)));            // FIELD_MESSAGE
    }
  }
  (void)mb_model_list_free(list);  // 对 NULL 是 no-op
  return env->NewStringUTF(out.c_str());
}

JNIEXPORT jboolean JNICALL
Java_com_magicblocks_pad_MbRenderer_nativeLoadModel(JNIEnv* env, jclass, jlong handle,
                                                    jstring path, jstring name) {
  BindLogScope log_scope(env);
  mb_session* s = unpack_session(handle);
  if (s == nullptr || path == nullptr) return JNI_FALSE;
  const std::string p = to_utf8(env, path);
  const std::string n = to_utf8(env, name);
  const mb_err e = mb_session_load_model(s, p.c_str(), name != nullptr ? n.c_str() : nullptr);
  return e == MB_OK ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_magicblocks_pad_MbRenderer_nativeClearModel(JNIEnv*, jclass, jlong handle) {
  mb_session* s = unpack_session(handle);
  if (s != nullptr) (void)mb_session_clear_model(s);
}

// Surface -> ANativeWindow*。ANativeWindow_fromSurface 会给引用计数 +1,
// 这**一个**引用由引擎持有:attach 成功后一直在用,detach 时由 SceneRenderer 还掉。
// 所以这里**不能** release。这一步与 C ABI 无关(它只碰 Android 平台类型),留在 JNI 层。
JNIEXPORT jlong JNICALL
Java_com_magicblocks_pad_MbRenderer_nativeWindowFromSurface(JNIEnv* env, jclass,
                                                            jobject surface) {
  if (surface == nullptr) return 0;
#if defined(MB_ANDROID_USE_NDK)
  ANativeWindow* win = ANativeWindow_fromSurface(env, surface);
  return static_cast<jlong>(reinterpret_cast<std::intptr_t>(win));
#else
  (void)env;
  return 0;
#endif
}

}  // extern "C"

// ============================================================================
// 桌面语料的拼装钩子(只在 MB_JNI_TRANSPORT_HARNESS 里编)
// ============================================================================
//
// 为什么要有它:协议是"两侧合写一个字符串",而两侧的**拼装代码**在真实数据上根本
// 凑不出想测的形状 —— 造一个"目录不存在"很容易,造一个"模型名里带换行"就得上
// 文件系统 + 特定命名。更要命的是**Kotlin 侧无法在桌面上跑**:它 import 了 android.*。
//
// 所以把"给定一组条目 → 拼成协议文本"这一段从 nativeListModels 里抽出来成
// transport_append_models(),再开一个只在测试定义里可见的入口,让桌面程序能:
//   1. 用它拼出与**真机同一段代码**产出的字节,与 golden 语料逐字节比;
//   2. 把同一批字节交给**真实 Kotlin 编译产物**(JVM 反射调 MbAbiProtocol.parseModelList)。
// 三方吃同一批字节,协议漂移就不可能只漂一侧。
//
// 它不进真机产物(没有 MB_JNI_TRANSPORT_HARNESS 就没有这段),
// 所以 Android .so 的导出表里不会多出一个可以"注入任意模型列表"的入口。
#if defined(MB_JNI_TRANSPORT_HARNESS)
namespace {

// 与 Java_com_magicblocks_pad_MbRenderer_nativeListModels 用的是同一段拼装代码。
void transport_append_models(std::string& out, const std::vector<mb_model_entry>& entries) {
  out += MB_PROTOCOL_LINE_SEP_STR;
  for (const mb_model_entry& entry : entries) {
    auto sanitize = [](const char* src, size_t cap) {
      size_t len = 0;
      while (len < cap && src[len] != '\0') ++len;
      std::string v(src, len);
      for (char& c : v) {
        if (std::strchr(MB_PROTOCOL_SANITIZE_CHARS, c) != nullptr && c != '\0') {
          c = MB_PROTOCOL_SANITIZE_CHAR;
        }
      }
      return v;
    };
    out += sanitize(entry.name, sizeof(entry.name));
    out += MB_PROTOCOL_LINE_SEP_STR;
    out += sanitize(entry.path, sizeof(entry.path));
    out += MB_PROTOCOL_LINE_SEP_STR;
    out += std::to_string(entry.bytes);
    out += MB_PROTOCOL_LINE_SEP_STR;
    out += entry.ok != 0 ? "1" : "0";
    out += MB_PROTOCOL_LINE_SEP_STR;
    out += sanitize(entry.message, sizeof(entry.message));
    out += MB_PROTOCOL_LINE_SEP_STR;
  }
}

mb_model_entry make_entry(const char* name, const char* path, unsigned long long bytes, int ok,
                          const char* message) {
  mb_model_entry e{};
  std::snprintf(e.name, sizeof(e.name), "%s", name != nullptr ? name : "");
  std::snprintf(e.path, sizeof(e.path), "%s", path != nullptr ? path : "");
  std::snprintf(e.message, sizeof(e.message), "%s", message != nullptr ? message : "");
  e.bytes = bytes;
  e.ok = ok != 0 ? 1u : 0u;
  return e;
}

}  // namespace

extern "C" {

// 首行到第一个条目的前一行(含换行):"err=<code>\n"。`code` 传 MB_OK 时就是成功形态但 0 条目。
JNIEXPORT jstring JNICALL
Java_com_magicblocks_pad_MbRendererTransportTest_nativeHeaderForErr(JNIEnv* env, jclass, jint code) {
  std::string out = MB_PROTOCOL_STATUS_PREFIX;
  out += mb_err_str(static_cast<mb_err>(code));
  out += MB_PROTOCOL_LINE_SEP_STR;
  return env->NewStringUTF(out.c_str());
}

// header + `count` 个条目。每个条目的 5 个字段来自 5 个平行数组(按下标对齐),
// 用平行数组而不是编码字符串,是为了让测试能塞进真正的 TAB / LF 而不经过任何转义。
JNIEXPORT jstring JNICALL
Java_com_magicblocks_pad_MbRendererTransportTest_nativeBuildModelList(
    JNIEnv* env, jclass, jint code, jobjectArray names, jobjectArray paths, jlongArray bytes,
    jintArray oks, jobjectArray messages) {
  std::string out = MB_PROTOCOL_STATUS_PREFIX;
  out += mb_err_str(static_cast<mb_err>(code));
  const jsize count = names == nullptr ? 0 : env->GetArrayLength(names);

  std::vector<jstring> jn(static_cast<size_t>(count));
  std::vector<jstring> jp(static_cast<size_t>(count));
  std::vector<jstring> jm(static_cast<size_t>(count));
  std::vector<jlong> jb(static_cast<size_t>(count), 0);
  std::vector<jint> jo(static_cast<size_t>(count), 0);

  // 逐个 Get*Region / GetObjectArrayElement:桌面 stub 只实现这些成员,
  // 用了别的就链接期炸 —— 那正是"替身没有偷偷兜底"的证据。
  if (bytes != nullptr && count > 0) env->GetLongArrayRegion(bytes, 0, count, jb.data());
  if (oks != nullptr && count > 0) env->GetIntArrayRegion(oks, 0, count, jo.data());
  for (jsize i = 0; i < count; ++i) {
    if (names != nullptr) jn[static_cast<size_t>(i)] = env->GetObjectArrayElement(names, i);
    if (paths != nullptr) jp[static_cast<size_t>(i)] = env->GetObjectArrayElement(paths, i);
    if (messages != nullptr) jm[static_cast<size_t>(i)] = env->GetObjectArrayElement(messages, i);
  }

  std::vector<mb_model_entry> entries;
  entries.reserve(static_cast<size_t>(count));
  for (jsize i = 0; i < count; ++i) {
    const std::string n = to_utf8(env, jn[static_cast<size_t>(i)]);
    const std::string p = to_utf8(env, jp[static_cast<size_t>(i)]);
    const std::string m = to_utf8(env, jm[static_cast<size_t>(i)]);
    entries.push_back(make_entry(n.c_str(), p.c_str(),
                                 static_cast<unsigned long long>(jb[static_cast<size_t>(i)]),
                                 jo[static_cast<size_t>(i)], m.c_str()));
  }
  transport_append_models(out, entries);
  for (jsize i = 0; i < count; ++i) {
    if (jn[static_cast<size_t>(i)] != nullptr) env->DeleteLocalRef(jn[static_cast<size_t>(i)]);
    if (jp[static_cast<size_t>(i)] != nullptr) env->DeleteLocalRef(jp[static_cast<size_t>(i)]);
    if (jm[static_cast<size_t>(i)] != nullptr) env->DeleteLocalRef(jm[static_cast<size_t>(i)]);
  }
  return env->NewStringUTF(out.c_str());
}

}  // extern "C"
#endif  // MB_JNI_TRANSPORT_HARNESS
