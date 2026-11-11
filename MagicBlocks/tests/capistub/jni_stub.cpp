// capistub/jni_stub.cpp —— capistub/jni.h 的桌面实现(把日志转进内存)
//
// 用途:让桌面冒烟程序能真的调 Java_com_magicblocks_pad_MbRenderer_* 这 12 个入口,
// 并把 MbRenderer.logNative 收到的内容存下来供断言。
#include "jni.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {
std::string g_last_message;
std::string g_last_created_string;
}  // namespace

// 冒烟程序据此检查"SDK 的日志真的经回调到达了 UI 侧"。
extern "C" const char* mb_test_jni_last_log() { return g_last_message.c_str(); }
extern "C" const char* mb_test_jni_last_string() { return g_last_created_string.c_str(); }

jclass JNIEnv::FindClass(const char*) const { return reinterpret_cast<jclass>(0x1); }
jmethodID JNIEnv::GetStaticMethodID(jclass, const char*, const char*) const {
  return reinterpret_cast<jmethodID>(0x2);
}
jstring JNIEnv::NewStringUTF(const char* s) const {
  g_last_created_string = s != nullptr ? s : "";
  return reinterpret_cast<jstring>(0x3);
}
jstring JNIEnv::NewStringUTF(jclass, const char* s) const { return NewStringUTF(s); }
const char* JNIEnv::GetStringUTFChars(jstring s, jboolean*) const {
  return reinterpret_cast<const char*>(s);
}
void JNIEnv::ReleaseStringUTFChars(jstring, const char*) const {}
void JNIEnv::CallStaticVoidMethod(jclass, jmethodID, ...) const {
  /* 只取第一个可变参数。⚠ 这个替身里 `...` 之前**只有 2 个具名形参**(jclass、jmethodID),
   * 而 x64 Windows 会把这 2 个整型实参直接放寄存器(RCX/RDX)、把可变参数写到
   * "影子空间"(shadow space),两者不连续。所以这里不要在替身里用 va_list ——
   * 任何拿不准的 varargs 读写都会让"冒烟结果"取决于替身自己有没有写错。
   * 真机上这里由 libart 处理,替身只需要一个稳定的"最后一条日志"记录。
   * (第一版写了 `va_start(ap, 0)`,取到的是影子空间里的 jmethodID,
   *  于是 std::string 赋值时 C0000005,崩溃点离原因很远 —— 记在这里。) */
  g_last_message = g_last_created_string;  // 日志内容就是紧随其前的 NewStringUTF 参数
}
jboolean JNIEnv::ExceptionCheck() const { return JNI_FALSE; }
void JNIEnv::ExceptionClear() const {}
void JNIEnv::DeleteLocalRef(jobject) const {}
