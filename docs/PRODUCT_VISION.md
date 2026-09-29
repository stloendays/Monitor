# Monitor Hub 产品愿景与最终形态

## 1. 一句话定义

> **Monitor Hub 是面向用户的长任务与 Agent 运维控制台：统一展示任务进度、问题、Agent 修复过程和最终结果，并把需要决策或已完成的信息可靠返回主 Agent。**

它最终不是一个“日志窗口”，也不是一个新的主 Agent。

用户看到的核心问题应该始终是：

- 现在有哪些项目在跑？
- 每个项目做到哪里了？
- 哪个任务出了问题？
- 系统是否已经自动处理？
- 子 Agent 做了什么？
- 修复后是否真的恢复正常？
- 哪件事需要我/主 Agent 决定？
- 最后交付了什么？

---

## 2. 核心用户故事

用户对主 Agent 说：

> 帮我把这批任务全部跑完。

之后理想流程是：

```text
Main Agent 创建工作
        ↓
长任务自动注册到 Monitor Hub
        ↓
Monitor 持续采集状态
        ↓
UI 展示 Project / Task / Progress
        ↓
发现问题
        ↓
按权限判断 L1 / L2 / L3
        ↓
L1: 确定性处理
或
L2: 分派 Child Agent
或
L3: 通知 Main Agent / 用户
        ↓
记录调查、动作、重启、验证全过程
        ↓
任务恢复
        ↓
所有 completion criteria 满足
        ↓
生成最终 summary + artifacts
        ↓
通知 Main Agent：项目已完成
```

用户不需要持续盯 terminal，但始终保留可见性和决策权。

---

## 3. 产品中的六个核心对象

### Project

一个用户能理解的工作单元，例如：

- CeOx/Rh 计算；
- PUR 数据分析；
- QoI benchmark；
- ML training campaign。

Project 有整体目标、完成标准、任务集合、最终 artifacts。

### Task

真正执行的单个运行单元，例如：

- 一个 PBS job；
- 一个结构优化；
- 一个 Python training run；
- 一个后处理步骤。

Task 必须有稳定 `task_id`。

### Issue

一次具体问题的完整生命周期。

Issue 不是“一条红色日志”，而是：

```text
detected
→ classified
→ assigned
→ investigating
→ action selected
→ action applied
→ restarted
→ verified
→ resolved
```

### Agent Run

一次 child agent 的受控处理过程。

用户应该能看到摘要化的：

- 为什么启动；
- 读取了什么证据；
- 得出什么判断；
- 执行什么动作；
- 动作是否成功；
- 最终结论。

### Event

不可变的事实记录，例如：

```text
task.started
task.progress
issue.detected
agent.started
agent.action
issue.resolved
task.completed
project.completed
```

UI 最终应主要由 normalized state + event timeline 驱动。

### Artifact

任务/项目的交付物，例如：

- result table；
- RESULTS.md；
- checkpoint；
- figure；
- model；
- report。

---

## 4. 最终 UI 信息架构

建议最终导航：

```text
Overview
Projects
Attention
Agent Activity
Completed
Settings
```

其中最重要的是 Overview、Project Detail 和 Attention。

---

## 5. Overview — 控制塔

首页不是设置页，而是“现在系统整体发生了什么”。

建议结构：

```text
┌──────────────────────────────────────────────────────────────┐
│ Monitor Hub                              Agent ● Connected   │
├───────────────┬──────────────────────────────────────────────┤
│ Projects      │ Overall                                   │
│               │                                            │
│ CeOx/Rh       │ Running            7                       │
│ PUR           │ Agent handling     2                       │
│ QoI           │ Need attention     1                       │
│ ML training   │ Completed          5                       │
│               │                                            │
│               │ Recent Activity                            │
│               │ 15:42 task resumed                         │
│               │ 15:38 child agent applied recovery         │
│               │ 15:20 project completed                    │
└───────────────┴──────────────────────────────────────────────┘
```

首页必须优先展示：

- 项目总体健康；
- completion progress；
- running / queue / repairing / attention 数量；
- 最近关键事件；
- 当前真正需要用户处理的事项。

不要把 raw logs 放在首页。

---

## 6. Project Detail

点进项目后，用户首先看到：

### Project summary

```text
CeOx/Rh

Progress
███████████████░░░ 82%

14 completed
2 running
1 queued
1 agent handling
0 need user
```

### Workflow stages

可以同时表达阶段性进度：

```text
Preparation
→ Submission
→ Running
→ Validation
→ Post-processing
→ Summary
→ Completed
```

百分比和 workflow stage 都要有，因为“完成 80%”不能代替“现在正在做什么”。

### Task list

```text
✔ Ce4O8 clean
✔ Ce4O6 L1
⟳ Ce4O6 L2
⚙ Ce4O6 L3    Agent handling
◷ Ce4O6 L4    queued
```

Task row 必须可下钻。

---

## 7. Task Detail

Task Detail 是用户理解“一个具体任务”的地方。

建议包含：

```text
Task
Ce4O6 L3

Status
Running

Current stage
Ionic relaxation 38 / 60

Job
PBS 1420121

Host
Vanda

Started
13:42

Last update
15:51
```

再向下展示：

- structured parameters；
- files/log/result；
- 当前 issue；
- Agent actions；
- command（默认只复制，不执行）；
- raw evidence。

优先展示结构化信息，raw log 放在最后一级。

---

## 8. Issue / Agent Timeline

这是 Monitor Hub 和普通 job monitor 最大的产品差异。

例如：

```text
14:32  issue.detected
       SCF reached NELM without convergence

14:33  agent.started
       Troubleshooting child agent assigned

14:34  agent.evidence
       OUTCAR and OSZICAR inspected

14:35  agent.decision
       Failure classified as oscillatory SCF

14:36  agent.action
       Applied policy-approved recovery

14:37  task.restarted
       PBS 1420192 submitted

14:49  recovery.verified
       Electronic convergence normal

14:50  issue.resolved
```

用户默认看到的是可读摘要。

需要审计时再展开：

- raw command；
- exact diff；
- source files；
- full stream-json transcript。

---

## 9. Attention Inbox

所有需要用户/主 Agent 决定的事情集中到一个明确入口。

例如：

```text
Need decision · CeOx/Rh · task L7

Two scientifically distinct recovery options are available.
Changing the selected option modifies the calculation methodology.

[View evidence] [Send to Main Agent]
```

Attention 不应与普通 warning 混在一起。

一个好的系统应该让用户相信：

> 如果 Attention 是空的，当前没有系统无法自行合法处理的事情。

---

## 10. Agent Activity

可以提供独立的 Agent Activity 页面，但它是审计/理解入口，不是主导航核心。

显示：

- active child agents；
- 所属 project / task / issue；
- elapsed time；
- current phase；
- policy/authority level；
- actions taken；
- outcome。

必须明确区分：

```text
Main Agent
Monitor Hub
Child Agent
Deterministic automation
```

不能都叫 “Agent”。

---

## 11. 完成后的体验

`project.completed` 不等于 UI 变绿。

完成时系统应该：

1. 验证 completion criteria；
2. 检查 planned task 分母；
3. 汇总 failed/restarted/recovered 数量；
4. 收集 final artifacts；
5. 生成 final summary；
6. 写入 durable event/outbox；
7. 通知 Main Agent；
8. UI 显示 Completed。

最终消息示例：

```text
CeOx/Rh campaign completed.

18 / 18 planned tasks completed.
17 completed without intervention.
1 required automatic recovery and passed validation afterward.
0 unresolved issues.

Artifacts:
- results/batch2_table.md
- RESULTS.md
- figures/energy_order.png
```

---

## 12. 自动接入本地 Agent

最终目标：

> 一切被标记为“需要监控”的长任务，都可以自动进入 Monitor Hub。

不要求主 Agent 手工编辑一个 JSON 文件。

Main Agent / project agent 可以发送一个标准 command：

```text
monitor.register
```

包含：

- project identity；
- task identity；
- execution target；
- completion criteria；
- monitor cadence；
- allowed recovery policy；
- forbidden actions；
- result/artifact expectations；
- notification target。

Monitor Hub 负责注册、运行和后续状态管理。

---

## 13. Agent 处理模型

### L1 — deterministic

确定性、已授权动作。

### L2 — child agent

按项目 policy 在有限范围内分析和修复。

### L3 — main agent / user

需要改变目标、方法、关键参数、资源投入或其他实质决策。

UI 应直接显示当前 issue 属于哪个 authority level。

---

## 14. UI 设计原则

### 14.1 状态优先

始终可见：

- status；
- progress；
- problem；
- current action；
- decision required。

### 14.2 帮助文案二级化

解释性的文字：

- tooltip；
- info button；
- help panel。

不要占据主操作区域。

### 14.3 错误不能藏起来

以下内容不能只出现在 hover：

- error；
- failed recovery；
- stale monitor；
- attention；
- blocked task；
- user decision required。

### 14.4 Progressive disclosure

建议层级：

```text
Project
→ Task
→ Issue
→ Agent Run
→ Raw Evidence
```

### 14.5 Structured before raw

优先：

```text
Issue: SCF non-convergence
Action: policy-approved restart
Outcome: recovered
```

而不是先展示 500 行 terminal。

---

## 15. 技术架构目标

最终推荐：

```text
┌─────────────────────────────────────┐
│ UI                                  │
│ Qt / future web                     │
└──────────────────┬──────────────────┘
                   │ normalized model
┌──────────────────▼──────────────────┐
│ Monitor Core                        │
│ state / progress / health / issues │
└──────────┬────────────────┬─────────┘
           │                │
┌──────────▼──────┐ ┌───────▼────────┐
│ Monitor Adapter │ │ Agent Gateway   │
│ local/PBS/...   │ │ child/main      │
└──────────┬──────┘ └───────┬────────┘
           │                │
           └────────┬───────┘
                    │
┌───────────────────▼─────────────────┐
│ Contract / Event Layer              │
│ task / issue / agent / completion   │
└─────────────────────────────────────┘
```

关键原则：

> UI、monitor adapter、Agent implementation 不直接依赖彼此内部实现；它们依赖稳定 contract。

---

## 16. 兼容当前系统的迁移方式

不要求一次性重写全部 monitor。

现有：

- generic `hub_status.json`；
- legacy Markdown；
- QoI status；
- stream-json takeover；

都可以继续由 adapter 转成 normalized model。

Event layer 可以先由现有 snapshot/adapter **合成事件**。

后续新 monitor 原生输出 event/structured state，旧格式逐步 deprecated。

这样不会为了最终架构把现有可运行系统推倒重来。

---

## 17. 产品验收标准

当软件成熟时，一个普通用户应能够在不打开 terminal 的情况下回答：

1. 当前有哪些项目正在运行？
2. 每个项目完成了多少？
3. 哪些 task 正常、排队、失败或被 Agent 处理？
4. 一个问题为什么发生？
5. Agent 做了什么？
6. 修复后是否经过验证？
7. 有没有需要我决定的事情？
8. 已完成项目的最终结果在哪里？
9. 主 Agent 是否已经收到完成/阻塞信息？

如果这些问题只能通过读 raw log 回答，说明产品还没有达到目标。
