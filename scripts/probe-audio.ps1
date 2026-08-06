<#
.SYNOPSIS
    Reports what the cameras actually send on the audio track.

.DESCRIPTION
    The MISS packet header names a codec and the flags carry a sample rate, but
    nothing in this project had ever consumed an audio packet, so which of the
    four codecs a given model uses was unknown. This runs the real app against
    the configured cameras and reads back what the stream worker logged.

    It also reports whether the separate audio command had to be sent, which is
    the bridge's evidence that enableaudio alone is not enough on a model.

.PARAMETER Seconds
    How long to leave the app running. The grace period before the bridge falls
    back to the separate audio command is 3 s, so anything under about 15 s
    cannot tell a slow camera from a silent one.
#>
[CmdletBinding()]
param(
    [int]$Seconds = 30
)

$ErrorActionPreference = 'Stop'

Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class Win {
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr FindWindowW(string cls, string title);
    [DllImport("user32.dll")]
    public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
}
"@

$repo = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $repo 'build\msvc\RelWithDebInfo\XiaomiViewer.exe'
if (-not (Test-Path $exe)) { throw "Not built: $exe" }

$log = Join-Path $env:APPDATA 'XiaomiViewer\xiaomi-viewer.log'

Get-Process XiaomiViewer -ErrorAction SilentlyContinue | ForEach-Object { $_.Kill(); $_.WaitForExit() }
Start-Sleep -Milliseconds 500

Write-Host '==> starting the app'
Start-Process $exe | Out-Null

$window = [IntPtr]::Zero
for ($i = 0; $i -lt 60 -and $window -eq [IntPtr]::Zero; $i++) {
    Start-Sleep -Milliseconds 250
    $window = [Win]::FindWindowW('XiaomiViewerWindow', 'Xiaomi Camera Viewer')
}
if ($window -eq [IntPtr]::Zero) { throw 'The window never appeared.' }

Write-Host "==> streaming for $Seconds s"
Start-Sleep -Seconds $Seconds

Write-Host '==> closing the app'
[Win]::PostMessageW($window, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
$deadline = (Get-Date).AddSeconds(20)
while ((Get-Process XiaomiViewer -ErrorAction SilentlyContinue) -and (Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 250
}
Get-Process XiaomiViewer -ErrorAction SilentlyContinue | ForEach-Object { $_.Kill() }

# The app truncates the log at startup, so the whole file is this run. Opened
# by hand because Get-Content will not share with a process still holding it.
$stream = [System.IO.File]::Open($log, 'Open', 'Read', 'ReadWrite')
$reader = New-Object System.IO.StreamReader($stream)
$lines = $reader.ReadToEnd() -split "`r?`n"
$reader.Dispose()

Write-Host ''
$found = $lines | Select-String -Pattern 'audio|first keyframe|connected over'
if ($found) {
    $found | ForEach-Object {
        $color = if ($_ -match 'no audio arrived') { 'Yellow' }
                 elseif ($_ -match 'audio is') { 'Green' }
                 else { 'Gray' }
        Write-Host $_.Line -ForegroundColor $color
    }
} else {
    Write-Host 'Nothing was logged about audio. Did any camera connect at all?' -ForegroundColor Yellow
    $lines | Select-String -Pattern 'ERROR|WARN' | ForEach-Object { Write-Host $_.Line }
}
