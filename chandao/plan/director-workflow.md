# 软件总监交付流程：如何启用

本仓库把「软件总监 + 四步交付」做成了 **Skill + Agent**，可进 git、多人共享。

## 创建了什么

| 类型 | 名称 | 路径 |
| --- | --- | --- |
| Skill | `software-director-delivery` | `.cursor/skills/software-director-delivery/SKILL.md` |
| 主 Agent | `software-director` | `.cursor/agents/software-director.md` |
| 执行 Agent | `delivery-implementer` | `.cursor/agents/delivery-implementer.md` |
| 测试 Agent | `delivery-tester` | `.cursor/agents/delivery-tester.md` |
| 排查 Agent | `delivery-triage` | `.cursor/agents/delivery-triage.md` |

Skill 负责编排；总监 Agent 负责解析需求与派工。后三个是被派工的专业 subagent。若 Task 工具里暂时看不到自定义类型，Skill 会回退到内置 `generalPurpose` / `explore` / `shell`。

## 启用 Skill

Cursor 会从项目 `.cursor/skills/` 自动发现 Skill，**无需命令行注册**。

1. 打开侧边栏 **Customize（自定义）** → **Skills**。
2. 找到 `software-director-delivery`。若列表里是关闭状态，打开它。
3. 本 Skill 设置了 `disable-model-invocation: true`，**不会在普通写代码时自动插入**，避免和正在做的应用开发抢上下文。
4. 需要跑交付流程时，在 Agent 聊天输入：

```text
/software-director-delivery
```

然后贴上需求（范围、技术栈、工作区、验收，知道多少写多少）。

也可以说：

```text
/software-director-delivery 按软件总监流程做：……（需求原文）
```

若 Customize 里没有出现该 Skill：确认文件在 `.cursor/skills/software-director-delivery/SKILL.md`，重开 Agent 聊天或 Reload Window。

## 启用 Agent

自定义 Agent 放在 `.cursor/agents/`，项目内共享，提交到 git 即可。

在 Agent 聊天用名字调用：

```text
/software-director 解析这个需求并先出计划，不要直接改代码：……
```

或自然语言：

```text
用 software-director 当软件总监，先对齐计划再派工。
```

派工阶段总监会调用：

```text
/delivery-implementer
/delivery-tester
/delivery-triage
```

一般不必手动调后三个；走 `/software-director-delivery` 后由总监按 Skill 分配。

## 四个职责怎么跑

1. **接受需求，制定计划，与用户头脑风暴，制定最终计划并输出保存。**  
   留在当前对话。产出 `plan/delivery-plan.md`。你确认前不大改代码。
2. **分配 Agent 执行计划。**  
   派 `delivery-implementer`（或 `generalPurpose`）。记录 `plan/execution-log.md`。
3. **分配 Agent 执行测试。**  
   派 `delivery-tester`（或 `shell`）。默认：**编译成功且产物出现在约定 install 目录**（本仓库常见 `_install/`）。记录 `plan/test-report.md`。
4. **分配 Agent 做测试后的问题排查、审核、修复建议/再执行。**  
   失败则 `delivery-triage`（或 `explore`）分析 → 执行 Agent 修改 → 再测。最多 5 轮。仍失败则把阻塞项写进 `plan/results/final-result.md` 交给你。

没有第 5 个职责。

## 输入 / 输出

- **输入**：需求原文、工作区路径、技术约束。
- **输出**：`plan/delivery-plan.md`、`plan/execution-log.md`、`plan/test-report.md`、`plan/triage-log.md`、`plan/results/final-result.md`。

不要和 `plan/project-plan.md` 抢同一个文件。中文计划/结果；代码标识符按项目约定。

## 与其他任务并行

「实现产品」和「沉淀 Skill/Agent」可以同时进行，但文件所有权分开：

- 交付执行：产品源码 + 上面列出的 `plan/delivery-*.md` / `plan/results/`
- 流程沉淀：`.cursor/skills/`、`.cursor/agents/`、本文件 `plan/director-workflow.md`

## 建议调用方式

新需求从计划做到验收：

```text
/software-director-delivery

需求：……
工作区：本仓库
约束：CMake + Qt，产物安装到 _install
验收：编译成功且 _install 下出现约定可执行文件
```
