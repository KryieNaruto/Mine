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
import kotlin.math.exp
import kotlin.math.hypot
import kotlin.math.max
import kotlin.math.min
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
 * 交互（方向按用户看到的物理屏幕算，见 [applyRotationDelta]）：
 *   单指左右拖 → 左右转（yaw，绕世界 Y 轴）
 *   单指上下拖 → 上下转（pitch，绕世界 X 轴）
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

    // ---------------------------------------------------------------- 视角状态
    private var yaw = 0f
    private var pitch = 0f
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

    /** 引擎侧一次采样的性能快照，供 Compose 状态栏展示。 */
    data class PerfSnapshot(
        val fps: Float,
        val frameMsAvg: Float,
        val frameMsMax: Float,
        val renderFailRate: Float,
        val msaa: Int,
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
        Log.i(MbRenderer.TAG, "屏幕朝向 rotation=$rotation（旋转轴映射已随之切换）")
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

    // ================================================================ 惯性
    //
    // 设计要点：
    //  1. 用 Choreographer 跟随屏幕刷新率逐帧推进（不是固定 sleep），
    //     这样 60/90/120Hz 设备手感一致。
    //  2. 速度来自**松手前最后一段位移 / 时间**，所以"滑得快就转得快"。
    //  3. 速度按指数衰减 v *= exp(-k*dt)，k 越大停得越快。指数衰减的好处是
    //     数学上永远不会真正到 0，所以再配一个"速度阈值"来收尾，避免无限重绘。
    //  4. 用 frameTimeNanos 做真实 dt，掉帧时不会突然转一大截。

    private val choreographer = Choreographer.getInstance()
    private var flingRunning = false
    private var lastFrameNanos = 0L

    // 角速度（弧度/秒）
    private var yawVel = 0f
    private var pitchVel = 0f
    // 平移速度（世界单位/秒）
    private var panXVel = 0f
    private var panYVel = 0f

    // 松手前的采样，用来算速度
    private var trackStartTime = 0L
    private var tracking = false
    // 滑动窗口锚点（见 updateWindow / startFlingFromTrack）
    private var winStartMs = 0L
    private var winYaw0 = 0f
    private var winPitch0 = 0f
    private var winPanX0 = 0f
    private var winPanY0 = 0f

    private val flingFrame = Choreographer.FrameCallback { frameTimeNanos -> stepFling(frameTimeNanos) }

    init {
        holder.addCallback(this)
        // 让引擎的 swapchain image 直接浮在窗口最上层时不会被 Compose 遮住
        holder.setFormat(android.graphics.PixelFormat.OPAQUE)
        setZOrderOnTop(false)   // 保持在下面，Compose 浮在上面
        isFocusable = true
    }

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
        // 尺寸变了要重建交换链
        session.detachSurface()
        attach(holder)
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        Log.i(MbRenderer.TAG, "surfaceDestroyed")
        stopFling()
        stopRenderLoop()
        session.detachSurface()
    }

    private fun attach(holder: SurfaceHolder) {
        val w = if (surfaceWidth > 0) surfaceWidth else width
        val h = if (surfaceHeight > 0) surfaceHeight else height
        if (w <= 0 || h <= 0) return
        // 采样数必须在 attachSurface 之前送进引擎：交换链要按它决定建几张 MS 图。
        session.setMsaa(msaaSamples)
        session.attachSurface(holder.surface, w, h)
        // 记下**实际生效**的采样数（设备不支持时后端会退回 1x），UI 显示的是这个值。
        msaaEffective = session.msaaSamples()
        Log.i(MbRenderer.TAG, "MSAA 请求 ${msaaSamples}x → 实际 ${msaaEffective}x（支持: ${supportedMsaaLevels()}）")
        // 建好交换链后立刻画一帧，避免第一屏是黑的
        renderOnce()
        // 再起常驻循环：切模型/转视角都要有帧；surfaceDestroyed 会把它停掉。
        startRenderLoop()
    }

    private fun renderOnce() {
        // 计时包住 session.render()：这段时间 = 录制 + 提交 + 等 GPU 完成 + present，
        // 也就是"这一帧让渲染线程占用了多久"。它不等于 GPU 执行时间（那是另一回事），
        // 但正是它决定渲染线程能不能跑满目标帧率。
        val t0 = System.nanoTime()
        val ok = session.render(yaw, pitch, scale, panX, panY)
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

    override fun dispatchTouchEvent(ev: MotionEvent): Boolean {
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
        stopFling()                       // 手指一落下就掐断惯性（"抓住"画面）
        lastX = event.x
        lastY = event.y
        lastPinchDist = 0f
        lastFocusX = event.x
        lastFocusY = event.y
        beginTrack(event)
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
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
                    beginTrack(event)
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
                beginTrack(event)
            }

            MotionEvent.ACTION_MOVE -> {
                if (event.pointerCount >= 2) {
                    handleTwoFinger(event)
                } else {
                    handleOneFinger(event)
                }
                updateWindow(event)
                renderOnce()
            }

            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                lastPinchDist = 0f
                // 松手：按最近这段的瞬时速度启动惯性
                if (event.actionMasked == MotionEvent.ACTION_UP) {
                    startFlingFromTrack(event)
                }
                tracking = false
            }
        }
        return true
    }

    /**
     * 单指：拖动 = **环绕摄像机检视模型**（不是转物体本身）。
     *
     * 映射约定（**屏幕**方向 → **视觉**方向，与 MainActivity 的横屏锁定配套）：
     *   屏幕左右滑 → 相机绕世界 Y 轴绕行（yaw）
     *   屏幕上下滑 → 相机绕世界 X 轴俯仰（pitch）
     *
     * 为什么方向按"物理屏幕"算而不是直接拿 MotionEvent 的 x/y：
     * 这台 pad 是横屏应用但面板物理竖置（SurfaceFlinger 会做 ROTATE_90）。
     * SurfaceView 挂在 ROTATE_90 的窗口下时，MotionEvent 的 x/y 是**逻辑横屏**
     * 坐标系，而用户看到的画面是**物理竖屏**排布 —— 手指在物理屏上左右滑，
     * 事件里给出的却是一截 dY。不做换算，就会出现"左右拖像上下转"。
     * 换算表由 [displayRotation] 给出（Activity 用 [setRotationListener] 同步）。
     *
     * 这里**没有**任何阈值/死区：按下即锚定，之后每一像素位移都立刻换算成角度。
     * （曾经靠 ACTION_MOVE 是否到达来"意外"产生一段死区，已在 distributeEvent
     *  里修掉，见那里的说明。）
     */
    private fun handleOneFinger(event: MotionEvent) {
        val dx = event.x - lastX
        val dy = event.y - lastY
        applyRotationDelta(dx, dy)
        lastX = event.x
        lastY = event.y
        // 单指阶段不产生平移，但焦点要跟着走，否则从双指切回来时会跳
        lastFocusX = event.x
        lastFocusY = event.y
    }

    /**
     * 把一截**逻辑坐标**下的位移换算成视觉上的相机环绕量。
     *
     * displayRotation 是 Display.getRotation()：
     *   0/180（逻辑横屏，与物理排布一致）→ 直接用
     *   90/270（图片被转了 90°，事件仍按逻辑横屏上报）→ 交换 x/y
     *
     * 符号：两个方向都写成"元素往哪边推，画面就往哪边动"（跟手）。
     *   vx > 0（往右推）→ yaw -= ：相机往左绕，物体看起来跟着手指往右转
     *   vy > 0（往下推）→ pitch +=：相机抬高，从上方俯视，顶面露出、物体下移
     * 物体本身在世界里不动，动的永远是摄像机（见 scene_renderer.cpp record_into）。
     */
    private fun applyRotationDelta(dx: Float, dy: Float) {
        val swapped = displayRotation == Surface.ROTATION_90 || displayRotation == Surface.ROTATION_270
        val vx = if (swapped) dy else dx   // 视觉水平位移
        val vy = if (swapped) dx else dy   // 视觉垂直位移
        // ROT_PER_PX 是弧度/像素：拖过整个屏幕高度大约转 130°
        yaw -= vx * ROT_PER_PX
        pitch += vy * ROT_PER_PX
        pitch = pitch.coerceIn(-PITCH_LIMIT, PITCH_LIMIT)
    }

    /** 双指：距离变化 = 缩放，焦点移动 = 平移。 */
    private fun handleTwoFinger(event: MotionEvent) {
        // --- 缩放 ---
        val d = pinchDistance(event)
        if (lastPinchDist > 0f && d > 0f) {
            scale = (scale * (d / lastPinchDist)).coerceIn(MIN_SCALE, MAX_SCALE)
        }
        lastPinchDist = d

        // --- 平移 ---
        //
        // 把屏幕像素位移换算成世界位移：屏幕上"整个高度"对应的世界跨度
        // 正好是 2 * dist * tan(fov/2)，所以
        //     世界每像素 = 2*dist*tan(fov/2) / surfaceHeight
        // dist 随 scale 变化，于是放大后同样的手指位移走的世界距离更小 ——
        // 效果是"画面贴着手指走"，缩放级别不影响跟手程度。
        val fx = focusX(event)
        val fy = focusY(event)
        val dxPx = fx - lastFocusX
        val dyPx = fy - lastFocusY
        lastFocusX = fx
        lastFocusY = fy

        val worldPerPx = worldUnitsPerPixel()
        // 屏幕 y 向下，世界 y 向上 → 取反
        panX -= dxPx * worldPerPx
        panY += dyPx * worldPerPx
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

    // ================================================================ 速度追踪

    private fun beginTrack(event: MotionEvent) {
        trackStartTime = event.eventTime
        tracking = true
        // 重置滑动窗口：每次手势阶段切换（单指↔双指）都从当前位置重新锚定，
        // 避免把上一阶段的速度算进这一阶段。
        winStartMs = event.eventTime
        winYaw0 = yaw
        winPitch0 = pitch
        winPanX0 = panX
        winPanY0 = panY
    }

    /**
     * 用**滑动窗口**估算松手瞬间的速度。
     *
     * 为什么不直接用"按下 → 松手"的平均速度：用户可能按住不动 2 秒再快速
     * 一甩，平均值会被前面的静止摊薄成接近 0，惯性就没了。
     *
     * 也不能只用最后一次 MOVE 的瞬时速度：手指抬起前常有几十毫秒没有事件
     * （或最后一段位移是 0），那样算出来会突然变 0。
     *
     * 所以取最近 [FLING_WINDOW_MS] 内的角度/平移变化来算 —— 既反映"最后甩得多快"，
     * 又不会被单帧抖动带偏。窗口尚未积累够时，退化成"从按下到现在的平均值"。
     */
    private fun startFlingFromTrack(event: MotionEvent) {
        if (!tracking) return
        val now = event.eventTime
        val spanMs = (now - trackStartTime).toFloat()
        if (spanMs < MIN_TRACK_MS) return      // 太短，速度不可信

        // 窗口内实际经过的时间：窗口够长用窗口，否则用全程
        val windowMs = min(spanMs, FLING_WINDOW_MS)
        val dt = windowMs / 1000f

        yawVel = (yaw - winYaw0) / dt
        pitchVel = (pitch - winPitch0) / dt
        panXVel = (panX - winPanX0) / dt
        panYVel = (panY - winPanY0) / dt

        if (abs(yawVel) + abs(pitchVel) + abs(panXVel) + abs(panYVel) < MIN_FLING_SPEED) {
            return  // 基本没动，不用惯性（省电，也避免极限值抖动）
        }
        // 速度夹上限：防止极快的一甩把画面"甩飞"
        yawVel = yawVel.coerceIn(-MAX_YAW_VEL, MAX_YAW_VEL)
        pitchVel = pitchVel.coerceIn(-MAX_YAW_VEL, MAX_YAW_VEL)
        startFling()
    }

    /**
     * 维护滑动窗口的起点：每次 MOVE 后调用。
     *
     * 做法很朴素 —— 窗口只由最近一次"锚点"决定：锚点比窗口更旧时就推进到
     * 当前值并重置锚点时间。因为 MOVE 事件本身很密（60~120Hz），
     * 这种"粗粒度滚动"的误差只有一帧，对惯性初速度足够了。
     */
    private fun updateWindow(event: MotionEvent) {
        val now = event.eventTime
        if (winStartMs == 0L) {
            winStartMs = now
            winYaw0 = yaw
            winPitch0 = pitch
            winPanX0 = panX
            winPanY0 = panY
            return
        }
        if (now - winStartMs > FLING_WINDOW_MS.toLong()) {
            winStartMs = now
            winYaw0 = yaw
            winPitch0 = pitch
            winPanX0 = panX
            winPanY0 = panY
        }
    }

    // ================================================================ 惯性推进

    private fun startFling() {
        if (flingRunning) return
        flingRunning = true
        lastFrameNanos = 0L
        choreographer.postFrameCallback(flingFrame)
    }

    private fun stopFling() {
        if (!flingRunning) return
        flingRunning = false
        choreographer.removeFrameCallback(flingFrame)
        yawVel = 0f
        pitchVel = 0f
        panXVel = 0f
        panYVel = 0f
    }

    private fun stepFling(frameTimeNanos: Long) {
        if (!flingRunning) return

        // 首帧没有上一帧时间戳：记下来，下一帧再算 dt（避免出现一个巨大的 dt）
        if (lastFrameNanos == 0L) {
            lastFrameNanos = frameTimeNanos
            choreographer.postFrameCallback(flingFrame)
            return
        }
        val dt = (frameTimeNanos - lastFrameNanos) / 1_000_000_000f
        lastFrameNanos = frameTimeNanos
        if (dt <= 0f) {
            choreographer.postFrameCallback(flingFrame)
            return
        }

        // 指数衰减：v *= exp(-k*dt)。
        // 用 exp 而不是 (1-k*dt)：后者在掉帧(dt 大)时会变成负数 → 反向抖动。
        val decay = exp(-FLING_DAMPING * dt)

        yaw += yawVel * dt
        pitch += pitchVel * dt
        pitch = pitch.coerceIn(-PITCH_LIMIT, PITCH_LIMIT)
        panX -= panXVel * dt
        panY += panYVel * dt

        yawVel *= decay
        pitchVel *= decay
        panXVel *= decay
        panYVel *= decay

        // 俯仰撞到上下限时，把速度也归零，否则会"贴着墙蹭"
        if (abs(pitch) >= PITCH_LIMIT - 1e-4f) pitchVel = 0f

        renderOnce()

        val speed = abs(yawVel) + abs(pitchVel) + abs(panXVel) + abs(panYVel)
        if (speed > MIN_FLING_SPEED) {
            choreographer.postFrameCallback(flingFrame)
        } else {
            stopFling()
        }
    }

    // ================================================================ 对外接口

    /** 缩放若干倍（给 UI 的 +/- 按钮用；双指手势走的是同一份 scale 状态）。 */
    fun zoomBy(factor: Float) {
        stopFling()
        scale = (scale * factor).coerceIn(MIN_SCALE, MAX_SCALE)
        renderOnce()
    }

    /** 当前视觉状态，供 HUD 显示。 */
    fun viewState(): String =
        "yaw=${"%.2f".format(yaw)} pitch=${"%.2f".format(pitch)} " +
            "zoom=${"%.2f".format(scale)} pan=(${"%.2f".format(panX)},${"%.2f".format(panY)})"

    /** 复位视角：回正 + 缩放回 1.0 + 平移归零，并立刻重画一帧。 */
    fun resetView() {
        stopFling()
        yaw = 0f
        pitch = 0f
        scale = 1f
        panX = 0f
        panY = 0f
        renderOnce()
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

    // 类型必须显式写出：`val x = FrameCallback { ... postFrameCallback(x) }`
    // 会让类型推断撞上递归（Kotlin: TYPECHECKER_HAS_RUN_INTO_RECURSIVE_PROBLEM）。
    private val renderLoop: Choreographer.FrameCallback = object : Choreographer.FrameCallback {
        override fun doFrame(frameTimeNanos: Long) {
            if (!renderLoopRunning) return
            renderOnce()
            choreographer.postFrameCallback(this)
        }
    }

    /** 启动常驻渲染循环（幂等）。 */
    fun startRenderLoop() {
        if (renderLoopRunning) return
        if (!holder.surface.isValid) return
        renderLoopRunning = true
        choreographer.postFrameCallback(renderLoop)
    }

    /** 停止常驻渲染循环（幂等）。 */
    fun stopRenderLoop() {
        if (!renderLoopRunning) return
        renderLoopRunning = false
        choreographer.removeFrameCallback(renderLoop)
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
        renderOnce()
        return ok
    }

    fun dispose() {
        stopFling()
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
        stopFling()
        msaaSamples = if (samples <= 0) 1 else samples
        val holder = this.holder ?: return msaaEffective
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

    private companion object {
        // fps 统计窗口：短于 1 秒数值抖得没法看，长于 1 秒对"拖动时掉帧"反应太慢。
        const val FPS_WINDOW_NS = 1_000_000_000L

        // 相机
        const val CAM_DIST = 2.6f
        const val FOV_Y_RAD = (60.0 * Math.PI / 180.0).toFloat()

        // 相机环绕灵敏度：0.006 rad/px ≈ 0.34°/px。
        // 拖过 1440 逻辑像素（= 屏幕上 2160 物理像素的高度）约转 130°，
        // 拖过 900 像素约转 80° —— 一个模型转一圈在 2~3 次拖动的量级。
        const val ROT_PER_PX = 0.006f
        const val PITCH_LIMIT = 1.4835f   // ±85°：再往上 up 向量会退化

        // 缩放
        const val MIN_SCALE = 0.35f
        const val MAX_SCALE = 4.0f

        // 惯性
        // FLING_DAMPING 越大停得越快：2.2 大约 1 秒衰减到 10%，手感偏"顺滑带减速"。
        const val FLING_DAMPING = 2.2f
        // 低于这个速度就收尾（防止无限重绘）。
        const val MIN_FLING_SPEED = 0.02f
        // 采样窗口太短时速度不可信（比如 10ms 抖一下），不做惯性
        const val MIN_TRACK_MS = 20f
        // 速度估算的滑动窗口：只看最近 100ms 的位移，反映"最后甩得多快"
        const val FLING_WINDOW_MS = 100f
        // 角速度上限（rad/s）：约 3.3 圈/秒，再快也没意义
        const val MAX_YAW_VEL = 21f

        // 启动时的 MSAA 请求档位。4x 是画质/性能的常用折中；
        // 设备不支持时引擎会退回 1x 并打日志，UI 显示实际生效值。
        const val DEFAULT_MSAA_SAMPLES = 4
    }
}
