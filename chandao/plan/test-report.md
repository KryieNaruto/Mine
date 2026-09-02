# 交付测试报告 — 中心植物生长 Vulkan — Round 1 / 5

- 测试时间：2026-08-20 10:33–10:35（UTC+8）
- 测试环境：Windows 11 (26200)，PowerShell + Python 3.14 / Pillow，Visual Studio 18 2026 x64
- 截图工作目录：`D:\qsw\禅道\_shottest\round12`（本轮新建空目录）
- 对应变更：`plan/execution-log.md` 最新段「中心植物生长 Vulkan」（Seed/PlantScene、CMake 条件 HAS_VULKAN、VulkanPlantRenderer、圆心切换、`-c plant` 失败码）
- Vulkan 环境：**未安装 Vulkan SDK**（configure 日志：`Vulkan SDK not found; building without HAS_VULKAN`）

## 结论：**PASS**

Release 配置/构建/安装退出码均为 0，`_install/main.exe` 存在且为本轮产物（126464 字节，2026-08-20 10:32:54）。TimeText 回归 `-t 1 / 1.5 / 3` 全部退出码 0、严格 400×400，蓝色像素 1.5s > 1s。无 SDK 下 `-c plant` 退出码 2，写出 `plant-vulkan-failed.txt`（含 `plant-vulkan-failed`），**未**生成 `shot_0.5.png`。代码审查与无参 5 秒启动均通过。植物分阶段绿/棕像素验收为**环境阻塞：无 Vulkan SDK**，不把回退图当 PASS，也不因此把整份报告打成 FAIL。

## 1. 构建与安装（Release）—— PASS

| 命令 | 期望退出码 | 实际退出码 | 结果 |
|---|---|---|---|
| `cmake -S . -B _build -G "Visual Studio 18 2026" -A x64 -DCMAKE_INSTALL_PREFIX="_install" -DCMAKE_PREFIX_PATH="D:\Qt\6.8.3\msvc2022_64"` | 0 | 0 | PASS |
| `cmake --build _build --config Release` | 0 | 0（`main.vcxproj -> _build\main\Release\main.exe`） | PASS |
| `cmake --install _build --config Release` | 0 | 0（安装 `main.exe` + windeployqt） | PASS |

CMake 日志关键行：

```
-- Could NOT find WrapVulkanHeaders (missing: Vulkan_INCLUDE_DIR)
-- Vulkan SDK not found; building without HAS_VULKAN
-- Configuring done (0.2s)
-- Generating done (0.5s)
```

- **Vulkan 状态：building without HAS_VULKAN**（不是 “Vulkan found”）。
- `CMakeCache.txt`：`Vulkan_INCLUDE_DIR-NOTFOUND`、`Vulkan_LIBRARY-NOTFOUND`、`Vulkan_GLSLANG_VALIDATOR_EXECUTABLE-NOTFOUND`。
- 产物：`_install/main.exe` 与 `_build\main\Release\main.exe` 均为 **126464** 字节，LastWriteTime **2026-08-20 10:32:54**，互为一致。相对上一轮安装产物（99840 字节 / 2026-08-19）已更新。
- windeployqt 两条无害警告（缺 translations catalogs.json、缺 dxcompiler.dll/dxil.dll），不影响运行。
- 无 SDK 仍能 configure + Release，满足「无 SDK 仍能编」。

## 2. TimeText 回归 —— PASS

命令在空目录 `_shottest\round12` 下以 `Start-Process -Wait -PassThru` 同步执行，取真实退出码。

| 命令 | 退出码 | 产物 | 尺寸 | 结果 |
|---|---|---|---|---|
| `_install\main.exe -t 1` | 0 | `shot_1.png`（17264 字节） | 严格 400×400（RGB） | PASS |
| `_install\main.exe -t 1.5` | 0 | `shot_1.5.png`（17969 字节） | 严格 400×400（RGB） | PASS |
| `_install\main.exe -t 3` | 0 | `shot_3.png`（18656 字节） | 严格 400×400（RGB） | PASS |

蓝色像素判定阈值（记入报告）：`B > R+20` 且 `B > G+20` 且 `B > 80`。

| 图 | 全图蓝像素 | 圆环区（距圆心 90–134px）蓝像素 |
|---|---|---|
| `shot_1.png` | 1969 | 1099 |
| `shot_1.5.png` | 2514 | 1644 |
| `shot_3.png` | 4168 | 3298 |

趋势：1.5s 蓝像素 **2514 > 1969**（1s），圆环区 **1644 > 1099**。方向正确，PASS。

备用阈值 `B > R+20` 且 `B > 80`：2175 → 2740 → 4438，趋势相同。

## 3. 无 SDK / 无 HAS_VULKAN 植物失败路径 —— PASS

本机 configure 为 without HAS_VULKAN，按计划走失败码路径，**不用绿像素趋势把植物视觉项判 PASS**。

| 检查项 | 期望 | 实际 | 结果 |
|---|---|---|---|
| configure + Release | 成功 | 第 1 节退出码 0 | PASS |
| `_install\main.exe -t 0.5 -c plant` 退出码 | ≠ 0 | **2** | PASS |
| 工作目录出现 `plant-vulkan-failed.txt` | 是，内容含 `plant-vulkan-failed` | 存在（45 字节），全文见下 | PASS |
| 不得生成 `shot_0.5.png` | 不存在 | **未生成**（目录仅有 TimeText 三张 PNG + 失败日志） | PASS |
| `main/CMakeLists.txt` 无 glslang / 未 FOUND 分支 | 存在 else | `if(Vulkan_FOUND)` 内 `if(GLSLANG_VALIDATOR)` 的 else（第 94–96 行）+ 外层 else（第 97–99 行） | PASS |

`plant-vulkan-failed.txt` 全文：

```
plant-vulkan-failed
HAS_VULKAN not enabled
```

## 4. 植物分阶段视觉（绿/棕像素）—— 环境阻塞：无 Vulkan SDK

计划禁止在无 HAS_VULKAN 时用回退图当 PASS。下列项**未执行**（本应在 Vulkan found / HAS_VULKAN 时跑）：

- `-t 0.5 -c plant`：盆土色、绿极少
- `-t 2 -c plant`：绿显著 > 0.5
- `-t 4 -c plant`：绿 > 2
- `-t 7.5 -c plant`：绿 >> 4；中心无清晰 HH:MM:SS
- `-t 9.5 -c plant`：深棕树干占优，绿 < 7.5；中心有时间字
- `-t 13 -c plant`：完全体 + 时间字

状态：**环境阻塞：无 Vulkan SDK**。失败码路径已测过（第 3 节）、TimeText 回归通过、构建安装通过，故不把整份报告打成 FAIL。

## 5. 代码审查（只读）—— PASS

对照验收清单读源码，不以执行记录声称代替。本轮 **未编入** `vulkan_plant_renderer.cpp`（无 `HAS_VULKAN`），渲染器项按源码静态审查。

| 审查项 | 实际 | 结果 |
|---|---|---|
| 命中内圆 accept，未命中 ignore | `mousePressEvent`：`centerHit` 置 `m_centerPressed`（不 ignore）；else `event->ignore()`。`mouseReleaseEvent`：`m_centerPressed` 时在内圆松开则切换 TimeText↔Plant（不 ignore）；无按下标志则 `ignore()`。按下内圆后 `mouseMoveEvent` 提前 return，不把拖拽抢回去 | PASS |
| frameless 排除 centerHit | `FramelessWindow::mousePressEvent` 将 `centerHit(pos)` 与播放/「...」并列排除，交给控件，不触发窗口拖拽 | PASS |
| Vulkan 走 OPTIMAL 附件 + staging copy，不是把 COLOR_ATTACHMENT 当 HOST_VISIBLE | `ensureOffscreen`：color image `VK_IMAGE_TILING_OPTIMAL` + `COLOR_ATTACHMENT \| TRANSFER_SRC`，显存 `DEVICE_LOCAL`；renderpass 后 barrier 到 `TRANSFER_SRC_OPTIMAL`，`vkCmdCopyImageToBuffer` 到 staging；staging / vertex buffer 才是 `HOST_VISIBLE`。源码存在但本轮未编入 | PASS（源码；未编入） |
| `-c plant` 成功路径不走 painter 回退 | `main.cpp`：`plantShot && !widget.lastPlantVulkanOk()` 时 `writePlantVulkanFailed(...)` 返回 2，**不** `image.save`。`drawCenterContent`：`m_plantVulkanRequired` 为真时 `!drew` 不调用 `drawPlantMesh` | PASS |
| CMake else / 无 glslang 分支 | `Vulkan_FOUND` 假 → `building without HAS_VULKAN`；FOUND 但无 `glslangValidator` → 另一条 without HAS_VULKAN。均不编 vulkan 源、不定义 `HAS_VULKAN` | PASS |

## 6. 无参启动 5 秒不崩溃 —— PASS

| 步骤 | 实际 |
|---|---|
| `Start-Process _install\main.exe` | PID 25632 启动 |
| 等待 5 秒 | 进程仍在：`HasExited=False`，`Responding=True` |
| `Stop-Process -Force` | 已停掉，未再残留 |

未点击 UI。

## 汇总

| # | 项 | 结果 |
|---|---|---|
| 1 | Release 构建 + 安装，`_install/main.exe` 最新 | PASS |
| 2 | TimeText `-t 1/1.5/3` 400×400、退出 0、蓝像素 1.5>1 | PASS |
| 3 | 无 SDK：能编；`-c plant` 退出 2 + 失败日志 + 无充数 PNG | PASS |
| 4 | 植物分阶段绿/棕视觉 | 环境阻塞：无 Vulkan SDK |
| 5 | 圆心 accept/ignore、frameless 排除、OPTIMAL+staging、失败不存 PNG | PASS |
| 6 | 无参启动 5 秒不崩溃 | PASS |

**总评：PASS**（植物视觉为环境阻塞，单独列出，不计入 FAIL）。

---

# 交付测试报告 — 中心植物生长 Vulkan — Round 2 / 5

- 测试时间：2026-08-20 10:44–10:47（UTC+8）
- 测试环境：Windows 11 (26200)，PowerShell + Python 3.14 / Pillow，Visual Studio 18 2026 x64
- 截图工作目录：`D:\qsw\禅道\_shottest\round13`（本轮新建空目录）
- 对应变更：`plan/execution-log.md`「中心植物生长 Vulkan」；本轮本机已装 LunarG Vulkan SDK 1.4.357.0（`C:\VulkanSDK\1.4.357.0`）
- 进程环境：`$env:VULKAN_SDK="C:\VulkanSDK\1.4.357.0"`，`Path` 前置 `C:\VulkanSDK\1.4.357.0\Bin`

## 结论：**PASS**

Release 配置/构建/安装退出码均为 0。configure 日志含 `Vulkan found` 与 `glslangValidator`；clean rebuild 编入 `vulkan_plant_renderer.cpp`。`_install/main.exe` 已更新为 **143360** 字节（round 1 为 126464），`dumpbin /DEPENDENTS` 可见 `vulkan-1.dll`。TimeText `-t 1/1.5/3` 全部退出码 0、严格 400×400，蓝像素 1.5s > 1s。植物 Vulkan 六张截图全部退出码 0、400×400、**未**出现 `plant-vulkan-failed.txt`。绿像素趋势 0 → 130 → 610 → 3812 → 0（0.5/2/4/7.5/9.5），方向全部正确；7.5s 中心无 HH:MM:SS，9.5s 中心有 `00:00:01`，13s 完全体 + 休息色时间字 `00:00:07`。无参启动 5 秒进程仍在。

## 1. 构建与安装（Release）—— PASS

| 命令 | 期望退出码 | 实际退出码 | 结果 |
|---|---|---|---|
| `cmake -S . -B _build -G "Visual Studio 18 2026" -A x64 -DCMAKE_INSTALL_PREFIX="_install" -DCMAKE_PREFIX_PATH="D:\Qt\6.8.3\msvc2022_64"` | 0 | 0 | PASS |
| `cmake --build _build --config Release --target main --clean-first` | 0 | 0 | PASS |
| `cmake --install _build --config Release` | 0 | 0（安装 `main.exe` + windeployqt） | PASS |

说明：第一次增量 `cmake --build _build --config Release` 也退出 0，但只重编了 `qrc_plant_shaders_Release.cpp`。为确认编入 Vulkan 源，本轮对 `main` 做了 `--clean-first` 全量重编后再安装；验收产物即这次 clean rebuild。

CMake 日志关键行：

```
-- Vulkan found: C:/VulkanSDK/1.4.357.0/Lib/vulkan-1.lib
-- glslangValidator: C:/VulkanSDK/1.4.357.0/Bin/glslangValidator.exe
-- Configuring done (0.2s)
-- Generating done (8.2s)
```

clean rebuild 编译日志含：

```
Compiling plant.frag to SPIR-V
Compiling plant.vert to SPIR-V
...
vulkan_plant_renderer.cpp
...
main.vcxproj -> D:\qsw\禅道\_build\main\Release\main.exe
```

- `CMakeCache.txt`：`Vulkan_LIBRARY=C:/VulkanSDK/1.4.357.0/Lib/vulkan-1.lib`，`GLSLANG_VALIDATOR=C:/VulkanSDK/1.4.357.0/Bin/glslangValidator.exe`
- 产物：`_install/main.exe` 与 `_build\main\Release\main.exe` 均为 **143360** 字节，LastWriteTime **2026-08-20 10:45:27**，互为一致。相对 round 1（126464 字节 / 10:32:54）已更新。
- `vulkan_plant_renderer.obj`：130395 字节，2026-08-20 10:45:26。
- windeployqt 仍有 translations catalogs.json 警告，不影响运行。

## 2. TimeText 回归 —— PASS

命令在空目录 `_shottest\round13` 下以 `Start-Process -Wait -PassThru` 同步执行。

| 命令 | 退出码 | 产物 | 尺寸 | 结果 |
|---|---|---|---|---|
| `_install\main.exe -t 1` | 0 | `shot_1.png`（17264 字节） | 严格 400×400（RGB） | PASS |
| `_install\main.exe -t 1.5` | 0 | `shot_1.5.png`（17969 字节） | 严格 400×400（RGB） | PASS |
| `_install\main.exe -t 3` | 0 | `shot_3.png`（18656 字节） | 严格 400×400（RGB） | PASS |

蓝色像素判定阈值（与 round 1 相同）：`B > R+20` 且 `B > G+20` 且 `B > 80`。

| 图 | 全图蓝像素 | 圆环区（距圆心 90–134px）蓝像素 | 中心时间字（目视） |
|---|---|---|---|
| `shot_1.png` | 1969 | 1099 | `00:00:09` |
| `shot_1.5.png` | 2514 | 1644 | `00:00:09` |
| `shot_3.png` | 4168 | 3298 | `00:00:07` |

趋势：1.5s 蓝像素 **2514 > 1969**（1s），圆环区 **1644 > 1099**。方向正确，PASS。数值与 round 1 一致，无回归。

## 3. 植物 Vulkan 截图 —— PASS

六条命令均在 `_shottest\round13` 执行。全部退出码 **0**，生成 PNG，**未**出现 `plant-vulkan-failed.txt`。

| 命令 | 退出码 | 产物 | 尺寸 | 结果 |
|---|---|---|---|---|
| `_install\main.exe -t 0.5 -c plant` | 0 | `shot_0.5.png`（13289） | 400×400 | PASS |
| `_install\main.exe -t 2 -c plant` | 0 | `shot_2.png`（15437） | 400×400 | PASS |
| `_install\main.exe -t 4 -c plant` | 0 | `shot_4.png`（17116） | 400×400 | PASS |
| `_install\main.exe -t 7.5 -c plant` | 0 | `shot_7.5.png`（20809） | 400×400 | PASS |
| `_install\main.exe -t 9.5 -c plant` | 0 | `shot_9.5.png`（24029） | 400×400 | PASS |
| `_install\main.exe -t 13 -c plant` | 0 | `shot_13.png`（22592） | 400×400 | PASS |

绿像素阈值（写入报告，不可改方向）：**`G > R+20` 且 `G > B+20` 且 `G > 80`**。

中心文字框：`x∈[110,290), y∈[132,188)`（圆环圆心约 `(200,160)`）。

阶段 6 工作态时间字会淡入，实测主体色为 **(175,175,175)**，不是满不透明 `#E8E8E8`(232,232,232)。因此「近白文字」采用浅灰族：`min(R,G,B) ≥ 160` 且 `max−min ≤ 30`（覆盖淡入后的 `#E8E8E8` 家族）。严格 `#E8E8E8±20` 会把淡入字漏掉，不能用来判趋势。休息态时间字为 `#55B2E8`，用蓝像素计。

| 图 | 绿像素 | 深棕像素 | 中心浅灰文字 | 中心蓝文字 | 目视 |
|---|---|---|---|---|---|
| `shot_0.5.png` | **0** | 120 | 0 | 0 | 盆土，无芽 |
| `shot_2.png` | **130** | 120 | 0 | 0 | 单芽 |
| `shot_4.png` | **610** | 120 | 0 | 0 | 芽 + 两叶 |
| `shot_7.5.png` | **3812** | 474 | **0** | 0 | 枝叶茂盛；中心无 HH:MM:SS |
| `shot_9.5.png` | **0** | **2010** | **1041**（(175,175,175)×665） | 108 | 深棕树干占优；中心 `00:00:01` |
| `shot_13.png` | **0** | 1872 | 0 | **1400** | 完全体树干 + 休息蓝字 `00:00:07` |

趋势判定：

| 验收项 | 实测 | 结果 |
|---|---|---|
| 0.5s 盆土色，绿极少 | 绿=0，可见陶盆/土 | PASS |
| 2s 绿显著 > 0.5 | 130 > 0 | PASS |
| 4s 绿 > 2 | 610 > 130 | PASS |
| 7.5s 绿 >> 4 | 3812 ≥ 610×1.5（约 6.2 倍） | PASS |
| 7.5s 中心无清晰 HH:MM:SS | 中心浅灰=0；目视无时间字 | PASS |
| 9.5s 深棕树干占优，绿 < 7.5 | 绿 0 < 3812；棕 2010 | PASS |
| 9.5s 中心有时间字，近白/浅灰明显多于 7.5 | 中心浅灰 1041 > 0；目视 `00:00:01` | PASS |
| 13s 完全体 + 时间字 | 树干 + 中心蓝字 1400；目视 `00:00:07` | PASS |

绿像素序列：`[0, 130, 610, 3812, 0, 0]`，方向全部符合计划，无反转。

## 4. 无参启动 5 秒不崩溃 —— PASS

| 步骤 | 实际 |
|---|---|
| `Start-Process _install\main.exe` | PID 45140 启动 |
| 等待 5 秒 | 进程仍在：`HasExited=False`，`Responding=True` |
| `Stop-Process -Force` | 已停掉 |

未点击 UI。

## 5. 代码/链接审查（本轮 exe 已链 Vulkan）—— PASS

| 审查项 | 实际 | 结果 |
|---|---|---|
| 体积大于 round 1 的 126464 | **143360** | PASS |
| dumpbin 依赖 | 含 **`vulkan-1.dll`**（另有 Qt6Widgets/Gui/Core 等） | PASS |
| 编入 `vulkan_plant_renderer.cpp` | clean rebuild 日志出现该 cpp；obj 130395 字节 | PASS |
| SPIR-V | 日志 `Compiling plant.vert/frag to SPIR-V` | PASS |

## 汇总

| # | 项 | 结果 |
|---|---|---|
| 1 | Release configure（Vulkan found + glslangValidator）+ 构建 + 安装，exe 已更新 | PASS |
| 2 | TimeText `-t 1/1.5/3` 400×400、退出 0、蓝像素 1.5>1 | PASS |
| 3 | 植物 Vulkan 六张截图退出 0、无失败日志、绿/时间字趋势正确 | PASS |
| 4 | 无参启动 5 秒不崩溃 | PASS |
| 5 | exe 链 `vulkan-1.dll`，体积 143360 > 126464 | PASS |

**总评：PASS**
