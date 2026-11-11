/* capistub/jni.h —— **只为桌面自测**的最小 JNI 头替身(不是 NDK 头)。
 *
 * 为什么需要它:`src/android/jni_bridge.cpp` 只依赖 `<jni.h>` 与少数 JNI 原语。
 * 桌面上没有 NDK,而 t3 的血泪教训(t2 那一轮)是「编译过 ≠ 能用」:
 * 头文件 7 格编译矩阵全绿,C++ 侧 26 个符号却全被 mangle,链接期才炸。
 *
 * 所以桌面验收必须真的把 jni_bridge.cpp **编进来、链进来、跑起来**。
 *
 * ⚠ 形状必须**与 NDK 一致**,否则这个替身会替 NDK 掩盖问题(第一版就是教训):
 *   第一版把 JNIEnv 写成"函数指针成员",于是 `env->FindClass("x")` 变成
 *   "对函数指针传 2 个参数" → C2198 一大片。真 NDK 里 JNIEnv 是**带成员函数**
 *   的类(内部才是函数指针表),所以这里也用成员函数。
 *
 *   NDK 在 C++ 下还有一层:jclass/jmethodID 是重载的 struct(派生自 _jobject),
 *   从而有 `NewStringUTF(const char*)` 与 `CallStaticVoidMethod(jclass, jmethodID, ...)`
 *   两个重载。这里照抄这个形状 —— 少了它,替身"更宽松",就会把编译期能抓到的
 *   错误放到真机上。
 *
 * 真机构建(ANDROID 平台)永远用 NDK sysroot 里的真 jni.h,capistub 不进 include 路径。
 */
#ifndef MAGICBLOCKS_TEST_JNI_STUB_H
#define MAGICBLOCKS_TEST_JNI_STUB_H

#include <cstdarg>
#include <cstdint>

typedef uint8_t jboolean;
typedef int32_t jint;
typedef int64_t jlong;
typedef float jfloat;

constexpr jboolean JNI_FALSE = 0;
constexpr jboolean JNI_TRUE = 1;

/* 有意**没有**成员、没有非平凡初始化:NDK 这里就是一个空结构体。
 * 第一版写了 `void* opaque = nullptr;` —— 于是 "jstring 是 const char*" 这层
 * 隐式关系被悄悄切断,而它不会在任何一处编译报错,只在运行期炸(C0000005,
 * 且崩溃点离真正原因很远)。替身与真 NDK 形状不一致 = 替身在替 NDK 掩盖问题。 */
struct _jobject {};

/* NDK(C++ 模式)的写法:自 <jni.h> 引入起 jclass/jmethodID 就是派生自 _jobject 的
 * struct,从而有 `NewStringUTF(const char*)` / `CallStaticVoidMethod(cls, mid, ...)`
 * 两个重载。少了它,替身"更宽松",会把编译期能抓到的错误放到真机上。
 *
 * ⚠ _jstring 例外:NDK 里 (const char*) 能隐式转成 jstring(它就是 const char*),
 *   而 C++ 里**没有** "子类能隐式从基类指针构造" 这回事 —— 若照抄成
 *   `struct _jstring : _jobject`,凡是用字面量构造 jstring 的代码都会编不过。
 *   这里不给 _jstring 重载构造函数 = 与 NDK 一致。 */
struct _jclass : public _jobject {};
struct _jmethodID : public _jobject {};
struct _jstring : public _jobject {};

typedef _jobject* jobject;
typedef _jstring* jstring;
typedef _jclass* jclass;
typedef _jmethodID* jmethodID;

struct JNIEnv {
  /* ---- 成员函数形态(与 NDK 一致) ---- */
  jclass FindClass(const char*) const;
  jmethodID GetStaticMethodID(jclass, const char*, const char*) const;
  jstring NewStringUTF(const char*) const;
  jstring NewStringUTF(jclass, const char*) const;
  const char* GetStringUTFChars(jstring, jboolean*) const;
  void ReleaseStringUTFChars(jstring, const char*) const;
  void CallStaticVoidMethod(jclass, jmethodID, ...) const;  /* 替身里只记 NewStringUTF 的内容 */
  jboolean ExceptionCheck() const;
  void ExceptionClear() const;
  void DeleteLocalRef(jobject) const;
};

#define JNIEXPORT
#define JNICALL

#endif /* MAGICBLOCKS_TEST_JNI_STUB_H */
