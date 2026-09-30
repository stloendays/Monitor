# Durable Main-Agent Notification Outbox

Monitor Hub now has a durable local notification channel for facts that must survive GUI, Agent, or machine restarts.

Runtime path:

```text
MONITOR_HUB_DATA/
  events/
    <project_id>.jsonl
  outbox/
    notifications.jsonl
```

The outbox is append-only. Its current notification state is replayed from durable records instead of being stored only in memory or inferred from a green/red UI badge.

## What enters the outbox

Protocol v1 `notification.requested` is authoritative.

For migration safety, Monitor Hub also creates a deterministic synthetic notification when an older producer emits either of these required trigger facts without the corresponding notification event:

- `issue.user_action_required` → `reason=decision_required`
- `project.completed` → `reason=project_completed`

If an explicit request covers the same Issue/project, the compatibility bridge does not create a second notification.

Protocol events can include these optional payload fields:

```json
{
  "notification_id": "ntf_...",
  "target": "main_agent",
  "reason": "decision_required"
}
```

When `notification_id` is omitted on a request, Monitor Hub derives a stable ID from the event ID.

## Delivery semantics

The implementation follows **at-least-once delivery + idempotent consumer**:

- event facts are already deduplicated by `event_id`;
- each outbox transition has a stable `record_id`;
- repeated sync does not append duplicate pending/delivery records;
- acknowledgement is durable and idempotent;
- malformed JSONL lines are isolated instead of destroying the whole outbox;
- a short-lived filesystem lock prevents concurrent writers from interleaving records;
- stale lock directories older than five minutes are recoverable after a crash.

States currently projected:

```text
pending
delivered
acknowledged
```

A notification remains actionable until it is acknowledged.

## Main-Agent CLI

List current unacknowledged notifications:

```powershell
monitor_hub_cli --notifications
```

List the complete durable history projection, including acknowledged items:

```powershell
monitor_hub_cli --notifications-all
```

Acknowledge one notification after the main Agent/user has handled it:

```powershell
monitor_hub_cli --ack-notification ntf_auto_or_explicit_id
```

Optionally record a different consumer identity:

```powershell
monitor_hub_cli --ack-notification ntf_123 --notification-actor desktop-main-agent
```

The commands emit machine-readable JSON so a local main Agent, connector bridge, scheduled task, or future IPC service can consume the same contract.

## Separation from Issue state

Notification delivery is not the same as Issue resolution.

These facts remain distinct:

```text
agent.action_finished
issue.recovery_verified
issue.resolved
notification.requested
notification.delivered
notification.acknowledged
```

This prevents Monitor Hub from treating “the Agent finished an action” or “the UI showed a message” as proof that the underlying project recovered or that the main Agent received the message.

## Compatibility

This change is additive:

- existing `hub_status.json` semantics are unchanged;
- existing event envelopes remain Protocol v1;
- producers do not have to migrate immediately because the two mandatory high-value notification cases have a compatibility bridge;
- the outbox can be read without launching Qt;
- `--dump` retains its existing normalized-state behavior.

The next integration layer can transport pending outbox entries to a concrete main-Agent IPC/plugin endpoint and emit `notification.delivered` after successful handoff.
