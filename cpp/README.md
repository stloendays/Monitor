# Monitor Hub C++ migration — phase 1

This directory starts the C++20 migration of `hub/monitor_hub.py`. The Python implementation remains the production GUI during this phase; existing monitor scripts and status-file formats do not change.

Implemented in phase 1:

- project registry loading;
- generic `hub_status.json` adapter;
- status-row classification and automatic summary generation;
- runner normalization for `runner.kind = none` plus Task Scheduler fixture data;
- final-result list merging;
- health priority compatible with `docs/ARCHITECTURE.md` (`done → error → paused → attention → stale → working → ok`);
- the built-in “新任务办理” placeholder project;
- `monitor_hub_cli --dump` JSON output for compatibility testing;
- unit tests for the core rules;
- Windows/MSVC CI through GitHub Actions.

Still handled by Python and scheduled for the next phases:

- live Task Scheduler COM and WMI probing;
- detached-job probing and takeover indexing;
- Markdown and QoI adapters;
- stream-json transcript rendering;
- Qt 6 GUI;
- management actions, read-only Q&A, and setup-agent launching.

## Build on Windows

Use MSVC 2022 and vcpkg:

```powershell
cd cpp
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

## Compatibility CLI

Generate the existing demo data and point the C++ CLI at the same registry:

```powershell
python ..\tests\demo\make_demo.py $env:TEMP\monitor-hub-demo
$env:MONITOR_HUB_REGISTRY="$env:TEMP\monitor-hub-demo\demo_projects.json"
$env:MONITOR_HUB_DATA="$env:TEMP\monitor-hub-demo\hubdata"
$env:MONITOR_HUB_NO_DISCOVERY="1"
.\build\Release\monitor_hub_cli.exe --dump > cpp_demo_snapshots.json
```

Phase 1 intentionally migrates the deterministic core first. The production Python GUI remains the fallback until the Windows probe and Qt UI have parity tests.


## Phase 2: live Windows read-only probe

The Phase 2 branch adds a native Windows probe while keeping the migration read-only:

- Task Scheduler 2.0 COM reads monitor task names, state, last/next run time, result code, repetition interval and executable action.
- WMI reads `ProcessId`, `Name` and `CommandLine` for PowerShell, Python and Claude processes used by detached monitors.
- `monitor_hub_cli --probe-system` prints the raw probe as JSON.
- `monitor_hub_cli --dump` uses the live probe unless `--system-info FILE` is supplied.
- no Task Scheduler registration, enable/disable, interval change, process termination or job restart exists in this phase.

Examples:

```powershell
.\build\Release\monitor_hub_cli.exe --probe-system
.\build\Release\monitor_hub_cli.exe --dump
.\build\Release\monitor_hub_cli.exe --dump --system-info tests\fixture_system.json
```

CI runs `--probe-system` on a Windows runner after the MSVC build and CTest. A non-empty probe error fails the job.


## Phase 3: detached monitor discovery and state

The next read-only migration step covers monitors launched through the shared detached-job directory:

- discover unregistered job directories whose names contain `monitor`;
- parse canonical names such as `demo__monitor__local__15m`;
- infer project/scope/check interval for the runner-only view;
- read `pid` and `exitcode` files;
- verify a live PID against the probed process command line and the detached job directory;
- distinguish `running`, intentional `stopped`, normal `exit:0`, missing jobs, and unexpected `gone/exit:<code>` states.

This phase is still read-only. It does not stop, restart, or recreate detached jobs.


## Phase 4: legacy Markdown and takeover indexing

This phase makes the C++ snapshot layer understand the existing Vanda/HPC monitor format instead of requiring every old project to migrate to `hub_status.json` first.

Implemented:

- parse the first Markdown status table and notes;
- reproduce `done/run/queue/bad/other` row classification;
- extract legacy “下次检查” text when the scheduler has no next-run time;
- detect `ssh/remote monitor FAILED`;
- read legacy attention files and correlate them with recent takeover activity;
- honor legacy DONE marker files;
- index detached Claude takeover jobs by prefix;
- index `claude_takeover_<timestamp>.jsonl|md` files;
- parse the final Claude stream-json `result` event;
- surface friendly quota/login/service failure summaries.

The adapter remains read-only. Existing Markdown monitor outputs do not need to change.


## Phase 5: QoI compatibility without mandatory project imports

The C++ snapshot layer now supports the legacy `adapter: "qoi"` status shape.

Key migration rule:

- stored row values remain backward-compatible;
- optional top-level `live.{job}` values override checkpoint/failure counts and newest checkpoint time;
- task-level `workdir / log / result / params` become the common task-detail metadata;
- the C++ hub never imports or executes a project Python module;
- the Python hub uses `live.{job}` first and only falls back to `qoi_ext_monitor.scan()` for a RUNNING job whose live data is missing.

The long-term target remains the generic `hub_status.json` contract. See `docs/QOI_ADAPTER.md` and `hub/qoi_status.example.json`.


## Phase 6: setup-request parity and result discovery

The C++ snapshot layer now has parity for the hub's built-in “新任务办理” project:

- scan `<stamp>_request.md` and `<stamp>_report.md`;
- correlate each request with `hub-setup-<stamp>` detached Claude takeovers;
- parse `NEEDS_USER:` from setup reports;
- distinguish running / completed / completed-needs-user / interrupted setup requests;
- expose request/report/takeover files through task-detail metadata;
- surface setup failures and user decisions through the normal attention model.

Final deliverables also support project-level `results_glob` patterns in C++. Explicit results and glob-discovered files are merged into `results_list`, with duplicate paths removed.

Both features are still read-only discovery. They do not launch setup agents or mutate result files.
