// MagicBlocks —— mb_capi 桌面冒烟:真的链接、真的调用、真的逐个走 JNI 入口
//
// ============================================================================
// 为什么这个程序必须存在(而不是"编译过就算完")
// ============================================================================
// t2 的教训:mb_capi.h 的 7 格编译矩阵全绿,但 C++ 侧 26 个符号全被 mangle,
// 链接期 LNK2019 —— 编译期 100% 看不见。只验"头文件能编译"等于没验。
//
// 这个程序做三件事:
//   A. 链接层:直接调用 mb_abi_version / mb_session_create / destroy 等真符号。
//      任何一个名字对不上(缺 extern "C"、少导出、函数被改名)→ 链接期直接失败。
//   B. ABI 层:逐个入口验证错误码、字符串两段式、定长结构体尺寸、无效句柄不崩。
//   C. JNI 薄层:直接调 Java_com_magicblocks_pad_MbRenderer_* 这 12 个 C 函数
//      (用 tests/capistub/jni.h 这种最小 JNI 头),证明 jni_bridge.cpp 里每个
//      函数体真的只经 mb_capi_* 工作,且错误码语义传得出去。
//
// 运行环境:桌面真实 GPU(创建会话需要 Vulkan)。没有 GPU 时 B/C 里的设备相关断言
// 会**如实**报"设备不可用"并跳过,而不是伪造通过。
// ⚠ 这里的包含顺序是**故意的**,而且它本身就是一个断言:
//   windows.h → wingdi.h 里有 `#define MB_OK 0L`(MessageBox 返回值),与 ABI 的
//   `MB_OK = 0` 撞名。mb_capi.h §0 用 push_macro/undef/pop_macro 在自己作用域内压掉它,
//   所以**无论谁先 include**,头文件都能编译,而且调用方原本的 MB_OK 不被改坏。
//   A. 段里的 check_msgbox_mb_ok_untouched() 就是在验后半句。
#include <windows.h>

#include "magicblocks/mb_capi.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

#include <jni.h>  // 真的 NDK 用 NDK 的;桌面自测用 tests/capistub/jni.h(形状与 NDK 一致)

// JNI 入口(在 jni_bridge.cpp 里定义,C 链接)
extern "C" {
jlong Java_com_magicblocks_pad_MbRenderer_nativeCreate(JNIEnv*, jclass);
jboolean Java_com_magicblocks_pad_MbRenderer_nativeSurfaceCreated(JNIEnv*, jclass, jlong, jlong,
                                                                  jint, jint);
void Java_com_magicblocks_pad_MbRenderer_nativeSurfaceDestroyed(JNIEnv*, jclass, jlong);
jboolean Java_com_magicblocks_pad_MbRenderer_nativeRenderFrame(JNIEnv*, jclass, jlong, jfloat,
                                                               jfloat, jfloat, jfloat, jfloat,
                                                               jfloat, jfloat);
jstring Java_com_magicblocks_pad_MbRenderer_nativeStatus(JNIEnv*, jclass, jlong);
void Java_com_magicblocks_pad_MbRenderer_nativeDestroy(JNIEnv*, jclass, jlong);
void Java_com_magicblocks_pad_MbRenderer_nativeSetMsaa(JNIEnv*, jclass, jlong, jint);
jint Java_com_magicblocks_pad_MbRenderer_nativeMsaaSamples(JNIEnv*, jclass, jlong);
jstring Java_com_magicblocks_pad_MbRenderer_nativeSupportedMsaa(JNIEnv*, jclass, jlong);
jstring Java_com_magicblocks_pad_MbRenderer_nativeListModels(JNIEnv*, jclass, jstring);
jboolean Java_com_magicblocks_pad_MbRenderer_nativeLoadModel(JNIEnv*, jclass, jlong, jstring,
                                                             jstring);
void Java_com_magicblocks_pad_MbRenderer_nativeClearModel(JNIEnv*, jclass, jlong);
jlong Java_com_magicblocks_pad_MbRenderer_nativeWindowFromSurface(JNIEnv*, jclass, jobject);
}

namespace {

int g_pass = 0;
int g_fail = 0;
int g_skip = 0;

void check(bool cond, const char* what) {
  std::printf(" .. %s\n", what);  // 逐项打印:崩溃点才定位得到(Windows 下 stdout 重定向是全缓冲)
  std::fflush(stdout);
  if (cond) {
    ++g_pass;
  } else {
    ++g_fail;
    std::printf("  FAIL  %s\n", what);
  }
}

void skip(const char* what) {
  ++g_skip;
  std::printf("  SKIP  %s\n", what);
}

// ---------------------------------------------------------------- JNIEnv
//
// JNIEnv 的实现由 tests/capistub/jni_stub.cpp 提供(成员函数形态,与 NDK 同形)。
// 它把 MbRenderer.logNative 收到的消息存进内存,这里用 mb_test_jni_last_log() 取。
// 之所以不放一个 nullptr 表:jni_bridge.cpp 里任何一处多用了不在替身里的成员,
// 都必须在**链接期**炸 —— 那才是"替身没有偷偷帮它兜底"的证据。
// 注意:`JNIEnv` 上下文用**堆对象**,不是全局对象。
// 真机上 user_data 是 JNIEnv*(进程/线程栈或模块内的地址);全局对象位于 .data,
// 地址低 32 位可能全零,个别校验会把它当"空指针" —— 用堆对象更贴近真机形状。
JNIEnv& env() {
  static JNIEnv* s = new JNIEnv();
  return *s;
}

// 把一个 C 字符串"变成 jstring":替身的 GetStringUTFChars 直接把它当 const char* 用,
// 所以这里要求调用方传生命周期足够长的字面量(见下面的用法)。
jstring mkstr(const char* s) { return reinterpret_cast<jstring>(const_cast<char*>(s)); }

// 最近一次 NewStringUTF 的内容 / 最近一次 logNative 的消息(都由 capistub 记录)。
extern "C" const char* mb_test_jni_last_log();
extern "C" const char* mb_test_jni_last_string();
std::string last_string() { return mb_test_jni_last_string(); }
std::string last_log() { return mb_test_jni_last_log(); }

// ---------------------------------------------------------------- MB_OK 宏卫生
//
// mb_capi.h 在自己作用域内压掉 Windows 的 `#define MB_OK 0L`,结束时 pop 回来。
// 这里验证"调用方原本的 MB_OK 没被改坏":Windows 把 MB_OK 定义成 `0L`(long 字面量,
// 不是 int 字面量)。用 decltype 抓它的类型 —— 若恢复失败,这里会拿到 int,断言就红。
#if defined(_WIN32)
constexpr bool msgbox_mb_ok_is_platform_macro() {
#if defined(MB_OK)
  // WinUser.h: `#define MB_OK 0x00000000L`(值 0,文本后缀 L)。
  // 恢复失败时 MB_OK 会退化成头文件里的枚举常量:值同样是 0,但**类型是 int**。
  // 所以这里比类型:必须与 `0x00000000L` 同型(decltype 对纯右值不求 const)。
  return std::is_same<decltype(MB_OK), decltype(0x00000000L)>::value && MB_OK == 0;
#else
  return false;  // 被永久 undef 掉 = 调用方的 MessageBox 代码会编译不过
#endif
}
#endif
}  // namespace

int main() {
  std::printf("== A. 链接层(真符号,缺一个就是 LNK2019)\n");
  const std::uint32_t v = mb_abi_version();
  check(v == MB_ABI_VERSION, "mb_abi_version() == MB_ABI_VERSION");
  check(v != 0, "mb_abi_version() != 0");
  check(mb_abi_version_string(nullptr, 0) == 6, "version string 需要 6 字节(\"1.0.0\"+NUL)");
  {
    char buf[8] = {};
    check(mb_abi_version_string(buf, sizeof(buf)) == 6, "version string 两段式:所需长度");
    check(std::strcmp(buf, "1.0.0") == 0, "version string 内容为 1.0.0");
    char tiny[3] = {};
    check(mb_abi_version_string(tiny, sizeof(tiny)) == 6, "缓冲区太短仍返回所需长度");
    check(tiny[2] == '\0', "缓冲区太短时仍 NUL 结尾");
  }

  std::printf("== B. ABI 层:错误码/句柄/尺寸\n");
  check(MB_OK == 0, "MB_OK == 0");
#if defined(_WIN32)
  check(msgbox_mb_ok_is_platform_macro(),
        "调用方的 windows.h MB_OK(0L, long)在 include mb_capi.h 后未被改坏");
#endif
  check(std::strcmp(mb_err_str(MB_E_NO_SURFACE), "MB_E_NO_SURFACE") == 0, "mb_err_str 稳定名");
  check(mb_err_str(static_cast<mb_err>(999)) != nullptr, "越界错误码返回非 NULL");
  check(std::strcmp(mb_err_str(static_cast<mb_err>(999)), "MB_E_UNKNOWN") == 0, "越界 → MB_E_UNKNOWN");
  check(sizeof(mb_pose) == 28, "mb_pose == 28");
  check(sizeof(mb_session_desc) == 48, "mb_session_desc == 48");
  check(sizeof(mb_model_entry) == 464, "mb_model_entry == 464");
  check(sizeof(mb_perf_snapshot) == 40, "mb_perf_snapshot == 40");

  // 无效句柄:全部必须返回错误码而不是崩
  check(mb_session_is_valid(nullptr) == 0, "NULL 句柄无效");
  check(mb_session_destroy(nullptr) == MB_OK, "destroy(NULL) 幂等 no-op");
  check(mb_session_ready(nullptr) == MB_E_INVALID_ARG, "ready(NULL) → INVALID_ARG");
  check(mb_session_render(nullptr, mb_pose_default()) == MB_E_INVALID_ARG, "render(NULL) → INVALID_ARG");
  check(mb_session_detach_surface(nullptr) == MB_E_INVALID_ARG, "detach(NULL) → INVALID_ARG");
  check(mb_session_status_text(nullptr, nullptr, 0) == 0, "status_text(NULL) → 0");
  {
    const mb_perf_snapshot p = mb_session_perf(nullptr);
    check(p.struct_size == sizeof(mb_perf_snapshot), "perf(NULL) 仍填 struct_size");
  }

  // 逗号运算符陷阱(文档 §5.7 记录过):这里故意用 (a, b) 形式,必须能过编译
  check((mb_session_is_valid(nullptr), mb_session_destroy(nullptr)) == MB_OK, "逗号表达式可用");

  // 姿态默认值:单位四元数
  {
    const mb_pose p = mb_pose_default();
    check(p.q[3] == 1.f && p.q[0] == 0.f && p.scale == 1.f, "mb_pose_default 单位四元数");
  }

  // create 的参数校验(不需 GPU)
  {
    mb_session* s = reinterpret_cast<mb_session*>(0x1);
    check(mb_session_create(nullptr, &s) == MB_E_INVALID_ARG, "create(NULL desc) → INVALID_ARG");
    check(s == nullptr, "失败时 *out 被置 NULL");
    mb_session_desc bad = mb_session_desc_default();
    bad.struct_size = 4;
    check(mb_session_create(&bad, &s) == MB_E_INVALID_ARG, "struct_size 不对 → INVALID_ARG");
    bad = mb_session_desc_default();
    bad.msaa = 99;
    check(mb_session_create(&bad, &s) == MB_E_INVALID_ARG, "msaa 越界 → INVALID_ARG(不静默改写)");
  }

  std::printf("== C. 模型目录:空目录 vs 目录不存在(错误码必须可区分)\n");
  {
    mb_model_list* l = reinterpret_cast<mb_model_list*>(0x1);
    const mb_err e_missing = mb_session_scan_models(nullptr, "Z:\\definitely\\not\\here", &l);
    check(e_missing == MB_E_NOT_FOUND, "目录不存在 → MB_E_NOT_FOUND");
    check(l == nullptr, "失败时 *out 置 NULL");
    check(mb_model_list_free(nullptr) == MB_OK, "list_free(NULL) 幂等");
    check(mb_model_list_count(nullptr) == 0, "count(NULL) == 0");
    mb_model_entry ent{};
    check(mb_model_list_get(nullptr, 0, &ent) == MB_E_INVALID_ARG, "get(NULL list) → INVALID_ARG");
    // 空目录 → MB_OK + count==0。用一个真的临时目录,不伪造:
    // "目录不存在"与"目录存在但为空"必须给出可区分的错误码(旧实现两者都是 IoError)。
    {
      wchar_t tmpdir[MAX_PATH] = {};
      wchar_t name[MAX_PATH] = {};
      char narrow[MAX_PATH * 3] = {};
      const DWORD n = ::GetTempPathW(MAX_PATH, tmpdir);
      const bool made = n > 0 && n < MAX_PATH && ::GetTempFileNameW(tmpdir, L"mbc", 0, name) != 0;
      if (made) {
        // ⚠ 坑:GetTempFileName 建出来的是**文件** `mbc###.tmp`(带扩展名)。
        // 直接删文件、拿这个名字当目录用 → 目录仍然不存在 → 断言会"看起来"是 SDK 的错。
        // 正确做法:删文件 → 再建一个**同名目录**,盘上就有了一个真的空目录。
        ::DeleteFileW(name);
        const bool dir_ok = ::CreateDirectoryW(name, nullptr) != 0;
        ::WideCharToMultiByte(CP_UTF8, 0, name, -1, narrow, sizeof(narrow), nullptr, nullptr);
        mb_model_list* l2 = reinterpret_cast<mb_model_list*>(0x1);
        const mb_err e_empty =
            dir_ok ? mb_session_scan_models(nullptr, narrow, &l2) : MB_E_INTERNAL;
        std::fflush(stdout);
        check(e_empty == MB_OK, "空目录 → MB_OK(与'不存在'可区分)");
        check(l2 != nullptr && mb_model_list_count(l2) == 0, "空目录 count == 0");
        check(mb_model_list_free(l2) == MB_OK, "空目录 list_free");
        ::RemoveDirectoryW(name);
      } else {
        skip("空目录断言(无法创建临时目录)");
      }
    }
  }

  std::printf("== D. 设备相关:create / JNI 薄层(需要桌面 GPU)\n");
  mb_session_desc desc = mb_session_desc_default();
  mb_session* s = nullptr;
  const mb_err ce = mb_session_create(&desc, &s);
  if (ce != MB_OK) {
    // 如实报告,不当成败。这台机器没 GPU 时这里必然走到。
    std::printf("  NOTE  mb_session_create 返回 %s(本机无可用 Vulkan 设备?)\n", mb_err_str(ce));
    check(s != nullptr, "create 失败也交出句柄(status_text 可读原因)");
    if (s != nullptr) {
      char buf[512] = {};
      const size_t need = mb_session_status_text(s, buf, sizeof(buf));
      check(need > 0, "失败句柄的 status_text 非空");
      std::printf("        status: %s\n", buf);
      check(mb_session_destroy(s) == MB_OK, "destroy 失败句柄");
      check(mb_session_is_valid(s) == 0, "destroy 后句柄失效");
    }
    skip("render / attach / msaa 的路由断言(无设备)");
  } else {
    check(s != nullptr, "create 成功交出句柄");
    check(mb_session_is_valid(s) == 1, "新建句柄有效");
    check(mb_session_ready(s) == MB_OK, "create 成功后 ready == MB_OK");

    // 无 surface 时 render 必须是 MB_E_NO_SURFACE(而不是 INVALID_ARG / INTERNAL)
    check(mb_session_render(s, mb_pose_default()) == MB_E_NO_SURFACE, "无 surface render → NO_SURFACE");
    // 全 0 四元数 → INVALID_ARG
    mb_pose zero{};
    check(mb_session_render(s, zero) == MB_E_INVALID_ARG, "全 0 四元数 → INVALID_ARG");

    // attach 参数校验
    check(mb_session_attach_surface(s, MB_SURFACE_ANDROID, nullptr, 320, 240) == MB_E_INVALID_ARG,
          "attach(NULL window) → INVALID_ARG");
    check(mb_session_attach_surface(s, MB_SURFACE_ANDROID, reinterpret_cast<void*>(0x1), 0, 240) ==
              MB_E_INVALID_ARG,
          "attach(宽=0) → INVALID_ARG");
    check(mb_session_attach_surface(s, MB_SURFACE_XCB, reinterpret_cast<void*>(0x1), 320, 240) ==
              MB_E_UNSUPPORTED,
          "attach(XCB 未接线) → UNSUPPORTED(不谎称支持)");

    // MSAA 两段式
    {
      std::uint32_t eff = 0;
      check(mb_session_msaa_effective(s, &eff) == MB_OK, "msaa_effective ok");
      check(eff >= 1, "msaa_effective >= 1");
      size_t count = 0;
      check(mb_session_msaa_supported(s, nullptr, 0, &count) == MB_OK, "msaa_supported 两段式查询");
      check(count >= 1, "至少 1 个档位");
      std::vector<std::uint32_t> lv(count);
      check(mb_session_msaa_supported(s, lv.data(), lv.size(), &count) == MB_OK, "msaa_supported 填充");
      check(count >= 1 && lv[0] == 1, "档位从 1 开始");
      check(mb_session_set_msaa(s, static_cast<mb_msaa>(42)) == MB_E_INVALID_ARG, "set_msaa 越界 → INVALID_ARG");
    }

    check(mb_session_detach_surface(s) == MB_OK, "detach(未挂载)也成功");
    check(mb_session_destroy(s) == MB_OK, "destroy");
    check(mb_session_is_valid(s) == 0, "destroy 后句柄失效");
    check(mb_session_destroy(s) == MB_E_INVALID_ARG, "重复 destroy → INVALID_ARG(不 UB)");
    check(mb_session_ready(s) == MB_E_INVALID_ARG, "失效句柄 ready → INVALID_ARG");
  }

  std::printf("== E. JNI 薄层:12 个入口逐个走一遍(证明只经 mb_capi_*)\n");
  {
    std::fflush(stdout);
    const jlong h = Java_com_magicblocks_pad_MbRenderer_nativeCreate(&env(), nullptr);
    std::fflush(stdout);
    check(h != 0, "nativeCreate 返回非 0(失败也给句柄)");
    mb_session* js = reinterpret_cast<mb_session*>(static_cast<std::intptr_t>(h));
    check(mb_session_is_valid(js) == 1, "nativeCreate 的句柄能过 mb_session_is_valid");

    std::fflush(stdout);
    jstring st = Java_com_magicblocks_pad_MbRenderer_nativeStatus(&env(), nullptr, h);
    std::fflush(stdout);
    check(st != nullptr && !last_string().empty(), "nativeStatus 经 mb_session_status_text 出文本");
    check(last_string().find("frames=") != std::string::npos,
          "nativeStatus 文本是 SceneRenderer::status_text 的真实输出");
    // 日志通路:SDK 只认 mb_log_cb;JNI 层把它接到 MbRenderer.logNative。
    // create 成功/失败都会打一条,这里验证"真的到了 UI 侧",而不是被静默吞掉。
    check(!last_log().empty(), "MB log 回调经 JNI 薄层到达 MbRenderer.logNative");
    std::printf("        logNative 收到: %s\n", last_log().c_str());
    std::printf("        nativeStatus: %s\n", last_string().c_str());

    check(Java_com_magicblocks_pad_MbRenderer_nativeRenderFrame(
              &env(), nullptr, h, 0.f, 0.f, 0.f, 1.f, 1.f, 0.f, 0.f) == JNI_FALSE,
          "未挂载 surface 时 nativeRenderFrame == false(与旧契约一致)");
    check(Java_com_magicblocks_pad_MbRenderer_nativeSurfaceCreated(
              &env(), nullptr, h, 0, 320, 240) == JNI_FALSE,
          "window=0 → nativeSurfaceCreated == false");
    Java_com_magicblocks_pad_MbRenderer_nativeSurfaceDestroyed(&env(), nullptr, h);

    Java_com_magicblocks_pad_MbRenderer_nativeSetMsaa(&env(), nullptr, h, 8);
    check(Java_com_magicblocks_pad_MbRenderer_nativeMsaaSamples(&env(), nullptr, h) >= 1,
          "nativeMsaaSamples >= 1");
    jstring sup = Java_com_magicblocks_pad_MbRenderer_nativeSupportedMsaa(&env(), nullptr, h);
    check(sup != nullptr && !last_string().empty(), "nativeSupportedMsaa 非空");
    std::printf("        nativeSupportedMsaa: %s\n", last_string().c_str());

    // 模型列表:错误码在第一行,且不再有 "err\t原因" 混进数据区
    jstring dir = mkstr(R"(Z:\definitely\not\here)");
    jstring lst = Java_com_magicblocks_pad_MbRenderer_nativeListModels(&env(), nullptr, dir);
    check(lst != nullptr, "nativeListModels 返回非 NULL");
    check(last_string().rfind("err=MB_E_NOT_FOUND\n", 0) == 0, "nativeListModels 首行 err=MB_E_NOT_FOUND");
    check(last_string().find('\t') == std::string::npos, "nativeListModels 输出不含 TAB(协议已改)");
    std::printf("        nativeListModels: %s", last_string().c_str());

    jstring path = mkstr(R"(Z:\nope.obj)");
    check(Java_com_magicblocks_pad_MbRenderer_nativeLoadModel(&env(), nullptr, h, path, nullptr) ==
              JNI_FALSE,
          "nativeLoadModel 坏路径 == false");
    Java_com_magicblocks_pad_MbRenderer_nativeClearModel(&env(), nullptr, h);
    check(Java_com_magicblocks_pad_MbRenderer_nativeWindowFromSurface(&env(), nullptr, nullptr) == 0,
          "nativeWindowFromSurface(NULL) == 0");

    Java_com_magicblocks_pad_MbRenderer_nativeDestroy(&env(), nullptr, h);
    check(mb_session_is_valid(js) == 0, "nativeDestroy 后句柄失效");
    Java_com_magicblocks_pad_MbRenderer_nativeDestroy(&env(), nullptr, h);  // 幂等,不崩

    // 所有入口对 handle=0 都必须安全
    check(Java_com_magicblocks_pad_MbRenderer_nativeRenderFrame(
              &env(), nullptr, 0, 0.f, 0.f, 0.f, 1.f, 1.f, 0.f, 0.f) == JNI_FALSE,
          "handle=0 的 render == false(不崩)");
    check(Java_com_magicblocks_pad_MbRenderer_nativeMsaaSamples(&env(), nullptr, 0) == 1,
          "handle=0 的 msaa == 1");
    jstring nul = Java_com_magicblocks_pad_MbRenderer_nativeStatus(&env(), nullptr, 0);
    check(nul != nullptr && last_string() == "handle=null", "handle=0 的 status == \"handle=null\"");
  }

  std::printf("\n结果: passed=%d failed=%d skipped=%d\n", g_pass, g_fail, g_skip);
  if (g_skip > 0) std::printf("(skipped 项见上方 SKIP,未伪造通过)\n");
  return g_fail == 0 ? 0 : 1;
}
