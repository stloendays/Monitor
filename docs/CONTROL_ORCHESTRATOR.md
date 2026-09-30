# Control Orchestrator v1

The control orchestrator is the single schedulable entrypoint for Monitor Hub's automatic recovery control plane.

Instead of scheduling three independent commands, one tick performs:

```text
commands/inbox.jsonl
        |
        v
policy dispatch
        |
        +--> L1 deterministic dispatch
        +--> L2 child-Agent dispatch
        +--> L3 decision events
        |
        v
dispatch worker
        |
        +--> L1 handler execution
        +--> L2 launch/result reconciliation
        +--> durable escalation on failure/uncertainty
        |
        v
notification outbox sync
        |
        v
main-Agent pending notifications
```

## CLI

Run one normal tick:

```powershell
monitor_hub_orchestrator --tick
```

Run a tick that performs deterministic work and reconciles existing child-Agent results but does not launch new child Agents:

```powershell
monitor_hub_orchestrator --tick --no-launch-agents
```

Use an explicit Hub Data directory when needed:

```powershell
monitor_hub_orchestrator --tick --hub-data D:\MonitorHubData
```

The command returns one machine-readable JSON object containing:

- command-control result;
- worker result;
- current unacknowledged outbox projection;
- `needs_main_agent`.

## Tick ordering

Ordering is contractual:

1. **Command policy dispatch**
   - validate unprocessed commands;
   - route L1/L2;
   - turn L3 into durable Issue/notification events.
2. **Worker pass**
   - execute registered L1 handlers;
   - launch bounded L2 child Agents when allowed;
   - reconcile completed L2 results from previous ticks.
3. **Outbox synchronization**
   - materialize any notification events produced by steps 1–2;
   - return current unacknowledged main-Agent work.

This ordering means a new L3 escalation or deterministic-handler failure is visible in the outbox before the same tick returns.

## Idempotency

The orchestrator itself adds no new mutable store.

It relies on the existing durable idempotency keys:

- command `command_id`;
- dispatch `dispatch_id`;
- deterministic event IDs;
- worker receipts;
- notification record IDs and ACKs.

Therefore the same inbox may be revisited on every tick without re-running terminal actions.

## Scheduling

v1 deliberately exposes a one-shot tick rather than embedding an infinite loop.

This makes it suitable for:

- Windows Task Scheduler;
- cron/systemd timers;
- a future Qt background controller;
- a main-Agent supervisor;
- test harnesses.

A single global orchestrator schedule is preferred over creating one orchestrator task per monitored project. Project monitors produce commands/events; the global control plane consumes them.

## Authority behavior

The orchestrator does not add authority.

- L1 remains limited to policy-approved deterministic handlers.
- L2 remains limited to bounded child-Agent profiles and constraints.
- L3 remains user/main-Agent only.
- Successful action execution still does not imply recovery.

```text
task.restarted / agent.completed
        !=
issue.recovery_verified / issue.resolved
```

Recovery remains the responsibility of the owning monitor after observing downstream evidence.

## Main-Agent handoff

When the returned JSON contains:

```json
{
  "needs_main_agent": true
}
```

the durable outbox contains at least one unacknowledged notification.

The main Agent can inspect and ACK through the existing notification CLI contract.

## Tests

The dedicated orchestrator CTest covers:

- L1 command -> policy dispatch -> read-only worker execution in one tick;
- replay does not re-execute terminal work;
- L3 command -> decision notification -> durable outbox in one tick;
- ACK persists and clears `needs_main_agent` on a later tick.
