<#
.SYNOPSIS
    Records a few seconds from a live camera and inspects the file.

.DESCRIPTION
    Recording is one of the few features that cannot be judged from a screenshot:
    the question is whether the file a player opens contains the camera's own
    streams, at the right timing, and nothing re-encoded. So this drives the real
    app against a real camera, then hands the result to ffprobe.

    Both tracks are checked. The audio one is the easier of the two to get
    subtly wrong -- a track that is present, well-formed and silent looks fine
    to ffprobe -- so the level is measured as well as the format.

    The app is closed with WM_CLOSE rather than killed, because a killed process
    leaves the Matroska file without its trailer and that would be the harness's
    fault rather than the recorder's.

.PARAMETER Seconds
    How long to record.

.PARAMETER Global
    Uses Shift+R and verifies every video/audio track in the single global MKV.
#>
[CmdletBinding()]
param(
    [int]$Seconds = 20,

    # Time given to sign in from the saved token and get a first frame.
    [int]$WarmupSeconds = 16,

    [string]$Out = "$env:TEMP\xiaomi-recording",

    [switch]$Global,

    [int]$ExpectedVideoTracks = 0,

    [int]$ExpectedAudioTracks = 0
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

# Runs ffmpeg and returns everything it said, stderr included.
#
# ffmpeg reports on stderr whether or not it succeeded, and while
# $ErrorActionPreference is Stop, PowerShell turns anything a native command
# writes there into a terminating error. That would fail this check on the
# warnings it exists to show.
# Takes ffmpeg's arguments through $args rather than a param block, because a
# declared parameter list would try to bind -i as -InformationAction.
function Invoke-FFmpeg {
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & $ffmpeg -hide_banner -nostats @args 2>&1 | ForEach-Object { "$_" }
    } finally {
        $ErrorActionPreference = $previous
    }
}

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

# Listening is not needed to record audio, but it exercises the decoder against
# the same packets the recorder is storing, so a stream that cannot be played
# fails here rather than in someone's player.
Write-Host '==> listening with A'
[System.Windows.Forms.SendKeys]::SendWait('a')
Start-Sleep -Seconds 2

$recordKey = if ($Global) { '+r' } else { 'r' }
$recordLabel = if ($Global) { 'Shift+R' } else { 'R' }
Write-Host "==> starting the recording with $recordLabel"
[System.Windows.Forms.SendKeys]::SendWait($recordKey)
Start-Sleep -Seconds ([Math]::Min(6, $Seconds))
Save-Shot "$Out-live.png"

Start-Sleep -Seconds ([Math]::Max(0, $Seconds - 6))

Write-Host "==> stopping the recording with $recordLabel"
[System.Windows.Forms.SendKeys]::SendWait($recordKey)
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
if ($Global -and @($new).Count -ne 1) {
    throw "Global recording should create one MKV, but $(@($new).Count) appeared."
}

$failed = $false

foreach ($file in $new) {
    Write-Host ''
    Write-Host "==> $($file.Name)  $([Math]::Round($file.Length / 1MB, 1)) MB" -ForegroundColor Cyan
    & $ffprobe -hide_banner -v error -show_entries `
        'format=format_name,duration,bit_rate:stream=codec_type,codec_name,profile,width,height,avg_frame_rate,nb_frames,sample_rate,channels' `
        -of default=noprint_wrappers=1 $file.FullName

    if ($Global) {
        $probeText = (& $ffprobe -hide_banner -v error `
                -show_entries 'stream=index,codec_type,codec_name:stream_tags=title' `
                -of json $file.FullName) -join "`n"
        $probe = $probeText | ConvertFrom-Json
        $videoStreams = @($probe.streams | Where-Object { $_.codec_type -eq 'video' })
        $audioStreams = @($probe.streams | Where-Object { $_.codec_type -eq 'audio' })

        $wantedVideo = if ($ExpectedVideoTracks -gt 0) { $ExpectedVideoTracks } else { 1 }
        if (($ExpectedVideoTracks -gt 0 -and $videoStreams.Count -ne $wantedVideo) -or
            ($ExpectedVideoTracks -eq 0 -and $videoStreams.Count -lt $wantedVideo)) {
            Write-Host "expected $wantedVideo video track(s), found $($videoStreams.Count)" `
                -ForegroundColor Red
            $failed = $true
        }
        if (($ExpectedAudioTracks -gt 0 -and $audioStreams.Count -ne $ExpectedAudioTracks) -or
            ($ExpectedAudioTracks -eq 0 -and $audioStreams.Count -lt 1)) {
            $wantedAudio = if ($ExpectedAudioTracks -gt 0) { $ExpectedAudioTracks } else { 'at least 1' }
            Write-Host "expected $wantedAudio audio track(s), found $($audioStreams.Count)" `
                -ForegroundColor Red
            $failed = $true
        }

        $allStreams = @($videoStreams) + @($audioStreams)
        foreach ($stream in $allStreams) {
            $title = "$($stream.tags.title)"
            if ([string]::IsNullOrWhiteSpace($title)) {
                Write-Host "stream $($stream.index) has no track title" -ForegroundColor Red
                $failed = $true
            } else {
                Write-Host "stream $($stream.index): $title ($($stream.codec_name))" `
                    -ForegroundColor Green
            }

            Write-Host "--- decoding stream $($stream.index) ---"
            Invoke-FFmpeg -v warning -i $file.FullName -map "0:$($stream.index)" -f null - |
                ForEach-Object { Write-Host $_ }
            if ($LASTEXITCODE -ne 0) {
                Write-Host "stream $($stream.index) did not decode" -ForegroundColor Red
                $failed = $true
            }
        }

        foreach ($stream in $audioStreams) {
            Write-Host "--- measuring audio stream $($stream.index) ---"
            $levels = Invoke-FFmpeg -v info -i $file.FullName -map "0:$($stream.index)" `
                -af volumedetect -f null -
            $peak = @($levels | Select-String -Pattern 'max_volume: (-?[\d.]+) dB')
            if ($peak.Count -eq 0) {
                Write-Host "audio stream $($stream.index) could not be measured" -ForegroundColor Red
                $failed = $true
                continue
            }
            $db = [double]$peak[0].Matches[0].Groups[1].Value
            if ($db -le -90.0) {
                Write-Host "audio stream $($stream.index) is silent (peak $db dB)" `
                    -ForegroundColor Red
                $failed = $true
            } else {
                Write-Host "audio stream $($stream.index) peaks at $db dB" -ForegroundColor Green
            }
        }
        continue
    }

    # Decoding every frame is the only proof that the remux produced a stream a
    # player can actually follow: a file can be well-formed and still not decode.
    Write-Host '--- decoding every frame ---'
    Invoke-FFmpeg -v warning -i $file.FullName -f null - | ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -eq 0) {
        Write-Host 'decoded cleanly' -ForegroundColor Green
    } else {
        Write-Host 'the file did not decode' -ForegroundColor Red
        $failed = $true
    }

    Write-Host '--- audio ---'
    $codec = (& $ffprobe -hide_banner -v error -select_streams a:0 `
            -show_entries stream=codec_name -of csv=p=0 $file.FullName) -join ''
    if (-not $codec) {
        Write-Host 'no audio track' -ForegroundColor Red
        $failed = $true
        continue
    }
    Write-Host "audio track: $codec" -ForegroundColor Green

    # A track can be present, well-formed and completely silent, which is what a
    # wrong Opus header or a mis-timed packet stream tends to produce, so the
    # level is the assertion rather than the presence.
    $levels = Invoke-FFmpeg -v info -i $file.FullName -map 'a:0' -af volumedetect -f null -
    $peak = @($levels | Select-String -Pattern 'max_volume: (-?[\d.]+) dB')
    if ($peak.Count -eq 0) {
        Write-Host 'the audio track could not be measured' -ForegroundColor Red
        $levels | ForEach-Object { Write-Host $_ }
        $failed = $true
        continue
    }

    $db = [double]$peak[0].Matches[0].Groups[1].Value
    if ($db -le -90.0) {
        Write-Host "the audio track is silent (peak $db dB)" -ForegroundColor Red
        $failed = $true
    } else {
        Write-Host "audio peaks at $db dB" -ForegroundColor Green
    }
}

if ($failed) { throw 'The recording did not carry usable audio.' }
