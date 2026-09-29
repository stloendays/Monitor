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
