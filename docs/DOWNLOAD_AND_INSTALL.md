# Download and install Monitor Hub

## Recommended download

For most Windows users, download:

**`Monitor-Hub-<version>-win64-setup.exe`**

This is the recommended per-user installer for Windows 10/11 x64.

- no administrator permission is requested;
- installs under `%LOCALAPPDATA%\Programs\Monitor Hub`;
- creates a Start Menu shortcut;
- does not silently enable launch-at-login;
- uninstall removes managed application files but does not delete external project/workspace data.

## Other release assets

| Asset | Intended use |
|---|---|
| `Monitor-Hub-<version>-win64-setup.exe` | **Recommended** normal Windows installation |
| `Monitor-Hub-<version>-win64-portable.zip` | Portable/manual use without installer |
| `monitor-hub-windows-x64.zip` | **Updater payload** for Monitor Hub's managed update flow; not the normal manual download |
| `monitor-hub-windows-x64.zip.sha256` | SHA-256 for the updater payload |
| `SHA256SUMS.txt` | SHA-256 checksums for published release assets |

## Before you run the installer

Download only from the official GitHub Releases page for this repository.

If Windows SmartScreen or another reputation warning appears, verify:

1. the file came from the official `stloendays/Monitor` GitHub Release;
2. the version matches the release page;
3. the SHA-256 value matches `SHA256SUMS.txt`.

Do not bypass a warning for a file downloaded from an unknown mirror or forwarded by someone else.

## What the application does

Monitor Hub is a local monitoring and agent-control console.

It can:

- show local/HPC project progress;
- surface issues and recovery activity;
- integrate with local Claude Code CLI metadata and usage;
- dispatch policy-bounded deterministic or child-Agent recovery work;
- keep durable completion/escalation notifications;
- continue monitoring in the background when desktop settings enable that behavior.

It does **not** silently enable startup at login during installation.

## First launch

After installation:

1. launch **Monitor Hub** from the installer finish page or Start Menu;
2. review the Overview and Settings pages;
3. connect/verify your local Monitor registry and Claude CLI integration if needed;
4. use **Copy diagnostics** when troubleshooting instead of manually collecting raw logs.

Closing the main window may hide the application to the system tray when close-to-tray is enabled. Use **Quit** from Monitor Hub when you want to terminate the desktop application completely.

## Portable build

Extract:

`Monitor-Hub-<version>-win64-portable.zip`

and run:

`MonitorHub\bin\monitor_hub_qt.exe`

Keep the full extracted directory together because Qt, MSVC runtime, resources and helper executables are shipped app-local.

## Updates

Normal users should follow the stable GitHub Release channel.

A development checkout containing `.git` is not a self-update target. Development branches should never be overwritten by a packaged release.

The fixed-name `monitor-hub-windows-x64.zip` asset is intended for verified updater workflows. It contains a manifest with file sizes and SHA-256 hashes and should not be treated as the normal portable package.
