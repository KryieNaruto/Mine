// MagicBlocks —— JNI 桥：把 SceneRenderer 暴露给 Kotlin。
//
// 设计原则：Kotlin 侧不做任何 Vulkan 相关的事，只调用这几个函数。
// 所有错误都以字符串返回/回调给 Kotlin 打 logcat —— 不静默吞掉。
#include <jni.h>

#include <cstdint>
#include <string>

#if defined(MB_ANDROID_USE_NDK)
#include <android/native_window.h>
#include <android/native_window_jni.h>
#endif

#include "scene_renderer.h"

namespace {

using magicblocks::android::SceneRenderer;

inline jlong pack(SceneRenderer* p) { return reinterpret_cast<jlong>(p); }
inline SceneRenderer* unpack(jlong h) { return reinterpret_cast<SceneRenderer*>(h); }

// 把 C++ 侧的错误送到 Kotlin 的 MbRenderer.logNative(String)。
// 找不到类/方法时不抛异常（JNI 里抛异常会让后续调用全部失效）。
void log_native(JNIEnv* env, const std::string& msg) {
  if (!env) return;
  jclass cls = env->FindClass("com/magicblocks/pad/MbRenderer");
  if (!cls) {
    env->ExceptionClear();
    return;
  }
  jmethodID mid = env->GetStaticMethodID(cls, "logNative", "(Ljava/lang/String;)V");
  if (!mid) {
    env->ExceptionClear();
    env->DeleteLocalRef(cls);
    return;
  }
  jstring js = env->NewStringUTF(msg.c_str());
  env->CallStaticVoidMethod(cls, mid, js);
  if (env->ExceptionCheck()) env->ExceptionClear();
  env->DeleteLocalRef(js);
  env->DeleteLocalRef(cls);
}

}  // namespace

extern "C" {

JNIEXPORT jlong JNICALL
Java_com_magicblocks_pad_MbRenderer_nativeCreate(JNIEnv* env, jclass) {
  auto* r = new SceneRenderer();
  std::string err;
  if (!r->init(&err)) {
    // 不返回 0：让 Kotlin 的 status() 能读到失败原因。
    log_native(env, "init 失败: " + err);
  }
  return pack(r);
}

JNIEXPORT jboolean JNICALL
Java_com_magicblocks_pad_MbRenderer_nativeSurfaceCreated(JNIEnv* env, jclass, jlong handle,
                                                         jlong window, jint width, jint height) {
  auto* r = unpack(handle);
  if (!r) return JNI_FALSE;
  std::string err;
  void* win = reinterpret_cast<void*>(static_cast<std::intptr_t>(window));
  if (!r->attach_surface(win, static_cast<int>(width), static_cast<int>(height), &err)) {
    log_native(env, "attach_surface 失败: " + err);
    return JNI_FALSE;
  }
  return JNI_TRUE;
}

JNIEXPORT void JNICALL
Java_com_magicblocks_pad_MbRenderer_nativeSurfaceDestroyed(JNIEnv*, jclass, jlong handle) {
  if (auto* r = unpack(handle)) r->detach_surface();
}

JNIEXPORT jboolean JNICALL
Java_com_magicblocks_pad_MbRenderer_nativeRenderFrame(JNIEnv* env, jclass, jlong handle, jfloat yaw,
                                                      jfloat pitch, jfloat scale, jfloat pan_x,
                                                      jfloat pan_y) {
  auto* r = unpack(handle);
  if (!r) return JNI_FALSE;
  std::string err;
  if (!r->render_frame(yaw, pitch, scale, pan_x, pan_y, &err)) {
    log_native(env, "render_frame 失败: " + err);
    return JNI_FALSE;
  }
  return JNI_TRUE;
}

JNIEXPORT jstring JNICALL
Java_com_magicblocks_pad_MbRenderer_nativeStatus(JNIEnv* env, jclass, jlong handle) {
  auto* r = unpack(handle);
  const std::string s = r ? r->status_text() : std::string("handle=null");
  return env->NewStringUTF(s.c_str());
}

JNIEXPORT void JNICALL
Java_com_magicblocks_pad_MbRenderer_nativeDestroy(JNIEnv*, jclass, jlong handle) {
  delete unpack(handle);
}

// ---- MSAA 开关 ----
//
// 采样数必须在 attach_surface **之前**设定（交换链要按它决定建几张 MS 图），
// 所以这是一个"先设值、再 attach"的接口，不是 attach 之后能偷偷改的开关。
// Kotlin 侧切换 MSAA 的做法就是 detach → set → attach，与这里配套。

JNIEXPORT void JNICALL
Java_com_magicblocks_pad_MbRenderer_nativeSetMsaa(JNIEnv*, jclass, jlong handle, jint samples) {
  auto* r = unpack(handle);
  if (r != nullptr) r->set_msaa_samples(samples > 0 ? static_cast<std::uint32_t>(samples) : 1u);
}

// 实际生效的采样数：设备不支持时会低于请求值，UI 要显示这个值而不是请求值。
JNIEXPORT jint JNICALL
Java_com_magicblocks_pad_MbRenderer_nativeMsaaSamples(JNIEnv*, jclass, jlong handle) {
  auto* r = unpack(handle);
  return r ? static_cast<jint>(r->effective_msaa_samples()) : 1;
}

// 设备支持的采样数，逗号分隔（如 "1,4,8"）。
// 用字符串而不是 jintArray：省掉一次数组构造 + 逐元素读，Kotlin 侧 split 一下即可。
JNIEXPORT jstring JNICALL
Java_com_magicblocks_pad_MbRenderer_nativeSupportedMsaa(JNIEnv* env, jclass, jlong handle) {
  auto* r = unpack(handle);
  std::string s;
  if (r != nullptr) {
    const auto lv = r->supported_msaa_levels();
    for (std::size_t i = 0; i < lv.size(); ++i) {
      if (i) s += ',';
      s += std::to_string(lv[i]);
    }
  }
  if (s.empty()) s = "1";
  return env->NewStringUTF(s.c_str());
}

// Surface -> ANativeWindow*。ANativeWindow_fromSurface 会给引用计数 +1，
// 这**一个**引用由引擎持有：attach_surface 成功后一直在用，detach_surface 时由
// SceneRenderer 调 ANativeWindow_release 还掉。所以这里**不能** release。
JNIEXPORT jlong JNICALL
Java_com_magicblocks_pad_MbRenderer_nativeWindowFromSurface(JNIEnv* env, jclass,
                                                            jobject surface) {
  if (!surface) return 0;
#if defined(MB_ANDROID_USE_NDK)
  ANativeWindow* win = ANativeWindow_fromSurface(env, surface);
  return static_cast<jlong>(reinterpret_cast<std::intptr_t>(win));
#else
  (void)env;
  return 0;
#endif
}

}  // extern "C"
