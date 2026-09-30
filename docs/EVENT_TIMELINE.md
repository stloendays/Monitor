# Event Timeline and Issue Projection

Monitor Hub can now consume the Agent/Event Protocol v1 event stream as a first-class read-only source for issues, Agent activity, recovery verification, and user/main-Agent attention.

## Runtime layout

Protocol events are read from:

```text
MONITOR_HUB_DATA/
  events/
    <project_id>.jsonl
```

Each line is one v1 event envelope from `docs/AGENT_EVENT_PROTOCOL.md`. Existing `hub_status.json`, legacy Markdown, QoI adapters, Task Scheduler/WMI, and takeover records continue to work unchanged.

## Behavior

The C++ core:

- validates required v1 envelope fields without changing the existing status schema;
- deduplicates at-least-once delivery by `event_id`;
- isolates malformed lines instead of failing the whole project;
- preserves unknown future event types as generic timeline entries;
- correlates Issue lifecycle records by stable `issue_id`;
- keeps `agent.action_finished` distinct from `issue.recovery_verified` and `issue.resolved`;
- projects unresolved `issue.user_action_required` / L3 escalations into Overview Attention;
- projects `agent.*` facts into cross-project Agent Activity.

## UI semantics

The project-level **问题与 Agent** tab shows:

1. current Issue projections, including task, stage, authority, current Agent action and summary;
2. the event timeline, newest first;
3. diagnostics for duplicate or malformed input without hiding valid events.

L3 decisions remain visible until an explicit `issue.resolved` event clears them.

This phase is intentionally read-only. It does **not** dispatch recovery commands or let the UI infer recovery policy from logs. L1/L2 command dispatch should be added as a separate authority-aware control-plane change.

## Compatibility

This is an additive consumer of the existing Protocol v1 contract:

- no `hub_status.json` field is renamed or reinterpreted;
- no existing CLI flag is changed;
- old monitors do not need to emit events;
- event files can be introduced project by project;
- takeover history remains available as legacy/parallel Agent evidence.
