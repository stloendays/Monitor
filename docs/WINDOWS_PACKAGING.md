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

`windeployqt` deploys the Qt runtime into the staged `bin` directory. The packaging script then copies the official x64 Microsoft Visual C++ CRT DLLs from the active Visual Studio Redist tree into the same directory. This app-local CRT layout is required because `windeployqt --compiler-runtime` may emit only `vc_redist.x64.exe`; the portable ZIP and per-user installer must not depend on a separate system-wide redistributable installation.

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
4. stage app-local MSVC x64 CRT DLLs and smoke-test the staged executable;
5. generate update ZIP + checksum;
6. generate portable ZIP;
7. compile NSIS installer;
8. silently install into a temporary path;
9. run installed `monitor_hub_qt.exe --desktop-diagnostics`;
10. silently uninstall;
11. verify the installed executable was removed;
12. upload PR artifacts for inspection.

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


## MSVC runtime policy

Monitor Hub's Windows artifacts use an **app-local** Visual C++ runtime.

The packaging script searches, in order, from the active build environment and Visual Studio installation for the x64 `Microsoft.VC*.CRT` Redist directory, then copies its DLLs into `stage/bin`.

At minimum the staged layout must contain:

- `vcruntime140.dll`;
- `msvcp140.dll`.

The script copies the full matching CRT DLL set rather than guessing only the currently observed imports. This protects the Qt executable, CLI, and deployed Qt plugins from minor toolchain/runtime dependency differences.

The bundled `vc_redist.x64.exe` produced by some `windeployqt` versions is not treated as a substitute for app-local DLLs because:

- the portable ZIP does not run installers;
- the NSIS package is per-user and should not silently install a machine-wide prerequisite;
- the update payload must remain self-contained.

A missing CRT Redist directory remains a packaging failure rather than being silently ignored.
