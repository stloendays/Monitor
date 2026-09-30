# Command Dispatcher and Recovery Policy v1

Monitor Hub's command control plane separates **requested intent** from **execution authority**.

A command is never treated as permission by itself. The dispatcher validates the command against a project-scoped recovery policy and then routes it into one of three authority lanes:

```text
L1 command
  -> policy validates deterministic action
  -> HUB_DATA/dispatch/l1/<command>.json

L2 command
  -> policy validates bounded child-Agent action
  -> HUB_DATA/dispatch/l2/<command>.json

L3 command
  -> no executor queue
  -> issue.user_action_required
  -> notification.requested
  -> durable main-Agent outbox
```

This phase deliberately does **not** execute arbitrary shell commands from a command payload. L1/L2 dispatch files are durable requests for dedicated runners/gateways.

## Runtime layout

```text
MONITOR_HUB_DATA/
  commands/
    inbox.jsonl
    receipts.jsonl
  policies/
    <policy>.json
  dispatch/
    l1/
      <stable-command-file>.json
    l2/
      <stable-command-file>.json
  events/
    <project_id>.jsonl
  outbox/
    notifications.jsonl
```

All command processing is replay-safe:

- `command_id` is the idempotency key;
- processed command IDs are recovered from durable receipts;
- dispatch files use deterministic filenames;
- a crash after dispatch-file creation but before receipt append does not cause a second dispatch file;
- L3 event IDs are deterministic and checked before append;
- malformed inbox records are isolated.

## Command envelope

The dispatcher consumes Protocol v1 command objects from `commands/inbox.jsonl`.

Example L1 command:

```json
{
  "schema_version": 1,
  "command_id": "cmd_restart_ce4o6_l7_001",
  "command_type": "task.restart.requested",
  "requested_at": "2026-09-30T12:00:00+08:00",
  "project_id": "ceox-rh",
  "task_id": "ce4o6-l7",
  "issue_id": "iss_ce4o6_l7",
  "correlation_id": "corr_ce4o6_l7",
  "requested_by": {
    "kind": "monitor",
    "id": "ceox-rh__monitor__hpc__300m"
  },
  "authority": "L1",
  "policy_ref": "policies/ceox-rh-recovery-v1",
  "constraints": [
    "do not change scientific parameters"
  ],
  "completion_criteria": [
    "replacement job is observed by the monitor"
  ],
  "context_refs": [
    "OUTCAR",
    "OSZICAR"
  ],
  "payload": {
    "action_id": "restart_same_parameters"
  }
}
```

For L1/L2, `payload.action_id` is mandatory.

For L3, `policy_ref` and `action_id` are not required because the command is not executable; it is converted into a main-Agent/user decision.

## Policy schema

Policies are additive local control-plane contracts stored below `MONITOR_HUB_DATA/policies`.

Example:

```json
{
  "schema_version": 1,
  "policy_id": "ceox-rh-recovery-v1",
  "project_id": "ceox-rh",
  "actions": [
    {
      "action_id": "restart_same_parameters",
      "authority": "L1",
      "dispatch_kind": "deterministic",
      "handler": "restart_same_parameters",
      "allowed_command_types": [
        "task.restart.requested"
      ],
      "constraints": [
        "reuse validated checkpoint",
        "do not change scientific parameters"
      ],
      "completion_criteria": [
        "replacement job is submitted",
        "monitor observes replacement job"
      ],
      "enabled": true
    },
    {
      "action_id": "troubleshoot_known_failure",
      "authority": "L2",
      "dispatch_kind": "child_agent",
      "agent_profile": "bounded-troubleshooter",
      "allowed_command_types": [
        "agent.troubleshoot.request"
      ],
      "constraints": [
        "do not change scientific method",
        "do not create new project scope"
      ],
      "completion_criteria": [
        "evidence and analysis are recorded",
        "bounded action is queued or L3 escalation is emitted"
      ],
      "enabled": true
    }
  ]
}
```

### Policy-owned deterministic handler configuration

L1 actions may carry an optional `handler_config` object. For registered restart handlers this object is the execution authority.

```text
command payload -> requested intent only
policy handler_config -> executable/script/argv authority
```

The dispatcher copies `handler_config` from the selected policy action into the durable dispatch record. It never copies executable/script/argv values from the command payload into handler authority.

Registered examples are documented in `docs/DETERMINISTIC_HANDLERS.md`.

### Policy invariants

- `policy_ref` must be a relative `policies/...` path.
- Absolute paths, `..`, and backslash-based escapes are rejected.
- A policy is bound to one `project_id`.
- L1 actions must use `dispatch_kind=deterministic` and name a stable `handler`.
- L2 actions must use `dispatch_kind=child_agent` and name an `agent_profile`.
- An action must explicitly list allowed command types.
- Disabled actions fail closed.
- Command constraints may only add restrictions; policy constraints remain in the normalized dispatch record.
- The command authority must exactly match the policy action authority.

The dispatcher does not accept an arbitrary executable or shell command as authorization.

## CLI

Submit one command JSON object:

```powershell
monitor_hub_control --submit-command command.json
```

Process all unprocessed commands:

```powershell
monitor_hub_control --dispatch-once
```

Print a policy example:

```powershell
monitor_hub_control --policy-example
```

Override Hub Data for testing or isolated deployments:

```powershell
monitor_hub_control --dispatch-once --hub-data D:\MonitorHubData
```

The CLI returns machine-readable JSON for submit/dispatch operations.

## L1 dispatch

L1 produces a normalized durable record under `dispatch/l1` containing:

- original command identity;
- resolved policy/action identity;
- deterministic handler ID;
- merged policy + command constraints;
- merged completion criteria;
- evidence/context references.

A future deterministic runner consumes this record and must emit factual events about execution and recovery. The dispatcher itself does not mark the Issue resolved.

## L2 dispatch

L2 produces a normalized durable record under `dispatch/l2` containing the bounded child-Agent profile, constraints, completion criteria, and evidence references.

A future Agent gateway consumes this record and must return:

- evidence;
- analysis;
- selected action;
- action result;
- unresolved uncertainty.

The child Agent cannot expand itself into L3 authority.

## L3 escalation

An L3 command is not sent to either executor lane. Monitor Hub emits deterministic Protocol v1 events:

```text
issue.user_action_required
notification.requested(target=main_agent)
```

The existing durable notification outbox then preserves the decision request until it is acknowledged.

## Receipts

Every processed command produces one append-only receipt:

```text
queued_l1
queued_l2
needs_user
rejected
```

A command with an existing receipt is not processed again.

Rejection examples include:

- invalid command envelope;
- missing policy;
- project/policy mismatch;
- unknown or disabled action;
- authority mismatch;
- disallowed command type;
- unsafe policy reference.

## Security boundary

Command/event payloads must not contain credentials or private keys.

Project IDs accepted by this control plane are restricted to filesystem-safe stable IDs containing only letters, digits, `-`, `_`, and `.` because they are used in durable event filenames.

The control plane intentionally uses named handlers and Agent profiles instead of executing strings from untrusted payloads.

## Next layer

The next implementation should add two consumers without changing this contract:

1. **L1 deterministic runner** — maps stable handler IDs to audited adapter functions and emits execution/recovery events.
2. **L2 Agent gateway** — materializes a bounded Agent request, launches the configured child Agent, records evidence/actions, and escalates uncertainty to L3.

Those consumers should treat `dispatch_id` / `command_id` as idempotency keys and must never infer `issue.resolved` from successful process exit alone.
