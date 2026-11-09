package com.magicblocks.pad

import android.os.Bundle
import android.util.Log
import android.view.ViewGroup
import android.widget.FrameLayout
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.getValue
import androidx.compose.runtime.setValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.ComposeView
import androidx.compose.ui.text.font.FontWeight
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
 */
class MainActivity : ComponentActivity() {

    private lateinit var surfaceView: EngineSurfaceView

    private var statusText by mutableStateOf("引擎启动中…")
    private var showHud by mutableStateOf(true)

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
            onStatus = { s -> statusText = s }
        }

        val root = FrameLayout(this).apply {
            addView(surfaceView)   // 先加 = 在下层
        }

        val composeView = ComposeView(this).apply {
            setContent {
                OverlayUi(
                    status = statusText,
                    hudVisible = showHud,
                    onToggleHud = { showHud = !showHud },
                    onReset = { surfaceView.resetView() },
                    onZoomIn = { surfaceView.zoomBy(1.25f) },
                    onZoomOut = { surfaceView.zoomBy(0.8f) }
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
    }

    override fun onDestroy() {
        Log.i(MbRenderer.TAG, "MainActivity.onDestroy")
        surfaceView.dispose()
        super.onDestroy()
    }
}

/** Compose 覆盖层：只在上面浮一小块 HUD + 两个按钮，不挡住拖拽区域。 */
@Composable
private fun OverlayUi(
    status: String,
    hudVisible: Boolean,
    onToggleHud: () -> Unit,
    onReset: () -> Unit,
    onZoomIn: () -> Unit,
    onZoomOut: () -> Unit
) {
    Row(
        modifier = Modifier.fillMaxSize().padding(20.dp),
        verticalAlignment = Alignment.Top,
        horizontalArrangement = Arrangement.SpaceBetween
    ) {
        // 左上 HUD
        if (hudVisible) {
            Column(
                modifier = Modifier
                    .background(Color(0xCC0E1016), RoundedCornerShape(12.dp))
                    .padding(horizontal = 18.dp, vertical = 12.dp)
            ) {
                Text(
                    text = "MagicBlocks",
                    color = Color(0xFF7FD3FF),
                    fontSize = 20.sp,
                    fontWeight = FontWeight.Bold
                )
                Spacer(Modifier.size(6.dp))
                Text(text = "拖拽旋转 · 双指缩放", color = Color(0xFFBBBBCC), fontSize = 13.sp)
                Spacer(Modifier.size(4.dp))
                Text(text = status, color = Color(0xFF8899AA), fontSize = 11.sp)
            }
        }

        // 右上按钮组
        Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            Button(
                onClick = onZoomOut,
                colors = ButtonDefaults.buttonColors(containerColor = Color(0xE2454A5C))
            ) {
                Text("−", color = Color.White, fontSize = 18.sp)
            }
            Button(
                onClick = onZoomIn,
                colors = ButtonDefaults.buttonColors(containerColor = Color(0xE2454A5C))
            ) {
                Text("+", color = Color.White, fontSize = 18.sp)
            }
            Button(
                onClick = onReset,
                colors = ButtonDefaults.buttonColors(containerColor = Color(0xE6246B8F))
            ) {
                Text("复位", color = Color.White, fontSize = 14.sp)
            }
            Button(
                onClick = onToggleHud,
                colors = ButtonDefaults.buttonColors(containerColor = Color(0xE2454A5C))
            ) {
                Text(if (hudVisible) "隐藏" else "显示", color = Color.White, fontSize = 14.sp)
            }
        }
    }
}
