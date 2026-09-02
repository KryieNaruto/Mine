# 交付计划

## 需求原文

需求新增，之前中心样式需要你预留扩展，现在新增一种动画样式。目前构思如下：
1. 中间的数字样式点击后切换成动画样式，可以通过点击来回切换样式
2. 关于动画
	1）这是一个升级系统，是一个植物的生长过程，现在先只做 ’种子-发芽’ 阶段
	2）核心是需要能够选择种子，不同的种子，时间阶段数量不同。例如种子有5个时间阶段，则时间每过1/6，就会有一个新的状态。
	3）先做一个最基础的小树种子：有2个阶段——‘种子-发芽’，5个时间阶段
		· 时间刚开始，就是一个花盆，或者泥土，或者其他载物。
		· 阶段1——0-1/6时间：种子状态，即什么都看不见
		· 阶段2——1/6-2/6时间：抽芽状态，可以看到一个小芽
		· 阶段3——2/6-3/6时间：抽叶状态，小芽上多出两片叶子。
		· 阶段4——3/6-4/6时间：小树状态，开始基于时间变长生长，同时左右两侧基于时间产生叶子,同时树木部分变粗颜色变深，为下一阶段样式过度做准备。此时是持续动态
		· 阶段5——4/6-5/6时间：中树阶段，突然枝繁叶茂，变成一个树，然后长满树叶，此时高度缓慢生长
		· 阶段6——5/6-6/6时间：大树阶段，类似PVZ中的花园里的树，只能看见树身体。此时数字时间开始淡淡的显示，时间剩余越少，颜色越深越清晰。
	4）上述系统UI使用扁平风格化实现，使用Vulkan渲染。这是本次任务的重点工作

## 工作区

- 根目录：`D:\qsw\禅道`
- 源码目录：`main/src/`
- 构建目录：`_build/`
- 安装目录：`_install/`（产物 `_install/main.exe`）

## 范围

- 做：
  1. 点击表盘内圆：`TimeText` ↔ `Plant` 来回切换。
  2. 种子可选择的数据模型 + UI；本期只实现 `small_tree`（小树种子）。
  3. 小树：`timeStageCount = 5` → 6 个视觉态，工作进度每过 `1/6` 进入下一态，与用户阶段 1–6 一一对应。
  4. 「升级系统」本期只覆盖产品范围「种子-发芽」：一次工作计时内从盆土长到大树躯干。不做经验、关卡、第二种种子美术。
  5. 真正 Vulkan 提交画扁平植物，且 `-t -c plant` 能拍到。
  6. 圆环、按钮、设置、托盘、关闭动画、默认 `-t` TimeText 零回归。
- 不做：
  - 第二种种子美术、3D/贴图/粒子、改时长算法、git commit、由 Agent 安装 SDK。

## 技术栈与约束

- 语言 / 框架 / 构建系统：C++20 + Qt6 Widgets + CMake（Visual Studio 18 2026，x64）。
- Qt 路径：`-DCMAKE_PREFIX_PATH="D:\Qt\6.8.3\msvc2022_64"`。
- 构建：`cmake --build _build --config Release`；安装：`cmake --install _build --config Release`。
- Vulkan：`find_package(Vulkan QUIET)`；有 SDK 则 `HAS_VULKAN` + 离屏 Vulkan 渲染；无 SDK 仍须 configure/Release 成功。
- 已拍板的技术决策：
  - 生长只跟工作进度；休息强制 `p=1`（阶段 6 完全体）。
  - 选种子入口在「...」菜单；本期仅「小树种子」。
  - 阶段 6 时间文字用 QPainter 叠在 Vulkan 图上。
  - 不用 QRhiWidget、不用 qsb；离屏 Vulkan + staging 回读 + QImage blit。
  - 截图 `-c plant` 必须走 Vulkan，失败退出码 ≠ 0，并写旁路日志 `plant-vulkan-failed.txt`。

## 任务拆分

1. 任务：Seed 注册表 + PlantScene 六段几何 | 目录：`main/src/plant/` | 完成定义：色板与 Y 轴约定落地，phaseIndex 0..5 对应用户阶段 1..6。
2. 任务：CMake 条件化 HAS_VULKAN | 目录：`main/CMakeLists.txt`、`main/shaders/` | 完成定义：SPIR-V 输出到 `_build`，rcc 依赖 custom command；未 FOUND 不编 Vulkan 源。
3. 任务：VulkanPlantRenderer | 目录：`main/src/plant/vulkan_plant_renderer.*` | 完成定义：OPTIMAL 附件 + barrier + staging 回读 + 单次 init。
4. 任务：圆心点击切换、DPR blit、阶段6 时间字、休息 p=1 | 目录：`focus_timer_widget.*`、`frameless_window.*` | 完成定义：命中内圆 accept，未命中 ignore；frameless 排除中心热区。
5. 任务：选择种子菜单 + QSettings + CLI `-c plant` | 目录：`focus_timer_widget.*`、`main.cpp` | 完成定义：先 close 再回调；截图 Vulkan 失败非 0。

## 验收标准

- 默认：Release 编译成功，且产物出现在约定 install 目录（路径：`_install/main.exe`）
- 用户覆盖：
  - `-t 1` / `1.5` / `3`：400×400、退出码 0，TimeText 尽量零回归。
  - `-t 0.5 -c plant`：盆土色，绿极少。
  - `-t 2 -c plant`：绿显著大于 0.5。
  - `-t 4 -c plant`：绿大于 2。
  - `-t 7.5 -c plant`：绿远大于 4；中心无清晰 HH:MM:SS。
  - `-t 9.5 -c plant`：深棕树干占优，绿小于 7.5；中心有时间字。
  - `-t 13 -c plant`：完全体 + 时间字。
  - Vulkan 失败：退出码 ≠ 0，且有 `plant-vulkan-failed` 日志文件；不得充数 PNG。
  - 无 SDK：configure + Release 仍成功。
  - 无参启动 5 秒不崩溃。

## 风险与未知

- 风险：未装 SDK 或 GPU 无 Vulkan 运行时则植物截图验收阻塞；WIN32 无控制台，以退出码 + 小日志为准。
- 仍未知：无（用户已确认计划）。

## 派工策略

- 执行 Agent：delivery-implementer（回退 generalPurpose）
- 测试 Agent：delivery-tester（回退 shell）
- 排查 Agent：delivery-triage（回退 explore + generalPurpose）
- 最大测试-修复轮次：5

## 对齐状态

- [x] 用户已确认，可以执行
