package com.magicblocks.pad

import android.annotation.SuppressLint
import android.content.Context
import android.util.Log
import android.view.Choreographer
import android.view.MotionEvent
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
 * 交互：
 *   单指拖动  → 旋转（水平拖 yaw，垂直拖 pitch）
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
    }

    private fun renderOnce() {
        val ok = session.render(yaw, pitch, scale, panX, panY)
        val now = System.currentTimeMillis()
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

    override fun onTouchEvent(event: MotionEvent): Boolean {
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                stopFling()                       // 手指一落下就掐断惯性（"抓住"画面）
                lastX = event.x
                lastY = event.y
                lastPinchDist = 0f
                lastFocusX = event.x
                lastFocusY = event.y
                beginTrack(event)
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

    /** 单指：拖动 = 旋转。 */
    private fun handleOneFinger(event: MotionEvent) {
        val dx = event.x - lastX
        val dy = event.y - lastY
        // 0.006 rad/px：拖过整个屏幕宽度大约转半圈
        yaw += dx * ROT_PER_PX
        pitch += dy * ROT_PER_PX
        pitch = pitch.coerceIn(-PITCH_LIMIT, PITCH_LIMIT)
        lastX = event.x
        lastY = event.y
        // 单指阶段不产生平移，但焦点要跟着走，否则从双指切回来时会跳
        lastFocusX = event.x
        lastFocusY = event.y
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

    fun dispose() {
        stopFling()
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
        // 相机
        const val CAM_DIST = 2.6f
        const val FOV_Y_RAD = (60.0 * Math.PI / 180.0).toFloat()

        // 旋转：0.006 rad/px，拖过整个屏幕宽度大约转 130°
        const val ROT_PER_PX = 0.006f
        const val PITCH_LIMIT = 1.4835f   // ±85°

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
