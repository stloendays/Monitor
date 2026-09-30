# Monitor Hub 并行开发与兼容性契约

本文件定义 Monitor Hub 的长期开发治理规则。目标不是限制功能扩展，而是允许多个开发者、ChatGPT/Claude/Codex 对话和自动化 Agent 同时推进不同功能，同时避免版本漂移、接口互相覆盖和“各自都能跑，合起来就坏”的情况。

`AGENTS.md` 给出必须执行的简版规则；本文件解释具体做法。

---

## 1. 核心原则

Monitor Hub 采用：

> **Interface-first · branch-isolated · backward-compatible · CI-gated · main-only-release**

含义分别是：

1. **Interface-first**：先稳定公共契约，再允许各模块独立实现。
2. **Branch-isolated**：每项功能在自己的 branch/PR 中工作。
3. **Backward-compatible**：默认只做向后兼容的增量扩展。
4. **CI-gated**：接口兼容性必须由自动测试验证，而不是依赖开发者记忆。
5. **Main-only-release**：只有已进入 `main` 的代码才允许成为正式 Release。

---

## 2. 不存在永久固定的“开发分支”

不要在文档里写死“当前 canonical branch = xxx”。

本项目允许 stacked PR，因此任意时刻可能出现：

```text
main
└── phase A
    └── phase B
        ├── feature C
        └── feature D
```

同时也可能有从较早阶段分出的独立 UI branch。

因此每个 Agent 开始工作前必须实时发现：

- `main` 当前 HEAD；
- open PR；
- 每个 PR 的 `base` 和 `head`；
- 自己功能真正依赖的最新祖先；
- 自己要修改的共享文件是否正在被其他 PR 修改。

### 2.1 选择基线

如果功能完全独立：

```text
main → feature-x
```

如果功能依赖尚未合并的 phase：

```text
phase-n → feature-x
```

并且 PR 应 target `phase-n`，而不是伪装成直接对 `main` 的 PR。

### 2.2 基线发生变化时

如果另一个 Agent 在你开发期间把依赖线从：

```text
phase4
```

推进到：

```text
phase4 → phase5 → phase6
```

必须先：

1. compare 原 base 与最新依赖 head；
2. 找出重叠文件；
3. 将自己的修改重新应用/合并到最新代码；
4. 再更新或新建 PR。

禁止拿旧版共享文件全文覆盖新版文件。

---

## 3. 契约等级

### Tier 0 — 公共协议，最严格

这些内容视为软件公共 API：

| 契约 | 当前来源 |
|---|---|
| 通用监控状态 | `hub/hub_status.schema.json` |
| 任务下钻元数据 | `table.row_meta` |
| monitor 命名/身份 | `docs/MONITOR_NAMING.md` |
| CLI 参数和 JSON 输出 | `monitor_hub_cli` |
| Agent/Event 协议 | `docs/AGENT_EVENT_PROTOCOL.md` |
| Release/version | `VERSION`、release manifest |
| UI 输入模型 | normalized project/task/issue/event model |

Tier 0 的变化原则：

- 新字段优先 optional；
- 新状态必须定义旧客户端如何处理；
- 不得静默改变已有字段含义；
- 删除/改名必须先经历 deprecation；
- breaking change 必须升级 schema/protocol version。

### Tier 1 — 共享集成文件

这些文件不是公共 API，但多个功能经常同时修改：

```text
README.md
hub/monitor_hub.py
cpp/CMakeLists.txt
cpp/src/main.cpp
.github/workflows/*
docs/ARCHITECTURE.md
```

修改规则：

1. 写之前重新读取当前 branch HEAD；
2. 只做最小 patch；
3. 保留所有无关功能；
4. PR 前再次与 base HEAD 比较；
5. 发现同时修改时，必须进行显式 reconciliation。

### Tier 2 — 组件私有实现

例如：

```text
hub/updater.py
hub/apply_update.py
Qt-specific source files
某个 adapter 的私有实现
独立 tests/fixture
```

在不破坏 Tier 0 的前提下，组件内部可以自由重构。

---

## 4. 稳定身份

### 4.1 Project ID

`project_id` 是机器身份，不是显示名。

显示名可改，`project_id` 不应因为 UI 文案变化而改变。

### 4.2 Task ID

`task_id` 必须稳定。

UI selection persistence、事件关联、issue history、agent repair history 都依赖它。

不要使用临时行号作为长期 `task_id`。

### 4.3 Issue ID

同一故障生命周期应使用同一个 `issue_id`：

```text
detected → assigned → repairing → verified → resolved
```

同一个 issue 不应该在每一轮 monitor check 中产生一个全新的身份。

---

## 5. Schema 演化

### 5.1 允许的默认变化

例如：

```json
{
  "task_id": "slab-07",
  "log": "OUTCAR",
  "agent_state": "repairing"
}
```

在旧格式基础上新增 optional `agent_state` 是允许的。

### 5.2 不允许的隐式 breaking change

例如直接：

```text
task_id → task_uuid
```

或者把：

```text
error = monitor 自身故障
```

改成：

```text
error = 任意计算失败
```

都属于 breaking change。

正确迁移方式至少应包含一个阶段：

```text
reader supports old + new
writer emits compatible representation
fixtures cover both forms
all consumers migrate
old form deprecated
future major version may remove it
```

---

## 6. UI 与核心逻辑的边界

最终可能同时存在 Tkinter、Qt、Web 等 UI。

因此 UI 不能成为新的业务逻辑来源。

UI 应：

- 读取 normalized Project / Task / Issue / AgentRun / Event；
- 展示状态和时间线；
- 发送明确的 user intent；
- 打开路径、日志、结果；
- 展示需要用户决定的事项。

UI 不应：

- 自己解析某个 VASP/QoI 项目的原始格式；
- 根据 raw log 自己判断是否应该重投；
- 自己定义新的 health 优先级；
- 自己决定 child agent 的权限；
- 为了视觉需求改变核心字段语义。

如果 Qt 与 Python UI 对同一 snapshot 得出不同业务结论，这是 core/contract bug，而不是“两个 UI 各有自己的逻辑”。

---

## 7. Agent 边界

### Main Agent

负责：

- 用户总体目标；
- 新任务和研究/业务决策；
- L3 决策；
- 接收最终完成通知；
- 根据结果决定下一阶段。

### Monitor Hub

负责：

- 状态采集；
- 进度归一化；
- 问题检测；
- 依据 policy 进行确定性处理或分派 child agent；
- 记录全过程；
- 验证恢复；
- 向 main agent/用户升级问题；
- 汇报最终完成。

### Child Agent

负责：

- 一个明确 issue 或 bounded task；
- 在给定 policy/constraints 内调查和处理；
- 输出结构化行动记录与最终结果。

Child Agent 不得因为看到邻近问题就自行扩大项目范围。

---

## 8. 权限等级

### L1 — Deterministic

预先定义的机械操作，例如：

- 相同参数续算；
- 读取状态；
- 重试一个明确失败的网络查询；
- 汇总已经完成的数据。

不需要 LLM 自主判断。

### L2 — Policy-bounded child agent

子 Agent 可以判断，但必须受到项目 policy 约束。

例如：

- 分析 SCF 不收敛原因；
- 按已经批准的 troubleshooting playbook 选择一个处理方案；
- 修复后重新提交并验证。

### L3 — Main agent / user decision

包括：

- 改变科学方法或研究设计；
- 改变关键物理/化学参数且 policy 未授权；
- 增加新的研究对象；
- 删除/覆盖不可恢复数据；
- 出现多个实质不同方案；
- 需要额外成本/资源授权；
- 安全边界之外的动作。

L3 不能通过 child agent 自行“合理猜测”。

---

## 9. 并行开发工作流

每项功能建议遵循：

```text
discover topology
    ↓
choose dependency base
    ↓
create isolated branch
    ↓
implement component-local changes first
    ↓
patch shared integration files last
    ↓
run contract + component tests
    ↓
re-check base HEAD
    ↓
compare diff for accidental rollback
    ↓
open PR against actual parent
```

PR body 至少记录：

- base branch；
- base SHA；
- 依赖的其他 PR；
- 触碰的 Tier 0 contract；
- 触碰的 shared integration files；
- backward compatibility 说明；
- 测试结果。

---

## 10. 合并顺序

Stacked PR 必须按 dependency order 合并。

例如：

```text
main
└── phase4
    └── phase5
        └── phase6
            └── feature-x
```

正确顺序：

```text
phase4 → phase5 → phase6 → feature-x
```

上游合并后，下游应更新 base/rebase，并重新跑兼容性测试。

不要把 stacked PR 独立 squash 到 `main`，导致依赖历史重复或丢失。

---

## 11. 测试门槛

### 修改状态/schema

必须覆盖：

- legacy fixture；
- current fixture；
- malformed fixture；
- Python/C++ 对同一输入的语义一致性。

### 修改 CLI

必须验证：

- 旧 flag 仍可用；
- 旧 JSON 字段没有消失或改变含义；
- 新 flag 不抢占旧 flag 语义。

### 修改 UI

必须验证：

- 同一 snapshot 的状态/进度一致；
- task selection 由 stable `task_id` 维持；
- error/attention/working 不被隐藏；
- raw log 不是唯一的问题解释来源。

### 修改 Agent 协议

必须验证：

- event schema；
- idempotent handling；
- issue correlation；
- L1/L2/L3 escalation；
- completion notification；
- unknown new event 对旧 consumer 的安全处理。

### 修改 Release/updater

必须验证：

- single-source version；
- release only from integrated `main`；
- development checkout 不被覆盖；
- package/checksum/manifest；
- local registry/runtime data 不被覆盖。

---

## 12. 兼容性策略

推荐支持窗口：

- 当前 contract version；
- 至少一个明确的 legacy version/format，直到迁移完成。

适配器可以逐步淘汰，但必须通过显式 deprecation，而不是在重构时顺手删除。

新实现应优先做到：

```text
old producer → new consumer: works
new producer → old consumer: degrades safely when possible
new producer → new consumer: full functionality
```

---

## 13. Shared-file stale-write 禁令

这是多 Agent 开发最重要的具体规则之一。

禁止：

1. Agent A 在 10:00 读取 `cpp/src/main.cpp`；
2. Agent B 在 10:10 加入 `--probe-system`；
3. Agent A 在 10:20 用自己 10:00 的全文版本写回，只留下自己的 `--version`。

正确做法：

1. 写入前重新读取 HEAD；
2. 在最新版本上加入 `--version`；
3. 最终同时保留：

```text
--dump
--probe-system
--version
```

对于 `README.md`、CMake、workflow、GUI 主文件同样适用。

---

## 14. Definition of Done

功能完成必须同时满足：

- 实际功能实现；
- 现有行为未被意外回退；
- Tier 0 兼容性已验证；
- 必要 tests 已加入；
- 文档同步；
- PR 基线仍然正确；
- shared files 与最新 base 对齐；
- diff 中没有其他 Agent 功能被删除；
- CI 通过；
- 未完成依赖写在 PR 中，而不是藏在聊天记录里。

---

## 15. 后续强化

本契约应逐步变成可执行约束：

- contract fixtures；
- Python/C++ snapshot parity tests；
- event schema validator；
- PR CI compatibility checks；
- automated stale-base/shared-file checks；
- optional CODEOWNERS / component ownership；
- integration test covering Main Agent → Monitor → Child Agent → Monitor → Main Agent。

规则的最终目标不是“文档更漂亮”，而是让破坏兼容性的提交在进入 `main` 前自动失败。
