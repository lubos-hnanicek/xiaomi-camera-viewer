<#
.SYNOPSIS
    Records a few seconds from a live camera and inspects the file.

.DESCRIPTION
    Recording is one of the few features that cannot be judged from a screenshot:
    the question is whether the file a player opens contains the camera's own
    stream, at the right timing, and nothing re-encoded. So this drives the real
    app against a real camera, then hands the result to ffprobe.

    The app is closed with WM_CLOSE rather than killed, because a killed process
    leaves the Matroska file without its trailer and that would be the harness's
    fault rather than the recorder's.

.PARAMETER Seconds
    How long to record.
#>
[CmdletBinding()]
param(
    [int]$Seconds = 20,

    # Time given to sign in from the saved token and get a first frame.
    [int]$WarmupSeconds = 16,

    [string]$Out = "$env:TEMP\xiaomi-recording"
)

$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class Win {
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr FindWindowW(string cls, string title);
    [DllImport("user32.dll")]
    public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")]
    public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")]
    public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")]
    public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")]
    public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    public struct RECT { public int Left, Top, Right, Bottom; }
    public struct POINT { public int X, Y; }
}
"@

$repo = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $repo 'build\msvc\RelWithDebInfo\XiaomiViewer.exe'
$ffprobe = Join-Path $repo 'third_party\ffmpeg\bin\ffprobe.exe'
$ffmpeg = Join-Path $repo 'third_party\ffmpeg\bin\ffmpeg.exe'
if (-not (Test-Path $exe)) { throw "Not built: $exe" }

$recordings = Join-Path ([Environment]::GetFolderPath('MyVideos')) 'XiaomiViewer'
$before = if (Test-Path $recordings) { Get-ChildItem $recordings -Filter *.mkv } else { @() }

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

Write-Host "==> waiting $WarmupSeconds s for a camera to come up"
Start-Sleep -Seconds $WarmupSeconds

# 3 is SW_MAXIMIZE, so the tile is large enough for the pad to be drawn at all.
[Win]::ShowWindow($window, 3) | Out-Null
[Win]::SetForegroundWindow($window) | Out-Null
Start-Sleep -Milliseconds 800

Write-Host '==> focusing one tile with F'
[System.Windows.Forms.SendKeys]::SendWait('f')
Start-Sleep -Seconds 3

function Save-Shot($path) {
    $rect = New-Object Win+RECT
    [Win]::GetClientRect($window, [ref]$rect) | Out-Null
    $topLeft = New-Object Win+POINT
    [Win]::ClientToScreen($window, [ref]$topLeft) | Out-Null

    $w = $rect.Right - $rect.Left
    $h = $rect.Bottom - $rect.Top
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($topLeft.X, $topLeft.Y, 0, 0, $bmp.Size)
    $g.Dispose()

    $area = New-Object System.Drawing.Rectangle 0, ($h - [Math]::Min(460, $h)), ([Math]::Min(460, $w)), ([Math]::Min(460, $h))
    $pad = $bmp.Clone($area, $bmp.PixelFormat)
    $pad.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $pad.Dispose()
    $bmp.Dispose()
    Write-Host "==> $path"
}

Save-Shot "$Out-idle.png"

Write-Host '==> starting the recording with R'
[System.Windows.Forms.SendKeys]::SendWait('r')
Start-Sleep -Seconds ([Math]::Min(6, $Seconds))
Save-Shot "$Out-live.png"

Start-Sleep -Seconds ([Math]::Max(0, $Seconds - 6))

Write-Host '==> stopping the recording with R'
[System.Windows.Forms.SendKeys]::SendWait('r')
Start-Sleep -Seconds 3

Write-Host '==> closing the app'
[Win]::PostMessageW($window, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
$deadline = (Get-Date).AddSeconds(20)
while ((Get-Process XiaomiViewer -ErrorAction SilentlyContinue) -and (Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 250
}
Get-Process XiaomiViewer -ErrorAction SilentlyContinue | ForEach-Object { $_.Kill() }

$after = Get-ChildItem $recordings -Filter *.mkv
$new = $after | Where-Object { $before.Name -notcontains $_.Name }
if (-not $new) { throw "No recording appeared in $recordings" }

foreach ($file in $new) {
    Write-Host ''
    Write-Host "==> $($file.Name)  $([Math]::Round($file.Length / 1MB, 1)) MB" -ForegroundColor Cyan
    & $ffprobe -hide_banner -v error -show_entries `
        'format=format_name,duration,bit_rate:stream=codec_name,profile,width,height,avg_frame_rate,nb_frames' `
        -of default=noprint_wrappers=1 $file.FullName

    # Decoding every frame is the only proof that the remux produced a stream a
    # player can actually follow: a file can be well-formed and still not decode.
    Write-Host '--- decoding every frame ---'
    & $ffmpeg -hide_banner -v warning -i $file.FullName -f null - 2>&1 | ForEach-Object { $_ }
    if ($LASTEXITCODE -eq 0) { Write-Host 'decoded cleanly' -ForegroundColor Green }
}
