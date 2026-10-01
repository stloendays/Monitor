# Desktop resilience foundation

This layer completes the basic operational behavior expected from the Windows Qt desktop shell without changing Monitor Hub's Project/Task/Issue contracts or recovery authority.

## Application logging

The Qt desktop installs one process-wide Qt message handler after the application identity and version are configured.

Logs are stored under the current user's local application-data root:

```text
<Qt AppLocalDataLocation>/
  logs/
    monitor-hub.log
    monitor-hub.log.1
    monitor-hub.log.2
    monitor-hub.log.3
```

The active log rotates at 4 MiB. The tray action **打开日志目录** and `Ctrl+Shift+L` open the directory. Desktop diagnostics include the exact current log path.

Logs are support evidence. They do not become a second source of project health or recovery truth.

## Persistent UI/session state

`QSettings` stores:

- main-window geometry/state;
- last selected stable `project_id`;
- last selected main tab;
- a `clean_shutdown` session marker.

The UI state is checkpointed every 15 seconds and again while the application exits. A crash leaves the session marker dirty. On the next successful launch Monitor Hub reloads live project state and reports that the previous desktop session ended unexpectedly; it does not replay UI actions or recovery commands from the previous process.

## Notifications

Two notification sources coexist deliberately:

1. normalized project health transitions, for concise desktop state changes;
2. the durable notification outbox, for decision/completion facts that must survive process restarts.

Pending `main_agent` outbox records are surfaced once per desktop process. Repeated polling does not show the same notification again in that process. The durable outbox remains authoritative; a Windows toast is only a presentation/delivery surface and does not acknowledge the record.

## Network failure handling

Qt network reachability is treated as an advisory signal, not proof that a specific remote service is usable.

When Qt explicitly reports `Disconnected`:

- the desktop exposes an offline state and records it in the application log;
- local status reading and authorized deterministic control continue;
- failed control ticks are not aggressively retried while the network is known disconnected.

On explicit recovery:

- the retry counter is reset;
- project state is refreshed;
- one control tick is scheduled immediately.

Ordinary control failures also use bounded retry delays of 5, 15, 30, 60 and 120 seconds. The normal 60-second control timer remains in place and overlapping control processes are still prohibited.

## About and keyboard access

The desktop exposes an About dialog from the tray and `F1`. It shows the application version, Qt version, product role and shortcut reference.

Shortcuts:

| Shortcut | Action |
|---|---|
| `F5` | Refresh current Monitor state |
| `Ctrl+N` | Create a new monitor request |
| `Ctrl+K` | Focus the project list |
| `Alt+1` … `Alt+6` | Switch main tabs |
| `Ctrl+,` | Open Settings |
| `Ctrl+Shift+L` | Open application log directory |
| `F1` | Open About |
| `Ctrl+Q` | Explicitly quit Monitor Hub |

## Safety boundaries

This feature does not:

- change Agent/Event Protocol v1;
- change health priority or adapter semantics;
- acknowledge durable notifications merely because a toast was shown;
- infer a project failure solely from network reachability;
- repeat a recovery command simply because the previous desktop process crashed;
- alter L1/L2/L3 authority.

## Validation

Windows Qt CI builds the desktop, runs the QSettings state/session round-trip test, runs CLI diagnostics smoke tests, and then exercises the existing installer/portable packaging path.
