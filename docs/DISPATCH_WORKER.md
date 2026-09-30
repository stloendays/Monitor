# Dispatch Worker v1

The dispatch worker consumes policy-validated records produced by `monitor_hub_control`.

It is intentionally separate from command validation:

```text
command
  -> policy dispatcher
  -> durable L1/L2 dispatch record
  -> dispatch worker
  -> execution / child-Agent facts
  -> monitor independently verifies recovery
```

## CLI

Run one worker pass:

```powershell
monitor_hub_worker --run-once
```

Materialize/reconcile L2 work without launching new child Agents:

```powershell
monitor_hub_worker --run-once --no-launch-agents
```

Override Hub Data when needed:

```powershell
monitor_hub_worker --run-once --hub-data D:\MonitorHubData
```

## L1 behavior

Only audited built-in handler IDs are executable.

### `read_only_probe`

This built-in handler:

- requires absolute `context_refs`;
- only reads filesystem metadata;
- records existence, file/directory kind, file size, and modification time;
- emits `monitor.check_completed`;
- does not modify project files;
- does not emit `issue.resolved`.

Any L1 handler that is not compiled into the worker fails closed. The worker emits:

```text
issue.user_action_required
notification.requested
```

with `reason=unsupported_l1_handler`.

This is deliberate: a policy string is not enough to grant arbitrary local code execution.

## L2 child-Agent launch

For a validated L2 dispatch, the worker creates:

```text
MONITOR_HUB_DATA/
  agent-runs/
    <dispatch-id>/
      prompt.txt
      result.json
```

The prompt contains:

- stable dispatch/project/task/issue identity;
- the policy-resolved Agent profile;
- hard constraints;
- completion criteria;
- evidence/context references;
- an explicit L2-only authority boundary;
- a machine-readable result schema.

The worker reuses the existing Monitor Hub Claude runtime configuration:

- PowerShell from `MONITOR_HUB_PWSH` when set;
- detached helper from `MONITOR_HUB_DETACH`;
- Claude executable from `MONITOR_HUB_CLAUDE_EXE`;
- child-Agent model from `MONITOR_HUB_AGENT_MODEL` (default `sonnet`).

The launcher invokes PowerShell directly as a process with an argv vector; it does not execute a command payload through a generic shell.

## Agent result contract

The child Agent must atomically publish:

```json
{
  "schema_version": 1,
  "dispatch_id": "dsp:cmd_...",
  "success": true,
  "summary": "what was found or done",
  "action_type": "restart_same_parameters",
  "action_applied": true,
  "evidence_refs": [
    "OUTCAR",
    "submission_record.json"
  ],
  "uncertainty": "",
  "needs_user": "none"
}
```

If the child Agent needs L3 authority, `needs_user` contains the reason instead of `none`.

## Result reconciliation

A later worker pass reads `result.json`.

Successful bounded work may emit:

```text
agent.evidence_recorded
agent.action_finished
agent.completed
```

If the Agent reports unresolved L3 scope:

```text
agent.analysis_recorded
issue.user_action_required
notification.requested
```

If the Agent reports failure, the worker also escalates durably.

## Recovery invariant

The worker never converts child-Agent completion into Issue resolution.

These remain separate facts:

```text
agent.action_finished
agent.completed
     !=
issue.recovery_verified
issue.resolved
```

The owning monitor must observe downstream evidence such as a replacement job actually running, output continuing, convergence recovering, or the declared completion condition becoming true.

## Idempotency

Worker state is append-only:

```text
MONITOR_HUB_DATA/dispatch/worker-receipts.jsonl
```

Latest states include:

```text
launched
completed
needs_user
failed
```

Terminal dispatches are not executed again after restart.

Event IDs are deterministic per dispatch/suffix, so replay does not create a second logical Agent action or escalation.

## Working directory

For L2, the worker uses the first existing absolute context reference:

- if it is a directory, that directory becomes the Agent working directory;
- if it is a file, its parent directory is used;
- otherwise Hub Data is used as a safe fallback.

The prompt still requires the child Agent to locate and read project governance before making changes.

## Deliberately unsupported L1 actions

A generic `restart_same_parameters` worker is **not** implemented here because restart semantics are scheduler/project specific.

The correct next step is an audited deterministic handler registry where each stable handler ID is backed by a project/platform adapter with explicit argument and recovery-verification contracts. Until such a handler exists, a policy naming it will fail closed rather than running arbitrary shell text.
