# Native Qt automatic updates

This document defines the native desktop update path layered on the Windows packaging contract.

## Design goals

The desktop updater must:

- check only the repository's stable GitHub Release channel;
- use semantic-version comparison;
- never overwrite a Git development checkout;
- reuse the format-1 `manifest.json + payload/` package contract;
- verify the archive SHA-256 before extraction;
- reject archive path traversal and excessive expansion;
- verify every manifest file size and SHA-256;
- require the payload release marker version to match the manifest;
- require the Release tag version to match the manifest;
- apply only after the running Monitor Hub process exits;
- run the apply helper outside the live install tree so Windows file locks cannot block replacement;
- back up every replaced managed file;
- roll back files already replaced if apply fails;
- restart the desktop application in background mode after success.

## User experience

The tray menu contains an update action.

Normal state:

```text
检查更新…
```

When a stable update is discovered in a managed Release install:

```text
安装更新 vX.Y.Z…
```

Automatic checks run after startup and approximately every six hours when
**自动检查稳定更新** is enabled. An automatic check never opens a modal dialog.
It updates the tray action and may show one informational notification.

A manual check always gives explicit feedback.

## Development checkout rule

If the application has a `.git` ancestor:

```text
check stable Release
→ newer version may be reported
→ update action opens the Release page
→ download/apply is disabled
→ no checkout / pull / reset / file replacement
```

This preserves concurrent PRs, local edits, and other agent work.

## Managed Release rule

A release install is identified by an ancestor containing:

```text
.monitor-hub-release.json
```

The marker must use format 1. Packaging currently adds:

```json
{
  "format": 1,
  "version": "0.1.0",
  "install_kind": "desktop",
  "entrypoint": "bin/monitor_hub_qt.exe"
}
```

## Check and stage flow

```text
GET /repos/stloendays/Monitor/releases/latest
→ reject draft/prerelease
→ parse stable vMAJOR.MINOR.PATCH
→ compare with QApplication version
→ find:
     monitor-hub-windows-x64.zip
     monitor-hub-windows-x64.zip.sha256
→ HTTPS GitHub-host allowlist
→ download checksum
→ download archive
→ SHA-256 archive verification
→ bounded path-safe extraction
→ monitor_hub_updater.exe --verify-stage
→ verify manifest file sizes + hashes
→ verify payload release marker
→ verify manifest version == Release version
→ atomically retain the verified stage under user application data
```

The archive extractor rejects:

- entries escaping the staging directory;
- more than 5000 entries;
- a single entry above 512 MiB;
- total uncompressed data above 1 GiB.

## Detached apply helper

The live install contains:

```text
bin/monitor_hub_updater.exe
```

The GUI must not run that file in place during apply. Doing so would lock the updater
itself, and loading `Qt6Core.dll` from the install directory would also lock a managed
DLL.

Before apply, the GUI creates a unique helper run directory under user application
data and copies:

- `monitor_hub_updater.exe`;
- `Qt6Core.dll`;
- applicable app-local MSVC runtime DLLs.

The detached helper therefore loads no managed binary from the live installation.

Apply flow:

```text
GUI launches detached helper
→ GUI exits
→ helper waits for GUI PID
→ helper re-verifies install mode
→ helper re-verifies staged manifest/payload
→ helper creates versioned backup
→ helper replaces each managed file atomically
→ on failure:
     restore every file already replaced
→ on success:
     launch monitor_hub_qt.exe --background
→ exit helper
```

## Backup and rollback

Backups live outside the install root under the user's Monitor Hub update state.

A replacement is recorded only after the target write succeeds. If a later file fails,
the helper walks applied files in reverse order:

- previously existing files are restored from backup;
- newly introduced files are removed.

User settings, project/workspace files, logs, and other data outside the managed payload
are not part of update replacement.

## Settings

Desktop settings include:

- close to tray;
- important notifications;
- automatic stable-update checks;
- launch at login.

Automatic stable-update checks default on but are user-controlled. Manual checks remain
available even when automatic checks are disabled.

## CI requirements

The Windows packaging/update CI must cover:

1. build `monitor_hub_qt`, `monitor_hub_cli`, and `monitor_hub_updater`;
2. package the staged Release tree;
3. validate archive SHA-256;
4. validate every format-1 manifest entry;
5. run `monitor_hub_updater --verify-stage`;
6. create a temporary managed install;
7. intentionally corrupt a managed file;
8. apply the staged update through the helper;
9. verify the managed file is repaired;
10. verify a backup was written;
11. add a `.git` marker to another fake install and confirm apply is refused;
12. install the NSIS package and run installed diagnostics;
13. uninstall and verify managed binaries are removed.

## Release convergence

PR #5 established stable Release gates and the original package/update contract.
The native Qt updater intentionally reuses those contracts rather than copying the
Python GUI update implementation.

When the stacked branches converge, the stable Release workflow should retain:

- one authoritative VERSION source;
- tag == VERSION;
- tagged commit already contained in the canonical release branch;
- stable assets with deterministic names;
- updater ZIP + checksum;
- portable ZIP;
- installer;
- no stable publishing from feature branches.

The Qt update path then becomes the installed desktop consumer of those assets.
