package com.magicblocks.pad

import android.content.Context
import android.util.Log
import android.view.Surface
import java.io.File
import java.io.IOException

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

    /** 设定 MSAA 采样数。必须在 [EngineSession.attachSurface] **之前**调用。 */
    external fun nativeSetMsaa(handle: Long, samples: Int)

    /** 实际生效的采样数（设备不支持时会低于请求值）。 */
    external fun nativeMsaaSamples(handle: Long): Int

    /** 设备支持的采样数，逗号分隔（如 "1,4,8"）。 */
    external fun nativeSupportedMsaa(handle: Long): String

    /**
     * 扫描一个**真实目录**下的预设模型（只列 OBJ，不解析）。
     *
     * 返回格式（字段用 '\t' 分隔）：
     *   首行 "ok"            → 后面每行 name \t path \t bytes \t ok(0/1) \t message
     *   首行 "err\t<原因>"   → 目录不存在/不可读（**不是**空列表）
     */
    external fun nativeListModels(directory: String): String

    /** 切到某个预设模型。失败返回 false 且渲染器保持原状（不会被切回立方体）。 */
    external fun nativeLoadModel(handle: Long, path: String, name: String): Boolean

    /** 切回内置立方体。 */
    external fun nativeClearModel(handle: Long)

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

    /** 切到 `path` 指向的 OBJ（`name` 只用于日志/状态显示）。失败时画面保持原状。 */
    fun loadModel(path: String, name: String): Boolean =
        handle != 0L && MbRenderer.nativeLoadModel(handle, path, name)

    /** 切回内置立方体。 */
    fun clearModel() {
        if (handle != 0L) MbRenderer.nativeClearModel(handle)
    }

    /**
     * 设定 MSAA 采样数，返回**实际生效**的值。
     *
     * 采样数是交换链的属性（决定建几张多采样图），不能在 attach 之后改，
     * 所以这里要求调用方先 detachSurface() —— 由 [EngineSurfaceView.applyMsaa]
     * 负责整个 detach → set → attach 的时序。
     */
    fun setMsaa(samples: Int) {
        if (handle == 0L) return
        MbRenderer.nativeSetMsaa(handle, samples)
    }

    fun msaaSamples(): Int = if (handle == 0L) 1 else MbRenderer.nativeMsaaSamples(handle)

    /** 设备支持的采样数档位（升序，至少含 1）。 */
    fun supportedMsaa(): List<Int> =
        if (handle == 0L) listOf(1)
        else MbRenderer.nativeSupportedMsaa(handle)
            .split(',')
            .mapNotNull { it.trim().toIntOrNull() }
            .sorted()
            .ifEmpty { listOf(1) }

    fun destroy() {
        if (handle == 0L) return
        detachSurface()
        MbRenderer.nativeDestroy(handle)
        handle = 0L
        surfaceAttached = false
        ready = false
    }
}

/** 一个预设模型条目（由引擎目录扫描给出，UI 只负责显示 name）。 */
data class ModelItem(
    val name: String,
    val path: String,
    val bytes: Long,
    val ok: Boolean,
    val message: String
)

/** 预设模型目录的扫描结果。"空目录"和"目录读不到"是两件事，必须分开报。 */
sealed class ModelScan {
    data class Ok(val items: List<ModelItem>) : ModelScan()
    data class Failed(val reason: String) : ModelScan()
}

/**
 * 预设模型目录的**资源代理**。
 *
 * 引擎（`scan_model_dir`）只认真实文件路径，不认识 APK 里的 assets ——
 * 所以这里先把 assets 下的 OBJ 解到 cacheDir，再把那个目录交给引擎扫描。
 * 复制是按 "大小 + mtime + 数量" 跳过已是最新的文件：每次启动多花一次
 * 目录比对，换掉几十 MB 的重复拷贝。
 *
 * 注意：这里**只做文件搬运**，不解析 OBJ、不猜模型名 —— 名字由引擎扫描给出。
 */
object ModelCatalog {

    const val ASSET_DIR = "res/models"

    fun scan(context: Context): ModelScan {
        val dir = try {
            materialize(context)
        } catch (e: Exception) {
            return ModelScan.Failed("解包 assets/${ASSET_DIR} 失败: ${e.message}")
        }
        val raw = try {
            MbRenderer.nativeListModels(dir.absolutePath)
        } catch (e: Throwable) {
            // 引擎还没 init 时 nativeListModels 是纯目录扫描，不依赖句柄；真抛了就是缺陷。
            return ModelScan.Failed("nativeListModels 异常: ${e.message}")
        }

        val lines = raw.split('\n')
        if (lines.isEmpty() || lines[0] != "ok") {
            val reason = lines.firstOrNull()?.removePrefix("err\t") ?: "未知错误"
            return ModelScan.Failed("目录不可用（$reason）：$dir")
        }
        val items = lines.drop(1).filter { it.isNotEmpty() }.mapNotNull { line ->
            val f = line.split('\t')
            if (f.size < 5) return@mapNotNull null
            ModelItem(
                name = f[0],
                path = f[1],
                bytes = f[2].toLongOrNull() ?: 0L,
                ok = f[3] == "1",
                message = f[4]
            )
        }
        return ModelScan.Ok(items)
    }

    /** 把 assets/res/models 解到 cacheDir/models，返回该目录。 */
    private fun materialize(context: Context): File {
        val outDir = File(context.cacheDir, "models")
        if (!outDir.exists() && !outDir.mkdirs()) {
            throw IOException("无法创建目录 $outDir")
        }
        val assets = context.assets
        val names = assets.list(ASSET_DIR) ?: emptyArray()
        // 先清掉上次解出来、现在资产里已经没有的文件，否则列表会显示"幽灵模型"。
        val wanted = names.filter { it.endsWith(".obj", ignoreCase = true) }.toSet()
        outDir.listFiles()?.forEach { f ->
            if (f.isFile && f.name !in wanted && f.name != STAMP_FILE) f.delete()
        }

        // 判断"缓存里的那份是不是 APK 里这一份"：只比**内容长度**。
        //
        // 为什么不用时间戳：AssetFileDescriptor 没有 lastModified，只有 length；
        // 而"同一路径长度相同但内容不同"在模型资源上只可能是有人改了模型——
        // 那种情况靠下面的 cacheStamp 兜住：App 版本号一变就整目录重解包。
        val cacheStamp = File(outDir, STAMP_FILE)
        val currentStamp = context.packageManager
            .getPackageInfo(context.packageName, 0).lastUpdateTime.toString()
        val stampFresh = cacheStamp.isFile && cacheStamp.readText() == currentStamp
        if (!stampFresh) {
            outDir.listFiles()?.forEach { it.deleteRecursively() }
            outDir.mkdirs()
            Log.i(MbRenderer.TAG, "模型缓存失效（App 更新），重新解包")
        }

        for (name in wanted) {
            val dst = File(outDir, name)
            val assetPath = "$ASSET_DIR/$name"
            val srcLen: Long = try {
                assets.openFd(assetPath).use { it.length }
            } catch (e: IOException) {
                // 压缩存储的资产没有 fd → 拿不到条目长度，只能靠 cacheStamp 判断。
                -1L
            }
            val fresh = stampFresh && dst.isFile && (srcLen < 0L || dst.length() == srcLen)
            if (fresh) continue
            assets.open(assetPath).use { input ->
                dst.outputStream().use { output -> input.copyTo(output) }
            }
            Log.i(MbRenderer.TAG, "解包模型 $name（${dst.length()} 字节）")
        }
        cacheStamp.writeText(currentStamp)
        return outDir
    }

    private const val STAMP_FILE = ".stamp"
}
