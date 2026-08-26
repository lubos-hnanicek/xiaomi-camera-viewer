<#
.SYNOPSIS
    Exercises the bridge's SD card API against a camera.

.DESCRIPTION
    scripts/probe-rdt.ps1 and scripts/probe-playback.ps1 were how the protocol
    was worked out, one candidate payload at a time. This one is different: it
    calls the API the app calls -- recordings.list, playback.start,
    playback.stop -- and is the check that what was learnt actually survived
    into shipped code.

    Modes:

      list  fetch the catalogue and describe it: how many clips, what span they
            cover, how long they run and where the gaps are
      play  fetch the catalogue, ask for one clip, and watch whether frames
            actually arrive -- because a camera that answers "filefound" and
            then sends nothing is a failure that reads as a success

    The clip to play is chosen with -At, which takes any time PowerShell can
    parse and is matched to the clip covering it. Without one, the newest clip
    is used, that being the one certain to exist.

    Nothing here writes to the camera's configuration or its card.

    Reuses the saved session in %APPDATA%\XiaomiViewer\config.json, so it needs
    no credentials, but it must run as the same Windows user that signed in.
#>
[CmdletBinding()]
param(
    [ValidateSet('list', 'play')]
    [string]$Mode = 'list',

    [string]$Did,

    # Which moment to play. Matched to the clip covering it, since a clip's own
    # start is the only thing the camera accepts and those starts are arbitrary.
    [string]$At = '',

    # Which lens, on the models that have two. Empty lets the camera choose.
    [int[]]$Lenses = @(),

    # How long to watch for frames after playback starts.
    [int]$WatchSeconds = 8,

    # How much footage to ask for, in seconds. A clip is about a minute, so
    # anything longer asks the camera to carry on into the clips that follow --
    # which is the difference between a player that seeks and one that has to
    # stitch a minute at a time. Zero asks for the one clip.
    [int]$Span = 0,

    # TCP because the catalogue is rebuilt from a byte stream spanning a couple
    # of hundred transport messages, and UDP reorders. The result of getting
    # this wrong is not a degraded reading but a meaningless one.
    [ValidateSet('tcp', 'udp', '')]
    [string]$Transport = 'tcp',

    [string]$Lens = '',
    [string]$DllPath
)

$ErrorActionPreference = 'Stop'

if (-not $DllPath) {
    $DllPath = Join-Path (Split-Path -Parent $PSScriptRoot) 'build\msvc\RelWithDebInfo\xmbridge.dll'
}
if (-not (Test-Path $DllPath)) {
    throw "xmbridge.dll not found at $DllPath. Build first with scripts/build.ps1."
}
$DllPath = (Resolve-Path $DllPath).Path

$configPath = Join-Path $env:APPDATA 'XiaomiViewer\config.json'
if (-not (Test-Path $configPath)) {
    throw "No config at $configPath. Sign in with the app first."
}

$config = Get-Content $configPath -Raw | ConvertFrom-Json
$stored = $config.account.token
if (-not $stored -or -not $stored.StartsWith('dpapi:')) {
    throw "No saved token in the config."
}

Add-Type -AssemblyName System.Security
$protected = [Convert]::FromBase64String($stored.Substring('dpapi:'.Length))
$plain = [System.Security.Cryptography.ProtectedData]::Unprotect(
    $protected, $null, [System.Security.Cryptography.DataProtectionScope]::CurrentUser)
$token = [System.Text.Encoding]::UTF8.GetString($plain)

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class XmSd
{
    [DllImport("kernel32", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern IntPtr LoadLibrary(string path);

    [DllImport("kernel32", SetLastError = true)]
    private static extern IntPtr GetProcAddress(IntPtr module, string name);

    // Matches XmbFrame in include/xmbridge_types.h. Four ints, then a long the
    // compiler aligns to eight, then two more ints: thirty-two bytes either
    // side, which is why the default layout is safe here.
    [StructLayout(LayoutKind.Sequential)]
    public struct Frame
    {
        public int kind;
        public int codec;
        public int keyframe;
        public int sampleRate;
        public long ptsMs;
        public uint sequence;
        public uint size;
    }

    private delegate int CallFn(string method, string request, byte[] outBuf, int cap);
    private delegate IntPtr OpenFn(string request, byte[] outBuf, int cap);
    private delegate int CommandFn(IntPtr handle, string request, byte[] outBuf, int cap);
    private delegate int ReadFn(IntPtr handle, byte[] buf, int cap, ref Frame meta);
    private delegate void CloseFn(IntPtr handle);

    private static IntPtr module;

    public static void Load(string path)
    {
        module = LoadLibrary(path);
        if (module == IntPtr.Zero)
            throw new Exception("LoadLibrary failed: " + Marshal.GetLastWin32Error());
    }

    private static IntPtr Proc(string name)
    {
        IntPtr p = GetProcAddress(module, name);
        if (p == IntPtr.Zero) throw new Exception("missing export " + name);
        return p;
    }

    public static string Call(string method, string request)
    {
        CallFn fn = (CallFn)Marshal.GetDelegateForFunctionPointer(Proc("xmb_call"), typeof(CallFn));
        byte[] buffer = new byte[262144];
        int needed = fn(method, request, buffer, buffer.Length);
        if (needed > buffer.Length)
        {
            buffer = new byte[needed];
            needed = fn(method, request, buffer, buffer.Length);
        }
        if (needed < 0) throw new Exception(method + " failed with code " + needed);
        return Encoding.UTF8.GetString(buffer, 0, needed);
    }

    public static IntPtr Open(string request, out string response)
    {
        OpenFn fn = (OpenFn)Marshal.GetDelegateForFunctionPointer(Proc("xmb_stream_open"), typeof(OpenFn));
        byte[] buffer = new byte[65536];
        IntPtr handle = fn(request, buffer, buffer.Length);
        response = Encoding.UTF8.GetString(buffer).TrimEnd('\0');
        return handle;
    }

    // A catalogue of a fortnight's clips is a megabyte of JSON, so the buffer is
    // sized for the answer rather than for the usual case.
    public static string Command(IntPtr handle, string request)
    {
        CommandFn fn = (CommandFn)Marshal.GetDelegateForFunctionPointer(Proc("xmb_stream_command"), typeof(CommandFn));
        byte[] buffer = new byte[16 * 1024 * 1024];
        int needed = fn(handle, request, buffer, buffer.Length);
        if (needed > buffer.Length)
            throw new Exception("stream command wanted " + needed + " bytes");
        if (needed < 0) throw new Exception("stream command failed with code " + needed);
        return Encoding.UTF8.GetString(buffer, 0, needed);
    }

    public static int Read(IntPtr handle, byte[] buffer, ref Frame meta)
    {
        ReadFn fn = (ReadFn)Marshal.GetDelegateForFunctionPointer(Proc("xmb_stream_read"), typeof(ReadFn));
        return fn(handle, buffer, buffer.Length, ref meta);
    }

    public static void Close(IntPtr handle)
    {
        CloseFn fn = (CloseFn)Marshal.GetDelegateForFunctionPointer(Proc("xmb_stream_close"), typeof(CloseFn));
        fn(handle);
    }
}
"@ -Language CSharp

[XmSd]::Load($DllPath)

$userId = $config.account.user_id

Write-Host "==> restoring session for $userId" -ForegroundColor Cyan
$login = @{ user_id = $userId; region = $config.account.region; token = $token } | ConvertTo-Json -Compress
$res = [XmSd]::Call('login.token', $login) | ConvertFrom-Json
if (-not $res.ok) { throw "login.token failed: $($res.error)" }

$res = [XmSd]::Call('device.list', (@{ user_id = $userId } | ConvertTo-Json -Compress)) | ConvertFrom-Json
if (-not $res.ok) { throw "device.list failed: $($res.error)" }

Write-Host "==> cameras on the account" -ForegroundColor Cyan
$res.devices | ForEach-Object {
    Write-Host ("    {0,-12} {1,-24} {2}" -f $_.did, $_.model, $_.name) -ForegroundColor DarkGray
}

if ($Did) {
    $device = $res.devices | Where-Object { $_.did -eq $Did } | Select-Object -First 1
    if (-not $device) { throw "No device with did $Did in the account." }
} else {
    $device = $res.devices | Select-Object -First 1
}

Write-Host "==> $($device.name) [$($device.model)] did=$($device.did)" -ForegroundColor Cyan

$open = @{
    user_id   = $userId
    did       = $device.did
    model     = $device.model
    ip        = $device.ip
    channel   = $Lens
    transport = $Transport
} | ConvertTo-Json -Compress

# A camera that has just hung up stops answering the discovery datagram for a
# while and then comes back on its own, so a first failure says nothing about
# whether this will work.
$handle = [IntPtr]::Zero
for ($attempt = 1; $attempt -le 5; $attempt++) {
    $response = ''
    $handle = [XmSd]::Open($open, [ref]$response)
    if ($handle -ne [IntPtr]::Zero) {
        Write-Host "    $response" -ForegroundColor DarkGray
        break
    }
    if ($attempt -eq 5) { throw "stream open failed after $attempt attempts: $response" }
    Write-Host "    open failed, waiting 15s and trying again" -ForegroundColor DarkYellow
    Start-Sleep -Seconds 15
}

function Invoke-Stream([hashtable]$request) {
    $json = ConvertTo-BridgeJson $request
    $raw = [XmSd]::Command($handle, $json)
    return $raw | ConvertFrom-Json
}

# PowerShell 5.1's ConvertTo-Json unwraps a one-element array into a scalar.
# The camera's playback parser logs "chn no array" and answers nothing when
# the lens arrives as a bare number, so the brackets have to be written here.
function ConvertTo-BridgeJson([hashtable]$request) {
    $parts = New-Object System.Collections.Generic.List[string]
    foreach ($key in $request.Keys) {
        $value = $request[$key]
        $encoded = if ($null -ne $value -and $value.GetType().IsArray) {
            $items = foreach ($item in @($value)) {
                if ($item -is [string]) { ConvertTo-BridgeJsonString $item }
                else { "$item" }
            }
            '[' + ($items -join ',') + ']'
        } elseif ($value -is [string]) {
            ConvertTo-BridgeJsonString $value
        } elseif ($value -is [bool]) {
            if ($value) { 'true' } else { 'false' }
        } else {
            "$value"
        }
        $parts.Add(('"{0}":{1}' -f $key, $encoded))
    }
    return '{' + ($parts -join ',') + '}'
}

function ConvertTo-BridgeJsonString([string]$value) {
    $escaped = $value.Replace('\', '\\').Replace('"', '\"')
    return '"' + $escaped + '"'
}

function Format-Epoch([long]$epoch) {
    return [DateTimeOffset]::FromUnixTimeSeconds($epoch).ToLocalTime().ToString('yyyy-MM-dd HH:mm:ss')
}

function Get-Frames {
    $stats = Invoke-Stream @{ method = 'stats' }
    if (-not $stats.ok) { return -1 }
    return [long]$stats.frames
}

try {
    Write-Host "==> asking for the catalogue (this takes seconds)" -ForegroundColor Cyan
    $started = Get-Date
    $answer = Invoke-Stream @{ method = 'recordings.list'; channel = 0 }
    $elapsed = (Get-Date) - $started

    if (-not $answer.ok) { throw "recordings.list failed: $($answer.error)" }

    $clips = @($answer.clips)
    Write-Host ("    {0} clips in {1:N1}s" -f $clips.Count, $elapsed.TotalSeconds) -ForegroundColor Green

    if ($clips.Count -eq 0) { throw "the camera answered with an empty catalogue" }

    $first = $clips[0]
    $last = $clips[-1]
    Write-Host ("    oldest  {0}  {1}s" -f (Format-Epoch $first.start), $first.duration)
    Write-Host ("    newest  {0}  {1}s" -f (Format-Epoch $last.start), $last.duration)

    # Durations say what kind of recording this is: one length for nearly every
    # clip means continuous, a scatter means motion events.
    $durations = $clips | Group-Object duration | Sort-Object Count -Descending | Select-Object -First 4
    Write-Host ("    durations: {0}" -f
        (($durations | ForEach-Object { "$($_.Name)s x$($_.Count)" }) -join ', '))
    $events = @($clips | Where-Object { $_.event }).Count
    Write-Host ("    {0} of them marked as events" -f $events)

    # A gap is footage the card does not hold, and a player that does not know
    # where they are will offer clips that cannot be played.
    $gaps = 0
    $covered = 0
    for ($i = 1; $i -lt $clips.Count; $i++) {
        $covered += $clips[$i - 1].duration
        if ($clips[$i].start -gt ($clips[$i - 1].start + $clips[$i - 1].duration + 2)) { $gaps++ }
    }
    $span = $last.start + $last.duration - $first.start
    Write-Host ("    spans {0:N1} days, {1:N1} of them recorded, {2} gaps" -f
        ($span / 86400), ($covered / 86400), $gaps)

    if ($Mode -eq 'list') { return }

    # --- play -------------------------------------------------------------

    $clip = $last
    if ($At) {
        $want = [DateTimeOffset]::new([DateTime]::Parse($At)).ToUnixTimeSeconds()
        $clip = $clips | Where-Object { $_.start -le $want -and ($_.start + $_.duration) -gt $want } |
            Select-Object -First 1
        if (-not $clip) {
            throw ("no clip covers {0}; the card holds {1} to {2}" -f
                $At, (Format-Epoch $first.start), (Format-Epoch $last.start))
        }
        Write-Host ("==> {0} falls inside the clip starting {1}" -f $At, (Format-Epoch $clip.start)) `
            -ForegroundColor Cyan
    }

    # Measure the live rate first, in this same session. A recording is pushed
    # as fast as the link allows rather than at the rate it was shot, and the
    # difference is the whole reason a player has to pace by timestamp instead
    # of showing frames as they arrive -- but "fast" only means anything next to
    # the same camera's live rate.
    Write-Host "==> measuring the live rate" -ForegroundColor Cyan
    $liveStart = Get-Frames
    Start-Sleep -Seconds 4
    $liveRate = ((Get-Frames) - $liveStart) / 4.0
    Write-Host ("    {0:N1} frames per second live" -f $liveRate)

    $before = Get-Frames
    Write-Host ("==> asking for the clip at {0} ({1}s), live frames so far {2}" -f
        (Format-Epoch $clip.start), $clip.duration, $before) -ForegroundColor Cyan

    $wanted = if ($Span -gt 0) { $Span } else { $clip.duration }
    $request = @{
        method = 'playback.start'
        start  = $clip.start
        end    = $clip.start + $wanted
    }
    if ($Lenses.Count -gt 0) { $request['lenses'] = @($Lenses) }

    $status = Invoke-Stream $request
    if (-not $status.ok) { throw "playback.start failed: $($status.error)" }

    Write-Host ("    camera says: {0}" -f ($status | ConvertTo-Json -Compress)) -ForegroundColor Green
    if (-not $status.found) { throw "the camera did not find the clip it listed" }

    # Counted after the request, because live frames before it would make a
    # frozen session look like it was still moving. Read blocks until a frame
    # arrives on this session, and playback sent on the other lens never does,
    # so a timeout here is the evidence a successful reply is not enough.
    $before = Get-Frames
    Write-Host "==> waiting for this session's frame count to move" -ForegroundColor Cyan
    $arrivedBy = (Get-Date).AddSeconds($WatchSeconds)
    $moved = $false
    while ((Get-Date) -lt $arrivedBy) {
        Start-Sleep -Milliseconds 400
        $now = Get-Frames
        if ($now -gt $before) {
            $moved = $true
            break
        }
    }
    if (-not $moved) {
        Write-Host ("    NO FRAMES: the camera accepted the request and sent nothing this session could see (still {0})" -f $before) -ForegroundColor Red
    } else {
    # The answer is not the evidence. A camera that says it found the file and
    # then sends nothing has failed in a way that reads as success, so what is
    # reported here is the frames themselves -- and their timestamps, which are
    # what a player has to pace by when the camera sends faster than real time.
    Write-Host "==> reading frames" -ForegroundColor Cyan

    $buffer = New-Object byte[] (4 * 1024 * 1024)
    $meta = New-Object XmSd+Frame
    $deadline = (Get-Date).AddSeconds($WatchSeconds)
    $video = 0
    $audio = 0
    $keyframes = 0
    $firstPts = -1
    $lastPts = -1

    while ((Get-Date) -lt $deadline) {
        $n = [XmSd]::Read($handle, $buffer, [ref]$meta)
        if ($n -lt 0) {
            Write-Host "    read ended with code $n" -ForegroundColor DarkYellow
            break
        }

        if ($meta.kind -eq 2) { $audio++; continue }

        $video++
        if ($meta.keyframe -ne 0) { $keyframes++ }
        if ($firstPts -lt 0) {
            $firstPts = $meta.ptsMs
            Write-Host ("    first frame: pts {0}, codec {1}, {2} bytes, keyframe {3}" -f
                $meta.ptsMs, $meta.codec, $n, $meta.keyframe)
        }

        # A jump backwards is the switch from live to recorded: the two are on
        # different clocks, and a player that does not notice would treat the
        # whole recording as one long stall.
        if ($lastPts -ge 0 -and [Math]::Abs($meta.ptsMs - $lastPts) -gt 5000) {
            Write-Host ("    timestamp jumps from {0} to {1} at frame {2} (keyframe {3})" -f
                $lastPts, $meta.ptsMs, $video, $meta.keyframe) -ForegroundColor Yellow
            $firstPts = $meta.ptsMs
            $video = 1
            $keyframes = [int]($meta.keyframe -ne 0)
        }
        $lastPts = $meta.ptsMs
    }

    if ($video -eq 0) {
        Write-Host "    NO FRAMES: the camera accepted the request and sent nothing" -ForegroundColor Red
    } else {
        $wall = $WatchSeconds
        $covered = ($lastPts - $firstPts) / 1000.0
        Write-Host ("    {0} video frames ({1} keyframes) and {2} audio in {3}s" -f
            $video, $keyframes, $audio, $wall) -ForegroundColor Green
        Write-Host ("    timestamps span {0:N1}s of footage, so playback runs at {1:N2}x real time" -f
            $covered, ($covered / $wall)) -ForegroundColor Green

        # Which timeline the timestamps are on decides what a player can show.
        # An epoch would let it label the picture with the moment it was shot;
        # anything else has to be counted from the clip's start.
        $asEpoch = [DateTimeOffset]::FromUnixTimeMilliseconds($firstPts).ToLocalTime()
        if ($asEpoch.Year -gt 2020 -and $asEpoch.Year -lt 2100) {
            Write-Host ("    the first timestamp reads as {0:yyyy-MM-dd HH:mm:ss} in wall-clock terms" -f
                $asEpoch)
        } else {
            Write-Host ("    the first timestamp is {0}, which is not a wall clock" -f $firstPts)
        }
    }
    }

    Write-Host "==> back to live" -ForegroundColor Cyan
    $stop = Invoke-Stream @{ method = 'playback.stop' }
    if (-not $stop.ok) { Write-Host "    $($stop.error)" -ForegroundColor Red }

    $resumed = Get-Frames
    Start-Sleep -Seconds 3
    Write-Host ("    {0:N1} frames per second after stopping" -f (((Get-Frames) - $resumed) / 3.0))
} finally {
    [XmSd]::Close($handle)
}
