# Windows packaging and installer

The Qt desktop line packages Monitor Hub from one staged install tree so the portable build, installer, and updater payload cannot silently drift apart.

## Goals

The packaging layer produces three artifacts from the same staged files:

1. `monitor-hub-windows-x64.zip` + `.sha256` — update asset compatible with the updater contract in PR #5;
2. `Monitor-Hub-<version>-win64-portable.zip` — portable/manual distribution;
3. `Monitor-Hub-<version>-win64-setup.exe` — per-user NSIS installer.

Packaging a PR does **not** publish a GitHub Release. Stable publishing remains gated by the repository Release rules: the tagged commit must already be integrated into the canonical release branch.

## Staged install layout

The CMake install tree is the source of truth:

```text
stage/
  .monitor-hub-release.json
  VERSION
  README.md
  docs/
    DESKTOP_LIFECYCLE.md
  bin/
    monitor_hub_qt.exe
    monitor_hub_cli.exe
    Qt6*.dll
    platforms/
      qwindows.dll
    ...
```

`windeployqt` deploys the Qt runtime into the staged `bin` directory.

The release marker uses the existing updater marker contract:

```json
{
  "format": 1,
  "version": "0.1.0",
  "install_kind": "desktop",
  "entrypoint": "bin/monitor_hub_qt.exe"
}
```

The extra fields are additive. Consumers that only require `format` and `version` remain compatible.

## Update ZIP contract

The update ZIP keeps the package shape already established by PR #5:

```text
manifest.json
payload/
  .monitor-hub-release.json
  VERSION
  bin/
  ...
```

`manifest.json` format 1 lists every payload file with:

- safe relative path;
- byte size;
- SHA-256.

The archive itself also gets `monitor-hub-windows-x64.zip.sha256`.

This lets the Python updater and a future native Qt updater share the same package format without sharing implementation language.

## Portable package

The portable archive contains:

```text
MonitorHub/
  .monitor-hub-release.json
  VERSION
  bin/
  ...
```

It is a release-managed layout, not a Git working tree.

Portable users can run:

```powershell
.\MonitorHub\bin\monitor_hub_qt.exe
```

## Installer

The NSIS installer is deliberately per-user:

```text
%LOCALAPPDATA%\Programs\Monitor Hub
```

It does not request administrator privileges.

It creates:

- a Start Menu folder;
- a Monitor Hub shortcut;
- an uninstall shortcut;
- an HKCU uninstall registration.

It does **not** silently enable launch-at-login. That remains a user-controlled application setting.

On uninstall, the installer removes the optional `Monitor Hub` Run entry if present, but deliberately preserves user/application settings and project/workspace data stored outside the install directory.

## CI package smoke test

The Windows Qt workflow should validate the package, not just the developer build tree:

1. build Qt + CLI;
2. stage CMake install;
3. run `windeployqt`;
4. generate update ZIP + checksum;
5. generate portable ZIP;
6. compile NSIS installer;
7. silently install into a temporary path;
8. run installed `monitor_hub_qt.exe --desktop-diagnostics`;
9. silently uninstall;
10. verify the installed executable was removed;
11. upload PR artifacts for inspection.

## Local packaging

After building `cpp/build-qt` with Qt available:

```powershell
powershell -ExecutionPolicy Bypass -File packaging/windows/package_qt.ps1 `
  -RepoRoot . `
  -BuildDir cpp/build-qt `
  -QtBin C:\Qt\6.8.3\msvc2022_64\bin `
  -OutDir dist `
  -BuildInstaller
```

NSIS must be installed to build the setup executable. Omitting `-BuildInstaller` still produces the updater-compatible and portable ZIPs.

## Integration with automatic update

PR #5 and this packaging line intentionally share **format contracts**, not implementation code.

After the branch stacks converge, the Release workflow should:

- build the final Qt/CLI binaries;
- call this staging/packaging path;
- publish the exact updater-compatible asset names;
- keep PR #5's tag/VERSION/main-ancestry gates;
- use the desktop entrypoint when the native Qt update/restart flow is ready.

Until then, feature/PR packaging is only CI validation and artifact inspection, never a stable release.
