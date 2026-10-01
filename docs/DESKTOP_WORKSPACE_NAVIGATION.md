# Desktop workspace navigation

This layer adds desktop interaction and discovery features on top of the resilience
foundation without changing Monitor Hub's normalized state or Agent/Event contracts.

## Drag and drop

The Qt main window accepts local file/folder drops.

Behavior is deliberately conservative:

- dropping a directory offers **新建监控任务** (prefilled project directory) or
  **打开目录**;
- dropping `monitor_hub_projects.json` asks for confirmation, creates a desktop
  configuration backup, then switches the current registry path;
- common text/log/document/image files may be opened with the system default app;
- other dropped local paths are recorded in Recent Files but are not automatically
  executed/opened;
- multiple dropped paths are added to Recent Files without launching all of them.

No dropped file can expand Monitor Hub recovery authority.

## Recent Files

Monitor Hub remembers the 15 most recently opened local paths using `QSettings`.

Recent paths are populated by:

- task/log/result/registry/debug-path opens from the Qt UI;
- Claude workspace/transcript opens;
- event evidence opens;
- drag-and-drop.

The quick-action **Recent Files** menu opens existing paths and can clear the history.
Missing paths remain visible as disabled entries until the list is cleared or they are
naturally displaced.

Recent Files are included in configuration backups and global search.

## Global search

The sidebar exposes a visible search field; `Ctrl+F` focuses it.

Typing filters the left project list. Pressing Enter opens the full search surface.

The global search index is built from already-loaded normalized data:

- projects;
- task rows and `row_meta`;
- projected Issues;
- Protocol v1 events;
- Recent Files.

It does not parse raw logs or infer health independently from the core model.

Search results deep-link to the matching project/task/Issue/event where possible.
Recent Files open through the same safe local-path route used elsewhere in the UI.

## Configuration recovery

Desktop configuration can be snapshotted and restored from Settings.

A snapshot contains only Monitor Hub-managed, non-secret desktop configuration:

- close-to-tray;
- desktop notifications;
- automatic control preference;
- registry path;
- Hub-data path;
- launch-at-login state;
- Recent Files.

Snapshots use schema version 1, are written atomically as JSON under the user's local
application-data directory, and are capped at five files.

Monitor Hub automatically creates a snapshot before a real settings change. Users can
also create one manually.

Before restoring an older snapshot Monitor Hub tries to create an undo snapshot of the
current configuration. Project/task data, event JSONL, command queues, recovery state
and scientific/business outputs are never rolled back by desktop configuration restore.

Registry/Hub-data path changes fully take effect on the next application launch.

## Event timeline

The existing project Protocol v1 timeline is promoted into a more navigable surface.

It now supports:

- free-text filtering over event type, display name, task, Issue, source, authority,
  summary and evidence references;
- event-family filters for Issue, Agent, Task, Notification, Project and Monitor events;
- **仅当前任务** filtering;
- a dedicated Issue column;
- event ID/correlation/evidence detail in tooltips;
- double-click to open local evidence when available, otherwise drill down to the task;
- global-search deep links that clear conflicting filters and focus the matching event.

Filtering is presentation-only. It never changes the event projection or Issue lifecycle.

## Keyboard interaction

Existing shortcuts remain unchanged, with `Ctrl+F` added for workspace search.

## Compatibility

This feature does not change:

- Protocol v1 schemas or event meaning;
- stable project/task/issue/event IDs;
- health priority;
- adapter parsing;
- L1/L2/L3 authority;
- durable notification acknowledgement semantics;
- updater/package layout.

The feature is intended to remain safe when stacked on the desktop resilience PR.
