# Releasing Monitor Hub

Monitor Hub stable releases are produced only from the canonical `main` branch.

## Authoritative version

The repository root `VERSION` is the only hand-edited release version source.

CMake reads it directly and propagates it to:

- `CMAKE_PROJECT_VERSION`;
- `monitor_hub_cli --version`;
- `monitor_hub_qt --version`;
- Qt application diagnostics;
- staged `VERSION`;
- `.monitor-hub-release.json`;
- updater `manifest.json`;
- NSIS installer filename/version;
- GitHub tag and Release title.

Do not hand-edit independent versions in CMake, NSIS, package scripts or release metadata.

## Preparing a release

For version `X.Y.Z`:

1. make sure all feature/stacked PRs are integrated into `main`;
2. create `docs/releases/vX.Y.Z.md`;
3. update root `VERSION` to `X.Y.Z`;
4. open a release PR;
5. wait for C++ Core, Qt/package and Stable Release candidate CI to pass;
6. review installer/download wording and release notes;
7. merge the release PR into `main`.

The stable workflow is triggered by the `VERSION` change on `main`.

## Publish gates

Before publishing, CI verifies:

- SemVer syntax;
- release notes exist;
- publishing ref is `main`;
- release commit is contained in `origin/main`;
- the version tag does not already exist;
- compiled CLI version equals `VERSION`;
- CMake project version equals `VERSION`;
- all CTest tests pass;
- package manifest/hash validation passes;
- staged and installed desktop diagnostics report the release version;
- silent install/uninstall succeeds.

Only after those checks pass does the workflow create `vX.Y.Z` and publish the Release.

## Download assets

Normal users should use:

`Monitor-Hub-X.Y.Z-win64-setup.exe`

Portable users can use:

`Monitor-Hub-X.Y.Z-win64-portable.zip`

The fixed-name:

`monitor-hub-windows-x64.zip`

is the stable updater payload and should not be presented as the primary manual download.

Every stable Release also publishes `SHA256SUMS.txt`.

## Development checkout protection

A Git working tree is not a release-managed install.

If a directory contains `.git`, use normal Git workflows for updates. Do not let the updater overwrite a development checkout.
