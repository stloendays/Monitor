# Stable release process

Monitor Hub publishes stable Windows releases only from code already integrated into `main`.

## Authoritative version

The repository root `VERSION` is the only hand-edited application version.

The expected flow is:

```text
VERSION
  -> CMake project version
  -> compiled monitor_hub_cli --version
  -> staged VERSION
  -> .monitor-hub-release.json
  -> updater manifest
  -> installer/portable filenames
  -> Git tag v<version>
  -> GitHub Release
```

Do not hand-edit version strings in CMake, NSIS, release notes or package manifests independently.

## Release trigger

A stable release is triggered by merging a reviewed version bump to `main`.

The `Stable Release` workflow:

1. validates `VERSION` as SemVer;
2. requires `docs/releases/v<VERSION>.md`;
3. builds the complete C++/Qt install tree;
4. runs CTest;
5. verifies compiled `monitor_hub_cli --version`;
6. creates installer, portable archive and updater payload;
7. verifies updater manifest/file hashes;
8. performs a real silent installer/diagnostics/uninstall smoke test;
9. generates `SHA256SUMS.txt`;
10. creates a draft GitHub Release;
11. uploads release assets;
12. publishes it as the latest stable release only after all prior checks succeed.

## Release assets

Normal users should be directed to:

`Monitor-Hub-<version>-win64-setup.exe`

Additional assets:

- `Monitor-Hub-<version>-win64-portable.zip` — portable build;
- `monitor-hub-windows-x64.zip` — fixed-name verified updater package;
- `monitor-hub-windows-x64.zip.sha256` — updater archive checksum;
- `SHA256SUMS.txt` — published asset checksums.

The updater package is not the normal manual installation download.

## Development checkout protection

A directory containing `.git` is development state, not a release-managed install.

Release/update behavior must never:

- overwrite a Git worktree;
- run `git pull`, reset or checkout automatically;
- replace local developer files with release payloads.

Developers may be informed that a stable release exists, but updates to source checkouts remain explicit Git operations.

## Pre-release checklist

Before merging a version bump:

- all required feature PRs are integrated into `main`;
- main CI is green;
- public CLI/schema/event contracts are reviewed;
- Windows package/install smoke is green;
- release notes describe user-visible changes and known limitations;
- installer/download guidance matches actual asset names;
- no release artifact contains local project data, credentials or development-only files.

## Signing

Code signing is recommended when a trusted Windows signing identity becomes available.

Until then:

- release notes must not claim the installer is signed;
- users are directed to the official GitHub Release;
- `SHA256SUMS.txt` is published for integrity verification;
- update packages continue to verify hashes before apply.
