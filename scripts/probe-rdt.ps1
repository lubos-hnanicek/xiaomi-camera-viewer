<#
.SYNOPSIS
    Investigates the transport channels behind SD card playback.

.DESCRIPTION
    scripts/probe-playback.ps1 established that MISS command 0x10D draws no
    answer on the command channel, whatever payload it carries. This probe asks
    a different question: whether the answer was ever meant to arrive there.

    Xiaomi's plugin SDK gives a plugin two ways to reach a camera. One is MISS
    commands, sendP2PCommandToDevice, which is what this bridge implements. The
    other is a separate RDT path -- sendRDTJSONCommandToDevice, sendRDTCommandToDevice,
    bindRDTDataReceiveCallback -- with a per-device switch named
    setCurrentDeviceUseFixedRdtChannel. RDT is the reliable file-transfer channel
    of the peer-to-peer stack CS2 is modelled on, and a recording is a file.

    CS2 multiplexes four channels over one connection. This bridge opens 0 for
    commands and 2 for media; go2rtc's talkback writes speaker audio on 3, and
    nothing anywhere writes on 1. So the two candidates for RDT are 1 and 3, and
    until now a message arriving on either was counted and dropped.

    The modes, in the order worth running them:

      watch     a session that sends nothing, so that anything the tap catches
                later can be attributed to a command rather than to the camera
      channels  the experiment this probe exists for: send 0x110, whose reply we
                already know how to read, on each channel in turn
      isolate   the same candidates, one per session, because a camera that
                hangs up on the first one makes every later result meaningless
      discriminate
                what channel 1 objects to: length, framing or content
      shortness the control for that: whether a short message is fatal on the
                command channel too, which would make channel 1 unremarkable
      length    where the length limit falls
      playback  0x10D on every channel, with the tap running
      open      candidate channel-open frames on channel 1
      one       an arbitrary channel, command, body and encryption

    The point of the channels mode is that it varies one thing. Sending an
    unknown command on an unknown channel asks two questions at once and cannot
    answer either: silence means nothing. Device info is the one undocumented
    command whose reply we can already read, so where its answer comes back --
    or whether it comes back at all -- describes the channel and not the payload.

    Nothing here writes to the camera's configuration or its card.

    Reuses the saved session in %APPDATA%\XiaomiViewer\config.json, so it needs
    no credentials, but it must run as the same Windows user that signed in.
#>
[CmdletBinding()]
param(
    [ValidateSet('watch', 'channels', 'playback', 'open', 'one', 'isolate', 'discriminate',
        'shortness', 'length', 'index', 'listen', 'ask')]
    [string]$Mode = 'watch',

    # For the index mode: the hour to ask for, as the camera names its
    # directories -- ten digits, YYYYMMDDHH.
    [string]$Hour = '',

    # The whole request payload, when the hour alone is not the question. A
    # dual-lens camera keeps two recordings per hour and the key that chooses
    # between them is not known, so the payload has to be writable by hand.
    # {0} is replaced by the hour.
    [string]$IndexJson = '',

    # For the ask mode: the hours to try, and the payload shapes to try them
    # with. Every combination is asked, in one session, because the framing is
    # already settled and only the payload is in question. {0} is the hour.
    [string[]]$Hours = @(),
    [string[]]$Payloads = @('{"time":"{0}"}'),

    # The commands to try. The CW400's parser takes 1, 5, 6 and 11 and ignores
    # everything else without a word, but that is one model's firmware and the
    # CW500 answers 6 with an empty index, so its own set has to be found rather
    # than assumed.
    [int[]]$RdtCmds = @(),

    # Which RDT command to ask it with. The parser takes 1 for a recording, 5
    # for a snapshot, 6 for a record message and 11 for the event index, and
    # ignores everything else without a word. 11 is the one known to answer;
    # 6 is the candidate for continuous footage, which the event index does not
    # cover.
    [int]$RdtCmd = 11,

    # Print what the bridge actually returned, not what this script made of it.
    [switch]$Trace,

    # For the one mode. Cmd is written as four big-endian bytes ahead of Body,
    # which is how a MISS command is framed and also a way to place an arbitrary
    # four-byte magic, "IOTC" being 0x494F5443.
    [int]$Channel = 1,
    [int]$Cmd = 0x10D,
    [string]$Body = '',
    [bool]$Encrypt = $true,
    [int]$Repeat = 4,

    [string]$Did,
    [string]$Lens = '',

    # Empty lets the camera choose, which both models answer by choosing TCP
    # anyway. Asking for TCP outright is worse rather than better: it makes the
    # handshake accept only one of the two ready messages, and a CW400 that
    # answers the other one then looks like a camera that is not there.
    #
    # TCP is what this wants regardless. A reply large enough to matter here is a
    # file listing, which spans several packets, and the command channel buffers
    # nothing out of order over UDP.
    [ValidateSet('tcp', 'udp', '')]
    [string]$Transport = '',

    [string]$DllPath,

    [int]$WaitMs = 2000,

    # How long to leave a camera alone after it has hung up, and how many times
    # to try opening before giving up. A camera that ends a session ignores the
    # discovery datagram for several seconds afterwards, and one pushed harder
    # than that starts refusing the MISS login outright with code 3.
    [int]$SettleMs = 15000,
    [int]$OpenAttempts = 5
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

public static class XmRdt
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

        // Big enough that it never has to ask twice. Reading the camera's
        // replies empties them, so a call that answers "too small" has already
        // consumed what it declined to hand over, and calling again returns
        // nothing at all. That is not a hypothetical: it is what made a
        // hundred and ninety messages of recording index look like silence.
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

[XmRdt]::Load($DllPath)

$userId = $config.account.user_id

$login = @{ user_id = $userId; region = $config.account.region; token = $token } | ConvertTo-Json -Compress
Write-Host "==> restoring session for $userId (region $($config.account.region))" -ForegroundColor Cyan
$res = [XmRdt]::Call('login.token', $login) | ConvertFrom-Json
if (-not $res.ok) { throw "login.token failed: $($res.error)" }

$res = [XmRdt]::Call('device.list', (@{ user_id = $userId } | ConvertTo-Json -Compress)) | ConvertFrom-Json
if (-not $res.ok) { throw "device.list failed: $($res.error)" }
$devices = $res.devices

function Get-Prop([string]$did, [int]$siid, [int]$piid) {
    $req = @{ user_id = $userId; did = $did; props = @(@{ siid = $siid; piid = $piid }) } |
        ConvertTo-Json -Compress -Depth 5
    try {
        $r = [XmRdt]::Call('miot.get', $req) | ConvertFrom-Json
        if (-not $r.ok) { return '' }
        $entry = $r.result | Select-Object -First 1
        if ($entry.code -ne 0) { return '' }
        return [string]$entry.value
    } catch {
        return ''
    }
}

# A camera with no card has nothing to play back, so probing one would confuse a
# refusal with an empty result.
if ($Did) {
    $device = $devices | Where-Object { $_.did -eq $Did } | Select-Object -First 1
    if (-not $device) { throw "No device with did $Did in the account." }
} else {
    $device = $devices | Where-Object { (Get-Prop $_.did 4 1) -eq '0' } | Select-Object -First 1
    if (-not $device) { throw "No camera reports a ready card. Pass -Did explicitly." }
}

Write-Host "==> probing $($device.name) [$($device.model)] did=$($device.did) transport='$Transport'" -ForegroundColor Cyan

$open = @{
    user_id   = $userId
    did       = $device.did
    model     = $device.model
    ip        = $device.ip
    channel   = $Lens
    transport = $Transport
} | ConvertTo-Json -Compress

# Opening retries, because a camera that has just hung up on us stops answering
# the discovery datagram for a while and then comes back on its own. Without
# this, the first candidate that ends a session also ends the run, and the
# candidates after it go untested for a reason that has nothing to do with them.
function Open-Session {
    for ($attempt = 1; ; $attempt++) {
        $response = ''
        $handle = [XmRdt]::Open($open, [ref]$response)
        if ($handle -ne [IntPtr]::Zero) {
            Write-Host "    stream open: $response" -ForegroundColor DarkGray

            # An index is rebuilt from channel 1 as a byte stream, and UDP
            # reorders and loses. The result is not a degraded reading but a
            # meaningless one: the length prefix lands on the wrong bytes and
            # every message after it is misframed. Better said out loud than
            # mistaken for a camera with nothing to say.
            if ($response -match 'cs2\+udp' -and $Mode -in @('index', 'ask', 'listen')) {
                Write-Host "    WARNING: this session is UDP, so channel 1 cannot be reassembled" -ForegroundColor Red
                Write-Host "             pass -Transport tcp; a scrambled stream reads as silence" -ForegroundColor Red
            }

            # Anything said before the first candidate is not an answer to it,
            # so it goes. Except in listen mode, where it is the whole point:
            # the camera pushes its recording index on connect, unasked, and
            # this drain is what threw it away every time it was sent.
            if ($Mode -ne 'listen') {
                [XmRdt]::Command($handle, '{"method":"replies"}') | Out-Null
            }
            return $handle
        }

        if ($attempt -ge $OpenAttempts) {
            throw "stream open failed after $attempt attempts: $response"
        }
        Write-Host ("    open failed, waiting {0}s and trying again" -f ($SettleMs / 1000)) -ForegroundColor DarkYellow
        Start-Sleep -Milliseconds $SettleMs
    }
}

function Send-Channel([IntPtr]$handle, [int]$channel, [int]$cmd, [string]$body, [bool]$encrypt,
    [bool]$envelope = $true) {
    $req = @{
        method   = 'miss.channel'
        channel  = $channel
        cmd      = $cmd
        body     = $body
        encrypt  = $encrypt
        envelope = $envelope
    } | ConvertTo-Json -Compress
    try {
        [XmRdt]::Command($handle, $req) | Out-Null
    } catch {
        Write-Host "      send failed: $_" -ForegroundColor Red
    }
}

# Replies are what came back on the command channel; the tap is what came back
# anywhere else. Both drain on read, so they have to be read together or one
# call would swallow what the other was about to report.
function Read-Answers([IntPtr]$handle) {
    $raw = ''
    try {
        $raw = [XmRdt]::Command($handle, '{"method":"replies"}')
    } catch {
        # Not nothing: a read that throws and is treated as silence is how a
        # camera answering at length comes to look like a camera saying nothing.
        Write-Host ("      read failed: {0}" -f $_.Exception.Message) -ForegroundColor Red
        return @{ Replies = @(); Tap = @() }
    }

    if ($Trace) {
        Write-Host ("      raw answer: {0} chars, {1}" -f $raw.Length,
            $raw.Substring(0, [Math]::Min(160, $raw.Length))) -ForegroundColor DarkGray
    }

    $r = $raw | ConvertFrom-Json
    if (-not $r.ok) {
        Write-Host ("      read refused: {0}" -f $raw.Substring(0, [Math]::Min(300, $raw.Length))) -ForegroundColor Red
        return @{ Replies = @(); Tap = @() }
    }

    # An absent list wrapped in @() is a list holding one nothing, which reads
    # as an answer and prints as a blank line.
    $replies = if ($null -eq $r.replies) { @() } else { @($r.replies) }
    $tap = if ($null -eq $r.tap) { @() } else { @($r.tap) }
    $counts = if ($null -eq $r.unhandled) { @() } else { @($r.unhandled) }
    return @{ Replies = $replies; Tap = $tap; Unhandled = $counts }
}

# Frames and the session's own verdict on itself. A camera that hangs up stops
# the frames, and so does a camera that simply has nothing to send, so the error
# is what tells those apart.
function Get-Health([IntPtr]$handle) {
    $r = [XmRdt]::Command($handle, '{"method":"stats"}') | ConvertFrom-Json
    if (-not $r.ok) { return @{ Frames = -1; Error = 'stats failed' } }
    return @{ Frames = [int64]$r.frames; Error = [string]$r.error }
}

# A session that is not carrying video cannot measure whether a candidate stopped
# the video. Opening one and sending immediately is not enough: the earlier runs
# produced rows with no frames on either side of the candidate, which say nothing
# about the candidate at all. So a session is only handed out once it has been
# seen to carry frames.
function Open-HealthySession {
    for ($attempt = 1; ; $attempt++) {
        $handle = Open-Session

        $before = (Get-Health $handle).Frames
        Start-Sleep -Milliseconds $WaitMs
        $health = Get-Health $handle

        if (-not $health.Error -and ($health.Frames - $before) -gt 0) {
            Write-Host ("    warm: {0} frames before the candidate" -f ($health.Frames - $before)) -ForegroundColor DarkGray
            return $handle
        }

        $why = if ($health.Error) { $health.Error } else { 'no video' }
        Write-Host ("    session unusable ({0}), discarding it" -f $why) -ForegroundColor DarkYellow
        [XmRdt]::Close($handle)

        if ($attempt -ge $OpenAttempts) {
            throw "no healthy session after $attempt attempts"
        }
        Start-Sleep -Milliseconds $SettleMs
    }
}

# Reports where an answer came back, which is the whole measurement: the command
# channel, another channel, or nowhere. Returns whether the session survived.
function Try-Send([IntPtr]$handle, [int]$channel, [int]$cmd, [string]$body, [bool]$encrypt, [string]$name) {
    $before = (Get-Health $handle).Frames
    Send-Channel $handle $channel $cmd $body $encrypt
    Start-Sleep -Milliseconds $WaitMs
    $answers = Read-Answers $handle
    $health = Get-Health $handle
    $after = $health.Frames

    $wrapping = if ($encrypt) { 'encrypted' } else { 'plain' }
    $label = "ch{0} cmd={1:x} {2}" -f $channel, $cmd, $wrapping
    Write-Host ("  {0,-28} {1,-22} {2}" -f $name, $label, $body) -ForegroundColor Gray

    foreach ($reply in $answers.Replies) {
        Write-Host ("      command channel: {0}" -f $reply) -ForegroundColor Green
    }
    foreach ($msg in $answers.Tap) {
        Write-Host ("      elsewhere:       {0}" -f $msg) -ForegroundColor Yellow
    }
    if ($answers.Replies.Count -eq 0 -and $answers.Tap.Count -eq 0) {
        Write-Host ("      no answer, frames +{0}" -f ($after - $before)) -ForegroundColor DarkGray
    } else {
        Write-Host ("      frames +{0}" -f ($after - $before)) -ForegroundColor DarkGray
    }

    # Three outcomes, not two. A camera can drop the connection outright, and it
    # can also stop sending video while the connection is still open, which the
    # CW400 does and the CW500 does not. Both mean the candidate was refused;
    # collapsing them would hide which refusal a model uses.
    if ($health.Error) {
        Write-Host ("      SESSION ENDED: {0}" -f $health.Error) -ForegroundColor Red
        return 'ended'
    }
    if (($after - $before) -le 0) {
        Write-Host "      MEDIA STOPPED, connection not yet closed" -ForegroundColor Red
        return 'media-stopped'
    }
    return 'alive'
}

# Sends one RDT request in the framing the index mode established -- command as
# four little-endian bytes, then the payload length, then the payload, the whole
# encrypted and written to channel 1 with no envelope id -- and waits out the
# answer. Waiting has to outlast the camera's own thinking time: a full card
# takes seconds to walk before the first message appears, so quiet at the start
# means nothing and only quiet after it has begun speaking means it has finished.
function Get-RdtReply([IntPtr]$handle, [int]$rdtCmd, [byte[]]$payload, [int]$minRounds = 4) {
    # Every byte travels as a character in a JSON string, so none may be above
    # 0x7f: the bridge encodes the body as UTF-8 and anything higher would
    # arrive as two bytes and shift everything after it.
    $raw = -join ($(
        [BitConverter]::GetBytes([int]$payload.Length) | ForEach-Object { [char]$_ }
        $payload | ForEach-Object { [char]$_ }
    ))

    Send-Channel $handle 1 ($rdtCmd * 16777216) $raw $true $false

    $tap = @()
    $quiet = 0
    for ($round = 0; $round -lt 40 -and ($round -lt $minRounds -or $quiet -lt 3); $round++) {
        Start-Sleep -Milliseconds ([Math]::Max($WaitMs, 1500))
        $slice = Read-Answers $handle
        if ($slice.Tap.Count -eq 0) { $quiet++ } else { $quiet = 0 }
        $tap += $slice.Tap
    }
    return $tap
}

# The candidates the channels and isolate modes share, so that running them one
# per session tests the same things as running them all in one.
$channelCandidates = @(
    # 0 answers today, so it proves the session and the reader are working.
    @{ Channel = 0; Cmd = 0x110; Body = ''; Encrypt = $true;  Name = 'devinfo, control' }
    # 1 is the channel nothing is known to use.
    @{ Channel = 1; Cmd = 0x110; Body = ''; Encrypt = $true;  Name = 'devinfo, encrypted' }
    @{ Channel = 1; Cmd = 0x110; Body = ''; Encrypt = $false; Name = 'devinfo, plain' }
    # 3 carries speaker audio away from us, so a reply on it would be a surprise
    # worth having. 2 is left alone: it is the media channel, and writing on it
    # would disturb the video these measurements count frames from.
    @{ Channel = 3; Cmd = 0x110; Body = ''; Encrypt = $true;  Name = 'devinfo, encrypted' }
    @{ Channel = 3; Cmd = 0x110; Body = ''; Encrypt = $false; Name = 'devinfo, plain' }
)

# --- isolate: one session per candidate -------------------------------------
#
# Everything else in this script shares one session across its candidates, which
# is only sound while the camera tolerates them. It does not: a single message on
# channel 1 is enough to stop the media flow, and every later result on that
# session then measures a dead connection rather than the candidate.

# --- discriminate: what channel 1 objects to --------------------------------
#
# Channel 1 takes an encrypted command silently and refuses a plaintext one. The
# two differ in more than encryption, so on its own that does not say what the
# camera is checking. Three explanations fit, and one candidate each separates
# them:
#
#   length     the plaintext refusal is four bytes and the encrypted one sixteen,
#              so a parser wanting a minimum header would refuse the short one
#              whatever it contained. A sixteen-byte plaintext message tests it.
#   framing    the encrypted form begins with the 0x1001 envelope id. Sending
#              that id followed by bytes that are not a valid ciphertext keeps
#              the framing and throws away the content: the cipher is
#              unauthenticated, so the camera cannot reject it as forged, only as
#              a command id that decrypts to nonsense.
#   content    if neither of those is tolerated but a real encrypted command is,
#              the camera wants something it can actually decrypt and recognise.
$discriminateCandidates = @(
    @{ Cmd = 0x110;  Body = '';                 Encrypt = $true;  Name = 'encrypted, control' }
    @{ Cmd = 0x110;  Body = '';                 Encrypt = $false; Name = 'plain, 4 bytes' }
    @{ Cmd = 0x110;  Body = 'AAAAAAAAAAAA';     Encrypt = $false; Name = 'plain, 16 bytes' }
    @{ Cmd = 0x1001; Body = 'AAAAAAAAAAAA';     Encrypt = $false; Name = 'envelope, nonsense inside' }
)

# --- shortness: is a four-byte message fatal anywhere? ----------------------
#
# The control the channel modes never ran. Every message this bridge has ever
# sent on channel 0 is encrypted and therefore at least sixteen bytes, so a short
# one has never been tried there. If channel 0 dies of it too, then nothing about
# channel 1 is special: the camera simply cannot parse a data message that short,
# whichever channel carries it, and the earlier reading of channel 1 as a channel
# that inspects what it is sent does not survive.
$shortnessChannels = @(0, 1, 3)

if ($Mode -eq 'shortness') {
    Write-Host "==> a four-byte plaintext message on each channel" -ForegroundColor Cyan
    Write-Host "    channel 0 is the control: it is the one channel known to work" -ForegroundColor DarkGray

    foreach ($channel in $shortnessChannels) {
        $handle = Open-HealthySession
        try {
            $null = Try-Send $handle $channel 0x110 '' $false ("4 bytes on channel $channel")
        } finally {
            [XmRdt]::Close($handle)
        }

        Start-Sleep -Milliseconds $SettleMs
    }

    Write-Host "==> done" -ForegroundColor DarkGray
    return
}

# --- length: where the limit falls ------------------------------------------
#
# Four bytes is refused and sixteen is accepted. Where the boundary sits says
# what the far end is reading: a header it wants whole, or simply more than
# nothing.
if ($Mode -eq 'length') {
    Write-Host ("==> plaintext messages of growing length on channel {0}" -f $Channel) -ForegroundColor Cyan

    foreach ($extra in 0, 2, 4, 6, 8) {
        $body = 'A' * $extra
        $handle = Open-HealthySession
        try {
            $null = Try-Send $handle $Channel 0x110 $body $false ("{0} bytes" -f (4 + $extra))
        } finally {
            [XmRdt]::Close($handle)
        }

        Start-Sleep -Milliseconds $SettleMs
    }

    Write-Host "==> done" -ForegroundColor DarkGray
    return
}

if ($Mode -eq 'discriminate') {
    Write-Host "==> what channel 1 objects to, a fresh session per candidate" -ForegroundColor Cyan
    Write-Host "    16 bytes is the length of the encrypted control, so only the wrapping differs" -ForegroundColor DarkGray

    foreach ($candidate in $discriminateCandidates) {
        $handle = Open-HealthySession
        try {
            $null = Try-Send $handle 1 $candidate.Cmd $candidate.Body `
                $candidate.Encrypt $candidate.Name
        } finally {
            [XmRdt]::Close($handle)
        }

        Start-Sleep -Milliseconds $SettleMs
    }

    Write-Host "==> done" -ForegroundColor DarkGray
    return
}

if ($Mode -eq 'isolate') {
    Write-Host "==> device info (0x110), a fresh session per candidate" -ForegroundColor Cyan
    Write-Host "    each line is measured against a camera that has heard nothing else" -ForegroundColor DarkGray

    foreach ($candidate in $channelCandidates) {
        $handle = Open-HealthySession
        try {
            $outcome = Try-Send $handle $candidate.Channel $candidate.Cmd $candidate.Body `
                $candidate.Encrypt $candidate.Name

            # A session that survived the message is worth watching a moment
            # longer: a file listing takes longer to produce than a reply does.
            if ($outcome -eq 'alive') {
                Start-Sleep -Milliseconds $WaitMs
                $answers = Read-Answers $handle
                foreach ($reply in $answers.Replies) {
                    Write-Host ("      late, command channel: {0}" -f $reply) -ForegroundColor Green
                }
                foreach ($msg in $answers.Tap) {
                    Write-Host ("      late, elsewhere:       {0}" -f $msg) -ForegroundColor Yellow
                }
                $health = Get-Health $handle
                if ($health.Error) {
                    Write-Host ("      SESSION ENDED: {0}" -f $health.Error) -ForegroundColor Red
                } else {
                    Write-Host "      session still alive" -ForegroundColor DarkGreen
                }
            }
        } finally {
            [XmRdt]::Close($handle)
        }

        Start-Sleep -Milliseconds $SettleMs
    }

    Write-Host "==> done" -ForegroundColor DarkGray
    return
}

$handle = Open-Session

try {
    switch ($Mode) {
    'watch' {
        # The control. Anything that turns up while nothing is being sent is the
        # camera talking on its own and cannot be read as an answer to anything.
        Write-Host "==> idle session, sending nothing" -ForegroundColor Cyan
        for ($i = 1; $i -le $Repeat; $i++) {
            Start-Sleep -Milliseconds $WaitMs
            $answers = Read-Answers $handle
            if ($answers.Replies.Count -eq 0 -and $answers.Tap.Count -eq 0) {
                Write-Host ("  {0,2}. quiet" -f $i) -ForegroundColor DarkGray
            } else {
                foreach ($reply in $answers.Replies) {
                    Write-Host ("  {0,2}. command channel: {1}" -f $i, $reply) -ForegroundColor Yellow
                }
                foreach ($msg in $answers.Tap) {
                    Write-Host ("  {0,2}. elsewhere:       {1}" -f $i, $msg) -ForegroundColor Yellow
                }
            }
        }
    }

    'listen' {
        # Ask nothing and keep everything. The camera sends its recording index
        # on connect without being asked, and every earlier run threw it away
        # in the first quarter second of the session.
        Write-Host "==> saying nothing, keeping whatever the camera sends" -ForegroundColor Cyan

        $dump = Join-Path (Split-Path -Parent $PSScriptRoot) 'build\rdt'
        New-Item -ItemType Directory -Force -Path $dump | Out-Null
        $path = Join-Path $dump ("listen-{0}.txt" -f (Get-Date -Format 'yyyyMMdd-HHmmss'))

        $quiet = 0
        for ($round = 0; $round -lt 30 -and ($round -lt 6 -or $quiet -lt 3); $round++) {
            Start-Sleep -Milliseconds ([Math]::Max($WaitMs, 1000))
            $slice = Read-Answers $handle
            if ($slice.Tap.Count -eq 0 -and $slice.Replies.Count -eq 0) { $quiet++; continue }
            $quiet = 0

            $slice.Tap | Add-Content -Path $path
            foreach ($msg in $slice.Tap) {
                foreach ($line in ($msg -split "`n")) {
                    if ($line -match '^\s*hex:') { continue }
                    $trimmed = $line.Trim()
                    if ($trimmed.Length -gt 260) { $trimmed = $trimmed.Substring(0, 260) + ' ...' }
                    Write-Host ("  {0}" -f $trimmed) -ForegroundColor Green
                }
            }
        }
        Write-Host ("==> full text in {0}" -f $path) -ForegroundColor DarkGray
    }

    'ask' {
        # One session, many questions. The framing is settled, so re-deriving it
        # for every payload costs a session apiece and proves nothing; what is
        # not settled is which hour the camera thinks it has, and on a two-lens
        # camera, how it is asked for one lens rather than the other.
        if ($Hours.Count -eq 0) {
            $Hours = 0..5 | ForEach-Object { (Get-Date).AddHours(-$_).ToString('yyyyMMddHH') }
        }
        if ($RdtCmds.Count -eq 0) { $RdtCmds = @($RdtCmd) }

        Write-Host ("==> {0} command(s) x {1} hour(s) x {2} payload(s), one session" -f
            $RdtCmds.Count, $Hours.Count, $Payloads.Count) -ForegroundColor Cyan

        $dump = Join-Path (Split-Path -Parent $PSScriptRoot) 'build\rdt'
        New-Item -ItemType Directory -Force -Path $dump | Out-Null
        $path = Join-Path $dump ("ask-cmd{0}-{1}.txt" -f $RdtCmd, (Get-Date -Format 'yyyyMMdd-HHmmss'))

        foreach ($askCmd in $RdtCmds) {
            foreach ($hour in $Hours) {
                foreach ($shape in $Payloads) {
                    $json = $shape -replace '\{0\}', $hour

                    # Not every request is JSON. The CW500 reads the channel for
                    # its recording index as four raw bytes at offset 8 of the
                    # payload, so that payload has to be writable as bytes.
                    $payload = if ($json -like 'hex:*') {
                        $digits = $json.Substring(4)
                        [byte[]]@(0..($digits.Length / 2 - 1) | ForEach-Object {
                            [Convert]::ToByte($digits.Substring($_ * 2, 2), 16)
                        })
                    } else {
                        [System.Text.Encoding]::ASCII.GetBytes($json)
                    }

                    $health = Get-Health $handle
                    if ($health.Error) {
                        Write-Host "    session is gone, opening another" -ForegroundColor DarkYellow
                        [XmRdt]::Close($handle)
                        Start-Sleep -Milliseconds $SettleMs
                        $handle = Open-Session
                    }

                    $tap = Get-RdtReply $handle $askCmd $payload

                    ("=== cmd {0} {1}" -f $askCmd, $json) | Add-Content -Path $path
                    $tap | Add-Content -Path $path

                    # One line per question: what was asked and the size of what
                    # came back. A non-zero size is the whole finding; the bytes
                    # themselves are in the file.
                    # An index runs to hundreds of messages, so listing them one
                    # per line buries the answer. What matters per question is
                    # how many came back and how much they carried.
                    $count = 0
                    $bytes = 0
                    foreach ($msg in $tap) {
                        if ($msg -match 'rdt message, cmd (\d+), (\d+) bytes') {
                            $count++
                            $bytes += [int]$Matches[2]
                        }
                    }

                    $verdict = if ($count -eq 0) { 'silence' }
                        else { "{0} message(s), {1} bytes" -f $count, $bytes }

                    $colour = if ($bytes -gt 0) { 'Green' } else { 'DarkGray' }
                    Write-Host ("  cmd {0,-3} {1,-42} {2}" -f $askCmd, $json, $verdict) -ForegroundColor $colour
                }
            }
        }

        Write-Host ("==> full text in {0}" -f $path) -ForegroundColor DarkGray
    }

    'index' {
        # What the firmware says an RDT message is: a four byte command, a four
        # byte payload length, then the payload. Command 11 asks for a recording
        # index and its payload is JSON with a time in it, which the camera finds
        # by searching for "time", stepping over the seven characters of
        # time":" and reading digits until the closing quote. Ten digits name an
        # hour, and an hour is what Mi Home makes you pick before it shows you
        # clips.
        #
        # The camera's own sender, cs2_rdt_send, writes channel 1 with the
        # session's encryption and no envelope id in front, unlike the command
        # channel. What it does not settle is which way round the two integers
        # go, so that is asked both ways; the plaintext readings are kept as a
        # control.
        if (-not $Hour) { $Hour = (Get-Date).AddHours(-2).ToString('yyyyMMddHH') }
        Write-Host "==> asking channel $Channel, command $RdtCmd, for hour $Hour" -ForegroundColor Cyan

        $json = if ($IndexJson) { $IndexJson -replace '\{0\}', $Hour } else { '{"time":"' + $Hour + '"}' }
        Write-Host ("    payload: {0}" -f $json) -ForegroundColor DarkGray
        $length = $json.Length

        # A four byte integer, written both ways round. Cmd rides in the same
        # place a MISS command id would, so it is already big-endian on the wire;
        # the little-endian reading of 11 is the same bytes reversed.
        $cmdBig = $RdtCmd
        $cmdLittle = $RdtCmd * 16777216

        function New-Payload([int]$value, [bool]$bigEndian) {
            $bytes = [BitConverter]::GetBytes([int]$value)
            if ($bigEndian) { [array]::Reverse($bytes) }
            return (($bytes | ForEach-Object { '\u{0:x4}' -f $_ }) -join '')
        }

        # The bridge takes the body as a JSON string, so the length bytes travel
        # as escapes and the JSON that follows is plain text.
        $candidates = @(
            @{ Name = 'little-endian, as the camera sends'; Cmd = $cmdLittle; Big = $false; Encrypt = $true;  Envelope = $false }
            @{ Name = 'big-endian, as the camera sends';    Cmd = $cmdBig;    Big = $true;  Encrypt = $true;  Envelope = $false }
            @{ Name = 'little-endian, plaintext';           Cmd = $cmdLittle; Big = $false; Encrypt = $false; Envelope = $false }
            @{ Name = 'little-endian, with envelope';       Cmd = $cmdLittle; Big = $false; Encrypt = $true;  Envelope = $true }
        )

        foreach ($candidate in $candidates) {
            $escaped = New-Payload $length $candidate.Big
            $body = ($escaped -replace '\\u00([0-9a-f]{2})', '\u00$1')

            # Rebuild the body as a real string: the escapes above are what the
            # bridge's JSON decoder turns back into the four length bytes.
            $raw = -join ($(
                $bytes = [BitConverter]::GetBytes([int]$length)
                if ($candidate.Big) { [array]::Reverse($bytes) }
                $bytes | ForEach-Object { [char]$_ }
            )) + $json

            $before = Get-Health $handle
            Send-Channel $handle $Channel $candidate.Cmd $raw $candidate.Encrypt $candidate.Envelope

            # A record index runs to hundreds of messages, so one read is not
            # the answer -- it is the first slice of it. Keep reading until the
            # camera goes quiet twice over.
            # Listening has to outlast the camera's own thinking time. A full
            # card takes seconds to walk before the first message appears, so
            # quiet at the start means nothing yet; only quiet after it has
            # begun speaking means it has finished.
            $replies = @()
            $tap = @()
            $quiet = 0
            for ($round = 0; $round -lt 30 -and ($round -lt 8 -or $quiet -lt 3); $round++) {
                Start-Sleep -Milliseconds ([Math]::Max($WaitMs, 2000))
                $slice = Read-Answers $handle
                if ($slice.Unhandled.Count -gt 1) {
                    Write-Host ("      round {0,2}: channel 1 has seen {1}, this read carried {2}" -f
                        $round, $slice.Unhandled[1], $slice.Tap.Count) -ForegroundColor DarkGray
                }
                if ($slice.Replies.Count -eq 0 -and $slice.Tap.Count -eq 0) { $quiet++ } else { $quiet = 0 }
                $replies += $slice.Replies
                $tap += $slice.Tap
            }
            $answers = @{ Replies = $replies; Tap = $tap }
            $after = Get-Health $handle

            Write-Host ("  {0,-28} cmd=0x{1:x8} len={2}" -f $candidate.Name, $candidate.Cmd, $length) -ForegroundColor Gray
            if ($answers.Replies.Count -eq 0 -and $answers.Tap.Count -eq 0) {
                Write-Host ("      nothing, frames +{0} {1}" -f ($after.Frames - $before.Frames), $after.Error) -ForegroundColor DarkGray
            } else {
                foreach ($reply in $answers.Replies) {
                    Write-Host ("      command channel: {0}" -f $reply) -ForegroundColor Green
                }
                # A whole index is far too much to read on a terminal, so the
                # bulk goes to a file and only what identifies each message is
                # printed. The hex is what a later reader will want.
                $dump = Join-Path (Split-Path -Parent $PSScriptRoot) 'build\rdt'
                New-Item -ItemType Directory -Force -Path $dump | Out-Null
                $path = Join-Path $dump ("rdt-{0}-cmd{1}-{2}.txt" -f $Hour, $RdtCmd,
                    (Get-Date -Format 'HHmmss'))
                $answers.Tap | Set-Content -Path $path

                foreach ($msg in $answers.Tap) {
                    foreach ($line in ($msg -split "`n")) {
                        if ($line -match '^\s*hex:') { continue }
                        $trimmed = $line.Trim()
                        if ($trimmed.Length -gt 300) { $trimmed = $trimmed.Substring(0, 300) + ' ...' }
                        Write-Host ("      {0}" -f $trimmed) -ForegroundColor Green
                    }
                }
                Write-Host ("      full text written to {0}" -f $path) -ForegroundColor DarkGray
            }

            if ($after.Error) {
                Write-Host "      session is gone, opening another" -ForegroundColor DarkYellow
                [XmRdt]::Close($handle)
                Start-Sleep -Milliseconds $SettleMs
                $handle = Open-Session
            }

            # The list exists to find the framing, and the framing is found the
            # moment one candidate draws a reply on the RDT channel. Sending the
            # malformed rest afterwards costs a minute and teaches nothing.
            if ($answers.Tap.Count -gt 0) {
                Write-Host "      that framing works, so the remaining candidates are skipped" -ForegroundColor DarkGray
                break
            }
        }
    }

    'channels' {
        Write-Host "==> device info (0x110) on each channel, one session" -ForegroundColor Cyan
        Write-Host "    0 is the control and must answer; the question is what the others do" -ForegroundColor DarkGray
        Write-Host "    use -Mode isolate instead once a candidate has ended a session" -ForegroundColor DarkGray

        foreach ($candidate in $channelCandidates) {
            $null = Try-Send $handle $candidate.Channel $candidate.Cmd $candidate.Body `
                $candidate.Encrypt $candidate.Name
        }
    }

    'playback' {
        Write-Host "==> playback request (0x10D) off the command channel" -ForegroundColor Cyan
        Write-Host "    the payloads are the ones the command channel ignored" -ForegroundColor DarkGray

        $now = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
        $hourAgo = $now - 3600

        foreach ($channel in 1, 3) {
            $null = Try-Send $handle $channel 0x10D '' $true 'empty body'
            $null = Try-Send $handle $channel 0x10D '{}' $true 'empty object'
            $null = Try-Send $handle $channel 0x10D "{`"starttime`":$hourAgo,`"endtime`":$now}" $true 'starttime/endtime'
            $null = Try-Send $handle $channel 0x10D '{"type":"list"}' $false 'list, plain'
        }
    }

    'open' {
        # An RDT channel is created rather than simply used, so a camera that
        # ignores everything on channel 1 may be waiting to be asked. The frame
        # that asks is not documented; what is documented is that an RDT frame
        # begins with the four bytes "IOTC", which is what these send as the
        # command id.
        Write-Host "==> channel-open candidates on channel 1" -ForegroundColor Cyan
        Write-Host "    'IOTC' is 0x494f5443, the RDT frame magic" -ForegroundColor DarkGray

        $null = Try-Send $handle 1 0x494F5443 '' $false 'IOTC, empty'
        $null = Try-Send $handle 1 0x494F5443 'GC' $false 'IOTC, GC footer'
        $null = Try-Send $handle 1 0x00000000 '' $false 'four zero bytes'
        $null = Try-Send $handle 1 0x10D '' $false 'playback id, plain'
    }

    'one' {
        Write-Host ("==> channel {0} command {1:x} x{2}: {3}" -f $Channel, $Cmd, $Repeat, $Body) -ForegroundColor Cyan
        for ($i = 1; $i -le $Repeat; $i++) {
            $null = Try-Send $handle $Channel $Cmd $Body $Encrypt ("attempt $i")
        }
    }
    }

    Write-Host ""
    Write-Host "==> final stats" -ForegroundColor Cyan
    [XmRdt]::Command($handle, '{"method":"stats"}')

    # Cumulative, unlike the tap: this counts everything that arrived off the
    # command and media channels for the whole session, including whatever was
    # drained above.
    $final = [XmRdt]::Command($handle, '{"method":"replies"}') | ConvertFrom-Json
    if ($final.unhandled) {
        Write-Host "==> off-channel totals" -ForegroundColor Cyan
        foreach ($line in $final.unhandled) {
            Write-Host "    $line" -ForegroundColor Magenta
        }
    }
} finally {
    [XmRdt]::Close($handle)
    Write-Host "==> session closed" -ForegroundColor DarkGray
}
