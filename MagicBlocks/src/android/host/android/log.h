// MagicBlocks —— Android 头文件的主机端(宿主机构建)替身
//
// 为什么需要它：
//   src/android/scene_renderer.cpp 里的**管线描述**(cube_pipeline_desc /
//   model_pipeline_desc)是纯值语义的，宿主机上完全可以编译并断言 —— 而它们
//   编码的恰好是最容易悄悄错掉、错了又最难从画面上倒推的契约
//   (36 字节步长 vs 48 字节顶点)。为了能在 CI 上跑这份断言，宿主机也要能
//   编译这个 TU，但宿主机没有 NDK 的 <android/log.h>。
//
// 这个头**只在非 Android 构建**里经 CMake 注入 (MB_ANDROID_HOST_STUB)，
// 真实设备上 include 的仍然是 NDK 的 <android/log.h>，行为没有任何变化。
//
// 注意：这里**不提供** <android/native_window.h> 的替身。scene_renderer.cpp
// 只在 MB_ANDROID_USE_NDK 时才 include 它，宿主机构建不定义该宏，所以
// ANativeWindow* 一直是 void*，与 Android 构建的 ABI 形状一致。
#pragma once

#if !defined(MB_ANDROID_HOST_STUB)
#error "这个头只在主机测试替身场景下被注入(MB_ANDROID_HOST_STUB)"
#endif

#include <cstdarg>
#include <cstdio>

#if !defined(__ANDROID__)

#define ANDROID_LOG_UNKNOWN 0
#define ANDROID_LOG_DEFAULT 1
#define ANDROID_LOG_VERBOSE 2
#define ANDROID_LOG_DEBUG 3
#define ANDROID_LOG_INFO 4
#define ANDROID_LOG_WARN 5
#define ANDROID_LOG_ERROR 6
#define ANDROID_LOG_FATAL 7
#define ANDROID_LOG_SILENT 8

// 宿主机的 __android_log_print：退化成 stdout。
//
// 为什么不是空实现：测试里出现异常日志时，看不到就等于没有 —— 但这里的目标
// 只是"能编译、能链接"，所以直接打前缀而不是复制一份 logcat 格式。
inline int __android_log_print(int prio, const char* tag, const char* fmt, ...) {
  static const char* const kNames[] = {"?", "?", "V", "D", "I", "W", "E", "F", "S"};
  const char* level =
      (prio >= 0 && prio <= 8) ? kNames[prio] : "?";
  std::fprintf(stdout, "[%s/%s] ", level, tag ? tag : "mb");
  va_list ap;
  va_start(ap, fmt);
  std::vfprintf(stdout, fmt, ap);
  va_end(ap);
  std::fputc('\n', stdout);
  return 0;
}

#define __android_log_vprint(prio, tag, fmt, ap) __android_log_print(prio, tag, fmt)
#define __android_log_assert(cond, tag, fmt, ...) \
  ((void)(cond), __android_log_print(ANDROID_LOG_FATAL, tag, fmt))

#endif  // !defined(__ANDROID__)
