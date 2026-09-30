# Deterministic Handler Registry v1

The deterministic handler registry is the execution boundary for **L1** Monitor Hub actions.

A command cannot choose an executable, PBS script, or argv. The command can only request a pre-authorized `action_id`. The selected policy action freezes the handler and its `handler_config`, and the dispatcher copies that configuration into the durable dispatch record.

```text
command.action_id
  -> project policy
  -> fixed handler + fixed handler_config
  -> durable dispatch
  -> audited deterministic handler
```

Command payload values that look like `program`, `script_path`, or `arguments` are not used as execution authority.

## Registered handlers

### `read_only_probe`

Authority: L1

Behavior:
- reads only absolute `context_refs`;
- records existence, file/directory type, size, and modification time;
- emits `monitor.check_completed`;
- does not modify project files;
- does not emit recovery or resolution.

No `handler_config` is accepted.

### `local_process_restart_v1`

Authority: L1

Policy configuration:

```json
{
  "handler": "local_process_restart_v1",
  "handler_config": {
    "program": "C:/MonitorAdapters/restart-wrapper.exe",
    "arguments": [
      "--resume",
      "C:/Research/Example/checkpoint.json"
    ],
    "working_directory": "C:/Research/Example"
  }
}
```

Rules:
- `program` must be an absolute path;
- `arguments` must be an array of strings;
- `working_directory`, when present, must be absolute;
- the configured program must exist at execution time;
- the program is launched directly with an argv vector;
- no shell interpolation is used;
- launch is detached and the worker records the process ID when available.

This handler is appropriate for a pre-audited wrapper that starts/resumes the real local task and returns control to Monitor Hub.

A successful launch emits:
- `issue.action_applied` when an Issue ID exists;
- `task.restarted`.

It does **not** emit `issue.recovery_verified` or `issue.resolved`.

### `pbs_qsub_restart_v1`

Authority: L1

Policy configuration:

```json
{
  "handler": "pbs_qsub_restart_v1",
  "handler_config": {
    "script_path": "/home/user/project/restart.pbs",
    "working_directory": "/home/user/project"
  }
}
```

Rules:
- `script_path` must be an absolute path;
- `working_directory`, when present, must be absolute;
- the PBS script must exist at execution time;
- the exact script path comes only from policy;
- the worker invokes `qsub <script_path>` directly;
- the worker waits for the qsub command to return;
- exit code 0 means the submission request was accepted by the qsub command.

The qsub executable is deployment configuration, not command data:

```text
MONITOR_HUB_QSUB=/path/to/qsub
```

When the variable is absent, the worker uses `qsub` from PATH.

A successful qsub call emits:
- `issue.action_applied` when an Issue ID exists;
- `task.restarted`.

The Monitor must still independently observe the replacement PBS job before recovery can be verified.

## Policy validation

Policy parsing fails closed for malformed registered handler configuration.

Examples:
- relative local executable -> rejected;
- non-string `arguments` item -> rejected;
- relative PBS script -> rejected;
- non-object `handler_config` -> rejected;
- `handler_config` supplied to `read_only_probe` -> rejected.

Unknown future handler IDs can remain in policy for forward compatibility, but the worker will not execute them until a compiled/audited handler is registered.

## Failure behavior

A registered handler that cannot execute produces:

```text
worker receipt = failed
issue.user_action_required
notification.requested
```

with `reason=deterministic_handler_failed`.

An unknown handler produces the same durable escalation with `reason=unsupported_l1_handler`.

This makes missing executables, missing scripts, qsub failures, and unsupported adapters visible to the user/main Agent instead of silently retrying unsafe alternatives.

## Recovery invariant

The deterministic layer applies or requests a bounded mechanical action. It does not decide that the project recovered.

```text
issue.action_applied
task.restarted
        !=
issue.recovery_verified
issue.resolved
```

The owning monitor must verify downstream evidence such as:
- replacement process/job exists;
- output resumes;
- convergence/health criteria recover;
- project-specific completion criteria become true.

## Copyable examples

- `examples/command-control/recovery-policy-local-restart-v1.json`
- `examples/command-control/recovery-policy-pbs-restart-v1.json`

These examples intentionally keep executable/script authority in policy, never in the command payload.
