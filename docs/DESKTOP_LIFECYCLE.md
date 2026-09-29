# Qt desktop lifecycle

This document defines the desktop-process behavior for the Qt Monitor Hub shell.

## Current scope

The desktop lifecycle layer is intentionally separate from monitoring/business logic. It now provides:

- one interactive Qt instance per user session;
- per-user single-instance endpoint isolation;
- second-launch activation of the existing window instead of duplicate monitors;
- system-tray presence while the app is monitoring in the background;
- persistent close-to-tray preference;
- an explicit **Quit** action that really terminates Monitor Hub;
- an optional `--background` launch mode;
- persistent important-notification preference;
- project-health transition notifications sourced from the normalized Qt snapshot;
- user-controlled Windows launch-at-login registration;
- development-checkout protection so a `.git` working tree is never registered for startup;
- a tray Settings dialog;
- copyable desktop diagnostics;
- repository-owned PNG/SVG icon resources for the Qt window and tray;
- a native Windows `.ico` resource embedded into the Qt executable;
- a per-user installer and portable/update package path derived from one CMake install tree.

The controller/settings code lives in `qt_desktop_controller.*` and
`qt_desktop_settings.*`; the monitoring window does not own process-lifecycle policy.

## User-visible behavior

Normal launch:

```text
launch
→ acquire per-user single-instance endpoint
→ load desktop settings
→ create tray controller
→ baseline current project health without notification spam
→ show main window
```

Closing the main window with **关闭主窗口时继续在后台运行** enabled:

```text
close window
→ hide window
→ monitoring continues
→ one-time tray notification explains that the app is still running
```

With that setting disabled, closing the main window performs an explicit application quit.

Explicit exit:

```text
tray menu
→ Quit
→ stop notification timer
→ stop Qt application lifecycle
```

A second launch:

```text
second monitor_hub_qt process
→ connect to the first process through QLocalSocket
→ request activation
→ first process restores/focuses the window
→ second process exits
```

Background launch:

```powershell
monitor_hub_qt.exe --background
```

If the platform does not provide a system tray, `--background` degrades safely by
showing the main window rather than creating an invisible process.

## Settings

The tray menu exposes **设置…** with:

- **关闭主窗口时继续在后台运行** — default on;
- **显示重要桌面通知** — default on;
- **登录 Windows 时自动启动 Monitor Hub** — default off.

Desktop preferences use `QSettings` under the current user.

Windows launch-at-login uses the current user's Run key and launches:

```text
"<installed monitor_hub_qt.exe>" --background
```

No administrator permission is required.

A build launched from a directory with a `.git` ancestor is treated as a development
checkout. Development checkouts may inspect settings but cannot newly register launch
at login. This prevents temporary `cpp/build-qt/.../monitor_hub_qt.exe` paths from
becoming persistent startup entries.

## Notifications

Automatic desktop notifications are state-transition based.

The first poll records a baseline and emits nothing. Later polling can notify when a
project changes to:

- `attention` — user/main-agent attention is required;
- `error` — monitor/control-plane error;
- `stale` — status is no longer fresh;
- `working` after a problem — bounded background handling has started;
- `ok` after a problem — state recovered;
- `done` — project completed.

Repeated identical health/summary state does not repeatedly notify.

The desktop layer reads only the normalized project snapshot. It does not parse raw
logs or invent a separate health policy.

## Diagnostics

The tray menu action **复制诊断信息** copies non-secret desktop runtime information
such as version, Qt version, executable path, settings backend, development-checkout
status and desktop preferences.

The same read-only diagnostic path is available for CI/support:

```powershell
monitor_hub_qt.exe --desktop-diagnostics
```

It does not modify startup registration or settings.

## Boundaries

This phase does **not** yet add:

- an application log file / log viewer;
- issue-level and agent-run-level event notifications;
- updater restart integration;
- crash-session recovery journals;
- persisted window geometry and last-open project;

Those should be layered on top of the current controller/settings boundary rather than
implemented inside `QtMainWindow`.

## Acceptance checks

At minimum:

1. Qt target builds on Windows.
2. `monitor_hub_qt --help` exits successfully.
3. `monitor_hub_qt --desktop-diagnostics` exits successfully without mutation.
4. A normal launch displays one window and one tray icon when supported.
5. Closing the window hides it when close-to-tray is enabled.
6. Disabling close-to-tray makes close exit the application.
7. Tray **Open** restores the same window.
8. Tray **Quit** exits.
9. A second launch activates the first instance and exits.
10. Different logged-in users do not suppress each other's Monitor Hub instance.
11. `--background` starts hidden only when the tray is usable.
12. Development builds cannot newly register launch-at-login.
13. Installed builds can opt into launch-at-login without administrator rights.
14. Notification polling establishes a silent baseline before reporting transitions.
15. The Qt window/tray and native Windows executable use repository-owned icon assets.

The Windows packaging workflow now repeats diagnostics from a staged install and a silently installed NSIS build. Stable GitHub Release publishing remains a separate gated integration step.
