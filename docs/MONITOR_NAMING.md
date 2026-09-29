# Monitor naming and task metadata convention

This convention makes monitor identity and task details machine-readable. Existing monitors remain supported; new monitors should follow this format.

## 1. Monitor script filename

Use:

```text
monitor__<project>__<scope>__<interval>.py
```

Examples:

```text
monitor__ceox-rh__hpc__300m.py
monitor__qoi-ext__local__15m.py
monitor__pur-screening__local__30m.py
```

Rules:

- `project`: lowercase slug, letters/numbers/hyphens only.
- `scope`: execution domain or monitor role, e.g. `hpc`, `local`, `qoi`, `analysis`.
- `interval`: `15m`, `5h`, `1d`.
- double underscores `__` are structural separators; single hyphens remain available inside project names.
- do not encode scientific values such as `U=5`, `ENCUT=450` or model hyperparameters into the filename. Those belong in structured task metadata.

## 2. Windows Task Scheduler / detached job name

Use:

```text
<project>__monitor__<scope>__<interval>
```

Examples:

```text
ceox-rh__monitor__hpc__300m
qoi-ext__monitor__local__15m
```

The hub can parse the project, scope and nominal interval from this name even before the monitor is registered.

## 3. Script metadata: MONITOR_META

New Python monitor scripts should declare a literal top-level dictionary near the top of the file:

```python
MONITOR_META = {
    "schema": 1,
    "project_id": "ceox-rh",
    "scope": "hpc",
    "interval_min": 300,
    "display_name": "CeOx/Rh HPC monitor",
    "param_keys": ["ENCUT_eV", "ISMEAR", "SIGMA_eV", "U_Ce_eV"],
}
```

The hub reads this with Python AST + `ast.literal_eval`. It does **not** import or execute the monitor script. Keep `MONITOR_META` literal: strings, numbers, booleans, lists and dictionaries only.

Use the filename for stable identity; use `MONITOR_META` for human-readable labels and parameter declarations. Scientific/runtime values themselves still belong in each task's `row_meta.params`.

## 4. Per-task metadata in hub_status.json

Every displayed table row may have an aligned object in `table.row_meta`.

```json
{
  "table": {
    "cols": ["体系", "作业号", "状态", "进度", "E (eV)"],
    "rows": [
      ["slab_CO_bridge", "100245", "R", "离子步 31", "-262.8801"]
    ],
    "tags": ["run"],
    "row_meta": [
      {
        "task_id": "slab_CO_bridge",
        "job_id": "100245",
        "host": "vanda",
        "open_path": "D:\\Research\\CeOx\\slab_CO_bridge",
        "log": "D:\\Research\\CeOx\\slab_CO_bridge\\OUTCAR",
        "result": "D:\\Research\\CeOx\\results\\slab_CO_bridge.md",
        "script": "monitor__ceox-rh__hpc__300m.py",
        "command": "qsub run.pbs",
        "params": {
          "ENCUT_eV": 450,
          "ISMEAR": 1,
          "SIGMA_eV": 0.1,
          "U_Ce_eV": 5.0
        }
      }
    ]
  }
}
```

The array is positional: `row_meta[i]` describes `rows[i]`.

Recommended keys:

- `task_id`: stable task identifier.
- `job_id`: scheduler/process job ID.
- `host`: machine or HPC alias.
- `open_path`: preferred path opened when the user double-clicks the row.
- `path` / `workdir`: fallback task directory.
- `log`: primary log/output file.
- `result`: task-level result file.
- `script`: monitor or worker script.
- `command`: launch/submit command.
- `params`: structured parameters relevant to this task.

## 5. UI behavior

For generic monitors, the hub now preserves `row_meta`. On the Progress tab:

- double-click a row or press Enter to open `open_path`;
- fallback order is `open_path → path → workdir → log → result`;
- if no path exists, the hub shows the available task/job/host/script/command/parameter metadata instead.

This keeps the visible table concise while making each row drillable.

## 6. Compatibility

Old status files without `row_meta` continue to work unchanged. The naming parser is advisory for legacy monitors; it never changes their execution behavior.
