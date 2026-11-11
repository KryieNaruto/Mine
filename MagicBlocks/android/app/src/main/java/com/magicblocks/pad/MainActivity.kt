package com.magicblocks.pad

import android.os.Bundle
import android.util.Log
import android.view.Surface
import android.view.ViewGroup
import android.widget.FrameLayout
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.ComposeView
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

/**
 * 宿主 Activity。
 *
 * 布局是**两层兄弟节点**：
 *   FrameLayout
 *     ├─ EngineSurfaceView   ← 引擎的 Vulkan 输出（下层，独立合成层）
 *     └─ ComposeView         ← UI 覆盖层（上层，Compose 自绘）
 *
 * 刻意**不**把 SurfaceView 放进 Compose 的 AndroidView 里：嵌套会导致
 * SurfaceView 的独立合成层和 Compose 的绘制顺序打架（黑闪、裁剪错位）。
 * 兄弟叠放后两边彻底解耦：引擎只管写自己的 buffer，Compose 只管画控件，
 * 且引擎那条路径上没有一次 CPU 回读。
 *
 * UI 覆盖层按 **UI 布局 v2** 排布：
 *   左 = 预设模型列表（点一条 → 引擎加载并绘制该模型）
 *   右 = 功能列表（复位 / MSAA 档位 / 复位视角…）
 *   中间留空 = 3D 画面 + 拖拽区（不铺任何满屏容器，否则会吞掉手势）
 */
class MainActivity : ComponentActivity() {

    private lateinit var surfaceView: EngineSurfaceView

    private var statusText by mutableStateOf("引擎启动中…")
    private var showHud by mutableStateOf(true)

    // 性能 HUD 数据。由 EngineSurfaceView 在渲染线程按 1 秒窗口算出，
    // 这里只在 UI 线程读快照。用 mutableStateOf 是为了值变化能触发重组 —— 
    // 若用普通字段，Compose 不会知道 fps 变了，HUD 会一直停在上一次的数字。
    private var perf by mutableStateOf(
        EngineSurfaceView.PerfSnapshot(0f, 0f, 0f, 0f, 1)
    )

    // MSAA 状态。msaaLevels 在引擎 attach 后才拿得到（设备真实支持的档位），
    // 所以初值只有 1x；msaaCurrent 存的是**实际生效**的采样数，不是请求值。
    private var msaaLevels by mutableStateOf(listOf(1))
    private var msaaCurrent by mutableStateOf(1)

    // ---------------------------------------------------------------- 预设模型列表
    //
    // 列表数据来自引擎扫描（经 ModelCatalog 把 assets 解到缓存目录）。
    // 三种状态分开存，UI 才能说清"是空的"还是"读不到目录"：
    //   modelScan == null && modelError == null → 还在扫
    //   modelScan 是 Ok（可能 0 条）           → 正常
    //   modelError 非空                         → 目录不可用，必须显示原因
    private var modelScan by mutableStateOf<ModelScan?>(null)
    private var modelError by mutableStateOf<String?>(null)
    // 选中的模型；null = 内置立方体（列表顶上单独一条）
    private var selectedModel by mutableStateOf<ModelItem?>(null)
    // 视图状态的版本号：引擎是命令式的，Compose 拿不到它的状态，用版本号驱动重绘
    private var viewRevision by mutableStateOf(0)

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        Log.i(MbRenderer.TAG, "MainActivity.onCreate")

        // 全屏沉浸 + edge-to-edge。
        //
        // 这里**必须**同时做三件事，否则底部会留一条 96px 的黑色导航栏：
        //   1. setDecorFitsSystemWindows(false)：让内容铺到系统栏底下；
        //   2. WindowCompat.setDecorFitsSystemWindows + enableEdgeToEdge 等价；
        //   3. 隐藏系统栏并允许它们被手势临时唤出（BEHAVIOR_SHOW_TRANSIENT_BARS）。
        // 早先用旧的 systemUiVisibility 位掩码，Android 14 上不生效，
        // 结果 SurfaceView 只拿到 2160x1344（屏幕 1440 − 导航栏 96），
        // 屏幕底部就空出黑边。现在 SurfaceView 拿满 2160x1440。
        WindowCompat.setDecorFitsSystemWindows(window, false)
        window.statusBarColor = android.graphics.Color.TRANSPARENT
        window.navigationBarColor = android.graphics.Color.TRANSPARENT
        WindowInsetsControllerCompat(window, window.decorView).apply {
            hide(WindowInsetsCompat.Type.systemBars())
            systemBarsBehavior =
                WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        }

        surfaceView = EngineSurfaceView(this).apply {
            layoutParams = FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT
            )
            onStatus = { s ->
                statusText = s
                // 引擎 attach 完成后才会有真实的设备档位，所以在这里同步一次。
                // 注意要 runOnUiThread：onStatus 是从渲染路径回调进来的。
                runOnUiThread {
                    val lv = try { supportedMsaaLevels() } catch (e: Throwable) { listOf(1) }
                    if (lv != msaaLevels) msaaLevels = lv
                    msaaCurrent = try { effectiveMsaaSamples() } catch (e: Throwable) { 1 }
                    // fps 每秒更新一次，这里 500ms 采一次快照就够了：
                    // 读取一个 data class 是原子的，不会读到半更新的状态。
                    perf = currentPerf()
                }
            }
            // 屏幕朝向（Display.getRotation()）：决定手势在逻辑坐标下的换算方式，
            // 详见 EngineSurfaceView.applyRotationDelta。
            setRotationListener { rotation ->
                Log.i(MbRenderer.TAG, "display rotation=$rotation (ROTATE_90=${rotation == 1})")
                viewRevision++
            }
        }

        val root = FrameLayout(this).apply {
            addView(surfaceView)   // 先加 = 在下层
        }

        val composeView = ComposeView(this).apply {
            setContent {
                // 版本号读一次，保证引擎内的视角/模型变化能触发一次重组
                viewRevision
                OverlayUi(
                    status = statusText,
                    perf = perf,
                    hudVisible = showHud,
                    onToggleHud = { showHud = !showHud },
                    onResetView = {
                        surfaceView.resetView()
                        viewRevision++
                    },
                    onZoomIn = {
                        surfaceView.zoomBy(1.25f)
                        viewRevision++
                    },
                    onZoomOut = {
                        surfaceView.zoomBy(0.8f)
                        viewRevision++
                    },
                    msaaLevels = msaaLevels,
                    msaaCurrent = msaaCurrent,
                    onSelectMsaa = { next ->
                        // 档位来自设备上报；切完一定要把**实际生效值**读回来（可能被静默降级）。
                        Log.i(MbRenderer.TAG, "切换 MSAA -> 请求 $next x（支持: $msaaLevels）")
                        msaaCurrent = surfaceView.applyMsaa(next)
                        Log.i(MbRenderer.TAG, "MSAA 实际生效: ${msaaCurrent}x")
                        viewRevision++
                    },
                    modelScan = modelScan,
                    modelError = modelError,
                    selectedModel = selectedModel,
                    onToggleHudVisible = {
                        showHud = !showHud
                        viewRevision++
                    },
                    onSelectModel = { item ->
                        if (surfaceView.selectModel(item)) {
                            selectedModel = item
                        } else {
                            // 加载失败：画面保持原状，状态栏里会出现引擎给的原因。
                            // 列表不切换到这一条 —— 不能让 UI 显示一个没画出来的模型。
                            statusText = surfaceView.viewState()
                        }
                        viewRevision++
                    },
                    onRefreshModels = { loadModelList() }
                )
            }
        }
        // 后加 = 在上层
        root.addView(
            composeView,
            FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT
            )
        )

        setContentView(root)

        // 资源解包 + 目录扫描放在首帧之后：它会读 assets 字节，别拖慢第一屏。
        root.post { loadModelList() }
    }

    override fun onResume() {
        super.onResume()
        syncDisplayRotation()
    }

    /** 把当前 Display 的朝向喂给渲染面（决定手势坐标换算）。 */
    private fun syncDisplayRotation() {
        // Display 与 Surface.ROTATION_* 同值（0/90/180/270 的常量），这里只用
        // Surface 的常量，避免为一行代码引入第二个同名常量来源。
        val rotation: Int = try {
            @Suppress("DEPRECATION")
            windowManager.defaultDisplay.rotation
        } catch (e: Throwable) {
            Log.w(MbRenderer.TAG, "读不到 display rotation: ${e.message}")
            Surface.ROTATION_0
        }
        surfaceView.setDisplayRotation(rotation)
    }

    /**
     * 扫一遍预设模型目录。
     *
     * 在**后台线程**解包+扫描：assets 里几个 OBJ 一共 ~1MB，拷贝 + 目录遍历
     * 在低端机上够到卡一帧了，放主线程会让第一屏黑一下。
     * 结果回到主线程写状态（Compose 的 mutableState 只能在主线程改）。
     */
    private fun loadModelList() {
        val app = applicationContext
        Thread({
            val r = ModelCatalog.scan(app)
            runOnUiThread {
                when (r) {
                    is ModelScan.Ok -> {
                        modelScan = r
                        modelError = null
                        Log.i(MbRenderer.TAG, "预设模型 ${r.items.size} 个: " + r.items.joinToString { it.name })
                    }
                    is ModelScan.Failed -> {
                        modelScan = null
                        modelError = r.reason
                        Log.e(MbRenderer.TAG, "预设模型目录不可用: ${r.reason}")
                    }
                }
            }
        }, "model-scan").start()
    }

    override fun onDestroy() {
        Log.i(MbRenderer.TAG, "MainActivity.onDestroy")
        surfaceView.dispose()
        super.onDestroy()
    }
}

// ---------------------------------------------------------------- 配色（与 HUD 一致）

private val kPanelBg = Color(0xCC0E1016)
private val kPanelBorder = Color(0x55263148)
private val kAccent = Color(0xFF7FD3FF)
private val kTextDim = Color(0xFF8899AA)
private val kTextBody = Color(0xFFBBBBCC)
private val kGreen = Color(0xFF7FE0A0)
private val kSelectedBg = Color(0xFF1F3A55)
private val kButtonBg = Color(0xE2454A5C)
private val kButtonActive = Color(0xE2246B8F)

/**
 * Compose 覆盖层（UI 布局 v2）：
 *
 *   [左] 预设模型列表            [中] 3D 画面（不挡手势）            [右] 功能列表
 *
 * 左右两列都用 `fillMaxHeight` 撑满高度、`width` 固定；中间**没有**任何满屏容器，
 * 保证 SurfaceView 能收到单指拖拽（Compose 只拦截自己命中的按钮/列表区域）。
 */
@Composable
private fun OverlayUi(
    status: String,
    perf: EngineSurfaceView.PerfSnapshot,
    hudVisible: Boolean,
    onToggleHud: () -> Unit,
    onResetView: () -> Unit,
    onZoomIn: () -> Unit,
    onZoomOut: () -> Unit,
    msaaLevels: List<Int>,
    msaaCurrent: Int,
    onSelectMsaa: (Int) -> Unit,
    modelScan: ModelScan?,
    modelError: String?,
    selectedModel: ModelItem?,
    onToggleHudVisible: () -> Unit,
    onSelectModel: (ModelItem?) -> Unit,
    onRefreshModels: () -> Unit
) {
    Row(
        modifier = Modifier.fillMaxSize().padding(14.dp),
        horizontalArrangement = Arrangement.SpaceBetween,
        verticalAlignment = Alignment.Top
    ) {
        // ------------------------------------------------ 左：预设模型列表
        ModelPanel(
            scan = modelScan,
            error = modelError,
            selected = selectedModel,
            onSelect = onSelectModel,
            onRefresh = onRefreshModels
        )

        Spacer(Modifier.width(16.dp))

        // ------------------------------------------------ 中：HUD（顶部，不挡拖拽）
        Column(
            modifier = Modifier.weight(1f).padding(top = 4.dp),
            horizontalAlignment = Alignment.CenterHorizontally
        ) {
            if (hudVisible) {
                Column(
                    modifier = Modifier
                        .background(Color(0x990E1016), RoundedCornerShape(12.dp))
                        .padding(horizontal = 14.dp, vertical = 8.dp)
                ) {
                    Text(
                        text = "MagicBlocks · " +
                            (selectedModel?.name ?: "内置立方体"),
                        color = kAccent,
                        fontSize = 15.sp,
                        fontWeight = FontWeight.Bold
                    )
                    Spacer(Modifier.size(3.dp))
                    Text(text = "左右滑 → 左右转 · 上下滑 → 上下转 · 双指缩放/平移", color = kTextBody, fontSize = 12.sp)
                    Spacer(Modifier.size(6.dp))
                    PerfIndicator(perf)
                    Spacer(Modifier.size(4.dp))
                    Text(text = status, color = kTextDim, fontSize = 10.sp, maxLines = 2, overflow = TextOverflow.Ellipsis)
                }
            }
        }

        Spacer(Modifier.width(16.dp))

        // ------------------------------------------------ 右：功能列表
        FunctionPanel(
            msaaLevels = msaaLevels,
            msaaCurrent = msaaCurrent,
            hudVisible = hudVisible,
            onZoomIn = onZoomIn,
            onZoomOut = onZoomOut,
            onResetView = onResetView,
            onToggleHudVisible = onToggleHudVisible,
            onSelectMsaa = onSelectMsaa,
            onToggleStatus = onToggleHud
        )
    }
}

/**
 * fps 性能指示条。
 *
 * 只做"显示"这一件事：数据全部来自 [EngineSurfaceView.currentPerf]，
 * 它是在引擎渲染成功后真实统计出来的（详见那边的注释），
 * 所以这里的数字**反映引擎出帧**，不是 Compose 自己的重绘次数。
 *
 * 颜色分档是给人看的门槛，不是精度：
 *   >=50 绿（满帧附近） / 30~49 黄（明显掉帧但还能用） / <30 红（卡）
 *   fps==0 灰：还没采到第一个窗口（刚启动），或引擎一帧都没成功。
 */
@Composable
private fun PerfIndicator(perf: EngineSurfaceView.PerfSnapshot) {
    val fps = perf.fps
    val color = when {
        fps <= 0f -> Color(0xFF666C7A)
        fps >= 50f -> Color(0xFF7FE0A0)
        fps >= 30f -> Color(0xFFE8C15A)
        else -> Color(0xFFE8665A)
    }
    Row(verticalAlignment = Alignment.Bottom) {
        Text(
            text = if (fps <= 0f) "--" else "%.0f".format(fps),
            color = color,
            fontSize = 22.sp,
            fontWeight = FontWeight.Bold
        )
        Spacer(Modifier.size(3.dp))
        Column {
            Text("fps", color = color, fontSize = 11.sp)
            // 平均/峰值耗时：平均告诉你稳态，峰值告诉你有没有偶发卡顿。
            if (fps > 0f) {
                Text(
                    text = "%.1f ms  峰值 %.0f ms".format(perf.frameMsAvg, perf.frameMsMax),
                    color = kTextDim,
                    fontSize = 9.sp
                )
            }
        }
        Spacer(Modifier.size(8.dp))
        Text("MSAA ${perf.msaa}x", color = kTextDim, fontSize = 9.sp)
        // 失败率只在真的出现失败时才显示，正常运行时不要占地方。
        if (perf.renderFailRate > 0f) {
            Spacer(Modifier.size(6.dp))
            Text(
                text = "失败 %.0f%%".format(perf.renderFailRate),
                color = Color(0xFFE8665A),
                fontSize = 9.sp
            )
        }
    }
}

/** 左列：预设模型列表。把"空列表 / 读不到目录 / 正在扫"三种情况分开画。 */
@Composable
private fun ModelPanel(
    scan: ModelScan?,
    error: String?,
    selected: ModelItem?,
    onSelect: (ModelItem?) -> Unit,
    onRefresh: () -> Unit
) {
    PanelColumn(title = "预设模型") {
        LazyColumn(
            modifier = Modifier.fillMaxHeight(0.86f).fillMaxSize(),
            contentPadding = PaddingValues(vertical = 4.dp)
        ) {
            item {
                ModelRow(
                    label = "内置立方体",
                    sub = "不加载文件",
                    enabled = true,
                    selected = selected == null,
                    accent = kAccent
                ) { onSelect(null) }
            }
            when {
                error != null -> item {
                    Column(Modifier.padding(horizontal = 8.dp, vertical = 6.dp)) {
                        Text("目录不可用", color = Color(0xFFFF9E9E), fontSize = 12.sp)
                        Text(error, color = kTextDim, fontSize = 10.sp)
                    }
                }
                scan == null -> item {
                    Text(
                        "扫描中…",
                        color = kTextDim,
                        fontSize = 12.sp,
                        modifier = Modifier.padding(horizontal = 8.dp, vertical = 6.dp)
                    )
                }
                (scan as ModelScan.Ok).items.isEmpty() -> item {
                    Text(
                        "目录里没有 OBJ",
                        color = kTextDim,
                        fontSize = 12.sp,
                        modifier = Modifier.padding(horizontal = 8.dp, vertical = 6.dp)
                    )
                }
                else -> items((scan as ModelScan.Ok).items) { m ->
                    ModelRow(
                        label = m.name,
                        sub = if (m.ok) "${m.bytes / 1024} KB" else m.message,
                        enabled = m.ok,
                        selected = selected?.path == m.path,
                        accent = if (m.ok) kGreen else Color(0xFFFF9E9E)
                    ) { if (m.ok) onSelect(m) }
                }
            }
        }
        Spacer(Modifier.height(6.dp))
        SmallButton("重新扫描", onClick = onRefresh, modifier = Modifier.fillMaxSize())
    }
}

/** 列表里的一行：名字 + 副标题（大小 / 错误原因）。 */
@Composable
private fun ModelRow(
    label: String,
    sub: String,
    enabled: Boolean,
    selected: Boolean,
    accent: Color,
    onClick: () -> Unit
) {
    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(vertical = 2.dp)
            .clip(RoundedCornerShape(8.dp))
            .background(if (selected) kSelectedBg else Color(0x221A1F2B))
            .border(
                width = 1.dp,
                color = if (selected) accent else Color(0x1AFFFFFF),
                shape = RoundedCornerShape(8.dp)
            )
            .clickable(enabled = enabled) { onClick() }
            .padding(horizontal = 10.dp, vertical = 7.dp)
            .alpha(if (enabled) 1f else 0.45f)
    ) {
        Text(
            text = (if (selected) "▶ " else "") + label,
            color = if (selected) accent else Color.White,
            fontSize = 14.sp,
            fontWeight = FontWeight.SemiBold,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis
        )
        Text(
            text = sub,
            color = kTextDim,
            fontSize = 10.sp,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis
        )
    }
}

/** 右列：功能列表（缩放 / 复位 / HUD / MSAA 档位）。 */
@Composable
private fun FunctionPanel(
    msaaLevels: List<Int>,
    msaaCurrent: Int,
    hudVisible: Boolean,
    onZoomIn: () -> Unit,
    onZoomOut: () -> Unit,
    onResetView: () -> Unit,
    onToggleHudVisible: () -> Unit,
    onSelectMsaa: (Int) -> Unit,
    onToggleStatus: () -> Unit
) {
    PanelColumn(title = "功能") {
        Column(modifier = Modifier.fillMaxHeight(0.86f)) {
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Button(
                    onClick = onZoomOut,
                    modifier = Modifier.weight(1f),
                    colors = ButtonDefaults.buttonColors(containerColor = kButtonBg)
                ) { Text("−", color = Color.White, fontSize = 18.sp) }
                Button(
                    onClick = onZoomIn,
                    modifier = Modifier.weight(1f),
                    colors = ButtonDefaults.buttonColors(containerColor = kButtonBg)
                ) { Text("+", color = Color.White, fontSize = 18.sp) }
            }
            Spacer(Modifier.height(8.dp))
            FunctionButton("复位视角", onClick = onResetView, active = false)
            Spacer(Modifier.height(6.dp))
            FunctionButton(
                if (hudVisible) "隐藏信息" else "显示信息",
                onClick = {
                    onToggleHudVisible()
                    onToggleStatus()
                },
                active = false
            )

            Spacer(Modifier.height(12.dp))
            Text("抗锯齿 MSAA", color = kAccent, fontSize = 12.sp, fontWeight = FontWeight.SemiBold)
            Spacer(Modifier.height(6.dp))
            msaaLevels.forEach { lv ->
                // 档位直接来自设备上报（vkGetPhysicalDeviceImageFormatProperties），
                // 不会出现"点了 8x 却悄悄退回 4x"的假按钮。
                FunctionButton(
                    label = if (lv <= 1) "关闭 MSAA" else "${lv}x（实际 ${if (msaaCurrent > 1) "${msaaCurrent}x" else "关"}）",
                    onClick = { onSelectMsaa(lv) },
                    active = lv == msaaCurrent
                )
                Spacer(Modifier.height(6.dp))
            }
        }
    }
}

@Composable
private fun PanelColumn(title: String, content: @Composable () -> Unit) {
    Column(
        modifier = Modifier
            .width(232.dp)
            .fillMaxHeight()
            .background(kPanelBg, RoundedCornerShape(14.dp))
            .border(1.dp, kPanelBorder, RoundedCornerShape(14.dp))
            .padding(12.dp)
    ) {
        Text(title, color = kAccent, fontSize = 15.sp, fontWeight = FontWeight.Bold)
        Spacer(Modifier.height(8.dp))
        content()
    }
}

@Composable
private fun FunctionButton(label: String, onClick: () -> Unit, active: Boolean) {
    Button(
        onClick = onClick,
        modifier = Modifier.fillMaxSize(),
        contentPadding = PaddingValues(vertical = 6.dp),
        colors = ButtonDefaults.buttonColors(
            containerColor = if (active) kButtonActive else kButtonBg
        )
    ) {
        Text(label, color = Color.White, fontSize = 13.sp, maxLines = 1)
    }
}

@Composable
private fun SmallButton(label: String, onClick: () -> Unit, modifier: Modifier = Modifier) {
    Button(
        onClick = onClick,
        modifier = modifier.height(34.dp),
        contentPadding = PaddingValues(vertical = 2.dp),
        colors = ButtonDefaults.buttonColors(containerColor = kButtonBg)
    ) {
        Text(label, color = Color.White, fontSize = 12.sp)
    }
}
