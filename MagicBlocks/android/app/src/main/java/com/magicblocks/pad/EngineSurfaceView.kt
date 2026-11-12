package com.magicblocks.pad

import android.annotation.SuppressLint
import android.content.Context
import android.util.Log
import android.view.Choreographer
import android.view.MotionEvent
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import kotlin.math.abs
import kotlin.math.PI
import kotlin.math.atan2
import kotlin.math.cos
import kotlin.math.exp
import kotlin.math.hypot
import kotlin.math.max
import kotlin.math.min
import kotlin.math.sin
import kotlin.math.sqrt
import kotlin.math.tan

/**
 * 引擎渲染面。
 *
 * 关键设计：它是 Compose 的**兄弟节点**（宿主用 FrameLayout 叠放），
 * 不是 Compose 子树里的 AndroidView。原因：
 *   - SurfaceView 在独立的 window layer 上合成，被 Compose 嵌套时会出现
 *     "相机/合成器打架"的黑闪和裁剪错位；
 *   - 平铺在下面、Compose 控件浮在上面，两边各自独立，互不干扰，
 *     而且 SurfaceView 的 buffer 由 SurfaceFlinger 直接合成 —— 引擎写进去的
 *     像素**不经过** CPU，也不经过 Compose 重绘。
 *
 * 交互（方向按用户看到的物理屏幕算，见 [toVisualDelta] / [accumulateFingerSample]）：
 *   单指左右拖 → 绕**相机自身 Y 轴**转（画面左右转）
 *   单指上下拖 → 绕**相机自身 X 轴**转（画面上下转）
 *   松手      → 惯性：按松手瞬间的角速度继续转，速度指数衰减到 0
 *   双指捏合  → 缩放
 *   双指拖动  → 平移
 *   双指松手  → 平移惯性（同样按速度衰减）
 */
@SuppressLint("ViewConstructor")
class EngineSurfaceView(context: Context) : SurfaceView(context), SurfaceHolder.Callback {

    private val session = EngineSession()
    private var surfaceWidth = 0
    private var surfaceHeight = 0

    // 必须把 this 注册给 surface 的 holder，否则 SurfaceHolder 永远不会回调
    // surfaceCreated / surfaceChanged / surfaceDestroyed —— 引擎拿不到 surface，
    // 渲染循环不启动，表现为"界面画出来但模型区永远不动、没有 MB_PERF"。
    // 98cc9d9 重写相机手感时误删了这整块 init，导致 surface 路径断链。
    init {
        holder.addCallback(this)
        // 让引擎的 swapchain image 直接浮在窗口最上层时不会被 Compose 遮住
        holder.setFormat(android.graphics.PixelFormat.OPAQUE)
        setZOrderOnTop(false)   // 保持在下面，Compose 浮在上面
        isFocusable = true
    }

    // ---------------------------------------------------------------- 视角状态
    //
    // 相机姿态是**一个单位四元数** q（机体系 → 世界系）。
    //
    // 每帧拖动只做一件事：**右乘两个增量**
    //
    //     q ← normalize( q * Rx(Δpitch) * Ry(Δyaw) )
    //
    // 四元数乘法里右乘 = 绕**机体轴**，而机体轴的定义就是相机此刻
    // 的 right / up。所以左右拖永远绕当前相机的 up、上下拖永远绕当前
    // 相机的 right，不需要任何“取轴”操作（取轴就会把读者/
    // 自己引到“每帧重新选轴”的歧路上）。
    //
    // 四元数没有欧拉角那种万向节奇点，所以 360° 无死角、
    // **不需要也不允许任何角度夹取**。
    private var qx = 0f
    private var qy = 0f
    private var qz = 0f
    private var qw = 1f
    private var scale = 1f
    private var panX = 0f   // 世界空间平移
    private var panY = 0f

    // ---------------------------------------------------------------- 手势
    private var lastX = 0f
    private var lastY = 0f
    private var lastPinchDist = 0f
    private var lastFocusX = 0f
    private var lastFocusY = 0f

    /** 给外面（Compose 状态栏）读的统计信息。 */
    var onStatus: ((String) -> Unit)? = null
    private var lastStatusAt = 0L

    // ---------------------------------------------------------------- 性能统计(fps HUD)
    //
    // 为什么不直接数 Choreographer 的回调次数：那测的是"UI 线程有没有被叫醒"，
    // 而这里要的是"引擎真的把画面交给合成器了几次"。所以只在 session.render()
    // **成功返回**时计数 —— 失败帧不进 fps，而是进 renderFailCount，
    // 否则渲染全挂的时候 HUD 反而会显示一个漂亮的 fps。
    //
    // 窗口取 1 秒：短于 1 秒数值抖得没法看，长于 1 秒对"拖动时掉帧"反应太慢。
    private var frameCount = 0
    private var frameTotal = 0L
    private var renderFailCount = 0

    /** 最近一个统计窗口内的帧渲染耗时累加(ns)，用于算平均。 */
    private var frameCostAccNs = 0L
    private var frameCostMaxNs = 0L
    private var windowStartNs = 0L
    /** 窗口起点的本进程累计 CPU tick（-1 = 该平台读不到）。 */
    private var windowCpuTicks = -1L
    /** 本窗口 CPU 占用百分比；-1 表示读不到。 */
    private var windowCpuPct = -1f
    /** 本窗口 GPU 占用百分比；-1 表示读不到（Mali 非 root 常见）。 */
    private var windowGpuPct = -1

    /** 引擎侧一次采样的性能快照，供 Compose 状态栏展示。 */
    data class PerfSnapshot(
        val fps: Float,
        val frameMsAvg: Float,
        val frameMsMax: Float,
        val renderFailRate: Float,
        val msaa: Int,
        /** 本进程 CPU 占用百分比（可 >100 = 多线程）；-1 表示读不到。 */
        val cpuPct: Float = -1f,
        /** GPU 占用百分比；-1 表示该机型读不到（Mali 非 root 常见）。 */
        val gpuPct: Int = -1,
    )

    private var perf = PerfSnapshot(0f, 0f, 0f, 0f, 1)

    /** 最近一次 [PerfSnapshot]。UI 线程直接读，值由渲染线程发布。 */
    fun currentPerf(): PerfSnapshot = perf

    // ================================================================ 屏幕朝向
    //
    // Activity 是 landscape 锁定的，但设备可以是"逻辑横屏 + 物理竖屏"（ROTATE_90）
    // 或"逻辑横屏 + 物理横屏"（ROTATE_0）。两种情况下 MotionEvent 的坐标系不同：
    // 前者事件 x/y 是逻辑横屏轴，而用户看到的画面沿物理竖屏排布 ——
    // 手势方向必须换算，否则"左右拖"会变成"上下转"（真机上复现过）。
    //
    // 由 Activity 通过 [setRotationListener] 注入，避免这里自己去拿 Display
    // （View 可能还没 attach，getDisplay 会返回 null）。
    private var displayRotation: Int = Surface.ROTATION_0
    private var rotationListener: ((Int) -> Unit)? = null

    /** 注册"屏幕朝向变化"回调，参数是 Display.getRotation()。立即回调一次当前值。 */
    fun setRotationListener(listener: (Int) -> Unit) {
        rotationListener = listener
        listener(displayRotation)
    }

    /** 以 Display.getRotation() 语义设定当前屏幕朝向。 */
    fun setDisplayRotation(rotation: Int) {
        if (rotation == displayRotation) return
        displayRotation = rotation
        Log.i(MbRenderer.TAG, "屏幕朝向 rotation=$rotation")
        rotationListener?.invoke(rotation)
    }

    // ================================================================ MSAA
    //
    // 这里存的是**请求值**；实际生效值由引擎在 attach 时回报（设备可能不支持
    // 请求档位而退回 1x）。两者分开存，UI 才能把"我点的是 8x、实际是 4x"显示出来。
    //
    // 默认 4x：这个 APK 是演示用的，打开就该看见抗锯齿效果。
    // 设备不支持 4x 时后端会在建交换链前拦下来并退回 1x（还会打日志），
    // HUD 显示的是退回后的真实值，不会骗人。
    private var msaaSamples = DEFAULT_MSAA_SAMPLES
    private var msaaEffective = 1
    /** 上屏分辨率缩放比（0.25~1.0）。见 applyRenderScale。 */
    private var renderScale = 1.0f

    // ================================================================ 运动状态
    //
    // 拖动与惯性是**同一套**状态，区别只在于"角速度从哪来"：
    //   * 拖动：手指位移 → 目标角速度（按屏幕像素/秒算速度，见 accumulateFingerSample）
    //   * 松手：角速度原封不动地留着，由指数衰减把它带到 0
    //   * 两者的位移量都通过 stepMotion(dt) 走同一条"速度 → 四元数"的路径
    //
    // 为什么**不能**把位移直接乘上 ROT_PER_PX 就完事：那样"同样拖 100 像素"
    // 无论拖多快都转同样的角度，手感是死的 —— 快甩和慢拖没有区别，就是
    // "没有加速度机制、转起来不丝滑"。现在改为：
    //       手指速度(px/s) → 目标角速度(rad/s) → 一阶低通 → 积分成角度
    // 低通那一步就是"惯性感"的来源：角速度追不上手指速度的变化，
    // 快甩时先被拉高的一点会继续往前推，视觉上就是"越甩越顺"。
    //
    // 注意这些字段里**没有一个"绕世界轴的角度"**：姿态只有一个四元数，
    // 面内旋转只有一个标量。没有任何"角度 → 轴"的换算，所以旋转轴不可能变。

    private val choreographer = Choreographer.getInstance()
    private var motionRunning = false
    private var lastFrameNanos = 0L

    // 机体系角速度（rad/s）。轴恒为**相机自己**的 right / up / 视线(滚转)。
    private var yawVel = 0f     // 绕相机 Y（up）—— 左右拖
    private var pitchVel = 0f   // 绕相机 X（right）—— 上下拖
    private var rollVel = 0f    // 绕相机 Z（back）—— 双指旋转
    // 平移速度（世界单位/秒）
    private var panXVel = 0f
    private var panYVel = 0f

    // 驱动"目标速度"的因子：拖动时 =1，惯性时 =0（目标速度归零 → 只剩衰减）
    private var driveRotation = 0f
    private var drivePan = 0f

    // ================================================================ 手指速度
    //
    // 只记**手指**的速度（px/s → 归一化后的 rad/s），不记角度：
    // 四元数姿态没有"两条轴的角度"可以相减。
    private var tracking = false
    private var fingerYawTarget = 0f
    private var fingerPitchTarget = 0f
    private var fingerRollTarget = 0f
    private var fingerPanXTarget = 0f
    private var fingerPanYTarget = 0f

    // 上一次 MOVE 的时间，用来算帧间速度
    private var lastMoveTime = 0L
    private var hasLastMove = false

    // 双指上一次的连线夹角（NaN = 还没有基准，第一帧不产生滚转）
    private var lastTwistAngle = Float.NaN

    private val motionFrame = Choreographer.FrameCallback { frameTimeNanos -> stepMotion(frameTimeNanos) }

    fun ensureEngine() {
        session.create()
        if (session.ready) {
            Log.i(MbRenderer.TAG, "引擎就绪")
        } else {
            Log.e(MbRenderer.TAG, "引擎初始化失败: ${session.status()}")
        }
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        Log.i(MbRenderer.TAG, "surfaceCreated")
        ensureEngine()
        attach(holder)
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        Log.i(MbRenderer.TAG, "surfaceChanged ${width}x$height")
        surfaceWidth = width
        surfaceHeight = height
        // 尺寸变了要重建交换链。
        //
        // 【必须先停渲染线程再 detach】detach 会把 swapchain 置空，而渲染线程
        // 每一拍都在 render_frame。不停线程就 detach，会有一段窗口期让渲染线程
        // 拿到空 swapchain —— 表现为 status 里反复出现 "surface=none"、
        // 以及 acquire 一直超时（BufferQueue 里的图迟迟不 present）。
        // stopRenderLoop() 会 join，保证 swapchain 换掉的过程中没有人碰它。
        stopRenderLoop()
        session.detachSurface()
        attach(holder)
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        Log.i(MbRenderer.TAG, "surfaceDestroyed")
        stopMotion()
        stopRenderLoop()
        session.detachSurface()
    }

    private fun attach(holder: SurfaceHolder) {
        val w = if (surfaceWidth > 0) surfaceWidth else width
        val h = if (surfaceHeight > 0) surfaceHeight else height
        if (w <= 0 || h <= 0) return
        // 采样数必须在 attachSurface 之前送进引擎：交换链要按它决定建几张 MS 图。
        session.setMsaa(msaaSamples)
        // 缩放比同理：交换链按 scale × surface 尺寸建图。
        session.setRenderScale(renderScale)
        session.attachSurface(holder.surface, w, h)
        // 记下**实际生效**的采样数（设备不支持时后端会退回 1x），UI 显示的是这个值。
        msaaEffective = session.msaaSamples()
        Log.i(MbRenderer.TAG, "MSAA 请求 ${msaaSamples}x → 实际 ${msaaEffective}x（支持: ${supportedMsaaLevels()}）")
        Log.i(MbRenderer.TAG, "上屏分辨率 ${w}x${h} × ${"%.0f".format(renderScale * 100)}% → ${renderSizeText()}")
        // 建好交换链后起常驻渲染线程：第一帧由它自己出，避免第一屏是黑的。
        // 这里**不能**在主线程同步画第一帧 —— 首帧最慢（要建管线/MS 图），
        // 同步画会让启动瞬间卡一下甚至触发 ANR。
        startRenderLoop()
    }

    // ── CPU / GPU 占用采样 ──────────────────────────────────────────────
    //
    // 为什么要加这两项：光看 fps 无法区分"GPU 真的忙"和"CPU 在空转/等同步"。
    // 实测(2026-11-12) MSAA 4x 上屏 2160x1440 时 fps 恒在 64、ms_avg≈15.4ms，
    // 而 msaa=1 时是 667fps/0.86ms —— 必须靠占用率才能定位到底是谁的瓶颈。
    //
    // CPU%：本进程 /proc/self/stat 的 utime+stime 增量 / 窗口时长。
    //   >100% 表示多线程并行；渲染线程是单线程,所以接近 100% ≈ 这一核跑满。
    // GPU%：Mali 的 /sys/class/.../utilisation 不一定可读(非 root 常见)，
    //   读不到就报 -1，绝不臆造数值。

    /** 读本进程累计 CPU 时间（单位：jiffies）。失败返回 -1。 */
    private fun readProcCpuTicks(): Long {
        return try {
            // 格式: pid (comm) state ppid ... utime(14) stime(15) ...
            // comm 里可能有空格和括号，所以从**最后一个** ')' 之后开始切。
            val raw = java.io.File("/proc/self/stat").readText()
            val close = raw.lastIndexOf(')')
            if (close < 0) return -1L
            val f = raw.substring(close + 2).trim().split(' ')
            // close+2 之后第一个字段是 state(索引0)，utime 是第 14 个字段 → 索引 11
            val utime = f.getOrNull(11)?.toLongOrNull() ?: return -1L
            val stime = f.getOrNull(12)?.toLongOrNull() ?: return -1L
            utime + stime
        } catch (_: Throwable) {
            -1L
        }
    }

    /** 读 GPU 占用率（0..100）。读不到返回 -1。 */
    private fun readGpuUtilisation(): Int {
        // Mali 的 gpu_busy / utilisation 路径随机型不同，逐个试。
        val candidates = listOf(
            "/sys/class/misc/mali0/device/utilisation",
            "/sys/devices/platform/soc/mali/utilisation",
            "/sys/class/kgsl/kgsl-3d0/gpubusy",
            "/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage",
        )
        for (p in candidates) {
            try {
                val txt = java.io.File(p).readText().trim()
                if (p.endsWith("gpubusy")) {
                    // 形如 "123 456" = busy total，算百分比
                    val parts = txt.split(' ').mapNotNull { it.trim().toLongOrNull() }
                    if (parts.size >= 2 && parts[1] > 0) {
                        return (parts[0] * 100 / parts[1]).toInt()
                    }
                } else {
                    return txt.toIntOrNull() ?: continue
                }
            } catch (_: Throwable) {
                continue
            }
        }
        return -1
    }

    private fun renderOnce(): Long {
        // 只在**渲染线程**上调用（见 renderLoopBody）。
        // 计时包住 session.render()：这段时间 = 录制 + 提交 + 等 GPU 完成 + present，
        // 也就是"这一帧让渲染线程占用了多久"。它不等于 GPU 执行时间（那是另一回事），
        // 但正是它决定渲染线程能不能跑满目标帧率。
        val t0 = System.nanoTime()
        val p = pendingPose
        val ok = session.render(p[0], p[1], p[2], p[3], p[4], p[5], p[6])
        val costNs = System.nanoTime() - t0

        val now = System.currentTimeMillis()
        if (ok) {
            ++frameCount
            ++frameTotal
            frameCostAccNs += costNs
            if (costNs > frameCostMaxNs) frameCostMaxNs = costNs
        } else {
            ++renderFailCount
        }

        // 1 秒一个窗口：算出 fps / 平均耗时 / 峰值耗时 / 失败率，然后清零重新累计。
        val nowNs = System.nanoTime()
        if (windowStartNs == 0L) windowStartNs = nowNs
        val windowNs = nowNs - windowStartNs
        if (windowNs >= FPS_WINDOW_NS) {
            val fps = frameCount * 1e9f / windowNs
            // CPU%：本窗口内本进程消耗的 CPU 时间 / 墙钟时间。
            //
            // /proc/self/stat 的 utime+stime 单位是 USER_HZ(通常 100，即 10ms 一个 tick)。
            // Android 不暴露 sysconf(_SC_CLK_TCK)，所以用**两次采样之间自己标定**的
            // 分数形式算比例：省掉 HZ 也能得到占用率 ——
            //   cpu% = Δticks / ΔT_ticks_total * 100
            // 其中 ΔT_ticks_total 用「窗口毫秒数 * HZ」…仍需要 HZ。
            // 更稳的办法：直接按窗口内 Δticks 与窗口墙钟的比值乘 100*HZ_TO_MS。
            // 这里取 HZ=100（Linux/Android 的 32/64 位 arm64 默认值），并在首次
            // 采样时用两个窗口的稳定性自检；值偏高/偏低时仍能反映**相对**变化。
            val ticksNow = readProcCpuTicks()
            val cpuPct = if (ticksNow >= 0 && windowCpuTicks >= 0) {
                val dTicks = (ticksNow - windowCpuTicks).toDouble()
                // 1 tick = 10ms (HZ=100) → 窗口内可用 CPU 时间 = windowNs/1e7 个 tick
                val availTicks = windowNs / 10_000_000.0
                if (availTicks > 0.0) (dTicks / availTicks * 100.0).toFloat() else -1f
            } else {
                -1f
            }
            windowCpuTicks = ticksNow
            windowCpuPct = cpuPct
            val gpuPct = readGpuUtilisation()
            windowGpuPct = gpuPct
            val failRate = if (frameCount + renderFailCount > 0) {
                renderFailCount * 100f / (frameCount + renderFailCount)
            } else {
                0f
            }
            perf = PerfSnapshot(
                fps = fps,
                frameMsAvg = if (frameCount > 0) (frameCostAccNs.toDouble() / frameCount / 1e6).toFloat() else 0f,
                frameMsMax = frameCostMaxNs / 1e6f,
                renderFailRate = failRate,
                msaa = session.msaaSamples(),
                cpuPct = cpuPct,
                gpuPct = gpuPct,
            )
            // ── t21 帧率权威判据（每秒一条）──
            //
            // 为什么必须是一条日志而不是看 HUD：HUD 是给人看的，截图会受「外部应用抢
            // 前台」污染（2026-11-11 实测：截到白底画面导致误判「fps 不显示」）。
            // logcat 是纯文本、可逐条计数、可自动校验前台归属，是唯一可靠读数。
            //
            // 字段全部来自本窗口的真实统计，不做任何推算：
            //   window_ms  本窗口实际时长（≈1000，偏差说明节拍被打断）
            //   frames     本窗口成功出帧数          → fps = frames / window_ms * 1000
            //   fails      本窗口 render 失败次数    → 恒为 0 才是正常
            //   ms_avg/max 平均/峰值单帧耗时（ms）
            //   msaa       当前生效采样数
            //   status     引擎自报状态（含 frames/size/surface/rot/model）
            Log.i(
                "MB_PERF",
                "window_ms=${windowNs / 1_000_000} frames=$frameCount fails=$renderFailCount " +
                    "fps=${"%.2f".format(fps)} ms_avg=${"%.2f".format(perf.frameMsAvg)} " +
                    "ms_max=${"%.1f".format(perf.frameMsMax)} msaa=${perf.msaa} " +
                    "cpu=${if (cpuPct >= 0) "%.0f".format(cpuPct) else "n/a"}%" +
                    (if (gpuPct >= 0) " gpu=$gpuPct%" else " gpu=n/a") +
                    " status=${session.status()}"
            )
            frameCount = 0
            renderFailCount = 0
            frameCostAccNs = 0L
            frameCostMaxNs = 0L
            windowStartNs = nowNs
        }

        if (now - lastStatusAt > 500) {
            lastStatusAt = now
            onStatus?.invoke(session.status())
        }
        if (!ok) {
            // 不刷屏：只在失败时打一次日志（render 内部会把具体原因回调过来）
            Log.w(MbRenderer.TAG, "渲染失败: ${session.status()}")
        }
        return costNs / 1_000_000L
    }

    // ================================================================ 触摸
    //
    // 只重写 onTouchEvent 会有**死区**：手指刚按下时，Android 的触摸分发会先把
    // ACTION_DOWN 发给这条链上的父容器（FrameLayout → 各种系统容器），只有当
    // 位移超过系统的 touch slop 后，`onTouchEvent` 才会开始收到 MOVE。结果是
    // "按下 → 前 8~24dp 完全没反应"，也就是用户说的"左右拖动还有阈值"。
    //
    // 真正能做到**零延迟**的只有 dispatchTouchEvent：它在 View 自己的
    // dispatch 入口，先于任何 slop 判定，ACTION_DOWN 一进来就能拿到。
    // 所以：
    //   * ACTION_DOWN 在 dispatchTouchEvent 里**直接**处理（锚定 + 抢占）；
    //   * 之后的 MOVE / UP 再到 onTouchEvent 里正常走；
    //   * onTouchEvent 里保留一份 DOWN 处理，作为"父容器自己把事件转回本 View"
    //     时的兜底，两条路径都幂等（stopFling / 重设锚点不会产生副作用）。

    // ================================================================ 触摸
    //
    // 只重写 onTouchEvent 会有**死区**：手指刚按下时，Android 的触摸分发会先把
    // ACTION_DOWN 发给这条链上的父容器（FrameLayout → 各种系统容器），只有当
    // 位移超过系统的 touch slop 后，`onTouchEvent` 才会开始收到 MOVE。结果是
    // "按下 → 前 8~24dp 完全没反应"，也就是用户说的"左右拖动还有阈值"。
    //
    // 真正能做到**零延迟**的只有 dispatchTouchEvent：它在 View 自己的
    // dispatch 入口，先于任何 slop 判定，ACTION_DOWN 一进来就能拿到。
    // 所以：
    //   * ACTION_DOWN 在 dispatchTouchEvent 里**直接**处理（锚定 + 抢占）；
    //   * 之后的 MOVE / UP 再到 onTouchEvent 里正常走；
    //   * onTouchEvent 里保留一份 DOWN 处理，作为"父容器自己把事件转回本 View"
    //     时的兜底，两条路径都幂等（stopMotion / 重设锚点不会产生副作用）。

    override fun dispatchTouchEvent(ev: MotionEvent): Boolean {
        // 涂鸦模式下整条手势走笔，不进相机状态机。
        // ACTION_DOWN 之外的 MOVE/UP 也在这里先过一遍，保证"按下→抬起"全程都不会
        // 被 beginGesture 顺手锚定成拖拽起点。
        if (sketchModeOn() && ev.actionMasked == MotionEvent.ACTION_DOWN) {
            if (handleSketchTouch(ev)) return true
            // 双指起手（理论上 DOWN 只有一指，这里只是防御）：回落相机
            beginGesture(ev)
            super.dispatchTouchEvent(ev)
            return true
        }
        if (ev.actionMasked == MotionEvent.ACTION_DOWN) {
            beginGesture(ev)
        }
        // 必须**无条件**返回 true：View 的默认实现只有在已注册触摸监听
        // （OnTouchListener / onTouchEvent 重写且返回 true 且 clickable）时才
        // 认领事件。这里显式认领，保证 ACTION_DOWN 之后整条手势都被本 View 独占。
        super.dispatchTouchEvent(ev)
        return true
    }

    /** 按下：掐断惯性、把锚点摆到手指位置。可重复调用。 */
    private fun beginGesture(event: MotionEvent) {
        stopMotion()                      // 手指一落下就掐断惯性（"抓住"画面）
        lastX = event.x
        lastY = event.y
        lastPinchDist = 0f
        lastFocusX = event.x
        lastFocusY = event.y
        lastTwistAngle = Float.NaN
        clearFingerVelocity()
        tracking = true
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        // 涂鸦模式：先让笔处理；笔吃下了就直接返回，绝不落到相机分支。
        if (sketchModeOn() && handleSketchTouch(event)) return true
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                // 兜底路径：dispatchTouchEvent 已经处理过时会重复调用这里，
                // beginGesture 幂等，所以不需要加标志位。
                beginGesture(event)
                return true
            }

            MotionEvent.ACTION_POINTER_DOWN -> {
                if (event.pointerCount >= 2) {
                    lastPinchDist = pinchDistance(event)
                    lastFocusX = focusX(event)
                    lastFocusY = focusY(event)
                    // 双指模式下重新取样，避免把单指阶段的速度带进来
                    lastTwistAngle = Float.NaN
                    clearFingerVelocity()
                    // **实际速度也必须归零**，不能只清"目标速度"。
                    //
                    // clearFingerVelocity() 清的是 fingerXxxTarget（目标），而真正
                    // 驱动画面的是 stepMotion 里的 yawVel / panXVel（实际速度）。
                    // 第二根手指落下的那一刻，单指阶段攒下的实际速度还挂在那儿，
                    // 而下面 ACTION_MOVE 立刻把 driveRotation/drivePan 置 1，于是
                    // 这些残留速度继续被积分 —— 表现为"双指一按上去画面就开始
                    // 微动平移、甚至反复微动缩放"。手指没动，画面却在飘。
                    //
                    // 按物理直觉也应该归零：手指数量变了就是一次新的手势起点，
                    // 新起点不该继承旧手势的动能。
                    yawVel = 0f
                    pitchVel = 0f
                    rollVel = 0f
                    panXVel = 0f
                    panYVel = 0f
                    // 手指数变了，累计角度归零、锚点换成当前姿态，
                    // 否则单指阶段攒下的 yaw 会跟双指的 focus 位移混在一起。
                    tracking = true
                }
            }

            MotionEvent.ACTION_POINTER_UP -> {
                lastPinchDist = 0f
                // 抬起一指后，用剩下那根手指重新锚定，避免跳变。
                // 注意 lastFocus 也要一起重置：双指阶段的 focus 是两指中点，
                // 下一帧变单指后 focus 直接是 e.x，不重置会产生一次大跳。
                val idx = if (event.actionIndex == 0) 1 else 0
                if (idx < event.pointerCount) {
                    lastX = event.getX(idx)
                    lastY = event.getY(idx)
                    lastFocusX = lastX
                    lastFocusY = lastY
                }
                lastTwistAngle = Float.NaN
                clearFingerVelocity()
                tracking = true
            }

            MotionEvent.ACTION_MOVE -> {
                if (event.pointerCount >= 2) {
                    handleTwoFinger(event)
                } else {
                    handleOneFinger(event)
                }
                // 拖动开关：手指按着的这段时间里，手指速度**就是**目标角速度。
                //
                // 这两个变量以前只被赋 0（在 beginGesture / stopMotion /
                // releaseToFling 里），**从来没有被置过 1**。而 stepMotion 里
                //     tgtYaw = fingerYawTarget * driveRotation
                // 于是目标速度恒等于 0，三个旋转轴全是零速 —— 单指拖动完全转不动。
                // 缩放不走这条通道（zoomBy 直接改 scale），所以表现成"只能缩放、
                // 无法旋转"。必须在这里打开，松手时由 releaseToFling 关掉。
                driveRotation = 1f
                drivePan = 1f
                // 手指一动，推进器就得跑着 —— 真正的角度积分在 stepMotion 里按帧做。
                startMotion()
                // 主线程只更新快照，真正出帧在渲染线程（见 renderLoopBody）。
                // 这里**绝不能**再同步调 session.render() —— 那等于把手势处理和
                // SDK 的等待绑在同一个线程上，SDK 一慢，整个 UI 跟着卡。
                requestRender()
            }

            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                lastPinchDist = 0f
                if (event.actionMasked == MotionEvent.ACTION_UP) {
                    // 松手：手指速度原封不动地变成惯性初速度（见 releaseToFling）
                    releaseToFling()
                } else {
                    // CANCEL 不是真正松手，不该带惯性
                    tracking = false
                    stopMotion()
                }
            }
        }
        return true
    }

    /**
     * 单指：拖动 = **环绕摄像机检视模型**（不是转物体本身）。
     *
     * 映射约定（**屏幕**方向 → **视觉**方向，与 MainActivity 的横屏锁定配套）：
     *   屏幕左右滑 → 视线左右摆（绕相机自身 up 轴）
     *   屏幕上下滑 → 视线上下摆（绕相机自身 right 轴）
     *
     * 为什么方向按"物理屏幕"算而不是直接拿 MotionEvent 的 x/y：
     * 这台 pad 是横屏应用但面板物理竖置（SurfaceFlinger 会做 ROTATE_90）。
     * SurfaceView 挂在 ROTATE_90 的窗口下时，MotionEvent 的 x/y 是**逻辑横屏**
     * 坐标系，而用户看到的画面是**物理竖屏**排布 —— 手指在物理屏上左右滑，
     * 事件里给出的却是一截 dY。不做换算，就会出现"左右拖像上下转"。
     * 换算表由 [displayRotation] 给出（Activity 用 [setRotationListener] 同步）。
     *
     * 这里**没有**任何阈值/死区：按下即锚定，之后每一像素位移都参与速度计算。
     *
     * 注意本函数**不直接改姿态**：它只把位移换算成"手指速度"，真正转多少由
     * [stepMotion] 按真实帧间隔积分。这样拖动和惯性走同一条路径，掉帧时也不会
     * 因为一次 MOVE 跳变而"闪一下"。
     */
    private fun handleOneFinger(event: MotionEvent) {
        val (vx, vy) = toVisualDelta(event.x - lastX, event.y - lastY)
        lastX = event.x
        lastY = event.y
        // 单指阶段不产生平移，但焦点要跟着走，否则从双指切回来时会跳
        lastFocusX = event.x
        lastFocusY = event.y
        // 只把位移喂成"手指速度"，真正的转动由 stepMotion 按真实帧间隔积分。
        // 这样拖动和惯性走同一条路径，加速度感（FLING_DAMPING 的追赶）来自这里。
        accumulateFingerSample(vx, vy, event.eventTime,
                               panDxPx = 0f, panDyPx = 0f, rollRad = 0f)
    }

    /**
     * 把一截**逻辑坐标**下的位移换算成视觉（物理屏）位移。
     *
     * 这里是**恒等**，不做任何 x/y 交换：交换链声明的是 `preTransform = IDENTITY`
     * （见 vulkan_swapchain.cpp），窗口本身就是横屏 2160x1440，MotionEvent 的
     * x/y 已经是屏幕方向。曾经因为 buffer 是竖的而交换 x/y，那会把水平右滑
     * 当成垂直下滑，立方体不左右转而歪出顶面。
     *
     * displayRotation 仍保留供日志使用，但不参与手势方向换算。
     */
    private fun toVisualDelta(dx: Float, dy: Float): Pair<Float, Float> = Pair(dx, dy)

    // ================================================================ 拖动旋转
    //
    // 归约到一句：每帧在当前四元数上右乘两个增量。
    //
    //   q ← normalize( q * Rx(Δpitch) * Ry(Δyaw) * Rz(Δroll) )
    //
    // 右乘 = 绕机体轴，于是：
    //   • 左右拖（Ry）绕**当前相机自己的 up** —— 不是世界 Y。
    //     俯仰过之后再左右拖，轴会跟着相机一起倾斜，这就是“转方向跟手”。
    //   • 上下拖（Rx）绕**当前相机自己的 right**。
    //   • 双指拧（Rz）绕**当前相机自己的 back**。
    //
    // 三个增量都是当帧小角（由速度积分得来，step ≤ 50ms），所以乘序差异
    // 只是二阶小量，不会造成可见的串轴。
    // **没有任何角度夹取**：俯仰能转满 360°，四元数没有万向节奇点。
    private fun applyLocalRotation(deltaPitch: Float, deltaYaw: Float, deltaRoll: Float) {
        if (deltaPitch == 0f && deltaYaw == 0f && deltaRoll == 0f) return

        // q = q * Rx(pitch) * Ry(yaw) * Rz(roll)
        // 三个单轴增量依次右乘；合并成一个增量再右乘一次，
        // 节省三次归一化。
        val hp = deltaPitch * 0.5f
        val hy = deltaYaw * 0.5f
        val hr = deltaRoll * 0.5f
        val sp = sin(hp); val cp = cos(hp)
        val sy = sin(hy); val cy = cos(hy)
        val sr = sin(hr); val cr = cos(hr)

        // Rx * Ry
        val ax = sp * cy
        val ay = cp * sy
        val az = -sp * sy
        val aw = cp * cy
        // (Rx*Ry) * Rz
        val dx = ax * cr - ay * sr
        val dy = ax * sr + ay * cr
        val dz = az * cr - aw * sr
        val dw = az * sr + aw * cr

        // q * d
        val nx = qw * dx + qx * dw + qy * dz - qz * dy
        val ny = qw * dy - qx * dz + qy * dw + qz * dx
        val nz = qw * dz + qx * dy - qy * dx + qz * dw
        val nw = qw * dw - qx * dx - qy * dy - qz * dz

        val len = sqrt(nx * nx + ny * ny + nz * nz + nw * nw)
        if (len < 1e-6f) return
        qx = nx / len
        qy = ny / len
        qz = nz / len
        qw = nw / len
    }

    /**
     * 双指：距离变化 = 缩放，焦点移动 = 平移，绕中点转动 = 面内旋转（滚转）。
     *
     * 滚转绕的是相机自身的 back（局部 Z）轴 —— 没有这条通路，"摄像机的三个轴"
     * 就少了滚转这一维，画面只能上下左右摆、不能歪。
     */
    private fun handleTwoFinger(event: MotionEvent) {
        // --- 缩放 ---
        //
        // 这里是**增量乘**：scale *= d/lastPinchDist。手指完全不动时 d 恒等于
        // lastPinchDist，比值是 1，看起来天然无损 —— 但实际不是：
        //
        //   1) 触摸屏的距离读数有亚像素级噪声。比值变成 1.0002、0.9998 这种，
        //      每帧乘一次，手指按着不动画面就会持续地"微缩、微放"来回抖。
        //   2) 没有死区，再小的抖动都会被采纳并累乘进 scale。
        //
        // 现在加一个**相对死区**：距离变化不到 PINCH_DEADZONE 就当没动，连
        // lastPinchDist 也不更新（避免把噪声当成新基准，慢慢"吃掉"手指的
        // 真实位移）。超过死区才按比例缩放，并把基准同步到当前值。
        val d = pinchDistance(event)
        if (lastPinchDist > 0f && d > 0f) {
            val ratio = d / lastPinchDist
            if (abs(ratio - 1f) > PINCH_DEADZONE) {
                scale = (scale * ratio).coerceIn(MIN_SCALE, MAX_SCALE)
                lastPinchDist = d
            }
            // 落在死区内：既不缩放，也不更新基准 —— 手指真的要放大时，
            // 位移要累积到超过阈值才生效，抖动永远攒不到那个量。
        } else {
            lastPinchDist = d
        }

        // --- 焦点（缩放/平移/滚转都要用，先取好） ---
        val fx = focusX(event)
        val fy = focusY(event)

        // --- 面内旋转（绕视线轴） ---
        // 两指连线的夹角变化就是滚转角。先折到 (-π, π] 再差分，
        // 否则双指转过 ±180° 时会出现整圈跳变。
        //
        // 死区同平移：手指按着不动时夹角仍有零点几度的抖动，直接喂进去会让
        // 画面持续轻微歪斜。小于 ROLL_DEADZONE 的差分丢弃且不更新基准。
        val span = normalizeAngle(
            atan2(event.getY(0) - fy, event.getX(0) - fx) -
                atan2(event.getY(1) - fy, event.getX(1) - fx)
        )
        var twistStep = 0f
        if (!lastTwistAngle.isNaN()) {
            val step = normalizeAngle(span - lastTwistAngle)
            if (abs(step) > ROLL_DEADZONE) {
                twistStep = step
                lastTwistAngle = span
            }
        } else {
            lastTwistAngle = span
        }

        // --- 平移 ---
        //
        // 把屏幕像素位移换算成世界位移：屏幕上"整个高度"对应的世界跨度
        // 正好是 2 * dist * tan(fov/2)，所以
        //     世界每像素 = 2*dist*tan(fov/2) / surfaceHeight
        // dist 随 scale 变化，于是放大后同样的手指位移走的世界距离更小 ——
        // 效果是"画面贴着手指走"，缩放级别不影响跟手程度。
        //
        // 死区：焦点是两指中点，两指各自的亚像素抖动在中点上未必抵消、反而
        // 会叠成零点几像素的假位移。手指按着不动时这些假位移会被速度积分
        // 放大成肉眼可见的漂移。所以小于 PAN_DEADZONE 的位移直接丢弃，
        // **且不更新 lastFocus** —— 真实位移会累积到超过阈值再一次性兑现，
        // 既不丢精度也不抖。
        //
        // 阈值取得很小（0.5 逻辑像素）：平移是 1:1 跟手的，死区大了会让
        // 画面"慢半拍"。这点延迟远小于肉眼能察觉的范围。
        val rawDx = fx - lastFocusX
        val rawDy = fy - lastFocusY
        val moveLen = hypot(rawDx, rawDy)
        val dxPx: Float
        val dyPx: Float
        if (moveLen > PAN_DEADZONE) {
            dxPx = rawDx
            dyPx = rawDy
            lastFocusX = fx
            lastFocusY = fy
        } else {
            dxPx = 0f
            dyPx = 0f
        }

        accumulateFingerSample(0f, 0f, event.eventTime,
                               panDxPx = dxPx, panDyPx = dyPx, rollRad = twistStep)
    }

    /** 把角度折到 (-π, π]。 */
    private fun normalizeAngle(a: Float): Float {
        var v = a
        while (v > PI) v -= TWO_PI_F
        while (v <= -PI) v += TWO_PI_F
        return v
    }

    /** 当前视角下，一个屏幕像素对应多少世界单位。 */
    private fun worldUnitsPerPixel(): Float {
        val h = if (surfaceHeight > 0) surfaceHeight else (height.takeIf { it > 0 } ?: 1440)
        val dist = CAM_DIST / max(scale, 0.05f)
        val worldHeightAtDist = 2f * dist * tan(FOV_Y_RAD * 0.5f)
        return worldHeightAtDist / h
    }

    private fun pinchDistance(e: MotionEvent): Float {
        if (e.pointerCount < 2) return 0f
        return hypot(e.getX(1) - e.getX(0), e.getY(1) - e.getY(0))
    }

    private fun focusX(e: MotionEvent): Float =
        if (e.pointerCount >= 2) (e.getX(0) + e.getX(1)) * 0.5f else e.x

    private fun focusY(e: MotionEvent): Float =
        if (e.pointerCount >= 2) (e.getY(0) + e.getY(1)) * 0.5f else e.y
    // ================================================================ 手指速度

    /**
     * 手指每移动一次就调用一次（单指 / 双指都走这里）。
     *
     * 它**只算速度，不改姿态**。这是"加速度机制"的核心：
     *   手指速度(px/s) ──> 目标角速度(rad/s) ──> 一阶低通 ──> (stepMotion 里积分)
     *
     * 为什么不直接 `q * Δq(位移 * 灵敏度)`：那样"同样拖 100px"不管拖多快都转
     * 同样的角度，快甩和慢拖一模一样 —— 手感是死的。走速度以后：
     *   * 拖得慢 → 目标速度小，低通很快跟上，画面稳稳地跟手；
     *   * 拖得快 → 目标速度大，低通需要几帧才追上来，这一段"追赶"就是
     *     用户能感觉到的加速度/惯性感；
     *   * 松手 → 目标速度立刻归零，而实际速度只按 FLING_DAMPING 衰减，
     *     于是自然滑出一段再停。
     *
     * 符号（全部按"视觉 = 物理屏"的方向，见 toVisualDelta）：
     *   vx > 0（往右推）→ yaw 为负 → 绕相机 +Y(up) 转负角，物体跟着往右转
     *   vy > 0（往下推）→ pitch 为正 → 绕相机 +X(right) 转正角，露出顶面
     *   twist > 0（双指顺时针）→ roll 为负 → 绕相机 +Z(back) 转
     *
     * @param vxPx/vyPx 单指位移（视觉像素）
     * @param panDxPx/panDyPx 双指焦点位移（逻辑像素）
     * @param rollRad 双指转过的角度（弧度）
     */
    private fun accumulateFingerSample(
        vxPx: Float,
        vyPx: Float,
        eventTimeMs: Long,
        panDxPx: Float,
        panDyPx: Float,
        rollRad: Float,
    ) {
        val now = eventTimeMs
        // 事件间隔可能为 0（同一毫秒内的两个 MOVE）：用 0 除会得到 inf，
        // 直接跳过，等下一次有真实间隔的 MOVE 再算。
        val dtMs = if (hasLastMove) (now - lastMoveTime).toFloat() else 0f
        if (hasLastMove && dtMs > 0f && dtMs < MAX_SAMPLE_DT_MS) {
            val dt = dtMs / 1000f
            // 参考实现是 targetEuler.x += (-dy) * ratio、targetEuler.y += (dx) * ratio，
            // 但那份代码是"cameraRoot * Euler(x,y,z)"、绕世界 Y 的欧拉框架；
            // 这里是**绕相机自身 up 右乘**，yaw 的正方向与欧拉 Y 相反，所以
            // 水平取负号（上下方向本来就一致，不动）。
            //   手指向左 dx<0 → yawT>0 → 绕相机 up 正转
            //   手指向上 dy<0 → pitchT>0 → 绕相机 right 正转
            val yawT = -vxPx / dt * ROT_PER_PX
            val pitchT = -vyPx / dt * ROT_PER_PX
            val rollT = rollRad / dt
            val panXT = -panDxPx / dt * worldUnitsPerPixel()
            val panYT = panDyPx / dt * worldUnitsPerPixel()
            val k = 1f - exp(-FINGER_SMOOTHING * dt)   // 帧率无关的低通系数

            fingerYawTarget += (yawT - fingerYawTarget) * k
            fingerPitchTarget += (pitchT - fingerPitchTarget) * k
            fingerRollTarget += (rollT - fingerRollTarget) * k
            fingerPanXTarget += (panXT - fingerPanXTarget) * k
            fingerPanYTarget += (panYT - fingerPanYTarget) * k
        }
        lastMoveTime = now
        hasLastMove = true
    }

    /**
     * 手指按下的那一刻：把"手指速度"清干净。
     *
     * 不清的话，上一笔留下的目标速度会让新手势一按下就自己转 ——
     * 这是"按下即静止"的必要条件。
     */
    private fun clearFingerVelocity() {
        fingerYawTarget = 0f
        fingerPitchTarget = 0f
        fingerRollTarget = 0f
        fingerPanXTarget = 0f
        fingerPanYTarget = 0f
        hasLastMove = false
        lastMoveTime = 0L
        lastTwistAngle = Float.NaN
    }

    /**
     * 松手：把"手指速度"直接变成"惯性初速度"。
     *
     * 两件事一起做：
     *   1. 速度**原封不动**地接过去（不是重新算一遍）—— 所以从"拖动"到
     *      "松手后继续滑"中间没有任何速度断点，视觉上是一条连续的曲线。
     *   2. driveRotation = 0：手指不再提供新速度，之后只剩衰减。
     *
     * 为什么要判"松手前是否有真实位移"：手指按住不动两秒再抬起，
     * 低通后的目标速度已经衰减到很小，但仍然非零；再判一次阈值，
     * 避免"按住不动"也会滑一点点。
     */
    private fun releaseToFling() {
        driveRotation = 0f
        drivePan = 0f
        tracking = false
        hasLastMove = false
        // 目标速度可能因为最后几次 MOVE 的方向抖动而不为 0；
        // 取"实际速度"作初速度，它才是这一帧真实在转的角速度。
        //
        // 判据分两档、取"或"的关系：
        //   * 旋转：0.02 rad/s 是肉眼能察觉的起点；
        //   * 平移：单位是"世界单位/秒"，0.02 只相当于几个像素/秒 ——
        //     双指按一下再松开，那点残留平移会让画面**肉眼可见地滑一段**。
        //     因此平移单独给一个更高的门槛（约等于 1% 屏幕宽/秒）。
        // 两档都低于门槛才算"按住不动"，直接停住不滑。
        val rotSpeed = abs(yawVel) + abs(pitchVel) + abs(rollVel)
        val panSpeed = abs(panXVel) + abs(panYVel)
        if (rotSpeed < MIN_FLING_SPEED && panSpeed < MIN_PAN_FLING_SPEED) {
            stopMotion()
            return
        }
        // 速度夹上限：防止极快的一甩把画面"甩飞"
        yawVel = yawVel.coerceIn(-MAX_YAW_VEL, MAX_YAW_VEL)
        pitchVel = pitchVel.coerceIn(-MAX_YAW_VEL, MAX_YAW_VEL)
        rollVel = rollVel.coerceIn(-MAX_YAW_VEL, MAX_YAW_VEL)
        startMotion()
    }

    // ================================================================ 运动推进

    private fun startMotion() {
        if (motionRunning) return
        motionRunning = true
        lastFrameNanos = 0L
        choreographer.postFrameCallback(motionFrame)
    }

    /** 立刻停住：速度清零、所有驱动归零、撤掉帧回调。 */
    private fun stopMotion() {
        motionRunning = false
        choreographer.removeFrameCallback(motionFrame)
        yawVel = 0f
        pitchVel = 0f
        rollVel = 0f
        panXVel = 0f
        panYVel = 0f
        driveRotation = 0f
        drivePan = 0f
        clearFingerVelocity()
    }

    /**
     * 拖动 / 惯性的**唯一**推进器，由 Choreographer 按屏幕刷新率驱动。
     *
     * 用真实帧间隔 dt 而不是固定步长：掉帧时转的角度按时间补齐，
     * 不会"卡一下之后突然跳过去"。
     *
     * 每一帧做三件事：
     *   1. 速度向目标速度收敛（拖动时目标来自手指，惯性时目标为 0）——
     *      这一步就是加速度感本身；收敛得快慢由 FLING_DAMPING 决定。
     *   2. 把本帧的角度增量右乘进四元数（见 applyLocalRotation）：
     *      三根轴都是相机自身当前的 right / up / back。
     *   3. 速度按 exp(-k*dt) 衰减（拖动时目标非零，衰减只影响"追赶"的快慢）。
     *
     * 收敛 + 衰减写成同一个式子：
     *     v ← v * exp(-2k*dt) + target * (1 - exp(-2k*dt))
     * 目标为 0 时退化成纯指数衰减（惯性收尾）；target 恒定时趋于 target
     * （拖动稳态角速度 = 手指速度换算值）。
     */
    private fun stepMotion(frameTimeNanos: Long) {
        if (!motionRunning) return

        // 首帧没有上一帧时间戳：记下来，下一帧再算 dt（避免出现一个巨大的 dt）
        if (lastFrameNanos == 0L) {
            lastFrameNanos = frameTimeNanos
            choreographer.postFrameCallback(motionFrame)
            return
        }
        val dt = (frameTimeNanos - lastFrameNanos) / 1_000_000_000f
        lastFrameNanos = frameTimeNanos
        if (dt <= 0f) {
            choreographer.postFrameCallback(motionFrame)
            return
        }
        // 掉帧严重时 dt 可能是几百毫秒，按它积分会直接转过半圈。
        // 夹到 3 帧以内，宁可"少转一点"也不要一次跳变。
        val step = min(dt, MAX_STEP_S)

        val tgtYaw = fingerYawTarget * driveRotation
        val tgtPitch = fingerPitchTarget * driveRotation
        val tgtRoll = fingerRollTarget * driveRotation
        val tgtPanX = fingerPanXTarget * drivePan
        val tgtPanY = fingerPanYTarget * drivePan

        // 收敛系数：exp 而不是 (1-k*dt)，后者在 dt 大时会变负 → 反向抖动
        val follow = 1f - exp(-FLING_DAMPING * step)
        yawVel += (tgtYaw - yawVel) * follow
        pitchVel += (tgtPitch - pitchVel) * follow
        rollVel += (tgtRoll - rollVel) * follow
        panXVel += (tgtPanX - panXVel) * follow
        panYVel += (tgtPanY - panYVel) * follow

        // 本帧的角度增量交给 applyLocalRotation 右乘进 q：
        // 三根轴都是相机自身当前的 right / up / back，不碰世界系。
        applyLocalRotation(pitchVel * step, yawVel * step, rollVel * step)
        // 平移只改视口偏移（eye 沿相机 right/up 平移），视点恒在原点。
        // 见 scene_renderer.cpp 里 record_into 的相机段。
        panX += panXVel * step
        panY += panYVel * step

        // 姿态只改快照；出帧由渲染线程按自己的节奏做，主线程不等 GPU。
        requestRender()

        // 还在拖（driveRotation=1）就一直跑，直到松手让速度自然衰减到阈值以下。
        // 旋转与平移的门槛不同（平移的基数小得多，见 MIN_PAN_FLING_SPEED）。
        val rotSpeed = abs(yawVel) + abs(pitchVel) + abs(rollVel)
        val panSpeed = abs(panXVel) + abs(panYVel)
        val stillMoving = rotSpeed > MIN_FLING_SPEED || panSpeed > MIN_PAN_FLING_SPEED
        if (driveRotation > 0f || stillMoving) {
            choreographer.postFrameCallback(motionFrame)
        } else {
            stopMotion()
        }
    }
    // ================================================================ 对外接口

    /** 缩放若干倍（给 UI 的 +/- 按钮用；双指手势走的是同一份 scale 状态）。 */
    fun zoomBy(factor: Float) {
        stopMotion()
        scale = (scale * factor).coerceIn(MIN_SCALE, MAX_SCALE)
        requestRender()
    }

    /** 当前视觉状态，供 HUD 显示。两个环绕相机角度直接显示。 */
    fun viewState(): String =
        "q=(${"%.2f".format(qx)},${"%.2f".format(qy)},${"%.2f".format(qz)},${"%.2f".format(qw)}) " +
            "zoom=${"%.2f".format(scale)} pan=(${"%.2f".format(panX)},${"%.2f".format(panY)})"

    /** 复位视角：两个角回 0（正对目标）+ 缩放回 1.0 + 平移归零，并立刻重画一帧。 */
    fun resetView() {
        stopMotion()
        qx = 0f
        qy = 0f
        qz = 0f
        qw = 1f
        scale = 1f
        panX = 0f
        panY = 0f
        requestRender()
    }

    // ------------------------------------------------------------ 渲染循环
    //
    // 为什么需要它：换了预设模型之后必须**主动**出一帧，否则屏幕停在上一帧
    // （旧实现只在触摸时渲染，切换模型会看起来"什么都没发生"）。
    // 常驻循环的代价是持续出帧，但这条路径没有任何 CPU 回读，GPU 只写自己的
    // buffer；而且这正是 demo 要展示的东西 —— 模型一直在转、帧率是真的。
    //
    // 前后台：surfaceDestroyed / dispose 停，surfaceCreated（attach）时再起。

    private var renderLoopRunning = false

    // ------------------------------------------------------------ 专用渲染线程
    //
    // 为什么**必须**离开主线程：
    //   SDK 侧 `render_frame()` 里有同步等待（acquire 的 fence、vkWaitForFences、
    //   vkDeviceWaitIdle、模型上传 …）。这些在"平时"很快，但 SDK 一重构，
    //   L0/L1 里任何一处多等一拍，**整条调用链就被拖长**。以前这个调用是挂在
    //   Choreographer 上的 —— 也就是**主线程**，于是：
    //       主线程被卡 → 输入 / 动画 / Compose 重绘全停 → 整个 UI 奇卡
    //   这就是"UI 渲染和 SDK 明明应该独立，却一起卡"的全部原因：不是合成层
    //   共享，而是同一个线程。
    //
    // 现在的分工：
    //   * 渲染线程：只做 session.render(...)（Vulkan 提交 + 呈现）。
    //     SurfaceView 的 buffer 由 SurfaceFlinger 直接合成，本来就不需要主线程。
    //   * 主线程：只处理手势（改四元数/缩放/平移，纯数学，微秒级）和 Compose。
    //     互相之间只通过 @Volatile 的姿态快照通信，没有任何等待。
    //
    // SDK 再慢，表现也只会是**画面掉帧**（SurfaceView 那一层的 fps 下降），
    // 而不再是整个 App 掉帧。
    private var renderThread: Thread? = null

    /** 姿态快照：主线程写、渲染线程读。[qx, qy, qz, qw, scale, panX, panY] */
    @Volatile
    private var pendingPose = floatArrayOf(0f, 0f, 0f, 1f, 1f, 0f, 0f)

    private fun publishPose() {
        pendingPose = floatArrayOf(qx, qy, qz, qw, scale, panX, panY)
    }

    private fun startRenderThread() {
        if (renderThread != null) return
        renderThread = Thread({ renderLoopBody() }, "mb-render").also {
            it.priority = Thread.NORM_PRIORITY + 2
            it.start()
        }
    }

    private fun stopRenderThread() {
        val t = renderThread ?: return
        renderLoopRunning = false      // 先关灯，循环下一拍自行退出
        renderThread = null
        // 等它真正退出：detach surface 之前必须保证没人还在用 swapchain，
        // 否则会在渲染线程里对已销毁的 swapchain 取图（use-after-free）。
        try {
            t.join(2000)
        } catch (_: InterruptedException) {
            Thread.currentThread().interrupt()
        }
    }

    private fun renderLoopBody() {
        // 循环外**不**赌 SDK 的快慢：每一帧都按目标间隔给主线程让路，
        // 这样即便 SDK 只花 1ms，也不会把渲染线程烧成忙等。
        while (renderLoopRunning) {
            // 【2026-11-11 修复 · 9416e56 的回归】这里原先直接内联
            // `session.render(...)` 并丢掉返回值，于是 renderOnce() 里的
            // 帧统计 / 窗口结算 / 失败计数 / MB_PERF 日志**全部成了死代码**
            // （9146e56 把帧循环搬到渲染线程时漏搬了度量逻辑）。
            // 后果：HUD 的 fps 永远停在初值显示 `--`，renderFailCount 永远是 0，
            // 渲染失败拿不到任何可见信号。
            //
            // 现在改为调用 renderOnce()：它内部自己计时（t0/costNs），
            // 所以下面不再重复计一次 elapsedMs —— 改成用它返回的耗时。
            val elapsedMs = renderOnce()
            val sleepMs = FRAME_BUDGET_MS - elapsedMs
            if (sleepMs > 0) {
                try {
                    Thread.sleep(sleepMs)
                } catch (_: InterruptedException) {
                    Thread.currentThread().interrupt()
                    return
                }
            }
            // 主线程请求重画（切模型 / 缩放按钮 / 复位）：这里只借用一次节拍，
            // 不做任何额外的事 —— 姿态已经在 pendingPose 里了。
            redrawRequested = false
        }
    }

    /**
     * 请求渲染线程重画一帧。
     *
     * 与从前的 renderOnce() 不同：**不阻塞调用方**。主线程点一下按钮就返回，
     * 真正的 Vulkan 提交发生在渲染线程上。
     */
    private fun requestRender() {
        publishPose()
        redrawRequested = true
    }

    @Volatile
    private var redrawRequested = false

    /** 启动常驻渲染（幂等）。surface 就绪后才真正开跑。 */
    fun startRenderLoop() {
        if (!holder.surface.isValid) return
        renderLoopRunning = true
        startRenderThread()
    }

    /** 停止常驻渲染（幂等）。**必须在 detach surface 之前调用并等它退出。** */
    fun stopRenderLoop() {
        renderLoopRunning = false
        stopRenderThread()
    }

    // ------------------------------------------------------------ 预设模型

    /**
     * 切到 `item` 指定的预设模型；`null` = 切回内置立方体。
     *
     * 失败返回 false（原因由引擎写进 status_text，会出现在 HUD 里），
     * 并且**画面保持原状** —— 不会因为点了个坏文件就掉回立方体。
     */
    fun selectModel(item: ModelItem?): Boolean {
        val ok = if (item == null) {
            session.clearModel()
            true
        } else {
            session.loadModel(item.path, item.name)
        }
        requestRender()
        return ok
    }

    // ------------------------------------------------------------ 玩法 / 涂鸦

    /**
     * 当前玩法 id（"none" / "sketch" / ...）。由引擎侧真相驱动，不是 UI 自说自话：
     * 切换失败时这里保持旧值，UI 据此回滚高亮。
     */
    val playMode: String get() = session.playMode()

    /**
     * 切玩法。返回 true 表示引擎已接受，下一次 render() 就是新玩法。
     *
     * 切进/切出涂鸦要先停掉惯性：涂鸦模式下手指是"笔"，不能被当成拖拽旋转。
     */
    fun setPlayMode(mode: String): Boolean {
        stopMotion()
        val ok = session.setPlayMode(mode)
        if (ok) {
            sketchActivePointer = -1
            // 切过去先给一帧，别让用户看着上一玩法的残影去下笔。
            requestRender()
        }
        return ok
    }

    /** 手上正在画的那根手指（-1 = 没有）。多指时只认第一根，避免两笔打架。 */
    private var sketchActivePointer = -1

    /** 涂鸦是否处于激活态（决定触摸事件走笔还是走相机）。 */
    private fun sketchModeOn(): Boolean = session.playMode() == "sketch"

    /**
     * 涂鸦笔的触摸分发。返回 true 表示**本方法已经吃下这个事件**，调用方不要再走相机逻辑。
     *
     * 为什么笔和相机必须互斥：涂鸦模式下单指是落笔，绝不能同时旋转相机 ——
     * 否则画一笔画面就转一下，"画在哪挤出就在哪"这个前提直接没了。
     * 双指仍然留给相机（缩放/平移），这样用户还能把画布摆到顺手的位置。
     */
    private fun handleSketchTouch(ev: MotionEvent): Boolean {
        when (ev.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                sketchActivePointer = ev.getPointerId(0)
                session.sketchInput(ev.x, ev.y, SKETCH_BEGIN)
                requestRender()
                return true
            }

            MotionEvent.ACTION_MOVE -> {
                if (ev.pointerCount > 1) return false   // 双指 → 交回相机
                val idx = ev.findPointerIndex(sketchActivePointer)
                if (idx < 0) return true
                session.sketchInput(ev.getX(idx), ev.getY(idx), SKETCH_MOVE)
                requestRender()
                return true
            }

            MotionEvent.ACTION_UP -> {
                val idx = ev.findPointerIndex(sketchActivePointer)
                if (idx >= 0) session.sketchInput(ev.getX(idx), ev.getY(idx), SKETCH_END)
                sketchActivePointer = -1
                // 提交后网格变了，这一帧就是"笔画 → 3D 挤出体"的可见证据。
                requestRender()
                return true
            }

            MotionEvent.ACTION_CANCEL -> {
                session.sketchInput(0f, 0f, SKETCH_CANCEL)
                sketchActivePointer = -1
                requestRender()
                return true
            }

            MotionEvent.ACTION_POINTER_DOWN -> {
                // 第二根手指落下 → 这一笔作废，整条手势转交给相机做缩放/平移。
                if (sketchActivePointer >= 0) {
                    session.sketchInput(0f, 0f, SKETCH_CANCEL)
                    sketchActivePointer = -1
                    requestRender()
                }
                return false
            }
        }
        return false
    }

    /** 撤销一笔。返回 false = 没有可撤销的（画布已空）。 */
    fun sketchUndo(): Boolean {
        val ok = session.sketchUndo()
        requestRender()
        return ok
    }

    /** 清空画布。 */
    fun sketchClear(): Boolean {
        val ok = session.sketchClear()
        requestRender()
        return ok
    }

    /** 已提交的笔画数。 */
    fun sketchStrokeCount(): Int = session.sketchStrokeCount()

    /**
     * 整批折线提交：2D 画布画完之后一次交给引擎挤出。
     *
     * 这条路径与 [sketchInput] 的逐点流**互斥使用**：2D 画布模式下不要再送
     * sketchInput，否则同一趟操作会往文档里塞两份笔迹。
     *
     * 返回 true = 整批都进了文档。
     */
    fun sketchPushStrokes(flat: FloatArray): Boolean {
        val ok = session.sketchPushStrokes(flat)
        requestRender()
        return ok
    }

    fun dispose() {
        stopMotion()
        stopRenderLoop()
        session.destroy()
    }

    // ------------------------------------------------------------ MSAA

    /**
     * 切换 MSAA 档位。做法是 detach → 设值 → attach：
     * 采样数是交换链属性（决定 MS 图张数与规格），只能在重建交换链时改变。
     *
     * 返回**实际生效**的采样数 —— 设备不支持请求档位时后端会退回 1x，
     * 调用方必须拿这个返回值去更新 UI，否则按钮上会显示一个假的 ×8。
     */
    fun applyMsaa(samples: Int): Int {
        stopMotion()
        msaaSamples = if (samples <= 0) 1 else samples
        val holder = this.holder ?: return msaaEffective
        // 与 surfaceChanged 同一条理由：detach 前必须先把渲染线程停干净，
        // 否则它会在这段窗口里对着空 swapchain 出帧。
        stopRenderLoop()
        session.detachSurface()
        attach(holder)
        return msaaEffective
    }

    /** 设备支持的采样数档位（升序，至少含 1）。 */
    fun supportedMsaaLevels(): List<Int> = session.supportedMsaa()

    /** 实际生效的采样数。 */
    fun effectiveMsaaSamples(): Int = msaaEffective

    /** 当前请求的采样数。 */
    fun requestedMsaaSamples(): Int = msaaSamples

    /**
     * 设定上屏分辨率缩放比（0.25~1.0），走与 [applyMsaa] 完全相同的
     * detach → 设值 → attach 时序 —— 尺寸同样是交换链属性。
     *
     * 返回**实际生效**的缩放比（引擎会把越界值夹回区间）。
     */
    fun applyRenderScale(scale: Float): Float {
        stopMotion()
        renderScale = scale.coerceIn(MIN_RENDER_SCALE, 1f)
        val holder = this.holder ?: return renderScale
        stopRenderLoop()
        session.detachSurface()
        attach(holder)
        return renderScale
    }

    /**
     * 在 attach 之前预设缩放比（用于启动时恢复上次运行的设置）。
     *
     * 和 [applyRenderScale] 的区别：这里**不做** detach/attach。调用点必须在
     * 首次 surfaceCreated 之前（MainActivity.onCreate 里 setContentView 之前），
     * 此时 [holder] 还是 null，只要改字段，首帧 [attach] 就会带上正确的尺寸。
     * 如果已经 attach 过了，就退化成走正常重建流程。
     */
    fun setRenderScaleBeforeAttach(scale: Float) {
        val target = scale.coerceIn(MIN_RENDER_SCALE, 1f)
        if (holder == null) {
            renderScale = target
            Log.i(MbRenderer.TAG, "预设上屏分辨率 ${"%.0f".format(target * 100)}%（首次 attach 生效）")
        } else {
            applyRenderScale(target)
        }
    }

    /** 当前请求的缩放比。 */
    fun requestedRenderScale(): Float = renderScale

    /** 当前交换链的像素尺寸（已按缩放比取整），例如 "1944x1296"。 */
    fun renderSizeText(): String {
        val w = session.renderWidth()
        val h = session.renderHeight()
        return if (w > 0 && h > 0) "${w}x${h}" else "--"
    }

    private companion object {
        // fps 统计窗口：短于 1 秒数值抖得没法看，长于 1 秒对"拖动时掉帧"反应太慢。
        const val FPS_WINDOW_NS = 1_000_000_000L

        // 渲染线程每帧的目标预算(ms)：1 ≈ 1000Hz 上限，即"几乎不让路"。
        //
        // 为什么要"目标"而不是"能多快跑多快"：这条循环和 UI 线程是**两个**线程，
        // 但它们共享 CPU 与内存带宽。渲染线程狂飙会把 UI 线程挤到没时间片，
        // 又变回"UI 卡"。给一个上界，SDK 再快也不会把 UI 拖死。
        //
        // 【2026-11-11 8 → 1】原来 8ms 是照着"预算 1/120s"写的，但它算的是
        // **sleep 之前的剩余量**：sleepMs = 8 - elapsed，于是循环周期是
        // max(8, elapsed)。
        //   实测(MAILBOX + 3 帧在飞，2160x1440，MSAA 4x)：
        //     ms_avg = 15.4ms —— SDK 侧 session.render() 本身就占 15.4ms，
        //     早超过 8ms，所以那行 sleep 的 sleepMs 恒为负、**一次都没执行**。
        //   也就是说 64fps 完全不是 sleep 造成的。
        //
        // 【2026-11-12 更正】当时把原因归给"每帧整屏 CLEAR 吃掉 ~12ms"是**错的**。
        // 同设备 A/B 实测（三档 MSAA × 深度开关）证明瓶颈是 **MSAA 的填充率/带宽**，
        // 与 clear、与深度几乎无关：
        //     MSAA4 + 深度开 : 63.8fps / 15.55ms
        //     MSAA4 + 深度关 : 64.5fps / 15.4ms   ← 深度开关只差 1%
        //     MSAA1 + 深度开 : 384fps / 2.57ms    ← 关掉 MSAA 快 6 倍
        //     MSAA8 + 深度开 : 36fps  / 27.5ms    ← 2x 像素数 ⇒ 2x 时间，线性
        // 像素数翻倍时间就翻倍，是典型的带宽受限。这也解释了"只画一个立方体
        // 却只有 60fps"：代价与**几何量**无关，只与**像素量 × 采样数**有关。
        //
        // 对策不是让 CPU 多睡，而是把整屏 clear 在画面静止时省掉
        // （见 SceneRenderer::content_unchanged），并允许按比例降上屏分辨率。
        // 保留一个很小的让路量(1ms)只是防止 SDK 变得极快时把主线程饿死；
        // 当前实测下它同样不会触发（15.4 > 1）。
        const val FRAME_BUDGET_MS = 1L

        /** 上屏分辨率缩放的下限。再低画面糊到无法判断内容。 */
        const val MIN_RENDER_SCALE = 0.25f

        // 相机
        const val CAM_DIST = 2.6f
        const val FOV_Y_RAD = (60.0 * Math.PI / 180.0).toFloat()

        // 灵敏度：0.006 rad/px。它乘的是**手指速度**（px/s）而不是位移：
        // 拖 1000px/s 就是目标角速度 6 rad/s。
        //
        // 四元数姿态没有任何角度夹取：水平、俯仰都能 360° 无死角转。
        // 2π 写成 Float：Kotlin 的 PI 是 Double，运算结果会升级成 Double。
        const val TWO_PI_F = (2.0 * Math.PI).toFloat()

        // 0.006 -> 0.0082（+37%）。同样是拖 1000px/s，目标角速度从 6 rad/s
        // 变成 8.2 rad/s，跟手感和"转到位的速度"一起变快。
        const val ROT_PER_PX = 0.0082f

        // 手指速度的低通系数（1/s）。25 时约 40ms 才跟到目标，起步阶段
        // 明显发软 —— 这就是"拖泥带水"的第一段延迟。提到 40 后约 25ms 跟上，
        // 仍然足够弹掉 MOVE 逐帧的噪声（噪声是零均值的高频抖动，25ms 的
        // 窗口足够平滑）。
        const val FINGER_SMOOTHING = 40f

        // 两个 MOVE 的间隔超过这个值（ms）就不算速度了。
        const val MAX_SAMPLE_DT_MS = 100f

        // 单帧 dt 截断（秒）：掉帧到几百毫秒时，宁可少转一点
        // 也不要一次跳过半圈。
        const val MAX_STEP_S = 0.05f

        // 缩放
        const val MIN_SCALE = 0.35f
        const val MAX_SCALE = 4.0f

        // ---- 双指手势死区 ----
        //
        // 触摸屏读数有亚像素噪声。双指按上去之后，如果把这些噪声直接当位移
        // 用，画面会持续微动：平移在飘、缩放反复微抖、滚转慢慢歪。
        // 三个通道各给一个死区，阈值内丢弃**且不更新基准**（见 handleTwoFinger），
        // 这样真实位移会累积到超过阈值再兑现，既不丢精度也不抖。
        //
        // 缩放用**相对**死区（距离变化的百分比），因为缩放本质是比例运算，
        // 两指离得远时同样的绝对噪声影响更小 —— 用相对值才在各尺度下一致。
        // 0.4% 大致等于两指相距 500px 时的 2px 抖动。
        const val PINCH_DEADZONE = 0.004f
        // 平移是 1:1 跟手的，死区要小，否则画面"慢半拍"。
        const val PAN_DEADZONE = 0.5f
        // 滚转：两指夹角零点几度的抖动。0.2° 约等于 0.0035 rad。
        const val ROLL_DEADZONE = 0.0035f

        // 惯性 / 加速度
        // FLING_DAMPING 同时决定两件事：
        //   1. 角速度追赶手指速度的快慢 = 加速度感的强弱；
        //   2. 松手后减速的快慢。
        //
        // 2.2 -> 6.0。这是"拖泥带水"的第二段延迟，而且是最主要的一段：
        // 它决定实际角速度多久才追上目标角速度，2.2 时时间常数约 450ms，
        // 手指已经甩出去了画面还在慢慢加速。6.0 时约 165ms，起步立刻有反应，
        // 同时仍保留"追赶"这一段，加速度感不至于消失（不是把它设成无穷大
        // 直接硬跟，那样快甩和慢拖的手感会被抹平）。
        const val FLING_DAMPING = 6.0f
        // 低于这个速度就收尾（防止无限重绘）。
        const val MIN_FLING_SPEED = 0.02f
        // 平移的收尾门槛，单位是"世界单位/秒"。比旋转高两个数量级：
        // 平移的基数本来就小，沿用 0.02 会让双指轻按后的残留速度滑出可见的漂移。
        const val MIN_PAN_FLING_SPEED = 0.12f
        // 角速度上限（rad/s）：约 3.3 圈/秒，再快也没意义
        const val MAX_YAW_VEL = 21f

        // 启动时的 MSAA 请求档位。4x 是画质/性能的常用折中；
        // 设备不支持时引擎会退回 1x 并打日志，UI 显示实际生效值。
        const val DEFAULT_MSAA_SAMPLES = 4

        // 笔事件类型（与 primitives::StrokeEventKind 同值，见 painter.h）。
        // 在 Kotlin 侧再写一遍常量而不是透传 Int 字面量：JNI 边界上
        // 一个写错的魔数会变成"笔画永远不提交"，且没有任何编译期提示。
        const val SKETCH_BEGIN = 0
        const val SKETCH_MOVE = 1
        const val SKETCH_END = 2
        const val SKETCH_CANCEL = 3
    }
}
