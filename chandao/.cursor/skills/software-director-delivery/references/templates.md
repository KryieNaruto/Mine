# 交付文档模板

用户使用中文时，用中文填写。代码路径、目标名、CMake/Qt 标识符保持项目原样。

## plan/delivery-plan.md

```markdown
# 交付计划

## 需求原文
（原样粘贴）

## 工作区
- 根目录：
- 源码目录：
- 构建目录：
- 安装目录：

## 范围
- 做：
- 不做：

## 技术栈与约束
- 语言 / 框架 / 构建系统：
- 已拍板的技术决策（必须可落地，例如 CMake 生成器、Qt 路径变量、MSVC 架构）：

## 任务拆分
1. 任务：… | 目录：… | 完成定义：…

## 验收标准
- 默认：编译成功，且产物出现在约定 install 目录（路径：）
- 用户覆盖：

## 风险与未知
- 风险：
- 仍未知（已向用户确认则删除）：

## 派工策略
- 执行 Agent：delivery-implementer（回退 generalPurpose）
- 测试 Agent：delivery-tester（回退 shell）
- 排查 Agent：delivery-triage（回退 explore + generalPurpose）
- 最大测试-修复轮次：5

## 对齐状态
- [ ] 用户已确认，可以执行
```

## plan/execution-log.md

```markdown
# 执行记录

## 轮次
- 计划版本：plan/delivery-plan.md
- 开始时间：
- 使用的 Agent：

## 做了什么
- 修改的文件 / 目录：
- 未做（超出范围）：

## 结果
- 成功 / 部分完成 / 失败：
- 阻塞：

## 备注
```

## plan/test-report.md

```markdown
# 测试报告

- 轮次：N / 5
- 验收标准：
- 工作区：

## 命令与结果

| 步骤 | 命令 | 退出码 | 摘要 |
| --- | --- | --- | --- |

## 产物
- 约定 install 目录：
- 期望产物：
- 是否存在：

## 结论
- PASS / FAIL
- 失败日志摘要（若 FAIL）：
```

## plan/triage-log.md

```markdown
# 排查记录

- 对应测试轮次：
- Agent：

## 审核
- 是否真的按计划实现：
- 是否测到了验收标准：

## 根因
- 证据（文件/日志）：
- 结论：

## 修复建议（给执行 Agent）
1. 改哪些文件：
2. 最小改动是什么：
3. 改完如何再验：

## 是否建议停止并交还用户
- 否 / 是（原因）：
```

## plan/results/final-result.md

```markdown
# 交付结果

## 做了什么
- 需求：
- 计划：plan/delivery-plan.md
- 实际改动摘要：

## 结果是什么
- 验收：通过 / 未通过
- 测试轮次：
- 产物路径：

## 证据
- 执行记录：plan/execution-log.md
- 测试报告：plan/test-report.md
- 排查记录：plan/triage-log.md（若有）

## 残留风险 / 阻塞项
- 无 / 列出交给用户的问题
```
