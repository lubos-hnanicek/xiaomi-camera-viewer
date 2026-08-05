<#
.SYNOPSIS
    Starts the packaged build and checks it comes up.

.DESCRIPTION
    The point of dist/ is that it runs on its own, so this launches that copy
    rather than the build tree one and waits for its window before closing it
    again. Catches a missing DLL, which is the way a package usually breaks.
#>
[CmdletBinding()]
param(
    [string]$Configuration = 'RelWithDebInfo'
)

$ErrorActionPreference = 'Stop'

Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class Smoke {
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr FindWindowW(string cls, string title);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
}
"@

$exe = Join-Path (Split-Path -Parent $PSScriptRoot) "dist\XiaomiViewer-$Configuration\XiaomiViewer.exe"
if (-not (Test-Path $exe)) { throw "Not packaged: $exe" }

Get-Process XiaomiViewer -ErrorAction SilentlyContinue | ForEach-Object { $_.Kill(); $_.WaitForExit() }
Start-Sleep -Milliseconds 400

Write-Host "==> starting $exe"
$process = Start-Process $exe -PassThru

$window = [IntPtr]::Zero
for ($i = 0; $i -lt 60 -and $window -eq [IntPtr]::Zero; $i++) {
    Start-Sleep -Milliseconds 250
    if ($process.HasExited) { throw "It exited immediately with code $($process.ExitCode)." }
    $window = [Smoke]::FindWindowW('XiaomiViewerWindow', 'Xiaomi Camera Viewer')
}
if ($window -eq [IntPtr]::Zero) { throw "The window never appeared." }

Write-Host "==> window is up; giving it a moment to load the bridge"
Start-Sleep -Seconds 4
if ($process.HasExited) { throw "It died after opening, exit code $($process.ExitCode)." }

[Smoke]::PostMessageW($window, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
$process.WaitForExit(20000) | Out-Null
if (-not $process.HasExited) {
    $process.Kill()
    throw "It did not shut down when asked to close."
}

Write-Host "==> started, ran and closed cleanly (exit code $($process.ExitCode))" -ForegroundColor Green

$log = Join-Path $env:APPDATA 'XiaomiViewer\xiaomi-viewer.log'
if (Test-Path $log) {
    Write-Host "==> last few log lines"
    Get-Content $log -Tail 6 | ForEach-Object { Write-Host "    $_" -ForegroundColor DarkGray }
}
