<#
.SYNOPSIS
    Tests whether both lenses of a Xiaomi dual-lens camera share one MISS session.

.DESCRIPTION
    Opens the primary lens, then sends a second video-start command with both
    videoquality and videoquality2 enabled:

        {"videoquality":Q,"videoquality2":Q,"enableaudio":0}

    Media access units from that one session are captured as Annex-B video.
    Sequence/timestamp regressions and ffprobe output are reported, and ffmpeg
    builds a contact sheet for visual confirmation. A clean contact sheet that
    alternates between the two views would prove that both lenses arrived.

    The running viewer is closed first so it cannot consume the CW500's two
    connection slots. No camera settings or saved configuration are changed.
#>
[CmdletBinding()]
param(
    [string]$Did,
    [ValidatePattern('^[0-5]$')]
    [string]$Quality = '3',
    [ValidateRange(5, 120)]
    [int]$Seconds = 15,
    [string]$DllPath,
    [string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $DllPath) {
    $DllPath = Join-Path $repoRoot 'build\msvc\RelWithDebInfo\xmbridge.dll'
}
if (-not (Test-Path $DllPath)) {
    throw "xmbridge.dll not found at $DllPath. Build first with scripts/build.ps1."
}
$DllPath = (Resolve-Path $DllPath).Path

$ffprobe = Join-Path $repoRoot 'third_party\ffmpeg\bin\ffprobe.exe'
$ffmpeg = Join-Path $repoRoot 'third_party\ffmpeg\bin\ffmpeg.exe'
if (-not (Test-Path $ffprobe)) { throw "ffprobe not found at $ffprobe." }
if (-not (Test-Path $ffmpeg)) { throw "ffmpeg not found at $ffmpeg." }

$configPath = Join-Path $env:APPDATA 'XiaomiViewer\config.json'
if (-not (Test-Path $configPath)) {
    throw "No config at $configPath. Sign in with the app first."
}

if (-not $OutputDirectory) {
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $OutputDirectory = Join-Path $env:TEMP "xiaomi-dual-session-$stamp"
}
$OutputDirectory = [System.IO.Path]::GetFullPath($OutputDirectory)
[System.IO.Directory]::CreateDirectory($OutputDirectory) | Out-Null

# A concurrent viewer session would make this experiment inconclusive.
Get-Process XiaomiViewer -ErrorAction SilentlyContinue | ForEach-Object {
    Write-Host '==> closing the running Xiaomi Viewer' -ForegroundColor Cyan
    $_.Kill()
    $_.WaitForExit()
}
Start-Sleep -Milliseconds 750

$config = Get-Content $configPath -Raw | ConvertFrom-Json
$stored = $config.account.token
if (-not $stored -or -not $stored.StartsWith('dpapi:')) {
    throw 'No saved login token in the config.'
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

[StructLayout(LayoutKind.Sequential)]
public struct XmDualFrame
{
    public int Kind;
    public int Codec;
    public int Keyframe;
    public int SampleRate;
    public long PtsMs;
    public uint Sequence;
    public uint Size;
}

public static class XmDual
{
    [DllImport("kernel32", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern IntPtr LoadLibrary(string path);

    [DllImport("kernel32", SetLastError = true)]
    private static extern IntPtr GetProcAddress(IntPtr module, string name);

    private delegate int CallFn(string method, string request, byte[] outBuf, int cap);
    private delegate IntPtr OpenFn(string request, byte[] outBuf, int cap);
    private delegate int ReadFn(IntPtr handle, byte[] buffer, int cap, ref XmDualFrame frame);
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
        IntPtr proc = GetProcAddress(module, name);
        if (proc == IntPtr.Zero) throw new Exception("missing export " + name);
        return proc;
    }

    public static string Call(string method, string request)
    {
        CallFn fn = (CallFn)Marshal.GetDelegateForFunctionPointer(Proc("xmb_call"), typeof(CallFn));
        byte[] output = new byte[262144];
        int count = fn(method, request, output, output.Length);
        if (count < 0) throw new Exception(method + " failed with code " + count);
        return Encoding.UTF8.GetString(output, 0, count);
    }

    public static IntPtr Open(string request, out string response)
    {
        OpenFn fn = (OpenFn)Marshal.GetDelegateForFunctionPointer(
            Proc("xmb_stream_open"), typeof(OpenFn));
        byte[] output = new byte[65536];
        IntPtr handle = fn(request, output, output.Length);
        response = Encoding.UTF8.GetString(output).TrimEnd('\0');
        return handle;
    }

    public static int Read(IntPtr handle, byte[] buffer, ref XmDualFrame frame)
    {
        ReadFn fn = (ReadFn)Marshal.GetDelegateForFunctionPointer(
            Proc("xmb_stream_read"), typeof(ReadFn));
        return fn(handle, buffer, buffer.Length, ref frame);
    }

    public static string Command(IntPtr handle, string request)
    {
        CommandFn fn = (CommandFn)Marshal.GetDelegateForFunctionPointer(
            Proc("xmb_stream_command"), typeof(CommandFn));
        byte[] output = new byte[65536];
        int count = fn(handle, request, output, output.Length);
        if (count < 0) throw new Exception("stream command failed with code " + count);
        return Encoding.UTF8.GetString(output, 0, count);
    }

    public static void Close(IntPtr handle)
    {
        CloseFn fn = (CloseFn)Marshal.GetDelegateForFunctionPointer(
            Proc("xmb_stream_close"), typeof(CloseFn));
        fn(handle);
    }
}
"@ -Language CSharp

[XmDual]::Load($DllPath)

$userId = $config.account.user_id
$login = @{
    user_id = $userId
    region  = $config.account.region
    token   = $token
} | ConvertTo-Json -Compress

Write-Host "==> restoring Xiaomi session (region $($config.account.region))" -ForegroundColor Cyan
$response = [XmDual]::Call('login.token', $login) | ConvertFrom-Json
if (-not $response.ok) { throw "login.token failed: $($response.error)" }

$request = @{ user_id = $userId } | ConvertTo-Json -Compress
$response = [XmDual]::Call('device.list', $request) | ConvertFrom-Json
if (-not $response.ok) { throw "device.list failed: $($response.error)" }

if ($Did) {
    $device = $response.devices | Where-Object { $_.did -eq $Did } | Select-Object -First 1
    if (-not $device) { throw "No device with did $Did was found." }
} else {
    $candidates = @($response.devices | Where-Object {
        $_.model -match '(?i)(hlmax|500dh|cw500)'
    })
    if ($candidates.Count -eq 0) {
        throw 'No CW500/dual-lens camera was found. Pass -Did explicitly.'
    }
    if ($candidates.Count -gt 1) {
        $names = ($candidates | ForEach-Object { "$($_.name) [$($_.model)] did=$($_.did)" }) -join '; '
        throw "More than one dual-lens camera was found. Pass -Did explicitly: $names"
    }
    $device = $candidates[0]
}

Write-Host "==> probing $($device.name) [$($device.model)]" -ForegroundColor Cyan
Write-Host "    output: $OutputDirectory" -ForegroundColor DarkGray

$openRequest = @{
    user_id = $userId
    did     = $device.did
    model   = $device.model
    ip      = $device.ip
    channel = ''
    quality = $Quality
    audio   = $false
} | ConvertTo-Json -Compress

$openResponse = ''
$handle = [XmDual]::Open($openRequest, [ref]$openResponse)
if ($handle -eq [IntPtr]::Zero) { throw "stream open failed: $openResponse" }
Write-Host "    stream open: $openResponse" -ForegroundColor DarkGray

$h264Path = Join-Path $OutputDirectory 'both-lenses.h264'
$h265Path = Join-Path $OutputDirectory 'both-lenses.h265'
$metadataPath = Join-Path $OutputDirectory 'frames.csv'
$h264 = $null
$h265 = $null
$metadata = [System.IO.StreamWriter]::new(
    $metadataPath, $false, [System.Text.UTF8Encoding]::new($false))
$metadata.WriteLine('index,codec,keyframe,pts_ms,sequence,size')

$videoFrames = 0
$keyframes = 0
$sequenceRegressions = 0
$timestampRegressions = 0
$lastSequence = $null
$lastTimestamp = $null

try {
    $body = "{`"videoquality`":$Quality,`"videoquality2`":$Quality,`"enableaudio`":0}"
    $command = @{ method = 'miss.raw'; cmd = 0x102; body = $body } |
        ConvertTo-Json -Compress
    Write-Host "==> requesting both lenses in the existing session: $body" -ForegroundColor Cyan
    $commandResponse = [XmDual]::Command($handle, $command) | ConvertFrom-Json
    if (-not $commandResponse.ok) { throw "video-start command failed: $($commandResponse.error)" }

    $buffer = New-Object byte[] (4MB)
    $deadline = (Get-Date).AddSeconds($Seconds)

    while ((Get-Date) -lt $deadline) {
        $frame = New-Object XmDualFrame
        $count = [XmDual]::Read($handle, $buffer, [ref]$frame)
        if ($count -eq -2) {
            $buffer = New-Object byte[] ([Math]::Max([int]$frame.Size, $buffer.Length * 2))
            continue
        }
        if ($count -lt 0) {
            Write-Warning "stream ended with code $count"
            break
        }
        if ($frame.Kind -ne 1) { continue }

        $videoFrames++
        if ($frame.Keyframe -ne 0) { $keyframes++ }
        if ($null -ne $lastSequence -and $frame.Sequence -lt $lastSequence) {
            $sequenceRegressions++
        }
        if ($null -ne $lastTimestamp -and $frame.PtsMs -lt $lastTimestamp) {
            $timestampRegressions++
        }
        $lastSequence = $frame.Sequence
        $lastTimestamp = $frame.PtsMs
        $metadata.WriteLine("$videoFrames,$($frame.Codec),$($frame.Keyframe),$($frame.PtsMs),$($frame.Sequence),$count")

        switch ($frame.Codec) {
            1 {
                if ($null -eq $h264) { $h264 = [System.IO.File]::Create($h264Path) }
                $h264.Write($buffer, 0, $count)
            }
            2 {
                if ($null -eq $h265) { $h265 = [System.IO.File]::Create($h265Path) }
                $h265.Write($buffer, 0, $count)
            }
        }
    }
} finally {
    if ($null -ne $h264) { $h264.Dispose() }
    if ($null -ne $h265) { $h265.Dispose() }
    $metadata.Dispose()
    [XmDual]::Close($handle)
}

Write-Host ''
Write-Host '==> capture summary' -ForegroundColor Cyan
[PSCustomObject]@{
    VideoFrames          = $videoFrames
    Keyframes            = $keyframes
    SequenceRegressions  = $sequenceRegressions
    TimestampRegressions = $timestampRegressions
} | Format-List

$videoPath = if (Test-Path $h265Path) { $h265Path } elseif (Test-Path $h264Path) { $h264Path } else { $null }
if (-not $videoPath) {
    throw 'The combined request produced no supported video packets.'
}

Write-Host '==> ffprobe stream analysis' -ForegroundColor Cyan
& $ffprobe -v warning -show_entries 'stream=index,codec_name,width,height' -of json $videoPath

$contactSheet = Join-Path $OutputDirectory 'contact-sheet.jpg'
Write-Host '==> extracting a six-frame contact sheet' -ForegroundColor Cyan
& $ffmpeg -hide_banner -loglevel quiet -y -i $videoPath `
    -vf 'fps=1/2,scale=640:-2,tile=3x2' -frames:v 1 -update 1 $contactSheet

if (Test-Path $contactSheet) {
    Write-Host "Contact sheet: $contactSheet" -ForegroundColor Green
} else {
    Write-Warning 'ffmpeg could not decode a contact sheet; inspect its warnings and frames.csv.'
}
