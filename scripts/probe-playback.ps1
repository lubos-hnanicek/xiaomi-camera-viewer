<#
.SYNOPSIS
    Investigates whether a camera will play back its SD card recordings.

.DESCRIPTION
    MISS names a playback command pair (0x10D request, 0x10E response) and a
    playback speed command (0x10F), but no payload for any of them is documented
    and no implementation is public: go2rtc declares two of the three constants
    and sends none of them, and Xiaomi's plugin SDK lists the names with the
    doc comments shifted by one and no schema at all.

    So the shape has to be found the way the motor payload was: send candidates
    to a live camera and read what comes back. Every mode here is a question:

      status   is there a card, is it recording, how full is it
      devinfo  what the camera says about itself (0x110), which is the one
               undocumented command whose reply we can already read
      probe    what 0x10D answers to each candidate payload
      speed    what 0x10F answers, only worth running once probe finds a shape

    Nothing here writes to the camera's configuration or its card, and no
    candidate asks for a delete or a format.

    Reuses the saved session in %APPDATA%\XiaomiViewer\config.json, so it needs
    no credentials, but it must run as the same Windows user that signed in.
#>
[CmdletBinding()]
param(
    [ValidateSet('status', 'devinfo', 'probe', 'speed', 'idle', 'one')]
    [string]$Mode = 'status',

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
        byte[] buffer = new byte[262144];
        int needed = fn(handle, request, buffer, buffer.Length);
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
$handle = [XmPlay]::Open($open, [ref]$response)
if ($handle -eq [IntPtr]::Zero) { throw "stream open failed: $response" }
Write-Host "    stream open: $response" -ForegroundColor DarkGray

try {
    # Anything the camera said before the first candidate is not an answer to it.
    [XmPlay]::Command($handle, '{"method":"replies"}') | Out-Null

    function Send-Raw([int]$cmd, [string]$body) {
        $req = @{ method = 'miss.raw'; cmd = $cmd; body = $body } | ConvertTo-Json -Compress
        try {
            [XmPlay]::Command($handle, $req) | Out-Null
        } catch {
            Write-Host "    send failed: $_" -ForegroundColor Red
        }
    }

    # The bridge answers with the payload merged into the top-level object next
    # to ok, not nested under a result. Reading drains the log, so both halves
    # have to come out of the same call or one of them would swallow the other.
    $script:lastUnhandled = ''
    function Get-Replies {
        $r = [XmPlay]::Command($handle, '{"method":"replies"}') | ConvertFrom-Json
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
        $r = [XmPlay]::Command($handle, '{"method":"stats"}') | ConvertFrom-Json
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
    [XmPlay]::Command($handle, '{"method":"stats"}')
} finally {
    [XmPlay]::Close($handle)
    Write-Host "==> session closed" -ForegroundColor DarkGray
}
