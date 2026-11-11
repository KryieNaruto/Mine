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

## golden_l2.png —— L2 场景层基线

与 `golden_l0.png` / `golden_ui.png` 同尺寸（320x240 RGBA8），但是**另一套配色**：

| 项 | L0（golden_l0.png） | L2（golden_l2.png） |
| --- | --- | --- |
| 左上角实例 0 | 纯红 | **纯绿** |
| 清屏色 | 纯黑 (0,0,0) | **深蓝 (13,20,51)** |

两套基线**不能互相冒充**：`L2Golden.MatchesL2Baseline` 除了逐像素比对，还额外断言
"左上角必须是纯绿 / 右上角必须不是纯绿 / 背景必须偏蓝而不是纯黑"。
把 `golden_l2.png` 换成 L0 那份，第一条断言立刻失败。

**`frame_index` 必须是偶数**，绿色标定点才落在左上角。

重生成：

```powershell
MagicBlocks\build\magicblocks-l2-cli.exe record --scene demo --width 320 --height 240 \
    --frame-index 0 --record s.mbl2 --output MagicBlocks\tests\data\golden_l2.png
```

重生成后必须跑一遍 `magicblocks_test.exe --gtest_filter=L2Golden.*` 确认通过，
并核对左上角确实是纯绿、背景确实偏蓝。

## golden_fsk_*.png —— L4 魔法涂鸦（F-SK）基线

审计 `docs/superpowers/specs/2026-11-11-magicblocks-fsk-fvx-completion-audit.md` 的 GAP-07
指出：F-SK 的 golden **从来没有生成过**。这里补三份，均由 `magicblocks-fsk-cli render` 在
**SwiftShader** 上产出，320x240 RGBA8：

| 文件 | 生成参数 | 它钉住什么 |
| --- | --- | --- |
| `golden_fsk_persp.png` | `render --width 320 --height 240 --strokes 3 --camera perspective` | A1/A2 基线 |
| `golden_fsk_ortho.png` | 同上，`--camera ortho` | A3 相机双模态 |
| `golden_fsk_region.png` | `--width 320 --height 240 --strokes 2 --camera ortho --region` | R-SK-12 双表示 |

被 `SketchGolden.RenderedFrameMatchesBaselineAndPassesStructuralChecks` 等三条用例使用
（`tests/magicblocks_l4_sketch_test.cpp`），判据分三层：

1. **结构断言（不依赖 golden 内容）**：四角为背景色 `(15,18,26)`、
   前景像素占比 ∈ [3%, 60%]、颜色种数 > 2（证明有光照/深度）。
   这三条能挡住「整张上下颠倒 / 通道写反 / 空图」的错基线 ——
   而逐像素比对照样会通过那样的图（见上面 L0 的事故记录）。
2. **与 golden 逐像素比对**，容差 0。
3. **三份 golden 必须两两不同**：persp vs ortho 验 A3（实测差异 7561 像素），
   ortho vs region 验 R-SK-12（实测差异 14430 像素）。

### 重新生成

**只有渲染逻辑有意变更后才重新生成**，且必须走脚本（脚本里同时写着生成参数与结构断言）：

```powershell
$py = "C:\Users\Administrator\AppData\Local\Programs\Python\Python313\python.exe"
& $py MagicBlocks\scripts\fsk_golden.py all --cli MagicBlocks\build\magicblocks-fsk-cli.exe
```

`gen` 重新出图；`check` 对 golden **自身**做结构断言 + 两两差异断言（退出码 0 才算合格）。
`check` 可以脱离用例单独跑，这是防「错基线自我固化」的那一步。

## golden_l4_stack_{basic,no_overlay,inner_off,by_layer}.png —— F-VX 魔法叠叠乐基线

审计 `2026-11-11-magicblocks-fsk-fvx-completion-audit.md` 的 **GAP-07**（F-VX golden 从未落地）。
四张都是 320×200 RGBA8，**SwiftShader** 产出，清屏色 `#0f121a`。

| 文件 | 生成参数 | 背景占比 | 颜色种数 | SHA256（前 16） |
| --- | --- | --- | --- | --- |
| `golden_l4_stack_basic.png` | `--layers 8 --strokes 3`（网格指示缺省 on） | 90.35% | 52 | `AAC7E34DB20BD39A` |
| `golden_l4_stack_no_overlay.png` | 同上 + `--grid-overlay off` | 93.45% | 6 | `AF4D23888D3C4BF9` |
| `golden_l4_stack_inner_off.png` | 同上 + `--inner-pass off` | 90.35% | 39 | `E65DA0BBB656470B` |
| `golden_l4_stack_by_layer.png` | 同上 + `--color-mode by-layer` | 90.35% | 89 | `8424C5EC33A5987C` |

产出于 2026-11-11（fvx，独立构建目录 `build-fvx`）。关键差异：
`basic` vs `no_overlay` 相差 **3470 像素** ——「带网格指示」在像素上的可观测证据；
`basic` vs `inner_off` 相差 **2129 像素**（ON 颜色种数 5 > OFF 的 3）—— 内面 pass 确实参与绘制。

### 记录前先做结构验证（captain 2026-11-11 规则）

**只有结构性断言全对才允许把图写成 golden**；结构不对说明渲染是错的，带数字上报、不录基线。
脚本：`MagicBlocks/build-fvx/verify_structure.py`（本地验证工具，不入库）。它完全不读 golden，
只对"本次跑出来的图"断言：四角为 `#0f121a`、包围盒非满幅/非退化且大致居中、
前景占比 ∈[3%,60%]、按层改色出现 ≥8 档明暗、内面 ON 比 OFF 颜色更多且两图相差 >500 像素、
同参数两次逐字节相同。实测全部 PASS 后才记录本组 golden。

**`inner_off` 曾经被刻意排除**：captain 在共享构建目录观测到 `InnerPass*` 出现 diff=0
（内面 pass 根本没画），而 `build-fvx` 里 `StackRender*` 9/9 全绿 —— 观测不一致时不录基线。
该不一致已于同日解除（ON 5 色 > OFF 3 色、两图差 2129 像素），故现在纳入，并由
`StackGolden.InnerPassOffMatchesGoldenWithStructure` 额外断言"`inner_off` 必须不等于 `basic`"，
专门抓"内面 pass 静默失效"这类回归。

被 `StackGolden.*`（`tests/magicblocks_l4_stack_golden_test.cpp`）使用，判据四层：
① 四角必须是背景；② 画面中心 17×17 有非背景像素；③ 前景占比在区间内、颜色种数下限；
④ 与 golden 逐像素比（容差 4/255、差异像素 < 1%）。
另外 `GridOverlayChangesPixels` 独立于 golden：同参数只差 `--grid-overlay on/off`，两图必须差 >200 像素。

重生成（**只在渲染逻辑有意变更时**；生成后必须重跑 `verify_structure.py` 并核对上表数字）：

```powershell
$g = "MagicBlocks\tests\data"
MagicBlocks\build-fvx\magicblocks-l3-cli.exe stack --width 320 --height 200 --layers 8 --strokes 3 `
    --output "$g\golden_l4_stack_basic.png"
MagicBlocks\build-fvx\magicblocks-l3-cli.exe stack --width 320 --height 200 --layers 8 --strokes 3 `
    --grid-overlay off --output "$g\golden_l4_stack_no_overlay.png"
MagicBlocks\build-fvx\magicblocks-l3-cli.exe stack --width 320 --height 200 --layers 8 --strokes 3 `
    --inner-pass off --output "$g\golden_l4_stack_inner_off.png"
MagicBlocks\build-fvx\magicblocks-l3-cli.exe stack --width 320 --height 200 --layers 8 --strokes 3 `
    --color-mode by-layer --output "$g\golden_l4_stack_by_layer.png"
```

核对后跑 `magicblocks_test.exe --gtest_filter=StackGolden.*`。
