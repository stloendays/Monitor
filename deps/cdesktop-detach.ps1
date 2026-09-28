<#
Start a long-running command that survives the end of a cdesktop turn, or check on one.

cdesktop kills the Windows job object of each turn's `claude -p` process when the turn
ends; Start-Process, nohup and `&` children are inside that job and die with it. A process
created through WMI (Win32_Process.Create) is not in the job, so it keeps running.

  Start : pwsh -File cdesktop-detach.ps1 -Name polish -Command "python run.py" [-WorkDir D:\x]
  Status: pwsh -File cdesktop-detach.ps1 -Status polish [-Tail 20]
  List  : pwsh -File cdesktop-detach.ps1 -List
  Stop  : pwsh -File cdesktop-detach.ps1 -Stop polish

Each job lives in %LOCALAPPDATA%\cdesktop-jobs\<name>\ with run.ps1, output.log, pid,
started and exitcode (written when the command finishes). The command runs under
pwsh with the current PATH and working directory.
#>
[CmdletBinding(DefaultParameterSetName = 'Start')]
param(
    [Parameter(ParameterSetName = 'Start', Mandatory)][string]$Name,
    [Parameter(ParameterSetName = 'Start', Mandatory)][string]$Command,
    [Parameter(ParameterSetName = 'Start')][string]$WorkDir = (Get-Location).Path,
    [Parameter(ParameterSetName = 'Status', Mandatory)][string]$Status,
    [Parameter(ParameterSetName = 'Status')][int]$Tail = 20,
    [Parameter(ParameterSetName = 'List', Mandatory)][switch]$List,
    [Parameter(ParameterSetName = 'Stop', Mandatory)][string]$Stop
)
$ErrorActionPreference = 'Stop'
$root = Join-Path $env:LOCALAPPDATA 'cdesktop-jobs'
$pwsh = (Get-Process -Id $PID).Path

function Get-JobState([string]$n) {
    $d = Join-Path $root $n
    if (-not (Test-Path $d)) { return $null }
    $jobPid = [int](Get-Content (Join-Path $d 'pid') -ErrorAction SilentlyContinue)
    $exit = Get-Content (Join-Path $d 'exitcode') -ErrorAction SilentlyContinue
    $alive = $jobPid -and (Get-CimInstance Win32_Process -Filter "ProcessId=$jobPid" -ErrorAction SilentlyContinue |
        Where-Object { $_.CommandLine -like "*$d*" })
    $state = if ($null -ne $exit) { "finished (exit $exit)" } elseif ($alive) { 'running' } else { 'dead (no exit code recorded)' }
    [pscustomobject]@{ Name = $n; State = $state; Pid = $jobPid
        Started = Get-Content (Join-Path $d 'started') -ErrorAction SilentlyContinue; Dir = $d }
}

switch ($PSCmdlet.ParameterSetName) {
    'Start' {
        if ($Name -notmatch '^[\w.-]+$') { throw "Name may only contain letters, digits, '_', '.', '-'." }
        $d = Join-Path $root $Name
        $prev = Get-JobState $Name
        if ($prev -and $prev.State -eq 'running') { throw "Job '$Name' is already running (pid $($prev.Pid))." }
        if (Test-Path $d) { Remove-Item $d -Recurse -Force }
        New-Item -ItemType Directory -Force $d | Out-Null
        $q = { param($s) "'" + ($s -replace "'", "''") + "'" }
        @"
`$env:PATH = $(& $q $env:PATH)
Set-Location -LiteralPath $(& $q $WorkDir)
`$global:LASTEXITCODE = 0
try {
    & { $Command } *>> $(& $q (Join-Path $d 'output.log'))
    `$code = if (`$?) { [int]`$global:LASTEXITCODE } else { if (`$global:LASTEXITCODE) { [int]`$global:LASTEXITCODE } else { 1 } }
} catch {
    `$_ | Out-String | Add-Content -LiteralPath $(& $q (Join-Path $d 'output.log'))
    `$code = 1
}
Set-Content -LiteralPath $(& $q (Join-Path $d 'exitcode')) -Value `$code
"@ | Set-Content -LiteralPath (Join-Path $d 'run.ps1') -Encoding utf8
        $startup = New-CimInstance -ClassName Win32_ProcessStartup -ClientOnly -Property @{ ShowWindow = [uint16]0 }
        $r = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{
            CommandLine = "`"$pwsh`" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File `"$(Join-Path $d 'run.ps1')`""
            CurrentDirectory = $WorkDir
            ProcessStartupInformation = $startup
        }
        if ($r.ReturnValue -ne 0) { throw "Win32_Process.Create failed with code $($r.ReturnValue)." }
        Set-Content (Join-Path $d 'pid') $r.ProcessId
        Set-Content (Join-Path $d 'started') (Get-Date -Format 'yyyy-MM-dd HH:mm:ss')
        "started '$Name' pid=$($r.ProcessId) log=$(Join-Path $d 'output.log')"
    }
    'Status' {
        $s = Get-JobState $Status
        if (-not $s) { throw "No job named '$Status' in $root." }
        $s | Format-List | Out-String -Width 300
        $log = Join-Path $s.Dir 'output.log'
        if (Test-Path $log) { "--- last $Tail lines of output.log ---"; Get-Content $log -Tail $Tail }
    }
    'List' {
        if (Test-Path $root) { Get-ChildItem $root -Directory | ForEach-Object { Get-JobState $_.Name } | Format-Table -AutoSize | Out-String -Width 300 }
    }
    'Stop' {
        $s = Get-JobState $Stop
        if (-not $s) { throw "No job named '$Stop' in $root." }
        if ($s.State -ne 'running') { "'$Stop' is not running: $($s.State)"; break }
        & taskkill.exe /PID $s.Pid /T /F | Out-Null
        Set-Content (Join-Path $s.Dir 'exitcode') 'stopped'
        "stopped '$Stop' (pid $($s.Pid) and its children)"
    }
}
