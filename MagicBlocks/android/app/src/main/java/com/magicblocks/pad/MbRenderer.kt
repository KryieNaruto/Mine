package com.magicblocks.pad

import android.util.Log
import android.view.Surface

/**
 * 引擎侧的 JNI 绑定。
 *
 * 这里**只**做转发：不碰 Vulkan、不看像素、不做任何回读。
 * 真正的渲染全部在 C++ 侧直接写进 Surface 的 swapchain image，
 * 再 vkQueuePresentKHR 交给系统合成 —— 根本没有 CPU 读像素这一步。
 */
object MbRenderer {
    const val TAG = "MagicBlocks"

    init {
        System.loadLibrary("magicblocks")
    }

    /** 由 C++ 侧回调，把引擎的错误打进 logcat（不让它在 native 里被静默吞掉）。 */
    @JvmStatic
    fun logNative(msg: String) {
        Log.e(TAG, "[native] $msg")
    }

    external fun nativeCreate(): Long
    external fun nativeSurfaceCreated(handle: Long, window: Long, width: Int, height: Int): Boolean
    external fun nativeSurfaceDestroyed(handle: Long)
    external fun nativeRenderFrame(
        handle: Long,
        yaw: Float,
        pitch: Float,
        scale: Float,
        panX: Float,
        panY: Float
    ): Boolean
    external fun nativeStatus(handle: Long): String
    external fun nativeDestroy(handle: Long)

    /** Surface -> ANativeWindow*。这个指针由引擎持有，不需要也不允许 Kotlin 释放。 */
    external fun nativeWindowFromSurface(surface: Surface): Long
}

/**
 * 把 [Surface] 的生命周期和渲染调用包起来，给 Compose 用。
 *
 * 注意：所有方法都必须在**同一个线程**（Android View 的 UI 线程）调用，
 * 因为 Vulkan 设备/交换链是在那个线程上创建和使用的。
 */
class EngineSession {
    private var handle: Long = 0L
    private var surfaceAttached = false

    /** 引擎是否已经成功建好（设备 + 管线 + 缓冲）。 */
    var ready = false
        private set

    fun create() {
        if (handle != 0L) return
        handle = MbRenderer.nativeCreate()
        ready = handle != 0L
        Log.i(MbRenderer.TAG, "引擎句柄=0x${handle.toString(16)}")
    }

    /**
     * Surface 就绪时调用：在这张 Surface 上建交换链。
     * 成功后引擎就能直接渲染到它上面 —— 全程零 CPU 回读。
     */
    fun attachSurface(surface: Surface, width: Int, height: Int): Boolean {
        if (handle == 0L) return false
        val window = MbRenderer.nativeWindowFromSurface(surface)
        if (window == 0L) {
            Log.e(MbRenderer.TAG, "拿不到 ANativeWindow（Surface 可能已失效）")
            return false
        }
        val ok = MbRenderer.nativeSurfaceCreated(handle, window, width, height)
        surfaceAttached = ok
        Log.i(MbRenderer.TAG, "attachSurface ${width}x$height -> $ok")
        return ok
    }

    fun detachSurface() {
        if (handle == 0L || !surfaceAttached) return
        MbRenderer.nativeSurfaceDestroyed(handle)
        surfaceAttached = false
    }

    /**
     * 渲染并直接呈现一帧。
     *
     * yaw/pitch 单位是弧度，scale 是双指缩放倍数，
     * panX/panY 是世界空间的平移偏移（Kotlin 侧已按可视宽度换算）。
     */
    fun render(yaw: Float, pitch: Float, scale: Float, panX: Float, panY: Float): Boolean {
        if (handle == 0L || !surfaceAttached) return false
        return MbRenderer.nativeRenderFrame(handle, yaw, pitch, scale, panX, panY)
    }

    fun status(): String = if (handle == 0L) "no-handle" else MbRenderer.nativeStatus(handle)

    fun destroy() {
        if (handle == 0L) return
        detachSurface()
        MbRenderer.nativeDestroy(handle)
        handle = 0L
        surfaceAttached = false
        ready = false
    }
}
