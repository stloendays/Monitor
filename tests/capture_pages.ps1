# Screenshots each page announced by a tour script (marker file %TEMP%\hub_step.txt) and counts new visible console
# windows while the tour runs (the hub must never flash one). PrintWindow captures the window even when covered.
#   pwsh -File tests/capture_pages.ps1 [-OutDir <dir>] [-Prefix <name prefix>]
param([string]$OutDir = $env:TEMP, [string]$Prefix = '', [int]$Minutes = 4)
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices; using System.Text;
public class HubCap {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string c, string t);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint f);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, IntPtr l);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  public static string Consoles() { var sb2 = new StringBuilder(); EnumWindows((h, l) => { if (IsWindowVisible(h)) { var sb = new StringBuilder(256); GetClassName(h, sb, 256); var c = sb.ToString(); if (c == "ConsoleWindowClass" || c == "CASCADIA_HOSTING_WINDOW_CLASS" || c.Contains("PseudoConsole")) sb2.Append(h.ToString() + ";"); } return true; }, IntPtr.Zero); return sb2.ToString(); }
  public struct RECT { public int L, T, R, B; } }
"@
[HubCap]::SetProcessDPIAware() | Out-Null
New-Item -ItemType Directory -Force $OutDir | Out-Null
$marker = "$env:TEMP\hub_step.txt"; $seen = @{}; $deadline = (Get-Date).AddMinutes($Minutes)
$base = @{}; ([HubCap]::Consoles() -split ';') | Where-Object { $_ } | ForEach-Object { $base[$_] = 1 }; $new = @{}
while ((Get-Date) -lt $deadline) {
  Start-Sleep -Milliseconds 300
  ([HubCap]::Consoles() -split ';') | Where-Object { $_ -and -not $base[$_] } | ForEach-Object { $new[$_] = (Get-Date -Format 'HH:mm:ss') }
  $s = (Get-Content $marker -ErrorAction SilentlyContinue | Select-Object -First 1)
  if (-not $s -or $s -eq 'start' -or $seen[$s]) { continue }
  $seen[$s] = 1; if ($s -eq 'END') { break }
  $title = if ($s -eq 'new_request' -or $s -eq 'dialog') { '新建监控任务' } else { '监控总台' }
  $h = [HubCap]::FindWindow([NullString]::Value, $title); if ($h -eq [IntPtr]::Zero) { "no window '$title' for $s"; continue }
  if ([HubCap]::IsIconic($h)) { [HubCap]::ShowWindow($h, 9) | Out-Null; Start-Sleep -Milliseconds 800 }
  $r = New-Object HubCap+RECT; [HubCap]::GetWindowRect($h, [ref]$r) | Out-Null
  $bmp = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T); $g = [System.Drawing.Graphics]::FromImage($bmp); $hdc = $g.GetHdc(); [HubCap]::PrintWindow($h, $hdc, 2) | Out-Null; $g.ReleaseHdc($hdc)
  $out = Join-Path $OutDir "$Prefix$s.png"; $bmp.Save($out); "captured $out"
}
"new visible console windows during the tour: $($new.Count) $(($new.GetEnumerator() | ForEach-Object { "$($_.Key)@$($_.Value)" }) -join ', ')"
