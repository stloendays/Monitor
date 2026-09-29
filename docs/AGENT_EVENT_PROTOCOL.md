# Monitor Hub Agent / Event Protocol v1

本协议定义 Monitor Hub、monitor adapter、child agent、main agent 和 UI 之间的长期语义边界。

目标是让不同 Agent/不同 UI/不同执行环境能够替换，而不改变“发生了什么”的含义。

> **Events are facts. Commands are requests. State is a projection.**

---

## 1. 为什么 Event 和 Command 必须分开

错误做法：

```text
"restart"
```

它既可能表示“请求重启”，也可能表示“已经重启”。

正确做法：

```text
command: task.restart.requested
event:   task.restart.started
event:   task.restarted
event:   recovery.verified
```

因此：

- **Command** 表示希望系统做什么；
- **Event** 表示已经发生什么；
- **State/Snapshot** 是根据 events + live probe 得到的当前视图。

---

## 2. Event envelope

Protocol v1 的标准 event：

```json
{
  "schema_version": 1,
  "event_id": "evt_01H...",
  "event_type": "issue.detected",
  "occurred_at": "2026-09-29T15:30:22+08:00",

  "project_id": "ceox-rh",
  "task_id": "ce4o6-l3",
  "issue_id": "iss_01H...",
  "agent_run_id": null,

  "correlation_id": "corr_01H...",
  "source": {
    "kind": "monitor",
    "id": "ceox-rh__monitor__hpc__300m"
  },

  "severity": "warning",
  "payload": {}
}
```

### Required fields

- `schema_version`
- `event_id`
- `event_type`
- `occurred_at`
- `project_id`
- `source`
- `payload`

### Conditional identity fields

- `task_id`: task-scoped event
- `issue_id`: issue lifecycle event
- `agent_run_id`: child-agent run
- `correlation_id`: 将 command、issue、agent run 和 recovery 串成一个处理链

### event_id

必须唯一且稳定。

消费者需要能够根据 `event_id` 去重。

---

## 3. Source

推荐：

```json
{
  "kind": "monitor|core|ui|child_agent|main_agent|scheduler|adapter|updater",
  "id": "stable-component-id"
}
```

source 表示事实由谁报告，不表示谁拥有最终决策权。

---

## 4. Severity

v1 建议：

```text
info
warning
error
critical
```

severity 与 authority level 不是同一概念。

例如一个严重问题也可能通过 L1 自动恢复；一个不严重的问题也可能因为需要研究决策而属于 L3。

---

## 5. Core event types

### Project

```text
project.registered
project.updated
project.completed
project.blocked
```

### Task

```text
task.registered
task.queued
task.started
task.progress
task.paused
task.resumed
task.failed
task.restarted
task.completed
task.cancelled
```

### Issue

```text
issue.detected
issue.classified
issue.assigned
issue.investigating
issue.action_selected
issue.action_applied
issue.recovery_started
issue.recovery_verified
issue.resolved
issue.escalated
issue.user_action_required
```

### Child agent

```text
agent.started
agent.evidence_recorded
agent.analysis_recorded
agent.action_proposed
agent.action_started
agent.action_finished
agent.completed
agent.failed
```

### Artifact

```text
artifact.created
artifact.updated
artifact.validated
```

### Notification

```text
notification.requested
notification.delivered
notification.acknowledged
```

### Monitor

```text
monitor.registered
monitor.check_started
monitor.check_completed
monitor.stale
monitor.error
```

新 event type 可以增加，但旧 consumer 遇到未知 event 时应：

- 保存；
- 可显示 generic timeline entry；
- 不崩溃；
- 不自行赋予新的执行语义。

---

## 6. Command envelope

标准 command：

```json
{
  "schema_version": 1,
  "command_id": "cmd_01H...",
  "command_type": "agent.troubleshoot.request",
  "requested_at": "2026-09-29T15:31:00+08:00",

  "project_id": "ceox-rh",
  "task_id": "ce4o6-l3",
  "issue_id": "iss_01H...",
  "correlation_id": "corr_01H...",

  "requested_by": {
    "kind": "monitor",
    "id": "ceox-rh__monitor__hpc__300m"
  },

  "authority": "L2",
  "policy_ref": "policies/ceox-rh-recovery-v1",

  "constraints": [
    "do not change functional",
    "do not create new scientific systems"
  ],

  "completion_criteria": [
    "job is resubmitted or escalation is emitted",
    "recovery result is recorded"
  ],

  "context_refs": [
    "D:/Research/CeOx/L3/OUTCAR",
    "D:/Research/CeOx/L3/OSZICAR"
  ],

  "payload": {}
}
```

Command ID 必须支持幂等处理。

同一个 `command_id` 被重复投递时，不应执行两次危险动作。

---

## 7. Authority model

### L1

确定性、预授权。

例如：

```text
same-parameter restart
read-only probe
known retry
result aggregation
```

### L2

调用 child agent，但受到明确 policy 和 constraints 限制。

Child agent 必须返回：

- evidence；
- analysis；
- selected action；
- action result；
- unresolved uncertainty。

### L3

必须升级给 main agent / user。

例如：

- 改变科学方法；
- 改关键参数且 policy 未授权；
- 新增研究对象；
- 删除/覆盖不可恢复数据；
- 多个实质不同方案；
- 额外资源/成本授权；
- 超出既定 project scope。

Monitor Hub 应产生：

```text
issue.user_action_required
notification.requested
```

而不是让 child agent 自己选择。

---

## 8. Issue lifecycle

推荐状态机：

```text
detected
   ↓
classified
   ↓
assigned
   ↓
investigating
   ↓
action_selected
   ↓
action_applied
   ↓
recovery_started
   ↓
recovery_verified
   ↓
resolved
```

任何阶段都可能：

```text
→ escalated
→ user_action_required
```

### 8.1 resolved 的定义

不能因为“Agent 执行动作成功”就直接 resolved。

必须有后续验证，例如：

- 新 job 实际进入 running；
- SCF 恢复正常；
- output 继续增长；
- expected checkpoint 出现；
- completion condition 重新满足。

因此：

```text
action_finished != issue.resolved
```

---

## 9. Task lifecycle

基本状态：

```text
registered
queued
running
paused
failed
recovering
completed
cancelled
```

`recovering` 可以是 state projection，不一定需要独立 task event；它可以由 active unresolved issue + agent activity 推导。

UI 不应仅根据 scheduler 的 R/Q/F 单字母决定最终 task health。

---

## 10. Progress event

推荐：

```json
{
  "event_type": "task.progress",
  "payload": {
    "stage": "ionic_relaxation",
    "current": 38,
    "total": 60,
    "fraction": 0.6333,
    "message": "ionic step 38 / 60"
  }
}
```

`total` 允许为空，因为部分工作无法预先确定总步数。

不要伪造一个看似精确的百分比。

---

## 11. Agent action

建议把用户可读摘要和审计证据分开：

```json
{
  "event_type": "agent.action_finished",
  "payload": {
    "summary": "Restarted from the validated WAVECAR using the existing calculation parameters.",
    "action_type": "restart_same_parameters",
    "success": true,
    "evidence_refs": [
      "OUTCAR",
      "OSZICAR",
      "submission_record.json"
    ]
  }
}
```

UI 默认显示 `summary`。

用户展开时再读取 evidence。

---

## 12. Main Agent 回传

### 12.1 需要决策

产生：

```text
issue.user_action_required
notification.requested
```

notification payload 推荐：

```json
{
  "target": "main_agent",
  "reason": "decision_required",
  "summary": "Two scientifically different recovery options require a project-level decision.",
  "project_id": "ceox-rh",
  "task_id": "ce4o6-l7",
  "issue_id": "iss_..."
}
```

### 12.2 项目完成

必须产生：

```text
project.completed
notification.requested
```

`project.completed.payload` 推荐包含：

```json
{
  "planned_tasks": 18,
  "completed_tasks": 18,
  "failed_tasks": 0,
  "recovered_tasks": 1,
  "unresolved_issues": 0,
  "summary": "All planned calculations completed and passed final validation.",
  "artifacts": [
    {
      "label": "Results table",
      "path": "results/batch2_table.md"
    }
  ]
}
```

UI 变成绿色不是可靠回传机制。

必须有 durable notification/event。

---

## 13. Delivery semantics

推荐采用 **at-least-once delivery + idempotent consumer**。

原因：

- Agent/GUI/本机可能重启；
- 网络可能断；
- notification transport 可能暂时不可用。

因此：

- event 通过 `event_id` 去重；
- command 通过 `command_id` 去重；
- notification 未确认前保留在 durable outbox；
- consumer restart 后可重放。

不要依赖“只发一次，一定收到”。

---

## 14. Persistence

v1 不强制某一种数据库。

允许：

- JSONL append-only event store；
- SQLite；
- 后续 service/database。

但存储层不能改变 event 语义。

一个可接受的初期布局：

```text
HUB_DATA/
  events/
    <project_id>.jsonl
  outbox/
    notifications.jsonl
  issues/
  agent-runs/
```

---

## 15. 与现有 hub_status.json 的兼容

本协议不要求立即重写所有现有 monitor。

现有 adapter 可以从：

- generic `hub_status.json`；
- legacy Markdown；
- QoI status；
- Task Scheduler/WMI；
- takeover stream-json；

生成 normalized snapshot，并在检测到状态转换时合成 events。

例如：

```text
previous task tag = run
current task tag = done
→ synthesize task.completed
```

后续新 monitor 可以原生发 event。

旧 monitor 通过 adapter 继续工作。

---

## 16. Event 与 Snapshot 的关系

Event store 是历史事实。

Snapshot 是当前状态。

例如：

```text
issue.detected
agent.started
agent.action_finished
issue.recovery_verified
issue.resolved
```

最后 Project/Task snapshot 只需要显示：

```text
status = running
active_issue = none
last_recovery = resolved
```

但 timeline 仍可完整重放。

---

## 17. UI 消费规则

UI 应优先消费：

- normalized project/task snapshot；
- issue model；
- event timeline。

UI 不能：

- 根据某个项目 raw log 自己发明 issue；
- 从 Agent 文本里用关键词猜 authority；
- 看到 `agent.action_finished` 就假设恢复成功；
- 丢弃未知 event。

---

## 18. 安全与隐私

Event/command payload 中不得包含：

- token；
- password；
- API secret；
- SSH private key；
- 明文凭证。

路径、命令和日志如果可能包含敏感信息，应由 evidence reference 指向，而不是无限复制到 notification。

---

## 19. Protocol versioning

当前：

```text
schema_version = 1
```

v1 内：

- 可以增加 optional 字段；
- 可以增加新的 event type；
- consumer 对未知 optional 字段必须忽略；
- consumer 对未知 event type 必须安全保存/展示，而不能崩溃。

如果必须改变已有字段含义或 envelope 结构，则升级 major protocol version。

---

## 20. 最小端到端验收案例

未来 integration test 至少应覆盖：

```text
Main Agent
  → monitor.register command
Monitor
  → task.registered
  → task.started
  → task.progress
  → issue.detected
Monitor
  → agent.troubleshoot.request
Child Agent
  → agent.started
  → agent.evidence_recorded
  → agent.action_finished
Monitor
  → task.restarted
  → issue.recovery_verified
  → issue.resolved
  → task.completed
  → project.completed
  → notification.requested(target=main_agent)
Main Agent
  → notification.acknowledged
```

这个流程跑通，才表示 Monitor Hub 真正成为 Agent 工作流中的可靠协调层，而不只是一个 GUI。
