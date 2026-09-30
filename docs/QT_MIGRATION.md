# Qt 6 migration status

The Qt UI is intentionally isolated from the validated C++ core so desktop work cannot destabilize monitoring logic.

## Current Qt scope

The optional `monitor_hub_qt` target currently provides a read-only operational shell backed by the same C++ core as `monitor_hub_cli`:

- live refresh from Windows Task Scheduler + WMI;
- project sidebar with health state;
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
- optional `--background` launch mode.

The Python/Tkinter app remains the production fallback until the remaining surfaces reach parity.

## UI rule

The Qt UI follows `docs/UI_GUIDELINES.md`:

- operational state, warnings and required decisions remain visible;
- explanatory copy belongs in hover tooltips;
- panel-level help uses a small info button;
- errors are never hidden inside a tooltip.

Desktop lifecycle behavior is documented separately in `docs/DESKTOP_LIFECYCLE.md`.

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

## Not migrated yet

The following still stay in Python or in later desktop/release phases:

- pause / resume / run-now / interval management;
- read-only Claude Q&A;
- new-monitor setup-agent launching;
- Markdown and QoI-specific adapters;
- rich transcript rendering / follow mode;
- launch-at-login and persistent desktop settings;
- installer/shortcut icon packaging;
- notification routing from normalized project/issue events;
- updater restart integration and packaged release installation;
- crash-session recovery journals.

These should be moved only after the read-only Qt shell and desktop lifecycle are visually and behaviorally checked against the Python fallback.
