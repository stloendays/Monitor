param(
    [Parameter(Mandatory = $true)][string]$StageDir,
    [Parameter(Mandatory = $true)][string]$UpdateZip,
    [Parameter(Mandatory = $true)][string]$TempRoot
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Copy-Tree([string]$Source, [string]$Destination) {
    if (Test-Path -LiteralPath $Destination) {
        Remove-Item -LiteralPath $Destination -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $Destination | Out-Null
    Get-ChildItem -LiteralPath $Source -Force | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination $Destination -Recurse -Force
    }
}

function Invoke-Updater([string[]]$Arguments, [int]$ExpectedExit) {
    $updater = Join-Path $StageDir "bin\monitor_hub_updater.exe"
    & $updater @Arguments
    $actual = $LASTEXITCODE
    if ($actual -ne $ExpectedExit) {
        throw "updater exit code $actual, expected $ExpectedExit; args=$($Arguments -join ' ')"
    }
}

if (Test-Path -LiteralPath $TempRoot) {
    Remove-Item -LiteralPath $TempRoot -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $TempRoot | Out-Null

$extractor = Join-Path $StageDir "tools\safe_extract_update.ps1"
$verifiedStage = Join-Path $TempRoot "verified-stage"
powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $extractor -Archive $UpdateZip -Destination $verifiedStage
if ($LASTEXITCODE -ne 0) {
    throw "safe update extraction failed"
}

Invoke-Updater @("--verify-stage", $verifiedStage) 0

$expectedVersion = (Get-Content (Join-Path $StageDir "VERSION") -Raw).Trim()

# Normal apply: corrupt VERSION, apply, verify repair and backup.
$install = Join-Path $TempRoot "install-success"
$backups = Join-Path $TempRoot "backups-success"
Copy-Tree $StageDir $install
Set-Content -LiteralPath (Join-Path $install "VERSION") -Value "BROKEN-BEFORE-UPDATE" -Encoding ascii

Invoke-Updater @(
    "--pid", "0",
    "--stage", $verifiedStage,
    "--install-root", $install,
    "--backup-root", $backups,
    "--restart-exe", $env:ComSpec,
    "--restart-arg", "/c",
    "--restart-arg", "exit"
) 0

$repaired = (Get-Content (Join-Path $install "VERSION") -Raw).Trim()
if ($repaired -ne $expectedVersion) {
    throw "successful apply did not repair VERSION: $repaired"
}
$backedUpVersions = Get-ChildItem -LiteralPath $backups -Recurse -Filter VERSION -File
if (-not $backedUpVersions) {
    throw "successful apply did not create a VERSION backup"
}
if (-not ($backedUpVersions | Where-Object {
    (Get-Content $_.FullName -Raw).Trim() -eq "BROKEN-BEFORE-UPDATE"
})) {
    throw "VERSION backup did not preserve pre-update content"
}

# Development protection: a .git marker must make apply fail without mutation.
$devInstall = Join-Path $TempRoot "install-development"
$devBackups = Join-Path $TempRoot "backups-development"
Copy-Tree $StageDir $devInstall
New-Item -ItemType Directory -Force -Path (Join-Path $devInstall ".git") | Out-Null
Set-Content -LiteralPath (Join-Path $devInstall "VERSION") -Value "DEVELOPMENT-MUST-STAY" -Encoding ascii

Invoke-Updater @(
    "--pid", "0",
    "--stage", $verifiedStage,
    "--install-root", $devInstall,
    "--backup-root", $devBackups,
    "--restart-exe", $env:ComSpec,
    "--restart-arg", "/c",
    "--restart-arg", "exit"
) 1

$devVersion = (Get-Content (Join-Path $devInstall "VERSION") -Raw).Trim()
if ($devVersion -ne "DEVELOPMENT-MUST-STAY") {
    throw "development protection allowed a managed file mutation"
}

# Rollback injection: turn a managed DLL path into a directory so apply fails
# after earlier manifest entries have already been replaced.
$rollbackInstall = Join-Path $TempRoot "install-rollback"
$rollbackBackups = Join-Path $TempRoot "backups-rollback"
Copy-Tree $StageDir $rollbackInstall
Set-Content -LiteralPath (Join-Path $rollbackInstall "VERSION") -Value "ROLLBACK-MUST-RESTORE" -Encoding ascii

$blockPath = Join-Path $rollbackInstall "bin\Qt6Core.dll"
if (-not (Test-Path -LiteralPath $blockPath -PathType Leaf)) {
    throw "Qt6Core.dll missing; rollback injection target unavailable"
}
Remove-Item -LiteralPath $blockPath -Force
New-Item -ItemType Directory -Force -Path $blockPath | Out-Null

Invoke-Updater @(
    "--pid", "0",
    "--stage", $verifiedStage,
    "--install-root", $rollbackInstall,
    "--backup-root", $rollbackBackups,
    "--restart-exe", $env:ComSpec,
    "--restart-arg", "/c",
    "--restart-arg", "exit"
) 1

$rolledBack = (Get-Content (Join-Path $rollbackInstall "VERSION") -Raw).Trim()
if ($rolledBack -ne "ROLLBACK-MUST-RESTORE") {
    throw "rollback did not restore VERSION after injected apply failure"
}

Write-Host "native updater smoke tests passed"
