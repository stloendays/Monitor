param(
    [Parameter(Mandatory = $true)][string]$Archive,
    [Parameter(Mandatory = $true)][string]$Destination
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

Add-Type -AssemblyName System.IO.Compression.FileSystem

$archivePath = [System.IO.Path]::GetFullPath($Archive)
$destinationPath = [System.IO.Path]::GetFullPath($Destination)
$destinationRoot = $destinationPath.TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar

if (Test-Path -LiteralPath $destinationPath) {
    Remove-Item -LiteralPath $destinationPath -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $destinationPath | Out-Null

$zip = [System.IO.Compression.ZipFile]::OpenRead($archivePath)
try {
    if ($zip.Entries.Count -gt 5000) {
        throw "update archive contains too many entries"
    }

    [int64]$totalUncompressed = 0
    foreach ($entry in $zip.Entries) {
        if ($entry.Length -gt 512MB) {
            throw "update archive entry is too large: $($entry.FullName)"
        }
        $totalUncompressed += $entry.Length
        if ($totalUncompressed -gt 1GB) {
            throw "update archive expands beyond the 1 GiB safety limit"
        }

        $name = $entry.FullName.Replace('/', [System.IO.Path]::DirectorySeparatorChar)
        if ([string]::IsNullOrWhiteSpace($name)) { continue }

        $target = [System.IO.Path]::GetFullPath((Join-Path $destinationPath $name))
        if (-not $target.StartsWith(
                $destinationRoot,
                [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "archive entry escapes staging directory: $($entry.FullName)"
        }

        if ($entry.FullName.EndsWith('/')) {
            New-Item -ItemType Directory -Force -Path $target | Out-Null
            continue
        }

        $parent = Split-Path -Parent $target
        New-Item -ItemType Directory -Force -Path $parent | Out-Null
        [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $target, $true)
    }
}
finally {
    $zip.Dispose()
}
