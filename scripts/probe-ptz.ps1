<#
.SYNOPSIS
    Finds the motor command payload a camera accepts.

.DESCRIPTION
    The MISS motor command (0x112) carries a JSON payload whose shape is not
    documented anywhere and differs between models. go2rtc declares the command
    but never sends it, so there is no upstream implementation to copy.

    This sends candidate payloads to a live camera and reports, for each one,
    what the camera replied and whether the lens actually moved. Movement is
    judged by the camera's own position readback (hl-get-location, siid 6 piid
    12) rather than by the reply, because a camera that does not understand a
    payload typically answers nothing at all -- and one that answers is not
    necessarily moving.

    The camera will physically pan and tilt while this runs.

    Reuses the saved session in %APPDATA%\XiaomiViewer\config.json, so it needs
    no credentials, but it must run as the same Windows user that signed in.
#>
[CmdletBinding()]
param(
    # step       - that the shipping ptz.step command moves the right way
    # soak       - that a long burst of commands does not drop the stream
    # payload    - which JSON shape the motor command accepts
    # directions - what each operation number does
    # motion     - whether one command is a step or runs until told to stop
    # stop       - which payload halts a move in progress
    [ValidateSet('payload', 'directions', 'motion', 'stop', 'step', 'soak')]
    [string]$Mode = 'payload',
    # Which camera to probe. Defaults to the first one the cloud reports with a
    # motorised model.
    [string]$Did,
    # Lens to open. The fixed lens of a dual-lens model has no motor.
    [string]$Channel = '',
    [string]$DllPath,
    # How long to hold each candidate before reading the position back.
    [int]$HoldMs = 1500
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

$source = @"
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class XmPtz
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
        byte[] buffer = new byte[65536];
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
        byte[] buffer = new byte[65536];
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
"@

Add-Type -TypeDefinition $source -Language CSharp
[XmPtz]::Load($DllPath)

$userId = $config.account.user_id

$login = @{ user_id = $userId; region = $config.account.region; token = $token } | ConvertTo-Json -Compress
Write-Host "==> restoring session for $userId (region $($config.account.region))" -ForegroundColor Cyan
$res = [XmPtz]::Call('login.token', $login) | ConvertFrom-Json
if (-not $res.ok) { throw "login.token failed: $($res.error)" }

$res = [XmPtz]::Call('device.list', (@{ user_id = $userId } | ConvertTo-Json -Compress)) | ConvertFrom-Json
if (-not $res.ok) { throw "device.list failed: $($res.error)" }

# Models known to have a motor. Only used to pick a default target; an explicit
# -Did is taken as given.
$motorised = @('isa.camera.hlc8', 'isa.camera.hlc8a', 'isa.camera.500dh', 'isa.camera.hlmax')

if ($Did) {
    $device = $res.devices | Where-Object { $_.did -eq $Did } | Select-Object -First 1
    if (-not $device) { throw "No device with did $Did in the account." }
} else {
    $device = $res.devices | Where-Object { $motorised -contains $_.model } | Select-Object -First 1
    if (-not $device) { throw "No motorised camera found. Pass -Did explicitly." }
}

Write-Host "==> probing $($device.name) [$($device.model)] did=$($device.did) channel='$Channel'" -ForegroundColor Cyan

# Reads the camera's reported pan/tilt position. This is the ground truth for
# whether a payload did anything.
function Get-Position {
    $req = @{
        user_id = $userId
        did     = $device.did
        props   = @(@{ siid = 6; piid = 12 })
    } | ConvertTo-Json -Compress -Depth 5

    try {
        $r = [XmPtz]::Call('miot.get', $req) | ConvertFrom-Json
        if (-not $r.ok) { return "error: $($r.error)" }
        $entry = $r.result | Select-Object -First 1
        if ($entry.code -ne 0) { return "unsupported (code $($entry.code))" }
        return [string]$entry.value
    } catch {
        return "error: $_"
    }
}

# Sends a raw motor payload. The camera does not acknowledge these, so a
# timeout is the expected outcome and is not reported as a failure.
function Send-Motor([IntPtr]$handle, [string]$body) {
    try {
        [XmPtz]::Command($handle, (@{ method = 'ptz.raw'; body = $body } | ConvertTo-Json -Compress)) | Out-Null
    } catch { }
}

$open = @{
    user_id = $userId
    did     = $device.did
    model   = $device.model
    ip      = $device.ip
    channel = $Channel
} | ConvertTo-Json -Compress

$response = ''
$handle = [XmPtz]::Open($open, [ref]$response)
if ($handle -eq [IntPtr]::Zero) {
    throw "stream open failed: $response"
}
Write-Host "    stream open: $response" -ForegroundColor DarkGray

function Get-Point([string]$position) {
    try {
        $p = $position | ConvertFrom-Json
        return @{ X = [int]$p.x; Y = [int]$p.y }
    } catch {
        return $null
    }
}

# Each candidate is a payload for command 0x112. The direction is the same
# throughout (a pan, which moves the reported angle the most) so the only
# variable is the shape.
$payloadCandidates = @(
    @{ Name = 'direction+speed (string)';   Body = '{"direction":"left","speed":5}' },
    @{ Name = 'direction only (string)';    Body = '{"direction":"left"}' },
    @{ Name = 'operation (ipc019 style)';   Body = '{"operation":1}' },
    @{ Name = 'operation+speed';            Body = '{"operation":1,"speed":5}' },
    @{ Name = 'direction numeric';          Body = '{"direction":1,"speed":5}' },
    @{ Name = 'nested motor';               Body = '{"motor":{"operation":1}}' },
    @{ Name = 'direction+speed+time';       Body = '{"direction":"left","speed":5,"time":500}' },
    @{ Name = 'pan/tilt delta';             Body = '{"pan":-10,"tilt":0}' },
    @{ Name = 'x/y delta';                  Body = '{"x":-10,"y":0}' },
    @{ Name = 'cmd+speed';                  Body = '{"cmd":"left","speed":5}' }
)

# Payloads that might halt a move in progress.
$stopCandidates = @(
    @{ Name = 'operation 0';        Body = '{"operation":0}' },
    @{ Name = 'operation 5';        Body = '{"operation":5}' },
    @{ Name = 'operation 255';      Body = '{"operation":255}' },
    @{ Name = 'operation -1';       Body = '{"operation":-1}' },
    @{ Name = 'direction stop';     Body = '{"direction":"stop","speed":0}' },
    @{ Name = 'empty object';       Body = '{}' },
    @{ Name = 'stop flag';          Body = '{"stop":1}' }
)

function Probe-Payload {
    $results = @()

    foreach ($candidate in $payloadCandidates) {
        $start = Get-Position
        Send-Motor $handle $candidate.Body
        Start-Sleep -Milliseconds $HoldMs
        Send-Motor $handle '{"operation":0}'
        Start-Sleep -Milliseconds 500

        $end = Get-Position
        $moved = ($start -ne $end) -and ($start -notlike 'error*') -and ($start -notlike 'unsupported*')

        $results += [PSCustomObject]@{
            Candidate = $candidate.Name
            Moved     = if ($moved) { 'YES' } else { 'no' }
            From      = $start
            To        = $end
        }

        $colour = if ($moved) { 'Green' } else { 'DarkGray' }
        Write-Host ("    {0,-28} moved={1,-4} {2} -> {3}" -f `
            $candidate.Name, $(if ($moved) { 'YES' } else { 'no' }), $start, $end) -ForegroundColor $colour
    }

    Write-Host ""
    $results | Format-Table -AutoSize

    $winners = $results | Where-Object { $_.Moved -eq 'YES' }
    if ($winners) {
        Write-Host "Payload shapes that moved the lens:" -ForegroundColor Green
        $winners | ForEach-Object { Write-Host "  $($_.Candidate)" -ForegroundColor Green }
    } else {
        Write-Host "Nothing moved the lens. If every position read said 'unsupported'," -ForegroundColor Yellow
        Write-Host "the camera does not report position and movement must be watched." -ForegroundColor Yellow
    }
}

function Probe-Directions {
    Write-Host "    one pulse per operation, no stop command, position allowed to settle" -ForegroundColor DarkGray
    Write-Host "    a zero delta on the tilt axis can also mean it is against an end stop" -ForegroundColor DarkGray
    Write-Host ""

    $results = @()

    foreach ($op in 0..8) {
        $start = Get-Point (Get-Position)
        Send-Motor $handle "{`"operation`":$op}"
        Start-Sleep -Milliseconds 1800
        $end = Get-Point (Get-Position)

        if (-not $start -or -not $end) {
            Write-Host "    operation $op : position unreadable" -ForegroundColor Yellow
            continue
        }

        $dx = $end.X - $start.X
        $dy = $end.Y - $start.Y

        $axis = 'none'
        if ($dx -ne 0 -and $dy -eq 0) { $axis = if ($dx -gt 0) { 'pan +x' } else { 'pan -x' } }
        elseif ($dy -ne 0 -and $dx -eq 0) { $axis = if ($dy -gt 0) { 'tilt +y' } else { 'tilt -y' } }
        elseif ($dx -ne 0 -and $dy -ne 0) { $axis = 'both' }

        $results += [PSCustomObject]@{
            Operation = $op
            Axis      = $axis
            DX        = $dx
            DY        = $dy
            From      = "$($start.X),$($start.Y)"
            To        = "$($end.X),$($end.Y)"
        }

        $colour = if ($axis -eq 'none') { 'DarkGray' } else { 'Green' }
        Write-Host ("    operation {0}  {1,-8} dx={2,6} dy={3,6}" -f $op, $axis, $dx, $dy) -ForegroundColor $colour
    }

    Write-Host ""
    $results | Format-Table -AutoSize
}

function Probe-Motion {
    Write-Host "    pulsing one operation repeatedly, with no stop command at all," -ForegroundColor DarkGray
    Write-Host "    and letting the reported position settle between pulses" -ForegroundColor DarkGray
    Write-Host ""

    foreach ($op in 1, 2) {
        Write-Host "    operation $op" -ForegroundColor Cyan
        $previous = Get-Point (Get-Position)

        for ($i = 1; $i -le 8; $i++) {
            Send-Motor $handle "{`"operation`":$op}"
            Start-Sleep -Milliseconds 1500

            $now = Get-Point (Get-Position)
            if (-not $now) {
                Write-Host "      pulse $i : position unreadable" -ForegroundColor Yellow
                continue
            }
            Write-Host ("      pulse {0}  x={1,6} y={2,6}  step={3,6}" -f `
                $i, $now.X, $now.Y, ($now.X - $previous.X))
            $previous = $now
        }
        Write-Host ""
    }

    Write-Host "    A steady non-zero step per pulse means one command is one step and" -ForegroundColor Cyan
    Write-Host "    no stop is needed. A first pulse that runs to an end stop and then" -ForegroundColor Cyan
    Write-Host "    reports zero means movement is continuous." -ForegroundColor Cyan
}

function Probe-Stop {
    foreach ($candidate in $stopCandidates) {
        # Re-centre so there is always room to move away from an end stop.
        Send-Motor $handle '{"operation":2}'
        Start-Sleep -Seconds 2
        Send-Motor $handle '{"operation":0}'
        Start-Sleep -Milliseconds 800

        Send-Motor $handle '{"operation":1}'
        Start-Sleep -Milliseconds 400
        Send-Motor $handle $candidate.Body

        Start-Sleep -Milliseconds 600
        $first = Get-Point (Get-Position)
        Start-Sleep -Milliseconds 1200
        $second = Get-Point (Get-Position)

        if (-not $first -or -not $second) {
            Write-Host "    $($candidate.Name): position unreadable" -ForegroundColor Yellow
            continue
        }

        $halted = $first.X -eq $second.X
        $colour = if ($halted) { 'Green' } else { 'DarkGray' }
        Write-Host ("    {0,-20} halted={1,-4} x {2} -> {3}" -f `
            $candidate.Name, $(if ($halted) { 'YES' } else { 'no' }), $first.X, $second.X) -ForegroundColor $colour

        Send-Motor $handle '{"operation":0}'
        Start-Sleep -Milliseconds 500
    }

    Write-Host ""
    Write-Host "    'halted' only means something if the move was still running when" -ForegroundColor Cyan
    Write-Host "    the candidate was sent, so check the motion mode first." -ForegroundColor Cyan
}

# Exercises the command the application actually sends, so a passing run covers
# the whole path: direction name, operation mapping and transport.
function Probe-Step {
    Write-Host "    sending the shipping ptz.step command in each direction" -ForegroundColor DarkGray
    Write-Host ""

    # The camera's coordinates run opposite to the view on both axes: x grows as
    # the lens turns left and y grows as it tilts down.
    $expected = @(
        @{ Direction = 'right'; Axis = 'X'; Sign = -1 },
        @{ Direction = 'left';  Axis = 'X'; Sign = 1 },
        @{ Direction = 'up';    Axis = 'Y'; Sign = -1 },
        @{ Direction = 'down';  Axis = 'Y'; Sign = 1 }
    )

    $failures = 0

    foreach ($case in $expected) {
        $start = Get-Point (Get-Position)

        $sendError = ''
        try {
            $raw = [XmPtz]::Command($handle, (@{ method = 'ptz.step'; direction = $case.Direction } | ConvertTo-Json -Compress))
            $parsed = $raw | ConvertFrom-Json
            if (-not $parsed.ok) { $sendError = [string]$parsed.error }
        } catch {
            $sendError = "$_"
        }

        Start-Sleep -Milliseconds 1800
        $end = Get-Point (Get-Position)

        if ($sendError) {
            Write-Host "    $($case.Direction): rejected: $sendError" -ForegroundColor Red
            $failures++
            continue
        }
        if (-not $start -or -not $end) {
            Write-Host "    $($case.Direction): position unreadable" -ForegroundColor Yellow
            continue
        }

        $delta = if ($case.Axis -eq 'X') { $end.X - $start.X } else { $end.Y - $start.Y }
        $ok = ($delta * $case.Sign) -gt 0

        # The other axis must not move, or a direction is driving the wrong motor.
        $crosstalk = if ($case.Axis -eq 'X') { $end.Y - $start.Y } else { $end.X - $start.X }
        if ($crosstalk -ne 0) { $ok = $false }

        if (-not $ok) { $failures++ }
        $colour = if ($ok) { 'Green' } else { 'Red' }
        Write-Host ("    {0,-6} d{1}={2,6} other axis={3,4}  {4}" -f `
            $case.Direction, $case.Axis, $delta, $crosstalk,
            $(if ($ok) { 'as expected' } else { 'WRONG' })) -ForegroundColor $colour
    }

    Write-Host ""
    if ($failures -eq 0) {
        Write-Host "    All four directions moved the lens the way they are labelled." -ForegroundColor Green
    } else {
        Write-Host "    $failures direction(s) did not behave as labelled." -ForegroundColor Red
    }
}

# Every command the camera answers puts a message on a transport channel with
# room for only a handful, and an overflow there is fatal to the session. A burst
# of commands is what makes that show up, so this checks the stream is still
# delivering frames afterwards.
function Probe-Soak {
    $count = 40
    Write-Host "    sending $count steps back to back, then checking the stream survived" -ForegroundColor DarkGray

    $before = ([XmPtz]::Command($handle, '{"method":"stats"}') | ConvertFrom-Json).frames

    for ($i = 0; $i -lt $count; $i++) {
        $direction = if ($i % 2 -eq 0) { 'right' } else { 'left' }
        try {
            $raw = [XmPtz]::Command($handle, (@{ method = 'ptz.step'; direction = $direction } | ConvertTo-Json -Compress))
            $parsed = $raw | ConvertFrom-Json
            if (-not $parsed.ok) {
                Write-Host "    step $i rejected: $($parsed.error)" -ForegroundColor Red
                return
            }
        } catch {
            Write-Host "    step $i threw: $_" -ForegroundColor Red
            return
        }
        Start-Sleep -Milliseconds 150
    }

    Start-Sleep -Seconds 2

    $stats = [XmPtz]::Command($handle, '{"method":"stats"}') | ConvertFrom-Json
    Write-Host ""
    Write-Host "    frames before: $before   after: $($stats.frames)   dropped: $($stats.dropped)"
    Write-Host "    messages taken off the command channel: $($stats.replies)"
    Write-Host "    last one: $($stats.last_reply)"

    if ($stats.frames -gt $before) {
        Write-Host "    Stream still delivering after $count commands." -ForegroundColor Green
    } else {
        Write-Host "    Stream stopped delivering; the command channel probably overflowed." -ForegroundColor Red
    }
}

try {
    Write-Host "    position: $(Get-Position)" -ForegroundColor DarkGray
    Write-Host ""

    switch ($Mode) {
        'step'       { Probe-Step }
        'soak'       { Probe-Soak }
        'payload'    { Probe-Payload }
        'directions' { Probe-Directions }
        'motion'     { Probe-Motion }
        'stop'       { Probe-Stop }
    }
} finally {
    Send-Motor $handle '{"operation":0}'
    [XmPtz]::Close($handle)
}
