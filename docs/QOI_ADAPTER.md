# QoI compatibility adapter

The QoI adapter exists to keep the original QoI monitor format readable while Monitor Hub moves toward the common `hub_status.json` contract.

For new monitors, prefer `adapter: "generic"` and write `hub_status.json`. Use `adapter: "qoi"` only for existing QoI jobs that still emit the legacy status shape.

## Legacy status rows

```json
{
  "updated": "2026-09-29T10:00:00",
  "rows": [
    {
      "job": "density-fold2",
      "status": "RUNNING",
      "checkpoints": 610,
      "failures": 1,
      "total": 1200,
      "last_checkpoint_min_ago": 9,
      "rate_per_h": 138.2,
      "eta_h": 4.3,
      "branch_pushed": false
    }
  ]
}
```

Supported status prefixes:

- `COMPLETE`
- `RESULTS_WRITTEN_NOT_PUSHED`
- `TAKEOVER_ACTIVE`
- `CHECKPOINTS_DONE_NO_RESULTS`
- `RETRY_FAILED_ITEMS`
- `DEAD`
- `RUNNING`

## Data-only live bridge

The old Python hub imported the project's `qoi_ext_monitor.py` module to rescan checkpoint files between monitor rounds. That coupling is now optional.

A QoI monitor can write:

```json
{
  "live": {
    "density-fold2": {
      "checkpoints": 642,
      "failures": 1,
      "newest_checkpoint": "2026-09-29T09:58:00"
    }
  }
}
```

For Python:

1. `live[job]` is used first.
2. Only a RUNNING job missing from `live` may fall back to the legacy `qoi_ext_monitor.scan()` import.
3. completed/stopped jobs do not trigger that import.

For C++:

- only the status file is read;
- no project Python module is imported or executed;
- if `live[job]` is absent, the adapter uses the row's stored checkpoint/failure values.

`newest_checkpoint` may be an ISO local timestamp. The C++ adapter also accepts `newest_checkpoint_epoch`.

## Task drill-down

Legacy QoI rows may optionally add:

```json
{
  "workdir": "D:\\Research\\QoI\\fold2",
  "log": "D:\\Research\\QoI\\fold2\\worker.log",
  "result": "D:\\Research\\QoI\\results\\fold2.md",
  "params": {
    "fold": 2,
    "seed": 43,
    "batch_size": 64
  }
}
```

These are converted to the same `table.row_meta` used by generic monitors, so the task detail panel can show parameters and open the relevant files.

A full example is in `hub/qoi_status.example.json`.

## Migration target

The QoI adapter is a compatibility bridge, not the preferred long-term protocol. A future QoI monitor should write the generic status contract directly. Once all deployed QoI monitors do so, the project-specific import fallback can be deleted.
