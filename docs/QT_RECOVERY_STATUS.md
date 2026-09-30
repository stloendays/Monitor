# Qt Automatic Recovery Status

The **问题与 Agent** page now exposes the recovery lifecycle as a structured operational view instead of requiring users to infer progress from raw events.

## Visible hierarchy

For each Issue the page shows:

- task ID;
- stable Issue ID;
- recovery flow;
- current stage;
- next expected step;
- authority level.

The flow is presented as:

```text
问题 -> 处理 -> 动作 -> 验证 -> 解决
```

A decision-gated Issue uses:

```text
问题 -> 决策 -> 动作 -> 验证 -> 解决
```

The detailed Issue table and newest-first Protocol event timeline remain below this summary.

## Recovery stages

Core projection derives user-facing stages from Protocol v1 facts:

```text
detected
classified / assigned / investigating
agent_handling
action_selected
waiting_verification
recovery_verification
recovery_verified
resolved
needs_user
failed
```

Important semantic invariants:

```text
agent.action_finished
agent.completed
task.restarted
issue.action_applied
    !=
issue.recovery_verified
issue.resolved
```

After an automatic action or restart, the normal visible state is **动作已完成，等待恢复验证** until the owning monitor independently verifies downstream evidence.

## Attention behavior

`issue.user_action_required` projects to **等待用户/主 Agent 决策**.

When a later factual event shows that the decision has already been acted on, such as:

- `issue.action_applied`;
- `task.restarted`;
- `issue.recovery_started`;

the stale decision-attention flag is cleared.

A later `issue.recovery_verified` clears decision attention as well.

## Authority display

The existing explicit event authority remains authoritative.

For additive compatibility:

- child-Agent events infer L2 when no explicit authority has been seen;
- a `task.restarted` fact emitted by `monitor-hub-dispatch-worker` infers L1 when authority is otherwise unknown;
- `issue.user_action_required` infers L3 when authority is otherwise unknown.

These inferences affect presentation only; they do not grant execution authority.

## UI summary

The page header now prioritizes operational counts:

```text
未解决 N · 等待恢复验证 N · 等待决策 N · 协议事件 N
```

Duplicate/malformed event diagnostics remain available in the header tooltip.

## Compatibility

This change is additive:

- Protocol v1 JSON is unchanged;
- existing event files continue to load;
- Issue IDs and event IDs remain unchanged;
- unknown event types remain preserved;
- old producers do not need to emit new fields;
- Qt consumes the normalized core projection rather than parsing project logs itself.
