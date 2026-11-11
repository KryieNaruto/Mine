# UI ⇄ SDK 拆分隔离性验证报告

> 由 `MagicBlocks/tools/uisplit/verify_split.py` 自动生成。
> 每条断言都附**实际输出**；FAIL 就是 FAIL，不做美化。

- worktree: `E:\workspace\myspace\Mine\.agents\worktrees\20261111-ui-sdk-split\MagicBlocks`
- build dir: `E:\workspace\myspace\Mine\.agents\worktrees\20261111-ui-sdk-split\MagicBlocks\build-arch-t10`

## 总览

| PASS | FAIL | BLOCKED |
|---:|---:|---:|
| 25 | 0 | 0 |

**结论：全部通过。**

## A. 依赖扫描（静态）

| 断言 | 结果 | 实际输出 |
|---|---|---|
| `契约头存在` | **PASS** | mb_capi.h (28670 字节) |
| `头里无 std::` | **PASS** | 非注释代码里 std:: 出现 0 处 |
| `头里无 C++ 标准头` | **PASS** | 只 include 了 ['stddef.h', 'stdint.h'] |
| `头里无内部子目录引用` | **PASS** | device\|resources\|primitives\|scene\|backend 引用 0 处 |
| `头里无 mb_internal/` | **PASS** | mb_internal/ 引用 0 处 |
| `extern "C" 护栏` | **PASS** | 存在 extern "C" { 且由 __cplusplus 守卫 |
| `Kotlin 无内部引用` | **PASS** | 扫描 5 个 .kt 文件,违规 0 处 |
| `scene_renderer.h 无内部 include` | **PASS** | include ['cstdint', 'memory', 'string', 'vector'] —— SDK 内部头 0 个(PIMPL 只露标准库类型) |
| `公开目录只有 1 个头` | **PASS** | include/magicblocks/ = ['mb_capi.h'] |
| `契约头带 UTF-8 BOM` | **PASS** | 前 3 字节 EF BB BF(非 ASCII 字节 12199 个,全靠 BOM 标识编码) |

## B. 物理隔离（编译门控）

| 断言 | 结果 | 实际输出 |
|---|---|---|
| `正向:只给导出面能编过` | **PASS** | cl /I<导出面> 退出 0 |
| `负向:primitives/world.h` | **PASS** | 如预期编失败: 找不到头 |
| `负向:scene/scene.h` | **PASS** | 如预期编失败: 找不到头 |
| `负向:device/render_device.h` | **PASS** | 如预期编失败: 找不到头 |
| `负向:ui/ui_app.h` | **PASS** | 如预期编失败: 找不到头 |
| `负向:primitives/mesh.h` | **PASS** | 如预期编失败: 找不到头 |
| `负向:device/swapchain.h` | **PASS** | 如预期编失败: 找不到头 |
| `负向合计` | **PASS** | 6 个真实内部头在受限 include 下全部编失败 |
| `反向自证:探针指向真实内部头时找得到` | **PASS** | 同一探针在真实 include 树(-I include/)下不再报'找不到头' → 负向的红确实来自导出面收窄,不是文件名笔误 |
| `负向探针指向真实存在的内部头` | **PASS** | 6 个探针全部在 include/mb_internal/magicblocks/ 下真实存在 |
| `契约头不传 /utf-8 也能编(/W4 /WX)` | **PASS** | 4 个方言(C11/C17/C++17/C++20)全部退出 0:头靠 BOM 自证编码,调用方无需额外编译选项 |
| `include/ 整棵挪走后仍能编` | **PASS** | SDK 源码树此刻对编译器完全不可见,正向探针退出 0 |

## C. 契约验证

| 断言 | 结果 | 实际输出 |
|---|---|---|
| `probe_consumer 运行` | **PASS** | 退出码 0(编译→链接→运行 全通) |
| `运行时版本 == 头声明` | **PASS** | runtime=10000 == header=10000 |
| `桌面单元测试` | **PASS** | total=586 全部通过 |

## 说明：环境相关的 BLOCKED 项

- 运行层（真实 GPU 生命周期）在无 Vulkan 设备的机器上记 BLOCKED，既不冒绿也不恒红。证据链的编译层、链接层已实证。
- `Offscreen.FrameIndexChangesOutput` 有一条硬编码路径 `MB_GOLDEN_DIR + "/../../build/cli_out"`；`MB_GOLDEN_DIR` 是**源目录**的 `tests/data`，故它恒指向 `<MagicBlocks>/build/cli_out`，与 `--build-dir` 无关。该目录不存在时这条必红，属预知问题（low），与本次拆分无关。
- `StackGates.G3LayerPurityExitZero` 在 t9 完成前恒红（backend:: 违规 16 处），同样与本次拆分无关。

<!-- MANUAL:BEGIN -->

## D. 门控本身可红（反向控制实验）

"全绿"只有在能证明"它本来可以变红"时才有意义。以下实验**故意制造违规**，
确认门控确实抓得住。这不是门控的日常输出，是一次性的举证，手工执行。

### D1. A 段：往契约头里注入违规，逐条必须转红

对 `mb_capi.h` 的副本做单点篡改，每次只改一处：

| 注入内容 | 期望 | 实测 |
|---|---|---|
| `#include <vector>` | 转红 | **FAIL** `头里无 C++ 标准头 出现 C++ 头: ['vector']` |
| 真实代码里加 `std::memset(...)` | 转红 | **FAIL** `头里无 std:: 出现 1 处,如 [(95, 'static void z(void){std::memset...')]` |
| `#include "magicblocks/scene/scene.h"` | 转红 | **FAIL** `头里无内部子目录引用 出现: ['scene']` |
| 删掉 `extern "C" {` | 转红 | **FAIL** `extern "C" 护栏 缺 extern "C" —— C++ 侧符号会被 mangle,t2 的事故会重演` |
| 公开目录里多放一个头 | 转红 | **FAIL** `公开目录只有 1 个头 include/magicblocks/ = ['h.h']` |

注：把 `std::` 写进**注释**里**不会**转红（`strip_comments` 先剥注释）——
这是刻意的：注释里提到 `std::` 不构成耦合。

### D2. B 段：把内部头泄漏进导出面，负向门控必须"编过"

这是 B 段最关键的一步。同一份探针、同一条命令行，只改 `-I` 指向：

```
REAL export surface (isolated)   compiled_ok=False  header_not_found=True
LEAKED export surface (broken)   compiled_ok=True   header_not_found=False
```

`LEAKED` 是把 `include/mb_internal/magicblocks/device/result.h` 复制进导出面。
此时 `#include "magicblocks/device/result.h"` **编过了** —— 门控会判红。
说明：B 段报绿确实是因为"头不在导出面上"，不是因为探针写坏了或命令跑歪了。

### D3. 自测 31 条全绿

`MagicBlocks/tools/tests/test_check_ui_isolation.py --dll build-uisplit/magicblocks.lib`
→ `Ran 31 tests ... OK`（exit 0）。其中 N2/N6 本身就是"注入违规必须红"的用例。

## E. 实际执行的命令与退出码

| 命令 | 退出码 | 关键输出 |
|---|---:|---|
| `python MagicBlocks/tools/uisplit/verify_split.py` | 0 | `结果: PASS=25 FAIL=0 BLOCKED=0` |
| `python MagicBlocks/tools/check_ui_isolation.py --mode all --layer compile ...` | 0 | `结果: gates=10 failed=0 blocked=0` |
| `python MagicBlocks/tools/tests/test_check_ui_isolation.py --dll ...` | 0 | `Ran 31 tests ... OK` |
| `magicblocks-capi-probe.exe` | 0 | `[probe] PASS: 只靠 mb_capi.h 走完 ABI 握手 + 建会话 + 渲染 + 销毁` |
| `magicblocks_test.exe` | 0 | `[==========] 586 tests ... [ PASSED ] 586`（`assets/res` 在位；缺它则为 584/2，口径见上节） |
| `powershell -File tools\build.ps1 -Target test -BuildDir build-uisplit` | — | **文件不存在**（见下） |
| `powershell -File MagicBlocks\tools\build.ps1 -Target all -BuildDir build-uisplit` | 0 | `ninja: no work to do.` |

### 关于任务书里的 `tools\build.ps1`

任务书写的 `tools\build.ps1` 在仓库根**不存在**，实际路径是 `MagicBlocks\tools\build.ps1`：

```
The argument 'tools\build.ps1' to the -File parameter does not exist.
```

用正确路径执行 `-Target all` 得 `ninja: no work to do.`，退出码 0。
本文的 586/0 数据直接来自 `magicblocks_test.exe`（同一构建产物）。

## F. 验证过程中发现并修掉的问题

这几条是**验证脚本自身**的缺陷，不是 SDK 的问题，但都会让门控给出错误结论，
所以一并记录：

1. **门控数内部头时算成了 0（假绿倾向）。** t5 把 62 个内部头搬进
   `include/mb_internal/`，而 `INTERNAL_HEADER_PREFIXES` 里没有这个前缀。
   旧代码按前缀筛，于是报"内部 0 个、被挡在外面 0 个"——把 t5 的**搬运结果**
   误读成"根本没有内部头"。已改为按补集计算，并显式断言
   `公开 + 内部 == 总数`。现在报 `总 63 = 公开 1 + 内部 62`。

2. **`run()` 按 utf-8 读 cl.exe 输出，中文乱码导致判定失效。** cl.exe 用系统
   代码页 936 输出，utf-8 解码得到乱码，于是"这条 C1083 是找不到头还是找不到
   源文件"的判断全部落空，表现为"负向探针明明红了却判成证据不纯"——把一道
   好门控判死。已改为按 utf-8/gbk 双解码取最优。

3. **`compile_once` 用相对路径，`.cpp` 本身找不到。** 它以 `cwd=d` 运行，
   相对路径的源文件按 `d` 解析，得到 `C1083 无法打开源文件 probe.cpp`。
   这种 C1083 指向 `.cpp` 而不是头文件，会被误读成"内部头找不到"。
   已全部改绝对路径，并在判定时区分 `cannot open include file` 与
   `cannot open source file`。

4. **A 段原先把标准库头当成违规。** 第一版把 `scene_renderer.h` 里的
   `<cstdint>/<memory>/<string>/<vector>` 判红。PIMPL 露标准库类型是正当的，
   判据应当是"有没有 SDK 内部头"。已修正。
