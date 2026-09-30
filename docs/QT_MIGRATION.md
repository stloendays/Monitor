# Qt 6 migration status

The Qt UI is intentionally isolated from the validated C++ core so desktop work cannot destabilize monitoring logic.

## Current Qt scope

The optional `monitor_hub_qt` target currently provides a read-only operational shell backed by the same C++ core as `monitor_hub_cli`:

- live refresh from Windows Task Scheduler + WMI;
- project sidebar with health state;
- cross-project **Overview / control-tower** page;
- overview project-health and progress aggregation;
- cross-project **需要处理** queue built from normalized attention/error/stale state;
- cross-project recent Agent/takeover activity;
- double-click drill-down from Overview into the corresponding project;
- raw takeover evidence can be opened from the project takeover table;
- newbie-friendly two-row quick-debug action bar for project/task inspection plus registry/Hub/job-root access;
- context-aware command copy and bounded diagnostic-context copy without execution;
- Claude CLI sidebar card with local CLI/process detection;
- zero-token Claude statusLine bridge for 5-hour / 7-day usage and reset-time display;
- cdesktop-detach rate-limit event fallback when no statusLine cache is available;
- direct access to Claude config plus copyable `/usage` command;
- Claude session/workspace/activity metadata from statusLine without rendering conversation text;
- read-only workspace/transcript drill-down and recent Agent/tool metadata;
- project headline and runner state;
- progress table;
- dedicated task-detail pane;
- structured `row_meta.params` table;
- open task / log / result actions;
- copy-command action that never executes the command;
- takeover-history table;
- final-results table;
- selection persistence by `task_id` within a project;
- repository-owned PNG/SVG application icon resources;
- native Windows executable `.ico` resource;
- per-user single-instance desktop behavior;
- second-launch activation of the existing window;
- system-tray background presence;
- close-to-tray behavior with explicit **Quit**;
- optional `--background` launch mode;
- persistent close-to-tray and notification settings;
- user-controlled Windows launch-at-login with development-checkout protection;
- important project-health transition notifications;
- copyable desktop diagnostics and `--desktop-diagnostics`;
- CMake install layout plus updater-compatible ZIP, portable ZIP, and per-user NSIS installer packaging.

The Overview model is a read-only projection over normalized project snapshots and takeover records. It does **not** parse raw project logs or invent separate health semantics in the UI.

The Python/Tkinter app remains the production fallback until the remaining surfaces reach parity.

## UI rule

The Qt UI follows `docs/UI_GUIDELINES.md`:

- operational state, warnings and required decisions remain visible;
- explanatory copy belongs in hover tooltips;
- panel-level help uses a small info button;
- errors are never hidden inside a tooltip;
- raw evidence remains a drill-down surface rather than the primary Overview UX.

Desktop lifecycle behavior is documented separately in `docs/DESKTOP_LIFECYCLE.md`. Windows packaging and installer behavior is documented in `docs/WINDOWS_PACKAGING.md`.

## Build

Qt is optional and disabled by default so core CI stays lightweight.

With Qt 6.5+ Widgets + Network available through your normal Qt installation or package manager:

```powershell
cmake -S cpp -B cpp/build-qt `
  -DMONITOR_HUB_BUILD_QT=ON `
  -DCMAKE_PREFIX_PATH="C:\\Qt\\6.8.3\\msvc2022_64"
cmake --build cpp/build-qt --config Release --target monitor_hub_qt
```

Normal foreground launch:

```powershell
.\cpp\build-qt\Release\monitor_hub_qt.exe
```

Background-first launch:

```powershell
.\cpp\build-qt\Release\monitor_hub_qt.exe --background
```

The core-only build remains:

```powershell
cmake -S cpp -B cpp/build
cmake --build cpp/build --config Release
ctest --test-dir cpp/build -C Release --output-on-failure
```

The cross-project Overview aggregation and Claude CLI usage-snapshot parsing are covered by `monitor_hub_core_tests`, so those projections can be validated without Qt. The Python statusLine bridge has a separate sanitization test to ensure secret-looking input fields are not persisted.

## Not migrated yet

The following still stay in Python or in later desktop/release phases:

- pause / resume / run-now / interval management;
- read-only Claude Q&A;
- new-monitor setup-agent launching;
- Markdown and QoI-specific adapters;
- rich transcript rendering / follow mode;
- first-class Issue / AgentRun / Event timeline views driven by `AGENT_EVENT_PROTOCOL`;
- durable Attention/outbox semantics beyond current snapshot-derived attention;
- issue-level and agent-run desktop notifications beyond project-health transitions;
- updater restart integration and packaged release installation;
- application log-file / diagnostics viewer integration;
- crash-session recovery journals;
- persisted window geometry and last-open project.

These should be moved only after the read-only Qt shell and desktop lifecycle are visually and behaviorally checked against the Python fallback.
