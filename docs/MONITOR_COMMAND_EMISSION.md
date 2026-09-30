# Monitor Issue-to-Command Bridge

Project monitors are observers and recovery verifiers. They must not silently become recovery executors.

The control flow is:

```text
live evidence
  -> issue.detected / issue.classified
  -> policy action selected
  -> Protocol v1 command submitted
  -> global control orchestrator
  -> L1 deterministic handler OR L2 child Agent OR L3 escalation
  -> action/restart facts
  -> project monitor observes downstream state
  -> issue.recovery_started
  -> issue.recovery_verified
  -> issue.resolved
```

## Command submission

A monitor should materialize one command JSON object and submit it through:

```powershell
monitor_hub_control --submit-command command.json --hub-data <MONITOR_HUB_DATA>
```

The control CLI validates the envelope and deduplicates by `command_id`.

Directly appending an unvalidated executable action or calling the L1/L2 worker from the project monitor is not the intended boundary.

## Stable identities

Keep these identities stable across one recovery chain:

- `project_id`
- `task_id`
- `issue_id`
- `correlation_id`

Create a stable `command_id` for one issue/action attempt.

At-least-once delivery is allowed:

```text
same command_id submitted twice
-> one logical command
-> no second dangerous execution
```

If a later distinct recovery attempt is required, use a new command ID while preserving the same Issue/correlation identity unless the Issue itself has genuinely changed.

## Choosing authority

The monitor does not choose arbitrary execution content.

It selects an action already present in the project recovery policy:

### L1

Example command payload:

```json
{
  "action_id": "restart_pbs_same_parameters"
}
```

Executable/script/argv details remain in policy-owned `handler_config`.

### L2

Example command payload:

```json
{
  "action_id": "troubleshoot_known_failure"
}
```

The policy resolves this to a bounded `agent_profile`.

### L3

If the required decision is outside L1/L2 authority, submit/emit L3 decision work instead of inventing a recovery action.

## Monitor responsibilities after action

The monitor may observe facts such as:

```text
issue.action_applied
task.restarted
agent.action_finished
agent.completed
```

These facts mean an action occurred. They do not prove recovery.

When downstream verification begins, the monitor emits:

```text
issue.recovery_started
```

Only after the declared recovery criteria pass should it emit:

```text
issue.recovery_verified
issue.resolved
```

Examples of recovery evidence:

- replacement PBS job is actually visible/running;
- local replacement process is alive;
- output resumes changing;
- convergence criteria recover;
- expected checkpoint/result appears;
- the project-specific health rule returns to normal.

If verification fails, the Issue remains open. The monitor may submit another policy-authorized attempt or escalate, depending on the policy.

## Global orchestrator ownership

Project monitors produce commands and verify recovery.

They do not each create their own worker/orchestrator.

A single global control-plane tick processes all projects:

```powershell
monitor_hub_orchestrator --tick
```

This keeps execution idempotency, Agent launch, and durable main-Agent notification in one shared control plane.

## Setup Agent requirements

When creating or migrating a monitor, the setup Agent should configure this bridge only for actions explicitly authorized by the user.

If it cannot identify a valid policy action, exact deterministic restart entrypoint, or bounded L2 playbook, it must leave the recovery as L3 and report `NEEDS_USER`.
