# Qt desktop lifecycle

This document defines the desktop-process behavior for the Qt Monitor Hub shell.

## Current scope

The first desktop-lifecycle layer is intentionally separate from monitoring/business logic. It adds:

- one interactive Qt instance per user session;
- second-launch activation of the existing window instead of duplicate monitors;
- system-tray presence while the app is monitoring in the background;
- close-to-tray behavior when the tray is available;
- an explicit **Quit** action that really terminates Monitor Hub;
- an optional `--background` launch mode;
- a repository-owned application icon used by the Qt window and tray.

The controller lives in `qt_desktop_controller.*`; the monitoring window does not own process-lifecycle policy.

## User-visible behavior

Normal launch:

```text
launch
→ acquire single-instance endpoint
→ create tray controller
→ show main window
```

Closing the main window with a working system tray:

```text
close window
→ hide window
→ monitoring continues
→ one-time tray notification explains that the app is still running
```

Explicit exit:

```text
tray menu
→ Quit
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

If the platform does not provide a system tray, `--background` degrades safely by showing the main window rather than creating an invisible process.

## Boundaries

This phase does **not** yet add:

- launch-at-login registration;
- persistent desktop settings;
- native Windows executable/installer `.ico` packaging;
- notification routing from normalized project/issue events;
- updater restart integration;
- crash-session recovery journals.

Those should be layered on top of this controller rather than implemented inside `QtMainWindow`.

## Acceptance checks

At minimum:

1. Qt target builds on Windows.
2. `monitor_hub_qt --help` exits successfully.
3. A normal launch displays one window and one tray icon when supported.
4. Closing the window hides it instead of stopping monitoring.
5. Tray **Open** restores the same window.
6. Tray **Quit** exits.
7. A second launch activates the first instance and exits.
8. `--background` starts hidden only when the tray is usable.
9. The window/taskbar/tray use the repository-owned application icon.

The packaged-build release path should later repeat these checks outside the developer build tree.
