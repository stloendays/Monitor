param(
    [string]$RepoRoot = ".",
    [string]$BuildDir = "cpp/build-qt",
    [string]$Configuration = "Release",
    [string]$QtBin = "",
    [string]$OutDir = "dist",
    [switch]$BuildInstaller
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Resolve-FullPath([string]$Base, [string]$Path) {
    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }
    return [System.IO.Path]::GetFullPath((Join-Path $Base $Path))
}

function Get-Sha256([string]$Path) {
    return (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLowerInvariant()
}

function Copy-Tree([string]$Source, [string]$Destination) {
    New-Item -ItemType Directory -Force -Path $Destination | Out-Null
    Get-ChildItem -LiteralPath $Source -Force | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination $Destination -Recurse -Force
    }
}

function Find-MakeNsis {
    $cmd = Get-Command makensis.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }

    foreach ($candidate in @(
        "$env:ProgramFiles(x86)\NSIS\makensis.exe",
        "$env:ProgramFiles\NSIS\makensis.exe",
        "C:\ProgramData\chocolatey\bin\makensis.exe"
    )) {
        if ($candidate -and (Test-Path -LiteralPath $candidate)) {
            return $candidate
        }
    }
    throw "makensis.exe not found. Install NSIS or omit -BuildInstaller."
}

$repo = (Resolve-Path -LiteralPath $RepoRoot).Path
$build = Resolve-FullPath $repo $BuildDir
$out = Resolve-FullPath $repo $OutDir
$stage = Join-Path $out "stage"

if (-not (Test-Path -LiteralPath $build)) {
    throw "Build directory not found: $build"
}

if (-not $QtBin) {
    $qtExecutable = Get-Command windeployqt.exe -ErrorAction SilentlyContinue
    if (-not $qtExecutable) {
        throw "windeployqt.exe not found. Pass -QtBin or add Qt bin to PATH."
    }
    $QtBin = Split-Path -Parent $qtExecutable.Source
}
$QtBin = [System.IO.Path]::GetFullPath($QtBin)
$windeployqt = Join-Path $QtBin "windeployqt.exe"
if (-not (Test-Path -LiteralPath $windeployqt)) {
    throw "windeployqt.exe not found: $windeployqt"
}

if (Test-Path -LiteralPath $out) {
    Remove-Item -LiteralPath $out -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $out | Out-Null
New-Item -ItemType Directory -Force -Path $stage | Out-Null

Write-Host "Installing CMake targets into staging..."
& cmake --install $build --config $Configuration --prefix $stage
if ($LASTEXITCODE -ne 0) {
    throw "cmake --install failed with exit code $LASTEXITCODE"
}

$qtExe = Join-Path $stage "bin\monitor_hub_qt.exe"
$cliExe = Join-Path $stage "bin\monitor_hub_cli.exe"
if (-not (Test-Path -LiteralPath $qtExe)) {
    throw "Qt executable missing after install: $qtExe"
}
if (-not (Test-Path -LiteralPath $cliExe)) {
    throw "CLI executable missing after install: $cliExe"
}

Write-Host "Deploying Qt runtime..."
& $windeployqt --release --compiler-runtime --no-translations $qtExe
if ($LASTEXITCODE -ne 0) {
    throw "windeployqt failed with exit code $LASTEXITCODE"
}

& $qtExe --desktop-diagnostics
if ($LASTEXITCODE -ne 0) {
    throw "staged monitor_hub_qt.exe --desktop-diagnostics failed"
}

$cache = Join-Path $build "CMakeCache.txt"
$versionMatch = Select-String -LiteralPath $cache -Pattern '^CMAKE_PROJECT_VERSION:STATIC=(.+)$' | Select-Object -First 1
if (-not $versionMatch) {
    throw "CMake cache did not report CMAKE_PROJECT_VERSION"
}
$version = $versionMatch.Matches[0].Groups[1].Value.Trim()
if ($version -notmatch '^(0|[1-9][0-9]*)[.](0|[1-9][0-9]*)[.](0|[1-9][0-9]*)$') {
    throw "invalid staged desktop version: $version"
}
Set-Content -LiteralPath (Join-Path $stage "VERSION") -Value $version -Encoding ascii

$releaseMarker = [ordered]@{
    format = 1
    version = $version
    install_kind = "desktop"
    entrypoint = "bin/monitor_hub_qt.exe"
}
$releaseMarker | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $stage ".monitor-hub-release.json") -Encoding utf8

$iconSource = Join-Path $repo "cpp\resources\icons\monitor_hub.ico"
if (Test-Path -LiteralPath $iconSource) {
    $iconDestination = Join-Path $stage "resources\monitor_hub.ico"
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $iconDestination) | Out-Null
    Copy-Item -LiteralPath $iconSource -Destination $iconDestination -Force
}

foreach ($optional in @("README.md", "docs\DESKTOP_LIFECYCLE.md")) {
    $source = Join-Path $repo $optional
    if (Test-Path -LiteralPath $source) {
        $destination = Join-Path $stage $optional
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination) | Out-Null
        Copy-Item -LiteralPath $source -Destination $destination -Force
    }
}

$packageRoot = Join-Path $out "_update_package"
$payload = Join-Path $packageRoot "payload"
New-Item -ItemType Directory -Force -Path $payload | Out-Null
Copy-Tree $stage $payload

$manifestFiles = @()
Get-ChildItem -LiteralPath $payload -Recurse -Force -File | Sort-Object FullName | ForEach-Object {
    $relative = $_.FullName.Substring($payload.Length)
    $relative = $relative.TrimStart([char[]]@(
        [System.IO.Path]::DirectorySeparatorChar,
        [System.IO.Path]::AltDirectorySeparatorChar
    )).Replace("\\", "/")
    $manifestFiles += [ordered]@{
        path = $relative
        size = $_.Length
        sha256 = Get-Sha256 $_.FullName
    }
}

$manifest = [ordered]@{
    format = 1
    version = $version
    files = $manifestFiles
}
$manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $packageRoot "manifest.json") -Encoding utf8

$updateZip = Join-Path $out "monitor-hub-windows-x64.zip"
Compress-Archive -Path (Join-Path $packageRoot "*") -DestinationPath $updateZip -CompressionLevel Optimal -Force
$updateDigest = Get-Sha256 $updateZip
Set-Content -LiteralPath ($updateZip + ".sha256") -Value "$updateDigest  monitor-hub-windows-x64.zip" -Encoding ascii

$portableRoot = Join-Path $out "_portable"
$portableApp = Join-Path $portableRoot "MonitorHub"
New-Item -ItemType Directory -Force -Path $portableApp | Out-Null
Copy-Tree $stage $portableApp
$portableZip = Join-Path $out "Monitor-Hub-$version-win64-portable.zip"
Compress-Archive -Path (Join-Path $portableRoot "*") -DestinationPath $portableZip -CompressionLevel Optimal -Force

$installer = $null
if ($BuildInstaller) {
    $makensis = Find-MakeNsis
    $installer = Join-Path $out "Monitor-Hub-$version-win64-setup.exe"
    $script = Join-Path $repo "packaging\windows\MonitorHub.nsi"

    Write-Host "Building NSIS installer..."
    $makeArgs = @(
        "/DVERSION=$version",
        "/DSTAGE_DIR=$stage",
        "/DOUT_FILE=$installer",
        $script
    )
    & $makensis @makeArgs
    if ($LASTEXITCODE -ne 0) {
        throw "makensis failed with exit code $LASTEXITCODE"
    }
    if (-not (Test-Path -LiteralPath $installer)) {
        throw "installer was not produced: $installer"
    }
}

Remove-Item -LiteralPath $packageRoot -Recurse -Force
Remove-Item -LiteralPath $portableRoot -Recurse -Force

$result = [ordered]@{
    version = $version
    stage = $stage
    update_zip = $updateZip
    update_sha256 = $updateZip + ".sha256"
    portable_zip = $portableZip
    installer = $installer
}
$result | ConvertTo-Json -Depth 4
