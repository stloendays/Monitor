# Monitor Hub C++ migration

This directory starts the C++20 migration of `hub/monitor_hub.py`. The Python implementation remains the production GUI during this phase; existing monitor scripts and status-file formats do not change.

Implemented so far:

- project registry loading;
- generic `hub_status.json` adapter and status-contract validation;
- status-row classification, `row_meta` parity, result merging and health evaluation;
- live Windows Task Scheduler probing through the Task Scheduler COM API;
- live `Win32_Process` probing through WMI, without spawning PowerShell;
- `runner.kind = none / schtask / detach` state normalization;
- detached monitor auto-discovery under the cdesktop job root;
- detached and glob-style Claude takeover history indexing;
- deterministic `--system-info FILE` fixtures for CI and compatibility tests;
- `monitor_hub_cli --dump` JSON output;
- Windows/MSVC CI and unit tests.

Still handled by Python and scheduled for later phases:

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

The production Python GUI remains the fallback until the Qt UI and management/Q&A surfaces have parity tests. The C++ CLI now uses live Task Scheduler + WMI probing by default; pass `--system-info FILE` to force deterministic fixture data.
