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
 *
 * ============================================================================
 * UI 只碰下面那张表(ABI 表),没有别的
 * ============================================================================
 * 本文件的 15 个 `external fun` **就是** UI 与 SDK 之间的全部接触面。每一个都对应
 * `MagicBlocks/include/magicblocks/mb_capi.h` 里明确的入口(唯一例外
 * [nativeWindowFromSurface] 已单独标注原因),并在注释里写清了它要求的最低 ABI 版本。
 *
 * 三条硬约束(改动前先读):
 *   1. **不许出现 SDK 内部符号。** 这里 import 的只有 `android.*` 与 `java.io.*`;
 *      不 import 任何 C++ 类型/命名空间(device::、backend::、Vulkan 的 Vk*、
 *      core::、primitives::),也不反射它们 —— 反射 SDK 内部符号与 include 内部头
 *      是同一件事,只是更容易躲过 review。
 *   2. **不许出现协议字面量。** `nativeListModels` 返回文本的格式(首行标记、
 *      字段顺序、分隔符)全部由 [MbAbiProtocol] 持有,本文件只往那边递字符串。
 *      t3→t4 那次事故就是"两侧各写一份格式":C++ 改成 `err=MB_E_*`,Kotlin 仍在比
 *      `"ok"`,于是真机上模型列表**永远为空**,而桌面全绿。
 *   3. **删除/改名任何一项之前先想清楚版本。** 删一个入口就是 ABI 破坏性变更,
 *      要 bump `mb_capi.h` 的 MAJOR 并同步 [MbAbi.MAJOR] —— 否则老 UI 会拿着
 *      新 .so 去调一个已经不存在的符号(真机上表现为 UnsatisfiedLinkError)。
 *
 * 符号与描述符怎么被**机器**校验(而不是靠人眼):
 *   `tools/capi_protocol/gen_abi_table.py --check` 解析 `jni_bridge.cpp` 的每个
 *   `JNIEXPORT`,与本文件对应的签名(登记在 `tools/capi_protocol/protocol.json`)
 *   逐位比对;`MagicBlocks/tests/capi_modellist` 再把编译器生成的 Kotlin
 *   `@Metadata` 解出来,与同一份登记表逐位比对。两侧都红过才算对。
 */
object MbRenderer {
    const val TAG = "MagicBlocks"

    init {
        System.loadLibrary("magicblocks")
    }

    /** 由 C++ 侧回调，把引擎的错误打进 logcat（不让它在 native 里被静默吞掉）。
     *
     *  对应 `mb_capi.h` §5 的 `mb_log_cb` 回调(`jni_bridge.cpp` 里转接)。
     *  这不是 `external fun`,而是 C++ 反向调进来的静态方法 —— 名字与签名
     *  `logNative(Ljava/lang/String;)V` 是 JNI 层的约定,同样不能随便改
     *  (改了就是静默丢日志)。
     *  ABI 版本要求：1.0 */
    @JvmStatic
    fun logNative(msg: String) {
        Log.e(TAG, "[native] $msg")
    }

    // ==================================================================================
    // ABI 表 —— 以下是 UI 触碰 SDK 的全部 15 个入口，没有第 16 个
    // ==================================================================================
    // 列含义:
    //   [mb_capi 对应]  SDK 侧哪个入口在干活(空表示不经 mb_capi,见该项注释)
    //   [ABI >= 1.0]    UI 对这一项要求的最低 ABI 版本;`mb_abi_version()` 低于它就不能调
    //   [JNI 描述符]    括号内是参数,箭头后是返回值 —— Kotlin 编译器按这个规则生成
    //                  Java 侧的方法签名,改成别的类型就成了另一个方法(运行期
    //                  UnsatisfiedLinkError)。本表与 protocol.json 里的登记表一致。
    //
    // 顺序刻意与 jni_bridge.cpp 里 JNIEXPORT 的顺序对齐,方便并排 review。

    /**
     * 建会话(建设备 + 默认场景资源)。
     *
     * [mb_capi 对应] `mb_session_desc_default()` → `mb_session_create()`;失败时用 `mb_err_str()` 取原因
     * [ABI >= 1.0]
     * [JNI 描述符] `()J`
     *
     * **语义保持**:即使失败也返回**非 0** 句柄 —— 这是唯一能让 [nativeStatus] 读到
     * 失败原因的方式(旧 JNI 契约如此,t4 刻意不动)。
     */
    external fun nativeCreate(): Long

    /**
     * 把 `ANativeWindow*` 挂上去建交换链。
     *
     * [mb_capi 对应] `mb_session_attach_surface(s, MB_SURFACE_ANDROID, window, w, h)`
     * [ABI >= 1.0]
     * [JNI 描述符] `(JJII)Z`
     *
     * 失败返回 false,**旧 surface 保持可用**(不静默降级、不留半成品交换链)。
     */
    external fun nativeSurfaceCreated(handle: Long, window: Long, width: Int, height: Int): Boolean

    /**
     * 卸载 surface。返回后 SDK 保证不再碰它、也不再拿它的交换链出帧。
     *
     * [mb_capi 对应] `mb_session_detach_surface(s)`
     * [ABI >= 1.0]
     * [JNI 描述符] `(J)V`
     */
    external fun nativeSurfaceDestroyed(handle: Long)

    /**
     * 渲染并呈现一帧(无 CPU 回读)。
     *
     * [mb_capi 对应] `mb_session_render(s, mb_pose{q[4], scale, pan[2]})`
     * [ABI >= 1.0]
     * [JNI 描述符] `(JFFFFFFFFFF)Z`  —— 句柄 + 7 个 float(四元数 4 + 缩放 1 + 平移 2)
     *
     * 姿态语义(与 `mb_capi.h` §8 一致)：`q` 是**相机单位四元数**(相机系→世界系),
     * 沿相机自身轴累积旋转;`scale` 是双指缩放;`pan` 是世界空间平移,由 UI 按
     * "屏幕像素 / 当前可视宽度"换算好再传 —— SDK 不猜屏幕朝向(旋转补偿在 SDK 侧)。
     *
     * 线程：设计为在**单一**渲染线程上调用(见 `MbRenderer` 使用方 EngineSurfaceView 的渲染线程)。
     */
    external fun nativeRenderFrame(
        handle: Long,
        qx: Float,
        qy: Float,
        qz: Float,
        qw: Float,
        scale: Float,
        panX: Float,
        panY: Float
    ): Boolean

    /**
     * 状态文本(给 HUD 显示)。
     *
     * [mb_capi 对应] `mb_session_status_text()`(两段式缓冲区协议)
     * [ABI >= 1.0]
     * [JNI 描述符] `(J)Ljava/lang/String;`
     *
     * **格式不保证** —— 只用于显示。需要结构化数据请用 `mb_session_perf` 那条路。
     */
    external fun nativeStatus(handle: Long): String

    /**
     * 销毁会话(释放交换链 → 资源 → 设备)。
     *
     * [mb_capi 对应] `mb_session_destroy(s)`
     * [ABI >= 1.0]
     * [JNI 描述符] `(J)V`
     *
     * 对 0 句柄与已销毁句柄都是 no-op。
     */
    external fun nativeDestroy(handle: Long)

    /**
     * 设定**请求**的 MSAA 采样数。必须在 [EngineSession.attachSurface] **之前**调用
     * —— 采样数是交换链属性(决定建几张多采样图),attach 之后改不了。
     *
     * [mb_capi 对应] `mb_session_set_msaa(s, mb_msaa)`
     * [ABI >= 1.0]
     * [JNI 描述符] `(JI)V`
     */
    external fun nativeSetMsaa(handle: Long, samples: Int)

    /**
     * **实际生效**的采样数。设备不支持时会低于请求值,UI 必须显示这个值。
     *
     * [mb_capi 对应] `mb_session_msaa_effective(s, &out)`
     * [ABI >= 1.0]
     * [JNI 描述符] `(J)I`
     */
    external fun nativeMsaaSamples(handle: Long): Int

    /**
     * 设备支持的采样数档位,逗号分隔(如 `"1,4,8"`;无档位时给 `"1"`)。
     *
     * [mb_capi 对应] `mb_session_msaa_supported(s, buf, cap, &count)`(两段式)
     * [ABI >= 1.0]
     * [JNI 描述符] `(J)Ljava/lang/String;`
     */
    external fun nativeSupportedMsaa(handle: Long): String

    /**
     * 扫描一个**真实目录**下的预设模型(只列 OBJ,不解析)。
     *
     * [mb_capi 对应] `mb_session_scan_models()` + `mb_model_list_count/get/free` + `mb_err_str()`
     * [ABI >= 1.0]
     * [JNI 描述符] `(Ljava/lang/String;)Ljava/lang/String;`
     *
     * ⚠ **返回的是文本协议,不要在本文件里解析它。** 交给 [MbAbiProtocol.parseModelList]。
     * 格式(协议 v[MbAbi.MODEL_LIST_PROTOCOL_VERSION],详见 `mb_capi.h` §10):
     *   首行 `err=<MB_E_*>`(恒有,成功时是 `MB_OK`);
     *   此后每个模型 5 行,行序 name / path / bytes / ok / message,一行一个字段。
     *
     * "空目录"(`err=MB_OK` 且 0 条)与"目录不存在"(`err=MB_E_NOT_FOUND`)是两件事,
     * 必须能分别显示 —— 这是 t3 特意保留的可区分性。
     */
    external fun nativeListModels(directory: String): String

    /**
     * 切到 `path` 指向的 OBJ(`name` 只用于日志/状态显示)。
     *
     * [mb_capi 对应] `mb_session_load_model(s, path, name)`
     * [ABI >= 1.0]
     * [JNI 描述符] `(JLjava/lang/String;Ljava/lang/String;)Z`
     *
     * 失败返回 false 且渲染器保持原状(不会被切回立方体)。
     */
    external fun nativeLoadModel(handle: Long, path: String, name: String): Boolean

    /**
     * 切回内置立方体。
     *
     * [mb_capi 对应] `mb_session_clear_model(s)`
     * [ABI >= 1.0]
     * [JNI 描述符] `(J)V`
     */
    external fun nativeClearModel(handle: Long)

    /**
     * `Surface` → `ANativeWindow*`。这个指针由引擎持有,Kotlin 侧不需要也不允许释放。
     *
     * [mb_capi 对应] **无 —— 这是唯一不经 mb_capi 的入口**,理由是它只碰 Android 平台
     *              类型(`JNIEnv*`, `jobject`)而 mb_capi 刻意不认识 `jobject`;
     *              把它塞进 C ABI 只会让 `mb_capi.h` 为了一个平台细节 include `<jni.h>`。
     *              它不接触任何 SDK 内部符号(SDK 根本不参与),所以不构成耦合。
     *              `ANativeWindow_fromSurface` 给的那一份引用由 SDK 在 attach/detach 时
     *              自己 acquire/release —— 调用方不参与引用计数。
     * [ABI >= 1.0]
     * [JNI 描述符] `(Landroid/view/Surface;)J`  —— 非 static 面向对象类型签名
     */
    external fun nativeWindowFromSurface(surface: Surface): Long

    /**
     * 运行时库的打包 ABI 版本(`MAJOR*10000 + MINOR*100 + PATCH`)。
     *
     * [mb_capi 对应] `mb_abi_version()`
     * [ABI >= 1.0]
     * [JNI 描述符] `()I`
     *
     * 这是**加载完库之后第一个**该调的入口。[checkAbiVersion] 用它来判
     * "UI 与 .so 是不是一套",不匹配就明确报错而不是继续跑。
     */
    external fun nativeAbiVersion(): Int

    /**
     * `nativeListModels` 返回文本的**协议版本**(当前 1)。
     *
     * [mb_capi 对应] `mb_model_list_protocol_version()`
     * [ABI >= 1.0]
     * [JNI 描述符] `()I`
     *
     * 与 ABI 版本是**两件事**:ABI 版本管 26 个 C 入口的名字/签名/结构体,协议版本
     * 只管那一个字符串里的标记/分隔/字段顺序。所以接口版本对得上,也还要比这一项。
     */
    external fun nativeModelListProtocolVersion(): Int

    // ==================================================================================
    // 版本校验 —— UI 与 SDK 之间唯一需要"对表"的运行时检查
    // ==================================================================================

    /**
     * 校验结论。`null` = 这一侧与运行时库对得上,可以继续;否则就是**必须显示出来**的
     * 错误原因(不会崩、也不会静默降级)。
     */
    var abiVerdict: String? = null
        private set

    /**
     * 版本校验(幂等,可以重复调;应该在 `loadLibrary` 之后**立刻**调一次)。
     *
     * 查两件事,任何一件不满足都给出一条**能直接贴到 HUD 上**的说明:
     *   1. `mb_abi_version()` 与本编译期要求的 [MbAbi.requiredVersion] 是否相容;
     *   2. `mb_model_list_protocol_version()` 与 [MbAbi.MODEL_LIST_PROTOCOL_VERSION] 是否**相等**。
     *
     * 第 2 条比的是相等而不是"不小于":文本协议没有"尾部追加字段"这种扩展位,
     * 一个字段换位置,解析器的输出就变成另一批数据(而且很可能**看不出错**,
     * 只是名字和路径换了位置)。宁可直接拒绝,也不要猜着解析。
     *
     * @return `true` = 通过
     */
    fun checkAbiVersion(): Boolean {
        val problems = ArrayList<String>(2)

        val runtimeAbi = try {
            nativeAbiVersion()
        } catch (t: Throwable) {
            // 库太旧、根本没有这个入口 → 真机上是 UnsatisfiedLinkError。
            problems.add(
                "调不到 nativeAbiVersion():${t.javaClass.simpleName}: ${t.message}。" +
                    "这通常说明加载到的 libmagicblocks.so 比 UI 旧(没有这个入口)," +
                    "UI 需要 ABI ${MbAbi.format(MbAbi.requiredVersion)}。"
            )
            -1
        }
        if (runtimeAbi >= 0) {
            MbAbi.mismatchDetail(runtimeAbi)?.let { problems.add(it) }
        }

        val runtimeProto = try {
            nativeModelListProtocolVersion()
        } catch (t: Throwable) {
            problems.add(
                "调不到 nativeModelListProtocolVersion():${t.javaClass.simpleName}: ${t.message}。" +
                    "模型列表的文本格式无法确认,UI 不会去解析它。"
            )
            -1
        }
        if (runtimeProto >= 0 && runtimeProto != MbAbi.MODEL_LIST_PROTOCOL_VERSION) {
            problems.add(
                "模型列表协议版本不一致:UI 解析 v${MbAbi.MODEL_LIST_PROTOCOL_VERSION}," +
                    "SDK 产出 v$runtimeProto。文本协议不做向后兼容猜测,列表将被禁用。"
            )
        }

        abiVerdict = if (problems.isEmpty()) null else problems.joinToString(" ")
        if (abiVerdict == null) {
            Log.i(
                TAG,
                "ABI 校验通过:运行时 ${MbAbi.format(runtimeAbi)}($runtimeAbi)," +
                    "UI 至少需要 ${MbAbi.format(MbAbi.requiredVersion)};协议 v$runtimeProto"
            )
        } else {
            // 走两遍:Log.e 与 HUD 用的是同一句话,免得两边说法不一致。
            Log.e(TAG, "ABI 校验失败 —— $abiVerdict")
        }
        return abiVerdict == null
    }

    /**
     * 模型列表协议是否可用。`checkAbiVersion()` 报出"协议版本不一致"时为 false。
     *
     * 与 [abiUsable] 分开:ABI 对得上但协议变了(那是最坏的一种,因为调用约定没错、
     * 只是数据含义变了),引擎可以照跑,但列表不该被解析。
     */
    val protocolUsable: Boolean
        get() = abiVerdict == null || !abiVerdict!!.contains("模型列表协议版本不一致")

    /**
     * 这一侧到底能不能用引擎。
     *
     * 判据是"**还没校验过,或者校验通过了**":`abiVerdict == null` 既表示通过、也表示
     * 还没跑过校验,而"没跑过"不该被当成"不通过"(真机启动顺序里 [checkAbiVersion]
     * 一定在 onCreate 里先跑,但单测/其它宿主可能先 new 一个 EngineSession)。
     */
    val abiUsable: Boolean
        get() = abiVerdict == null
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
    fun render(qx: Float, qy: Float, qz: Float, qw: Float, scale: Float, panX: Float, panY: Float): Boolean {
        if (handle == 0L || !surfaceAttached) return false
        return MbRenderer.nativeRenderFrame(handle, qx, qy, qz, qw, scale, panX, panY)
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

/** 预设模型目录的扫描结果。"空目录"和"目录读不到"是两件事，必须分开报。
 *  （定义在 [MbAbiProtocol] 里 —— 那里是解析器的所在地，见该文件顶部的说明。） */

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
        // 协议版本对不上时**不去解析**:猜一个格式解析下去,最可能的结果是
        // "名字和路径换了位置"这种看不出错的数据。明确禁用比显示错数据好。
        if (!MbRenderer.protocolUsable) {
            return ModelScan.Failed(
                "模型列表协议与 SDK 不一致,已禁用扫描:${MbRenderer.abiVerdict}"
            )
        }
        val raw = try {
            MbRenderer.nativeListModels(dir.absolutePath)
        } catch (e: Throwable) {
            // 引擎还没 init 时 nativeListModels 是纯目录扫描，不依赖句柄；真抛了就是缺陷。
            return ModelScan.Failed("nativeListModels 异常: ${e.message}")
        }

        // ⚠ 解析**只在** MbAbiProtocol 里做。这个文件里不允许再出现任何对
        //   nativeListModels 返回文本的格式假设(首行标记/分隔符/字段下标)——
        //   t3→t4 的"列表永远为空"就是因为这里留了一份自己的假设。
        return when (val parsed = MbAbiProtocol.parseModelList(raw)) {
            is ModelScan.Ok -> ModelScan.Ok(parsed.items)
            // 失败时把目录带上:同一个错误码在不同目录下含义完全不同
            // (例如 MB_E_NOT_FOUND 指向的是哪个路径)。
            is ModelScan.Failed -> ModelScan.Failed("${parsed.reason}（目录：$dir）")
        }
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
