<#
.SYNOPSIS
    Looks for a field in the media header that says which lens sent a packet.

.DESCRIPTION
    A dual-lens camera interleaves both pictures on one media channel. The
    session tells them apart by their sequence numbers, which works only as long
    as the counters stay where they were last seen, and gets the two tiles the
    wrong way round when they do not.

    A field naming the lens would settle it outright. Only 20 of the 32 header
    bytes have a known meaning, so this opens both lenses, captures the headers
    as they arrive with the lane each was routed to, and reports every byte and
    every 32-bit word whose value separates the two lanes.

    Read the result as follows. A field listed as a clean discriminator holds one
    value for one lens and a different single value for the other, throughout the
    capture: that is the camera naming the lens. If nothing is listed, the header
    does not carry the lens and the routing has to stay a matter of inference.

    Reuses the saved session in %APPDATA%\XiaomiViewer\config.json, changes no
    camera settings, and closes the running viewer first so it cannot occupy the
    camera's two viewer slots.
#>
[CmdletBinding()]
param(
    [string]$Did,
    [ValidatePattern('^[0-5]$')]
    [string]$Quality = '3',
    [ValidateRange(5, 300)]
    [int]$Seconds = 20,
    # A different profile for the second lens, which is what a per-tile quality
    # override produces. The lens has to keep its own tile even though the
    # camera is encoding it differently.
    [ValidatePattern('^[0-5]$')]
    [string]$SecondaryQuality,

    [ValidateRange(1, 20)]
    [int]$Rounds = 1,

    # Capture each lens on its own at every quality profile and report the tag,
    # which is what says which bits of it name the lens and which only describe
    # the encoding. A profile the model does not support simply sends nothing.
    [switch]$QualitySweep,

    [string]$DllPath
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

$configPath = Join-Path $env:APPDATA 'XiaomiViewer\config.json'
if (-not (Test-Path $configPath)) {
    throw "No config at $configPath. Sign in with the app first."
}

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

public static class XmLens
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

    public static string Command(IntPtr handle, string request)
    {
        CommandFn fn = (CommandFn)Marshal.GetDelegateForFunctionPointer(
            Proc("xmb_stream_command"), typeof(CommandFn));
        byte[] output = new byte[1048576];
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

[XmLens]::Load($DllPath)

$userId = $config.account.user_id
$login = @{ user_id = $userId; region = $config.account.region; token = $token } |
    ConvertTo-Json -Compress

Write-Host "==> restoring Xiaomi session (region $($config.account.region))" -ForegroundColor Cyan
$response = [XmLens]::Call('login.token', $login) | ConvertFrom-Json
if (-not $response.ok) { throw "login.token failed: $($response.error)" }

$response = [XmLens]::Call('device.list', (@{ user_id = $userId } | ConvertTo-Json -Compress)) |
    ConvertFrom-Json
if (-not $response.ok) { throw "device.list failed: $($response.error)" }

if ($Did) {
    $device = $response.devices | Where-Object { $_.did -eq $Did } | Select-Object -First 1
    if (-not $device) { throw "No device with did $Did was found." }
} else {
    $candidates = @($response.devices | Where-Object { $_.model -match '(?i)(hlmax|500dh|cw500)' })
    if ($candidates.Count -ne 1) {
        throw "Expected exactly one dual-lens camera, found $($candidates.Count). Pass -Did."
    }
    $device = $candidates[0]
}

Write-Host "==> probing $($device.name) [$($device.model)]" -ForegroundColor Cyan

$currentQuality = $Quality

function Open-Lens([string]$channel) {
    $useQuality = $currentQuality
    if (-not $QualitySweep -and $SecondaryQuality -and $channel -ne '') {
        $useQuality = $SecondaryQuality
    }

    $request = @{
        user_id = $userId
        did     = $device.did
        model   = $device.model
        ip      = $device.ip
        channel = $channel
        quality = $useQuality
        audio   = $false
    } | ConvertTo-Json -Compress

    $openResponse = ''
    $handle = [XmLens]::Open($request, [ref]$openResponse)
    if ($handle -eq [IntPtr]::Zero) { throw "stream open (channel '$channel') failed: $openResponse" }
    return [PSCustomObject]@{ Handle = $handle; Response = $openResponse }
}

function Get-Samples([string[]]$channels, [int]$seconds) {
    $handles = @()
    try {
        foreach ($channel in $channels) {
            $opened = Open-Lens $channel
            $label = if ($channel -eq '') { 'lens 1' } else { "lens 2" }
            Write-Host "    $label`: $($opened.Response)" -ForegroundColor DarkGray
            $handles += $opened
        }

        Write-Host "    capturing for $seconds seconds" -ForegroundColor DarkGray
        Start-Sleep -Seconds $seconds

        $result = [XmLens]::Command($handles[0].Handle, (@{ method = 'media.headers' } |
            ConvertTo-Json -Compress)) | ConvertFrom-Json
        if (-not $result.ok) { throw "media.headers failed: $($result.error)" }
    } finally {
        for ($i = $handles.Count - 1; $i -ge 0; $i--) { [XmLens]::Close($handles[$i].Handle) }
    }

    # A camera that has just hung up ignores the next connection for a moment.
    Start-Sleep -Seconds 2

    $samples = @()
    foreach ($line in $result.headers) {
        if ($line -match '^lane (\d+): ([0-9a-f]+)$') {
            $hex = $matches[2]
            $bytes = for ($i = 0; $i -lt $hex.Length; $i += 2) {
                [Convert]::ToByte($hex.Substring($i, 2), 16)
            }
            $samples += [PSCustomObject]@{
                Lane  = [int]$matches[1]
                Bytes = @($bytes)
                Hex   = $hex
                # The top half of the flags word, which the single-lens phases
                # below establish as the camera's name for the lens.
                Tag   = '{0:x2}{1:x2}' -f $bytes[15], $bytes[14]
            }
        }
    }
    return $samples
}

function Show-ByteAnalysis($samples) {
    $width = $samples[0].Bytes.Count
    $lane0 = @($samples | Where-Object Lane -eq 0)
    $lane1 = @($samples | Where-Object Lane -eq 1)

    $rows = @()
    for ($i = 0; $i -lt $width; $i++) {
        $v0 = @($lane0 | ForEach-Object { $_.Bytes[$i] } | Sort-Object -Unique)
        $v1 = @($lane1 | ForEach-Object { $_.Bytes[$i] } | Sort-Object -Unique)
        $overlap = @($v0 | Where-Object { $v1 -contains $_ })

        $verdict = if ($overlap.Count -eq 0) {
            if ($v0.Count -eq 1 -and $v1.Count -eq 1) { 'CONSTANT AND DISTINCT' } else { 'disjoint' }
        } else { '' }

        $rows += [PSCustomObject]@{
            Offset  = $i
            Field   = switch ($i) {
                { $_ -lt 4 }                 { 'unread' }
                { $_ -ge 4 -and $_ -lt 8 }   { 'codec' }
                { $_ -ge 8 -and $_ -lt 12 }  { 'sequence' }
                { $_ -ge 12 -and $_ -lt 16 } { 'flags' }
                { $_ -ge 16 -and $_ -lt 24 } { 'timestamp' }
                default                      { 'unread' }
            }
            Lane0   = if ($v0.Count -le 4) { ($v0 | ForEach-Object { $_.ToString('x2') }) -join ',' } else { "$($v0.Count) values" }
            Lane1   = if ($v1.Count -le 4) { ($v1 | ForEach-Object { $_.ToString('x2') }) -join ',' } else { "$($v1.Count) values" }
            Verdict = $verdict
        }
    }
    $rows | Format-Table -AutoSize
}

if ($QualitySweep) {
    $sweep = @()
    foreach ($profile in 0..5) {
        $currentQuality = "$profile"
        Write-Host ""
        Write-Host "===== quality profile $profile =====" -ForegroundColor Yellow

        $row = [ordered]@{ Profile = $profile; Primary = '(silent)'; Secondary = '(silent)' }
        foreach ($lens in @{ Name = 'Primary'; Channel = '' }, @{ Name = 'Secondary'; Channel = '1' }) {
            try {
                $captured = Get-Samples @($lens.Channel) $Seconds
                $tags = @($captured.Tag | Sort-Object -Unique)
                if ($tags.Count -gt 0) { $row[$lens.Name] = $tags -join '/' }
            } catch {
                $row[$lens.Name] = '(failed)'
            }
        }
        $sweep += [PSCustomObject]$row
    }

    Write-Host ""
    Write-Host "==> flags tag per lens and profile" -ForegroundColor Cyan
    $sweep | Format-Table -AutoSize

    # Whatever stays the same for a lens across every profile, and differs
    # between the lenses, is the part that names the lens.
    $usable = @($sweep | Where-Object { $_.Primary -match '^[0-9a-f]{4}$' -and $_.Secondary -match '^[0-9a-f]{4}$' })
    if ($usable.Count -lt 2) {
        Write-Warning 'Too few profiles produced video to separate the lens bits from the encoding bits.'
        return
    }

    $differ = 0xFFFF
    $primaryVaries = 0
    foreach ($row in $usable) {
        $differ = $differ -band ([Convert]::ToInt32($row.Primary, 16) -bxor [Convert]::ToInt32($row.Secondary, 16))
        $primaryVaries = $primaryVaries -bor
            ([Convert]::ToInt32($usable[0].Primary, 16) -bxor [Convert]::ToInt32($row.Primary, 16))
    }

    Write-Host ("    bits that always differ between the lenses: 0x{0:x4}" -f $differ) -ForegroundColor Green
    Write-Host ("    bits that move with the profile:            0x{0:x4}" -f $primaryVaries)
    Write-Host ("    lens mask (differs between, steady within): 0x{0:x4}" -f ($differ -band -bnot $primaryVaries)) -ForegroundColor Green
    return
}

# --- Phase 1 and 2: each lens alone, which is the ground truth ---------------
#
# Before the combined command there is only one picture on the connection, so
# whatever the header says there belongs to the lens that was asked for. Nothing
# about the interleaving, and nothing about the routing under test, takes part.

Write-Host ""
Write-Host "===== phase 1: the primary lens alone =====" -ForegroundColor Yellow
$alonePrimary = Get-Samples @('') $Seconds
Write-Host "    $($alonePrimary.Count) headers, flags tag(s): $((($alonePrimary.Tag | Sort-Object -Unique)) -join ', ')"

Write-Host ""
Write-Host "===== phase 2: the secondary lens alone =====" -ForegroundColor Yellow
$aloneSecondary = Get-Samples @('1') $Seconds
Write-Host "    $($aloneSecondary.Count) headers, flags tag(s): $((($aloneSecondary.Tag | Sort-Object -Unique)) -join ', ')"

$primaryTags = @($alonePrimary.Tag | Sort-Object -Unique)
$secondaryTags = @($aloneSecondary.Tag | Sort-Object -Unique)

Write-Host ""
Write-Host "==> ground truth" -ForegroundColor Cyan
if ($primaryTags.Count -ne 1 -or $secondaryTags.Count -ne 1) {
    Write-Warning 'A lens on its own did not use a single flags tag; it does not name the lens.'
    return
}
if ($primaryTags[0] -eq $secondaryTags[0]) {
    Write-Warning "Both lenses use tag $($primaryTags[0]); the field does not identify the lens."
    return
}
Write-Host "    primary lens   = $($primaryTags[0])" -ForegroundColor Green
Write-Host "    secondary lens = $($secondaryTags[0])" -ForegroundColor Green

# --- Phase 3: both lenses, repeatedly ----------------------------------------
#
# The swap is intermittent, so one agreeing round proves nothing. Each round is
# a fresh session, which is what re-runs the decision that goes wrong.

$swaps = 0
for ($round = 1; $round -le $Rounds; $round++) {
    Write-Host ""
    Write-Host "===== phase 3, round $round of $Rounds`: both lenses =====" -ForegroundColor Yellow
    $both = Get-Samples @('', '1') $Seconds

    if ($both.Count -eq 0) { Write-Warning 'Nothing captured.'; continue }
    $byLane = $both | Group-Object Lane
    if ($byLane.Count -lt 2) {
        Write-Warning 'Only one lane saw traffic; the second lens never joined.'
        continue
    }

    if ($round -eq 1) {
        Write-Host ""
        Show-ByteAnalysis $both
    }

    $lane0Tags = @(($both | Where-Object Lane -eq 0).Tag | Sort-Object -Unique)
    $lane1Tags = @(($both | Where-Object Lane -eq 1).Tag | Sort-Object -Unique)

    $verdict = 'MIXED'
    if ($lane0Tags.Count -eq 1 -and $lane1Tags.Count -eq 1) {
        if ($lane0Tags[0] -eq $primaryTags[0] -and $lane1Tags[0] -eq $secondaryTags[0]) {
            $verdict = 'correct'
        } elseif ($lane0Tags[0] -eq $secondaryTags[0] -and $lane1Tags[0] -eq $primaryTags[0]) {
            $verdict = 'SWAPPED'
            $swaps++
        }
    } else {
        $swaps++
    }

    $colour = if ($verdict -eq 'correct') { 'Green' } else { 'Red' }
    foreach ($lane in 0, 1) {
        $expected = if ($lane -eq 0) { $primaryTags[0] } else { $secondaryTags[0] }
        $counts = $both | Where-Object Lane -eq $lane | Group-Object Tag |
            Sort-Object Name | ForEach-Object {
                $mark = if ($_.Name -eq $expected) { '' } else { '  <- wrong lens' }
                "{0}x{1}{2}" -f $_.Count, $_.Name, $mark
            }
        Write-Host ("    tile {0} (wants {1}): {2}" -f ($lane + 1), $expected, ($counts -join ', '))
    }
    Write-Host "    verdict: $verdict" -ForegroundColor $colour
}

Write-Host ""
Write-Host "==> $swaps of $Rounds round(s) put the wrong lens on a tile" -ForegroundColor Cyan
