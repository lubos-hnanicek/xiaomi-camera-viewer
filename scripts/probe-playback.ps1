<#
.SYNOPSIS
    Investigates whether a camera will play back its SD card recordings.

.DESCRIPTION
    MISS names a playback command pair (0x10D request, 0x10E response) and a
    playback speed command (0x10F), and for a long time no payload for any of
    them was documented anywhere: go2rtc declares two of the three constants and
    sends none of them, and Xiaomi's plugin SDK lists the names with the doc
    comments shifted by one and no schema at all.

    The shape is now known, and it did not come from guessing. IMILAB publishes
    camera firmware openly, those cameras are built from the same ipc_sdk as
    these, and the request parser is in the clear in the image. It reads
    sessionid, starttime, endtime, autoswitchtolive, offset, speed and
    avchannelmerge, and -- this is the part that matters -- when any one of them
    is missing it logs to a console nobody can read and returns without
    answering. That is why the guessing never worked and could never have
    worked: a malformed request and an unsupported command are the same silence.

    Every mode here is a question:

      status   is there a card, is it recording, how full is it
      devinfo  what the camera says about itself (0x110), which is the one
               undocumented command whose reply we could already read
      play     what 0x10D answers to the request the firmware asks for
      probe    the earlier experiment, kept because it is the evidence for the
               paragraph above: fourteen guesses, fourteen silences
      speed    what 0x10F answers, worth running once play works

    Nothing here writes to the camera's configuration or its card, and no
    candidate asks for a delete or a format.

    Reuses the saved session in %APPDATA%\XiaomiViewer\config.json, so it needs
    no credentials, but it must run as the same Windows user that signed in.
#>
[CmdletBinding()]
param(
    [ValidateSet('status', 'devinfo', 'play', 'at', 'hour', 'sweep', 'probe', 'speed', 'idle', 'one')]
    [string]$Mode = 'status',

    # For the play mode: which stretch of the card to ask for. The default looks
    # an hour back, far enough that a recording has been closed and indexed but
    # near enough that a card recording on motion is likely to hold something.
    [int]$AgoMinutes = 60,
    [int]$WindowMinutes = 10,

    # For the at mode: an instant Mi Home's timeline shows a recording for,
    # written the way you read it off the phone -- '2026-08-24 19:35'. Which
    # clock the camera keys its index on is the open question, so this is asked
    # several ways at once and the camera picks.
    [string]$At,

    # A clip's exact start, in seconds, as the recording index gives it. This is
    # not the same question as -At: the index names when a file was opened, and
    # the lookup wants that instant and not one the file merely covers.
    [long]$Epoch = 0,

    # For the hour mode: how far apart to place the probes, and whether to read
    # the hour as UTC rather than in this machine's zone.
    [int]$StepMinutes = 1,
    [switch]$AsUtc,

    # For the one mode: the command and body to send, and how many times. A
    # candidate that only answers sometimes has not been understood yet, so the
    # answer has to be shown to follow the command rather than the clock.
    [int]$Cmd = 0x10D,
    [string]$Body = '',
    [int]$Repeat = 4,

    # Which camera to probe. Defaults to the first one that reports a card.
    [string]$Did,

    # Lens to open, for a dual-lens model.
    [string]$Channel = '',

    [string]$DllPath,

    # How long to wait for a reply before moving to the next candidate. Replies
    # to the commands we do understand come back in well under a second.
    [int]$WaitMs = 2000
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

public static class XmPlay
{
    [DllImport("kernel32", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern IntPtr LoadLibrary(string path);

    [DllImport("kernel32", SetLastError = true)]
    private static extern IntPtr GetProcAddress(IntPtr module, string name);

    private delegate int CallFn(string method, string request, byte[] outBuf, int cap);
    private delegate IntPtr OpenFn(string request, byte[] outBuf, int cap);
    private delegate int CommandFn(IntPtr handle, string request, byte[] outBuf, int cap);
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

    public static string Command(IntPtr handle, string request)
    {
        CommandFn fn = (CommandFn)Marshal.GetDelegateForFunctionPointer(Proc("xmb_stream_command"), typeof(CommandFn));
        // Big enough that it never has to ask twice: reading the camera's
        // replies empties them, so a call that answers "too small" has already
        // consumed what it declined to hand over.
        byte[] buffer = new byte[16 * 1024 * 1024];
        int needed = fn(handle, request, buffer, buffer.Length);
        if (needed > buffer.Length)
            throw new Exception("stream command wanted " + needed + " bytes, which it has already discarded");
        if (needed < 0) throw new Exception("stream command failed with code " + needed);
        return Encoding.UTF8.GetString(buffer, 0, needed);
    }

    public static void Close(IntPtr handle)
    {
        CloseFn fn = (CloseFn)Marshal.GetDelegateForFunctionPointer(Proc("xmb_stream_close"), typeof(CloseFn));
        fn(handle);
    }
}
"@ -Language CSharp

[XmPlay]::Load($DllPath)

$userId = $config.account.user_id

$login = @{ user_id = $userId; region = $config.account.region; token = $token } | ConvertTo-Json -Compress
Write-Host "==> restoring session for $userId (region $($config.account.region))" -ForegroundColor Cyan
$res = [XmPlay]::Call('login.token', $login) | ConvertFrom-Json
if (-not $res.ok) { throw "login.token failed: $($res.error)" }

$res = [XmPlay]::Call('device.list', (@{ user_id = $userId } | ConvertTo-Json -Compress)) | ConvertFrom-Json
if (-not $res.ok) { throw "device.list failed: $($res.error)" }
$devices = $res.devices

function Get-Prop([string]$did, [int]$siid, [int]$piid) {
    $req = @{ user_id = $userId; did = $did; props = @(@{ siid = $siid; piid = $piid }) } |
        ConvertTo-Json -Compress -Depth 5
    try {
        $r = [XmPlay]::Call('miot.get', $req) | ConvertFrom-Json
        if (-not $r.ok) { return "error: $($r.error)" }
        $entry = $r.result | Select-Object -First 1
        if ($entry.code -ne 0) { return "unsupported (code $($entry.code))" }
        return [string]$entry.value
    } catch {
        return "error: $_"
    }
}

# The card, as MIoT describes it. Whether there is anything to play at all is the
# first question, and it is the only one the cloud can answer.
$cardStatus = @{
    0 = 'ready'; 1 = 'no card'; 2 = 'low space'; 3 = 'device error'; 4 = 'formatting'
    5 = 'ejected'; 6 = 'not initialised'; 7 = 'too small'; 8 = 'incompatible'; 9 = 'file error'
}
$recordingMode = @{ 0 = 'continuous'; 1 = 'on motion'; 2 = 'off' }

function Show-Status([object]$device) {
    $status = Get-Prop $device.did 4 1
    $total = Get-Prop $device.did 4 2
    $free = Get-Prop $device.did 4 3
    $used = Get-Prop $device.did 4 4
    $mode = Get-Prop $device.did 2 7

    # A camera that is offline, or one that does not carry the property at all,
    # answers with a message rather than a number.
    function Describe($value, $names) {
        if ($value -notmatch '^\d+$') { return $value }
        if ($names.ContainsKey([int]$value)) { return "$value ($($names[[int]$value]))" }
        return $value
    }
    function Gigabytes($value) {
        if ($value -match '^\d+$') { return [Math]::Round([double]$value / 1MB, 1) }
        return $value
    }

    [PSCustomObject]@{
        Camera    = $device.name
        Model     = $device.model
        Card      = Describe $status $cardStatus
        Recording = Describe $mode $recordingMode
        TotalGB   = Gigabytes $total
        FreeGB    = Gigabytes $free
        UsedGB    = Gigabytes $used
    }
}

if ($Mode -eq 'status') {
    $targets = if ($Did) { $devices | Where-Object { $_.did -eq $Did } } else { $devices }
    $targets | ForEach-Object { Show-Status $_ } | Format-List
    return
}

# --- Modes that need a live session ----------------------------------------

if ($Did) {
    $device = $devices | Where-Object { $_.did -eq $Did } | Select-Object -First 1
    if (-not $device) { throw "No device with did $Did in the account." }
} else {
    $device = $devices | Where-Object { (Get-Prop $_.did 4 1) -eq '0' } | Select-Object -First 1
    if (-not $device) { throw "No camera reports a ready card. Pass -Did explicitly." }
}

Write-Host "==> probing $($device.name) [$($device.model)] did=$($device.did) channel='$Channel'" -ForegroundColor Cyan

$open = @{
    user_id = $userId
    did     = $device.did
    model   = $device.model
    ip      = $device.ip
    channel = $Channel
} | ConvertTo-Json -Compress

$response = ''
$script:handle = [XmPlay]::Open($open, [ref]$response)
if ($script:handle -eq [IntPtr]::Zero) { throw "stream open failed: $response" }
Write-Host "    stream open: $response" -ForegroundColor DarkGray

try {
    # Anything the camera said before the first candidate is not an answer to it.
    [XmPlay]::Command($script:handle, '{"method":"replies"}') | Out-Null

    # A camera stops sending live video the moment it is asked for playback, and
    # the bridge, seeing a media path that has gone quiet, times the session out
    # a few seconds later. That is fine for one question and fatal for a walk:
    # everything after the timeout measures a dead session rather than a
    # candidate. So a long walk trades sessions in as it goes.
    #
    # It is not enough to open one. A session that is not carrying video answers
    # filenotfound to instants the card demonstrably holds -- that is what made
    # an hour of continuous recording look empty and sent this whole
    # investigation after a phantom. So a session is only asked a question once
    # it has been seen to stream.
    function Wait-Healthy([int]$seconds = 10) {
        for ($i = 0; $i -lt $seconds; $i++) {
            $before = Get-Frames
            Start-Sleep -Seconds 1
            if ((Get-Frames) -gt $before) { return $true }
        }
        return $false
    }

    function Reset-Session {
        try { [XmPlay]::Close($script:handle) } catch { }
        Start-Sleep -Milliseconds 1500

        $reopened = ''
        $script:handle = [XmPlay]::Open($open, [ref]$reopened)
        if ($script:handle -eq [IntPtr]::Zero) { throw "stream reopen failed: $reopened" }
        [XmPlay]::Command($script:handle, '{"method":"replies"}') | Out-Null

        if (Wait-Healthy) {
            Write-Host "    -- fresh session, streaming" -ForegroundColor DarkGray
        } else {
            Write-Host "    -- fresh session, but no video: its answers mean nothing" -ForegroundColor DarkYellow
        }
    }

    function Send-Raw([int]$cmd, [string]$body) {
        $req = @{ method = 'miss.raw'; cmd = $cmd; body = $body } | ConvertTo-Json -Compress
        try {
            [XmPlay]::Command($script:handle, $req) | Out-Null
        } catch {
            Write-Host "    send failed: $_" -ForegroundColor Red
        }
    }

    # The bridge answers with the payload merged into the top-level object next
    # to ok, not nested under a result. Reading drains the log, so both halves
    # have to come out of the same call or one of them would swallow the other.
    $script:lastUnhandled = ''
    function Get-Replies {
        $r = [XmPlay]::Command($script:handle, '{"method":"replies"}') | ConvertFrom-Json
        if (-not $r.ok) { return @() }

        # Whatever arrived on a channel the transport does not open. Cumulative,
        # so it is only worth reporting when it grows.
        $text = ($r.unhandled -join '; ')
        if ($text -and $text -ne $script:lastUnhandled) {
            Write-Host ("      unhandled: {0}" -f $text) -ForegroundColor Magenta
            $script:lastUnhandled = $text
        }
        return @($r.replies)
    }

    function Get-Frames {
        $r = [XmPlay]::Command($script:handle, '{"method":"stats"}') | ConvertFrom-Json
        if (-not $r.ok) { return -1 }
        return [int64]$r.frames
    }

    # Sends one candidate and reports what the camera said and whether the media
    # flow changed, which is the other way a playback request could show itself.
    function Try-Candidate([int]$cmd, [string]$name, [string]$body) {
        $before = Get-Frames
        Send-Raw $cmd $body
        Start-Sleep -Milliseconds $WaitMs
        $replies = Get-Replies
        $after = Get-Frames

        Write-Host ("  {0,-34} {1}" -f $name, $body) -ForegroundColor Gray
        if ($replies.Count -eq 0) {
            Write-Host ("      no reply, frames +{0}" -f ($after - $before)) -ForegroundColor DarkGray
        } else {
            foreach ($reply in $replies) {
                Write-Host ("      {0}" -f $reply) -ForegroundColor Green
            }
            Write-Host ("      frames +{0}" -f ($after - $before)) -ForegroundColor DarkGray
        }
    }

    switch ($Mode) {
    'devinfo' {
        # 0x110 takes no payload in every implementation that names it, so its
        # reply is the one sample of an undocumented reply we can get for free.
        # Worth having: if it lists capabilities, playback support may be in it.
        Write-Host "==> device info (0x110)" -ForegroundColor Cyan
        Try-Candidate 0x110 'devinfo, empty body' ''
        Try-Candidate 0x110 'devinfo, empty object' '{}'
    }

    'play' {
        Write-Host "==> playback request (0x10D), the shape the firmware parses" -ForegroundColor Cyan

        $end = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds() - ($AgoMinutes * 60)
        $start = $end - ($WindowMinutes * 60)

        # The camera stamps recordings with its own clock, and nothing says that
        # clock is UTC: the vendor service that sets it is called hl-set-timezone
        # and takes an offset. So ask twice, once each way, and let the answer
        # say which it was. filenotfound for one and filefound for the other is
        # itself the finding.
        $offset = [int][TimeZoneInfo]::Local.GetUtcOffset([DateTime]::Now).TotalSeconds

        function New-Request([long]$from, [long]$to) {
            $fields = [ordered]@{
                sessionid        = 1
                starttime        = $from
                endtime          = $to
                autoswitchtolive = 0
                offset           = 0
                speed            = 1
                avchannelmerge   = 1
            }
            return ($fields | ConvertTo-Json -Compress)
        }

        Write-Host ("    window {0:yyyy-MM-dd HH:mm:ss} .. {1:HH:mm:ss} UTC" -f
            [DateTimeOffset]::FromUnixTimeSeconds($start).UtcDateTime,
            [DateTimeOffset]::FromUnixTimeSeconds($end).UtcDateTime) -ForegroundColor DarkGray

        Try-Candidate 0x10D 'utc timestamps' (New-Request $start $end)
        if ($offset -ne 0) {
            Try-Candidate 0x10D 'local timestamps' (New-Request ($start + $offset) ($end + $offset))
        }

        # The control. If the firmware reading is right, dropping one required
        # field turns a camera that just answered back into a silent one, and
        # that is the whole explanation for the probe mode below.
        $incomplete = '{"sessionid":1,"starttime":' + $start + ',"endtime":' + $end + '}'
        Try-Candidate 0x10D 'control: missing fields' $incomplete

        # A playback stream arrives on the media path the live one uses, so the
        # frame counter is the second witness and worth a longer look than the
        # per-candidate wait allows.
        Write-Host "==> watching the media path" -ForegroundColor Cyan
        for ($i = 1; $i -le 5; $i++) {
            $before = Get-Frames
            Start-Sleep -Seconds 1
            $after = Get-Frames
            $replies = Get-Replies
            foreach ($reply in $replies) { Write-Host ("      {0}" -f $reply) -ForegroundColor Green }
            Write-Host ("  {0,2}s frames +{1}" -f $i, ($after - $before)) -ForegroundColor DarkGray
        }
    }

    'at' {
        if ($Epoch -gt 0) {
            $wall = [DateTimeOffset]::FromUnixTimeSeconds($Epoch).ToLocalTime()
            Write-Host ("==> asking for {0} exactly, which the index calls {1:yyyy-MM-dd HH:mm:ss} local" -f
                $Epoch, $wall) -ForegroundColor Cyan

            if (-not (Wait-Healthy)) {
                Write-Host "    no video, so nothing below is evidence" -ForegroundColor DarkYellow
            }

            # The clip itself, then its neighbours, which the index says begin a
            # minute either side. If the exact start is what the lookup wants,
            # all three answer and the instants between them do not.
            $id = 0
            foreach ($offset in @(0, -60, 60, 32)) {
                $id++
                if ($id -gt 1) { Reset-Session }

                $start = $Epoch + $offset
                $fields = [ordered]@{
                    sessionid        = $id
                    starttime        = $start
                    endtime          = $start + 600
                    autoswitchtolive = 1
                    offset           = 0
                    speed            = 1
                    avchannelmerge   = 1
                }
                $label = switch ($offset) {
                    0 { 'the clip start itself' }
                    32 { 'part way into the clip' }
                    default { "a clip {0:+#;-#}s away" -f $offset }
                }
                Try-Candidate 0x10D $label ($fields | ConvertTo-Json -Compress)
            }
            break
        }

        if (-not $At) { throw "The at mode needs -At, an instant Mi Home shows a recording for." }

        $wall = [DateTime]::Parse($At, [Globalization.CultureInfo]::InvariantCulture)
        Write-Host ("==> asking for {0:yyyy-MM-dd HH:mm:ss} as read off the phone" -f $wall) -ForegroundColor Cyan

        $localOffset = [TimeZoneInfo]::Local.GetUtcOffset($wall)
        $asLocal = [DateTimeOffset]::new($wall, $localOffset).ToUnixTimeSeconds()
        # Not $asUtc: that is the -AsUtc switch by another spelling, and this
        # language does not distinguish them.
        $sameDigitsAsUtc = [DateTimeOffset]::new($wall, [TimeSpan]::Zero).ToUnixTimeSeconds()

        # Mi Home draws the timeline in the phone's timezone, so the phone and
        # this machine agree on the wall clock. What nobody knows is what the
        # camera did with that instant on the way into its index: the honest
        # answer is to offer every plausible reading and see which one it owns.
        $readings = @(
            @{ Name = 'phone wall clock, local zone'; Epoch = $asLocal }
            @{ Name = 'same digits read as utc';      Epoch = $sameDigitsAsUtc }
            @{ Name = 'local, an hour earlier';       Epoch = $asLocal - 3600 }
            @{ Name = 'local, an hour later';         Epoch = $asLocal + 3600 }
        )

        # Recordings are named <minute>M<second>S_<unixtime>.mp4 inside an hour
        # directory, and Mi Home lists them a minute at a time, so a clip begins
        # on a minute boundary. Ask for the named instant exactly, and also a
        # minute either side, because it is not yet known whether the lookup
        # wants the file's own start or any instant the file covers.
        if (-not (Wait-Healthy)) {
            Write-Host "    no video on the first session, so nothing below is evidence" -ForegroundColor DarkYellow
        }

        $id = 0
        foreach ($reading in $readings) {
            foreach ($nudge in @(0, -60, 60)) {
                $id++
                if ($id -gt 1) { Reset-Session }

                $start = $reading.Epoch + $nudge
                $fields = [ordered]@{
                    sessionid        = $id
                    starttime        = $start
                    endtime          = $start + 600
                    autoswitchtolive = 1
                    offset           = 0
                    speed            = 1
                    avchannelmerge   = 1
                }
                $label = if ($nudge -eq 0) { $reading.Name } else { "{0} {1:+#;-#}s" -f $reading.Name, $nudge }
                Try-Candidate 0x10D $label ($fields | ConvertTo-Json -Compress)
            }
        }

        Write-Host "==> watching the media path" -ForegroundColor Cyan
        for ($i = 1; $i -le 8; $i++) {
            $before = Get-Frames
            Start-Sleep -Seconds 1
            $after = Get-Frames
            foreach ($reply in (Get-Replies)) { Write-Host ("      {0}" -f $reply) -ForegroundColor Green }
            Write-Host ("  {0,2}s frames +{1}" -f $i, ($after - $before)) -ForegroundColor DarkGray
        }
    }

    'hour' {
        # The fallback when a named instant misses: Mi Home offers an hour and
        # then the clips inside it, so walk that hour a minute at a time and let
        # the camera say which minutes it kept. This is the shape of the app's
        # own list, arrived at the long way round.
        if (-not $At) { throw "The hour mode needs -At, the hour to walk." }

        $wall = [DateTime]::Parse($At, [Globalization.CultureInfo]::InvariantCulture)
        $hour = [DateTime]::new($wall.Year, $wall.Month, $wall.Day, $wall.Hour, 0, 0)
        $zone = if ($AsUtc) { [TimeSpan]::Zero } else { [TimeZoneInfo]::Local.GetUtcOffset($hour) }
        $base = [DateTimeOffset]::new($hour, $zone).ToUnixTimeSeconds()

        Write-Host ("==> walking {0:yyyy-MM-dd HH}:00 minute by minute, {1}" -f $hour,
            $(if ($AsUtc) { 'read as utc' } else { 'read in the local zone' })) -ForegroundColor Cyan

        if (-not (Wait-Healthy)) {
            Write-Host "    no video on the first session, so nothing below is evidence" -ForegroundColor DarkYellow
        }

        for ($minute = 0; $minute -lt 60; $minute += $StepMinutes) {
            # Every candidate gets its own streaming session. One playback
            # request is all a session is good for, so anything cheaper than
            # this measures the session rather than the card.
            if ($minute -gt 0) { Reset-Session }

            $start = $base + ($minute * 60)
            $fields = [ordered]@{
                sessionid        = $minute + 1
                starttime        = $start
                endtime          = $start + 60
                autoswitchtolive = 1
                offset           = 0
                speed            = 1
                avchannelmerge   = 1
            }
            Try-Candidate 0x10D ("minute {0:00}" -f $minute) ($fields | ConvertTo-Json -Compress)
        }
    }

    'sweep' {
        # The request is understood; what is not known is which instant on the
        # card the camera will admit to holding. So walk back through the day and
        # let it say. autoswitchtolive is 1 here so a miss returns the session to
        # live video instead of leaving it stalled, which lets one session answer
        # every question instead of one each.
        Write-Host "==> sweeping backwards for a recording" -ForegroundColor Cyan

        $now = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
        $offset = [int][TimeZoneInfo]::Local.GetUtcOffset([DateTime]::Now).TotalSeconds

        $ages = @(
            @{ Name = 'now';          Seconds = 0 }
            @{ Name = '2 minutes';    Seconds = 120 }
            @{ Name = '10 minutes';   Seconds = 600 }
            @{ Name = '30 minutes';   Seconds = 1800 }
            @{ Name = '2 hours';      Seconds = 7200 }
            @{ Name = '6 hours';      Seconds = 21600 }
            @{ Name = '12 hours';     Seconds = 43200 }
            @{ Name = '1 day';        Seconds = 86400 }
            @{ Name = '2 days';       Seconds = 172800 }
            @{ Name = '7 days';       Seconds = 604800 }
        )

        function Send-Playback([long]$from, [long]$to, [string]$name) {
            $fields = [ordered]@{
                sessionid        = 1
                starttime        = $from
                endtime          = $to
                autoswitchtolive = 1
                offset           = 0
                speed            = 1
                avchannelmerge   = 1
            }
            Try-Candidate 0x10D $name ($fields | ConvertTo-Json -Compress)
        }

        foreach ($age in $ages) {
            $start = $now - $age.Seconds
            Send-Playback $start ($start + 300) ("utc, " + $age.Name)
        }

        if ($offset -ne 0) {
            Write-Host "==> the same walk on the camera's local clock" -ForegroundColor Cyan
            foreach ($age in $ages) {
                $start = $now - $age.Seconds + $offset
                Send-Playback $start ($start + 300) ("local, " + $age.Name)
            }
        }

        # Documented behaviour, and so a check on the reading rather than a
        # guess: a zero timestamp means go back to live.
        Send-Playback 0 0 'zero, switch to live'
    }

    'probe' {
        Write-Host "==> playback request (0x10D)" -ForegroundColor Cyan
        Write-Host "    an error reply naming a field it wanted would be the find here" -ForegroundColor DarkGray

        $now = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
        $hourAgo = $now - 3600
        $dayAgo = $now - 86400

        # Ordered cheapest-to-interpret first: a camera that rejects an empty or
        # malformed request often names the field it wanted, which is worth more
        # than any single guess landing.
        $candidates = @(
            @{ Name = 'empty body';              Body = '' }
            @{ Name = 'empty object';            Body = '{}' }
            @{ Name = 'operation, motor style';  Body = '{"operation":1}' }

            # The shape the rest of MISS uses: flat, lowercase, unseparated keys.
            @{ Name = 'starttime/endtime';       Body = "{`"starttime`":$hourAgo,`"endtime`":$now}" }
            @{ Name = 'start_time/end_time';     Body = "{`"start_time`":$hourAgo,`"end_time`":$now}" }
            @{ Name = 'time only';               Body = "{`"time`":$hourAgo}" }
            @{ Name = 'ms timestamps';           Body = "{`"starttime`":$($hourAgo * 1000),`"endtime`":$($now * 1000)}" }

            # Tuya's SDK, the nearest documented cousin, plays a clip by start,
            # stop and a point to begin at.
            @{ Name = 'start/stop/play';         Body = "{`"starttime`":$hourAgo,`"stoptime`":$now,`"playtime`":$hourAgo}" }

            # A listing request, which has to exist somewhere: nothing can ask
            # for a clip before it knows what the card holds.
            @{ Name = 'list, type key';          Body = '{"type":"list"}' }
            @{ Name = 'list, cmd key';           Body = '{"cmd":"list"}' }
            @{ Name = 'list, action key';        Body = '{"action":"list"}' }
            @{ Name = 'list, operation 0';       Body = '{"operation":0}' }
            @{ Name = 'filelist for a day';      Body = "{`"type`":`"list`",`"starttime`":$dayAgo,`"endtime`":$now}" }
            @{ Name = 'videoquality, like live'; Body = "{`"videoquality`":3,`"enableaudio`":0,`"starttime`":$hourAgo}" }
        )

        foreach ($candidate in $candidates) {
            Try-Candidate 0x10D $candidate.Name $candidate.Body
        }
    }

    'idle' {
        # The control for the probe mode: a session that sends nothing at all.
        # Anything that turns up here is the camera talking on its own, which
        # means it cannot be read as an answer to a candidate payload.
        Write-Host "==> idle session, sending nothing" -ForegroundColor Cyan
        for ($i = 1; $i -le $Repeat; $i++) {
            Start-Sleep -Milliseconds $WaitMs
            $replies = Get-Replies
            if ($replies.Count -eq 0) {
                Write-Host ("  {0,2}. quiet" -f $i) -ForegroundColor DarkGray
            } else {
                foreach ($reply in $replies) {
                    Write-Host ("  {0,2}. {1}" -f $i, $reply) -ForegroundColor Yellow
                }
            }
        }
    }

    'one' {
        Write-Host ("==> command {0:#x} x{1}: {2}" -f $Cmd, $Repeat, $Body) -ForegroundColor Cyan
        for ($i = 1; $i -le $Repeat; $i++) {
            Try-Candidate $Cmd ("attempt $i") $Body
        }
    }

    'speed' {
        Write-Host "==> playback speed (0x10F)" -ForegroundColor Cyan
        Try-Candidate 0x10F 'speed 1'      '{"speed":1}'
        Try-Candidate 0x10F 'speed 2'      '{"speed":2}'
        Try-Candidate 0x10F 'operation 1'  '{"operation":1}'
    }
    }

    Write-Host ""
    Write-Host "==> final stats" -ForegroundColor Cyan
    [XmPlay]::Command($script:handle, '{"method":"stats"}')
} finally {
    [XmPlay]::Close($script:handle)
    Write-Host "==> session closed" -ForegroundColor DarkGray
}
