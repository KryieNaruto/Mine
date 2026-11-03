# L0 测试数据

## golden_l0.png

离屏自检画面的 golden 基线，由 `magicblocks-l0-cli render --width 320 --height 240
--frame-index 0` 在 **SwiftShader** 上产出。

- 格式：PNG，320×240，RGB8 **RGBA**（color type 6），无压缩（zlib store 模式，便于逐像素确定性）
- 内容：4×4 实例化色块网格，实例 0 = 纯红 `(255,0,0,255)`

### 谁在用它

`Offscreen.MatchesGoldenSingleBaseline` 做两层校验：

1. **像素比对**：与实时渲染结果逐像素比较，容差 ≤1%（每通道 ≤2）。
2. **结构化断言**（不依赖 golden 内容）：
   - 左上角（`w/8, h/8`）必须是纯红：`R > 200` 且 `B < 60`
   - golden **自身**左上角也必须带这个标记
   - 右上角（`7w/8, h/8`）必须**不是**红色

第 2 组是为了防止「错基线自我固化」：如果画面上下颠倒、或 RGBA/BGRA 写反，
只要 golden 是跟着错误输出生成的，第 1 步就会「通过」。结构化断言把朝向和通道序
钉死，与 golden 文件内容无关。

> 实测过的坑：读回端做翻行、着色器 `cy` 也做翻行，两者相互抵消，画面朝向看着对、
> 而两处逻辑都是反的。当时先翻 golden 也「全绿」了。现在这组断言会直接失败
> （用翻转后的 golden 做过负向测试，确实 `FAILED`）。
>
> **2026-11 更新（L1 落地时重生成）**：旧 golden 是在「着色器 `cy` 翻行 + 读回也翻行」
> 这对相互抵消的错误组合下生成的。画面朝向看着对，但 `vUv.y` 被反向，
> 导致实例 1..15 的 UV 渐变系数取反（色块顶部实测 `u≈0.11`，正确应为 `≈0.89`）。
> L1 把读回改成分页分配器页、并修正行序后，色块位置与旧 golden 完全一致，
> 但渐变方向对不上（12172 像素差、均匀 ~0.88 倍）。判定旧 golden 是错向基线，
> 按本文流程重生成，重生成后左上角纯红、右上角非红、结构断言全过。
> 同期补齐了此前缺失的 `golden_ui.png`（`magicblocks-ui-cli render` 生成）。

### 重新生成

**只有在渲染逻辑有意变更后才重新生成**，且生成后必须确认左上角是纯红：

```powershell
MagicBlocks\build\magicblocks-l0-cli.exe render --width 320 --height 240 `
    --frame-index 0 --output MagicBlocks\tests\data\golden_l0.png
```

验证（不合格就别提交）：

```powershell
$py = "C:\Users\Administrator\AppData\Local\Programs\Python\Python313\python.exe"
& $py -c "import struct,zlib; ..."   # 或直接跑 ctest，结构化断言会拦住错基线
ctest --test-dir MagicBlocks\build -R Offscreen
```

### 跨驱动

同一份 golden 同时作为 lavapipe（Linux）的基线：两个软件光栅的插值结果在
≤1% 容差内一致。若在 lavapipe 上超出容差，**不要**放宽容差或另存基线，
先确认是否真的只差舍入，并在报告里说明差异再决定。

## golden_ui.png

UI 覆盖层（HUD）的 golden 基线，同样 **SwiftShader** 产出，320×240 RGBA8。

生成命令：

```powershell
MagicBlocks\build\magicblocks-ui-cli.exe render --width 320 --height 240 `
    --output MagicBlocks\tests\data\golden_ui.png
```

被 `ui_Offscreen.MatchesGolden`（`tests/ui_test.cpp`）使用。与 `golden_l0.png` 不同，
它**不**带 4x4 色块结构断言——UI 的判据是通道序/朝向由 `ui_Offscreen.RendersDeterministicFrame`
的确定性 + 同像素复现兜底。
