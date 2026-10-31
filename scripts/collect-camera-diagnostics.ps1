<#
.SYNOPSIS
    Collects a reviewable support archive for a camera we cannot access.

.DESCRIPTION
    Uses the viewer's saved Windows-user session, without changing its config.
    No settings, motor commands, firmware updates or recording downloads are
    requested. Media payloads and session keys are never written to the archive.
    Close the viewer and Mi Home live view before running this script.

.PARAMETER Did
    Device ID from the account list. Omit to choose a camera interactively.

.PARAMETER IncludeStorage
    Also inspect the SD catalogue on storage channels 0 and 10. This may take
    45 seconds per channel and reveals recording activity, but not footage.

.PARAMETER IncludeAppLog
    Include the redacted viewer log. It may describe other cameras as well.

.PARAMETER IncludeFirmwareInfo
    Query version/firmware URL endpoints, without downloading or installing.

.PARAMETER SelfTest
    Exercise archive creation, redaction and partial failure without a camera,
    saved account, DLL, or network access.
#>
[CmdletBinding()]
param(
    [string]$Did,
    [string]$DllPath,
    [string]$OutputDirectory = '',
    [string]$ResultPath = '',
    [string]$CancelPath = '',
    [ValidateRange(3, 30)][int]$ObserveSeconds = 8,
    [ValidateSet('auto', 'sd', 'hd', '0', '1', '2', '3', '4', '5')][string]$Quality = 'auto',
    [ValidateSet('', '0', '1', '2')][string]$Channel = '',
    [ValidateSet('', 'tcp', 'udp')][string]$Transport = '',
    [switch]$IncludeStorage,
    [switch]$IncludeAppLog,
    [switch]$IncludeFirmwareInfo,
    [switch]$SelfTest
)

$ErrorActionPreference = 'Stop'

function Protect-DiagnosticText([string]$Text) {
    $jsonStart = $Text.IndexOf('{')
    if ($jsonStart -ge 0) {
        try {
            $body = $Text.Substring($jsonStart) | ConvertFrom-Json -ErrorAction Stop
            $safeBody = Protect-DiagnosticValue $body
            return (Protect-DiagnosticText $Text.Substring(0, $jsonStart)) + (ConvertTo-Json -InputObject $safeBody -Depth 60 -Compress)
        } catch { }
    }
    foreach ($secret in $script:Secrets) {
        if ($secret) { $Text = $Text.Replace($secret, '[redacted]') }
    }
    $sensitive = '[\w-]*(?:token|password|passwd|security|signature|cookie|authorization|secret|private|public|serial|ssid|bssid|user[_-]?id|device[_-]?id|mac[_-]?address)[\w-]*|sign|did|uid|mac|name|ip|host|remote_addr'
    $Text = [regex]::Replace($Text,
        '(?i)("(?:' + $sensitive + ')"\s*:\s*)("(?:\\.|[^"\\])*"|[^,}\]\s]+)', '$1"[redacted]"')
    $Text = [regex]::Replace($Text,
        '(?i)(\b(?:' + $sensitive + ')\s*[=:]\s*)([^\s,;]+)', '$1[redacted]')
    $Text = [regex]::Replace($Text, '\b(?:\d{1,3}\.){3}\d{1,3}\b', '[address]')
    $Text = [regex]::Replace($Text, '(?i)\b(?:[0-9a-f]{2}[:-]){5}[0-9a-f]{2}\b', '[address]')
    return $Text
}

function Protect-DiagnosticValue($Value) {
    if ($null -eq $Value) { return $null }
    if ($Value -is [string]) { return (Protect-DiagnosticText $Value) }
    if ($Value -is [System.Collections.IDictionary] -or $Value -is [PSCustomObject]) {
        $result = [ordered]@{}
        $keys = if ($Value -is [System.Collections.IDictionary]) { $Value.Keys } else { $Value.PSObject.Properties.Name }
        foreach ($key in $keys) {
            if ($key -match '(?i)token|password|passwd|secret|security|signature|cookie|authorization|private|public|serial|ssid|bssid|user[_-]?id|device[_-]?id|mac[_-]?address' -or
                $key -match '^(?i:sign|user_id|uid|did|mac|serial|name|ssid|bssid|ip|host|remote_addr)$') {
                $result[$key] = '[redacted]'
            } else {
                $result[$key] = Protect-DiagnosticValue $Value.$key
            }
        }
        return $result
    }
    if ($Value -is [System.Collections.IEnumerable]) {
        $items = @(foreach ($item in $Value) { Protect-DiagnosticValue $item })
        return ,$items
    }
    return $Value
}

function Write-DiagnosticJson([string]$Name, $Value) {
    $safe = Protect-DiagnosticValue $Value
    $json = ConvertTo-Json -InputObject $safe -Depth 60
    [System.IO.File]::WriteAllText((Join-Path $script:BundleDirectory $Name), $json,
        [System.Text.UTF8Encoding]::new($false))
}

function New-FirmwareInfoRequest([string]$Path, [string]$AccountId, [string]$DeviceId) {
    return @{ user_id = $AccountId; path = $Path; params = (@{ did = $DeviceId; pid = 0 } | ConvertTo-Json -Compress) }
}

function Get-DiagnosticProperties($Specification) {
    return @(foreach ($service in $Specification.services) {
        foreach ($property in $service.properties) {
            if ($property.type -match '(?i):property:[^:]*(serial|device-id|user-id|mac-address|ssid|bssid|token|password|private|public-key)') { continue }
            if ($property.access -contains 'read' -and ($property.format -ne 'string' -or
                $property.type -match ':property:(firmware-revision|hardware-revision|manufacturer|model):')) {
                @{ siid = $service.iid; piid = $property.iid }
            }
        }
    })
}

function Write-AppDiagnosticStatus([string]$Status, [string]$Step = '') {
    if (-not $ResultPath) { return }
    $result = @{
        status = $Status
        step = $Step
        archive = $script:ArchivePath
        directory = $script:BundleDirectory
        error = $script:Summary.error
    } | ConvertTo-Json -Compress
    try {
        [System.IO.File]::WriteAllText($ResultPath, $result, [System.Text.UTF8Encoding]::new($false))
    } catch [System.IO.IOException] { }
}

function Test-DiagnosticCancellation {
    if ($CancelPath -and (Test-Path -LiteralPath $CancelPath)) {
        throw [System.OperationCanceledException]::new('Collection cancelled by the camera owner.')
    }
}

function Invoke-DiagnosticStep([string]$Label, $Request, [scriptblock]$Action) {
    Test-DiagnosticCancellation
    Write-Host "==> $Label" -ForegroundColor Cyan
    Write-AppDiagnosticStatus 'collecting' $Label
    $timer = [Diagnostics.Stopwatch]::StartNew()
    $response = $null
    try {
        $response = & $Action
        $success = $null -ne $response -and $response.ok -ne $false
    } catch {
        $success = $false
        $response = @{ ok = $false; error = $_.Exception.Message }
    }
    $script:Events.Add([ordered]@{
        utc = [DateTime]::UtcNow.ToString('o')
        step = $Label
        elapsed_ms = $timer.ElapsedMilliseconds
        success = $success
        request = Protect-DiagnosticValue $Request
        response = Protect-DiagnosticValue $response
    })
    Write-DiagnosticJson 'communication.json' @($script:Events.ToArray())
    return $response
}

function Invoke-StorageDiagnostics([scriptblock]$Command) {
    foreach ($storageChannel in @(0, 10)) {
        $request = @{ method = 'recordings.inspect'; channel = $storageChannel }
        $null = Invoke-DiagnosticStep "SD catalogue summary, channel $storageChannel" $request {
            & $Command $request
        }
        $request = @{ method = 'replies' }
        $null = Invoke-DiagnosticStep 'storage replies and RDT tap' $request {
            & $Command $request
        }
    }
}

function Complete-DiagnosticArchive {
    $script:Summary.completed_utc = [DateTime]::UtcNow.ToString('o')
    $script:Summary.successful_steps = @($script:Events | Where-Object { $_.success }).Count
    $script:Summary.failed_steps = @($script:Events | Where-Object { -not $_.success }).Count
    Write-DiagnosticJson 'communication.json' @($script:Events.ToArray())
    Write-DiagnosticJson 'summary.json' $script:Summary
    $notes = @'
CAMERA OWNER REPORT (fill in before sharing)
Retail model and hardware revision:
Firmware version shown in Mi Home:
Mi Home version, Android/iOS version and plugin version (if visible):
Account region:
Mains/battery powered; number of lenses; SD card inserted:
Works in Mi Home: live video / microphone / PTZ / SD playback:
What fails in XiaomiViewer, and what you expected:
Local network: same subnet, Wi-Fi/Ethernet, VPN/VLAN, firewall profile:
Approximate UTC time of the failure:

Review every file before sharing. Remove personal names, serials, locations,
network addresses and signed URLs that escaped automatic redaction.
Do not add config.json, account tokens, passwords, session keys or footage.
The collector does not upload anything and does not capture Mi Home traffic.
A partial report is useful. Silence is not proof of unsupported commands.
'@
    [System.IO.File]::WriteAllText((Join-Path $script:BundleDirectory 'owner-report.txt'), $notes,
        [System.Text.UTF8Encoding]::new($false))
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [System.IO.Compression.ZipFile]::CreateFromDirectory($script:BundleDirectory, $script:ArchivePath)
    Write-AppDiagnosticStatus $script:Summary.status
    Write-Host "Archive: $script:ArchivePath"
    Write-Host 'Review the extracted files before sharing. Nothing has been uploaded.'
}

$script:Secrets = @()
$script:Events = [System.Collections.Generic.List[object]]::new()
$script:Summary = [ordered]@{
    schema = 1
    started_utc = [DateTime]::UtcNow.ToString('o')
    collector = 'collect-camera-diagnostics.ps1'
    os = [Environment]::OSVersion.Version.ToString()
    process_architecture = [System.Runtime.InteropServices.RuntimeInformation]::ProcessArchitecture.ToString()
    powershell = $PSVersionTable.PSVersion.ToString()
    storage_requested = [bool]$IncludeStorage
    app_log_requested = [bool]$IncludeAppLog
    firmware_info_requested = [bool]$IncludeFirmwareInfo
    status = 'partial'
    privacy = 'No config, account credentials, session keys or media payloads. Review for remaining personal data.'
    limitations = 'Viewer-side protocol replies and bounded header samples, not a PCAP or a capture of Mi Home. Silence does not prove a command is unsupported.'
}

if (-not $OutputDirectory) {
    $OutputDirectory = [Environment]::GetFolderPath('DesktopDirectory')
    if (-not $OutputDirectory) { $OutputDirectory = (Get-Location).Path }
}
$OutputDirectory = [System.IO.Path]::GetFullPath($OutputDirectory)
$bundleName = 'XiaomiViewer-diagnostics-' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N').Substring(0, 8)
$script:BundleDirectory = Join-Path $OutputDirectory $bundleName
$script:ArchivePath = "$script:BundleDirectory.zip"
New-Item -ItemType Directory -Path $script:BundleDirectory -Force | Out-Null
Write-AppDiagnosticStatus 'collecting' 'Starting collector'

if ($SelfTest) {
    $script:Secrets = @('test-account-token', '987654321', 'My front door', 'same-size-AA', 'same-size-BB') | Sort-Object Length -Descending
    $firmwareRequest = New-FirmwareInfoRequest '/home/checkversion' '987654321' 'test-device-id'
    if ($firmwareRequest.path -ne '/home/checkversion' -or $firmwareRequest.params -isnot [string] -or
        ($firmwareRequest.params | ConvertFrom-Json).did -ne 'test-device-id') {
        throw 'Firmware request does not match cloud.raw path/JSON-string params contract.'
    }
    $fakeSpec = @{ services = @(@{ iid = 2; properties = @(
        @{ iid = 1; type = 'urn:miot-spec-v2:property:serial-number:00000001'; format = 'uint64'; access = @('read') },
        @{ iid = 2; type = 'urn:miot-spec-v2:property:firmware-revision:00000002'; format = 'string'; access = @('read') },
        @{ iid = 3; type = 'urn:miot-spec-v2:property:on:00000003'; format = 'bool'; access = @('read', 'write') },
        @{ iid = 4; type = 'urn:miot-spec-v2:property:location:00000004'; format = 'string'; access = @('read') },
        @{ iid = 5; type = 'urn:miot-spec-v2:property:on:00000005'; format = 'bool'; access = @('write') }
    ) }) }
    $readable = @(Get-DiagnosticProperties $fakeSpec)
    if ($readable.Count -ne 2 -or $readable[0].piid -ne 2 -or $readable[1].piid -ne 3) {
        throw 'Property selection included an identity, arbitrary string or write-only property.'
    }
    $sample = @{
        ok = $true; model = 'test.camera.unsupported'; token = 'test-account-token'
        account = @{ user_id = '987654321'; client_private = 'unknown-private-material' }
        serial_number = 123456789
        replies = @('cmd=0x111 {"nested":{"access_token":"unknown-response-secret"},"did":"987654321","fw":"1.2.3"}',
            'host=192.168.4.12 name=My front door same-size-AA same-size-BB')
        headers = @(@{ codec = 4242; channel = 10 }, @{ codec = 2; channel = 0 })
        one_item = @(@{ codec = 4242 })
        empty = @()
    }
    $null = Invoke-DiagnosticStep 'synthetic reply' @{ method = 'replies'; did = '987654321' } { $sample }
    $null = Invoke-DiagnosticStep 'synthetic failure' @{} { throw 'failure for 987654321 with test-account-token' }
    if ($IncludeStorage) {
        $originalLiveChannel = $Channel
        $storageRequests = [System.Collections.Generic.List[object]]::new()
        Invoke-StorageDiagnostics {
            param($storageRequest)
            $storageRequests.Add($storageRequest)
            @{ ok = $true }
        }
        if ($storageRequests.Count -ne 4 -or $storageRequests[0].channel -ne 0 -or
            $storageRequests[2].channel -ne 10 -or $Channel -ne $originalLiveChannel) {
            throw 'Storage probes changed the validated live channel or lost storage channel 10.'
        }
    }
    $script:Summary.status = 'self-test'
    Complete-DiagnosticArchive
    $archive = [System.IO.Compression.ZipFile]::OpenRead($script:ArchivePath)
    try {
        $text = ''
        foreach ($entry in $archive.Entries) {
            $reader = [System.IO.StreamReader]::new($entry.Open())
            try { $text += $reader.ReadToEnd() } finally { $reader.Dispose() }
        }
        foreach ($secret in @('test-account-token', '987654321', 'My front door', 'unknown-private-material', 'unknown-response-secret', '192.168.4.12', 'same-size-AA', 'same-size-BB', '123456789')) {
            if ($text.Contains($secret)) { throw "Self-test redaction failed for $secret" }
        }
        $events = Get-Content (Join-Path $script:BundleDirectory 'communication.json') -Raw -Encoding UTF8 | ConvertFrom-Json
        $expectedEvents = if ($IncludeStorage) { 6 } else { 2 }
        if ($events.Count -ne $expectedEvents -or $events[1].success -ne $false -or
            $events[0].response.headers.Count -ne 2 -or $events[0].response.model -ne 'test.camera.unsupported' -or
            $events[0].response.one_item -isnot [array] -or $events[0].response.empty.Count -ne 0) {
            throw 'Self-test lost protocol evidence or partial failure.'
        }
    } finally { $archive.Dispose() }
    if ($ResultPath) {
        $appResult = Get-Content -LiteralPath $ResultPath -Raw -Encoding UTF8 | ConvertFrom-Json
        if ($appResult.status -ne 'self-test' -or $appResult.archive -ne $script:ArchivePath -or
            $appResult.directory -ne $script:BundleDirectory) {
            throw 'App result lost archive status or paths.'
        }
    }
    Write-Host 'Self-test passed: archive, redaction, arrays and partial failure.'
    return
}

$handle = [IntPtr]::Zero
$script:CameraHandle = [IntPtr]::Zero
$userId = ''
try {
    Test-DiagnosticCancellation
    if (-not $DllPath) {
        $candidates = @(
            (Join-Path $PSScriptRoot 'xmbridge.dll'),
            (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\msvc\RelWithDebInfo\xmbridge.dll'),
            (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\msvc-arm64\RelWithDebInfo\xmbridge.dll'))
        $DllPath = $candidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    }
    if (-not $DllPath -or -not (Test-Path -LiteralPath $DllPath)) {
        throw 'xmbridge.dll not found. Run beside the packaged DLL or pass -DllPath.'
    }
    $DllPath = (Resolve-Path -LiteralPath $DllPath).Path
    $configPath = Join-Path $env:APPDATA 'XiaomiViewer\config.json'
    $config = Get-Content -LiteralPath $configPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $stored = $config.account.token
    if (-not $stored -or -not $stored.StartsWith('dpapi:')) { throw 'Sign in with XiaomiViewer first, as this Windows user.' }
    Add-Type -AssemblyName System.Security
    $protected = [Convert]::FromBase64String($stored.Substring(6))
    $plain = [Security.Cryptography.ProtectedData]::Unprotect($protected, $null,
        [Security.Cryptography.DataProtectionScope]::CurrentUser)
    $token = [Text.Encoding]::UTF8.GetString($plain)
    [Array]::Clear($plain, 0, $plain.Length)
    $userId = [string]$config.account.user_id
    $script:Secrets = @($token, $stored, $userId)
    foreach ($camera in $config.cameras) {
        $script:Secrets += @([string]$camera.did, [string]$camera.name, [string]$camera.ip, [string]$camera.mac)
    }
    $script:Secrets = @($script:Secrets | Where-Object { $_ } | Sort-Object Length -Descending)

    if (-not ('XmCameraDiagnostics' -as [type])) {
        Add-Type -Language CSharp -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class XmCameraDiagnostics
{
    [DllImport("kernel32", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern IntPtr LoadLibrary(string path);
    [DllImport("kernel32", SetLastError = true)]
    private static extern IntPtr GetProcAddress(IntPtr module, string name);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int CallFn([MarshalAs(UnmanagedType.LPUTF8Str)] string method,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string request, byte[] buffer, int capacity);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate IntPtr OpenFn([MarshalAs(UnmanagedType.LPUTF8Str)] string request, byte[] buffer, int capacity);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int CommandFn(IntPtr handle, [MarshalAs(UnmanagedType.LPUTF8Str)] string request, byte[] buffer, int capacity);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate void CloseFn(IntPtr handle);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate IntPtr VersionFn();
    private static IntPtr module;
    private static readonly byte[] buffer = new byte[16 * 1024 * 1024];

    public static void Load(string path)
    {
        module = LoadLibrary(path);
        if (module == IntPtr.Zero) throw new Exception("Cannot load bridge (check PowerShell/DLL architecture): " + Marshal.GetLastWin32Error());
    }
    private static Delegate Function(string name, Type type)
    {
        IntPtr proc = GetProcAddress(module, name);
        if (proc == IntPtr.Zero) throw new Exception("Missing bridge export " + name);
        return Marshal.GetDelegateForFunctionPointer(proc, type);
    }
    private static string Result(int size)
    {
        if (size < 0 || size > buffer.Length) throw new Exception("Bridge response size " + size + "; not retried because reads consume replies");
        return Encoding.UTF8.GetString(buffer, 0, size);
    }
    public static string Call(string method, string request)
    {
        return Result(((CallFn)Function("xmb_call", typeof(CallFn)))(method, request, buffer, buffer.Length));
    }
    public static IntPtr Open(string request, out string response)
    {
        Array.Clear(buffer, 0, buffer.Length);
        IntPtr handle = ((OpenFn)Function("xmb_stream_open", typeof(OpenFn)))(request, buffer, buffer.Length);
        int size = Array.IndexOf(buffer, (byte)0);
        response = Encoding.UTF8.GetString(buffer, 0, size < 0 ? buffer.Length : size);
        return handle;
    }
    public static string Command(IntPtr handle, string request)
    {
        return Result(((CommandFn)Function("xmb_stream_command", typeof(CommandFn)))(handle, request, buffer, buffer.Length));
    }
    public static void Close(IntPtr handle)
    {
        ((CloseFn)Function("xmb_stream_close", typeof(CloseFn)))(handle);
    }
    public static string Version()
    {
        return Marshal.PtrToStringAnsi(((VersionFn)Function("xmb_version", typeof(VersionFn)))());
    }
}
'@
    }
    [XmCameraDiagnostics]::Load($DllPath)
    $script:Summary.bridge_version = [XmCameraDiagnostics]::Version()
    $script:Summary.bridge_sha256 = (Get-FileHash -LiteralPath $DllPath -Algorithm SHA256).Hash
    $login = @{ user_id = $userId; region = $config.account.region; token = $token } | ConvertTo-Json -Compress
    $result = [XmCameraDiagnostics]::Call('login.token', $login) | ConvertFrom-Json
    if (-not $result.ok) { throw $result.error }
    $result = [XmCameraDiagnostics]::Call('device.list', (@{ user_id = $userId } | ConvertTo-Json -Compress)) | ConvertFrom-Json
    if (-not $result.ok) { throw $result.error }
    $devices = @($result.devices)
    foreach ($device in $devices) {
        $script:Secrets += @([string]$device.did, [string]$device.name, [string]$device.ip, [string]$device.mac)
    }
    $script:Secrets = @($script:Secrets | Where-Object { $_ } | Sort-Object Length -Descending)
    if (-not $Did) {
        $cameras = @($devices | Where-Object { $_.model -match '\.(camera|cateye|ipcamera)\.' })
        if (-not $cameras.Count) { throw 'No cameras in this region. Pass -Did for a device with an unrecognised model name.' }
        for ($index = 0; $index -lt $cameras.Count; $index++) {
            Write-Host "$($index + 1). $($cameras[$index].name) [$($cameras[$index].model)]"
        }
        $selection = 0
        if (-not [int]::TryParse((Read-Host 'Camera number'), [ref]$selection) -or
            $selection -lt 1 -or $selection -gt $cameras.Count) { throw 'Invalid camera number.' }
        $Did = [string]$cameras[$selection - 1].did
    }
    $device = $devices | Where-Object { $_.did -eq $Did } | Select-Object -First 1
    if (-not $device) { throw 'The selected device is not in the account.' }
    $script:Summary.model = $device.model
    $script:Summary.region = $config.account.region
    Write-DiagnosticJson 'device.json' $device

    $request = @{ user_id = $userId; did = $Did }
    $null = Invoke-DiagnosticStep 'cloud transport negotiation' $request {
        [XmCameraDiagnostics]::Call('device.diagnostics', ($request | ConvertTo-Json -Compress)) | ConvertFrom-Json
    }
    $specification = Invoke-DiagnosticStep 'MIoT specification lookup' @{ model = $device.model } {
        $instances = Invoke-RestMethod -Uri 'https://miot-spec.org/miot-spec-v2/instances?status=all' -TimeoutSec 20
        $instance = $instances.instances | Where-Object { $_.model -eq $device.model } | Select-Object -First 1
        if (-not $instance) { throw 'No public MIoT specification for this model.' }
        Invoke-RestMethod -Uri ('https://miot-spec.org/miot-spec-v2/instance?type=' + [Uri]::EscapeDataString($instance.type)) -TimeoutSec 20
    }
    if ($specification.services) { Write-DiagnosticJson 'miot-spec.json' $specification }

    if ($IncludeFirmwareInfo) {
        foreach ($api in @('/home/checkversion', '/v2/device/latest_ver', '/home/devupgrade_status')) {
            $request = New-FirmwareInfoRequest $api $userId $Did
            $null = Invoke-DiagnosticStep "firmware information $api" $request {
                [XmCameraDiagnostics]::Call('cloud.raw', ($request | ConvertTo-Json -Depth 5 -Compress)) | ConvertFrom-Json
            }
        }
    }
    $props = @(Get-DiagnosticProperties $specification)
    for ($offset = 0; $offset -lt $props.Count; $offset += 10) {
        $last = [Math]::Min($offset + 9, $props.Count - 1)
        $request = @{ user_id = $userId; did = $Did; props = @($props[$offset..$last]) }
        $null = Invoke-DiagnosticStep "read MIoT properties $offset..$last" $request {
            [XmCameraDiagnostics]::Call('miot.get', ($request | ConvertTo-Json -Depth 5 -Compress)) | ConvertFrom-Json
        }
    }
    $openRequest = @{ user_id = $userId; did = $Did; model = $device.model; ip = $device.ip; channel = $Channel; quality = $Quality; transport = $Transport; audio = $true }
    $null = Invoke-DiagnosticStep 'open live session' $openRequest {
        $openResponse = ''
        $script:CameraHandle = [XmCameraDiagnostics]::Open(($openRequest | ConvertTo-Json -Compress), [ref]$openResponse)
        $openResponse | ConvertFrom-Json
    }
    $handle = $script:CameraHandle
    if ($handle -ne [IntPtr]::Zero) {
        $request = @{ method = 'miss.raw'; cmd = 0x110; body = '{}' }
        $null = Invoke-DiagnosticStep 'request device information (0x110)' $request {
            [XmCameraDiagnostics]::Command($handle, ($request | ConvertTo-Json -Compress)) | ConvertFrom-Json
        }
        $timer = [Diagnostics.Stopwatch]::StartNew()
        $interval = [Threading.ManualResetEvent]::new($false)
        try {
            do {
                $request = @{ method = 'replies' }
                $null = Invoke-DiagnosticStep 'observe replies and RDT tap' $request {
                    [XmCameraDiagnostics]::Command($handle, ($request | ConvertTo-Json -Compress)) | ConvertFrom-Json
                }
                $interval.WaitOne(500) | Out-Null
            } while ($timer.Elapsed.TotalSeconds -lt $ObserveSeconds)
        } finally { $interval.Dispose() }
        foreach ($method in @('media.headers', 'stats')) {
            $request = @{ method = $method }
            $null = Invoke-DiagnosticStep $method $request {
                [XmCameraDiagnostics]::Command($handle, ($request | ConvertTo-Json -Compress)) | ConvertFrom-Json
            }
        }
        if ($IncludeStorage) {
            Invoke-StorageDiagnostics {
                param($storageRequest)
                [XmCameraDiagnostics]::Command($handle, ($storageRequest | ConvertTo-Json -Compress)) | ConvertFrom-Json
            }
        }
    }
    if ($IncludeAppLog) {
        $null = Invoke-DiagnosticStep 'viewer log' @{} {
            $logPath = Join-Path $env:APPDATA 'XiaomiViewer\xiaomi-viewer.log'
            $lines = @(Get-Content -LiteralPath $logPath -Tail 2000 -Encoding UTF8)
            Write-DiagnosticJson 'viewer-log.json' @{ lines = $lines }
            @{ ok = $true; lines = $lines.Count }
        }
    }
    if (@($script:Events | Where-Object { -not $_.success }).Count -eq 0 -and $handle -ne [IntPtr]::Zero) {
        $script:Summary.status = 'collected'
    }
} catch {
    if ($CancelPath -and (Test-Path -LiteralPath $CancelPath)) { $script:Summary.status = 'cancelled' }
    $script:Summary.error = Protect-DiagnosticText $_.Exception.Message
    Write-Warning $script:Summary.error
} finally {
    if ($script:CameraHandle -ne [IntPtr]::Zero) {
        try { [XmCameraDiagnostics]::Close($script:CameraHandle) } catch {
            $script:Summary.close_error = Protect-DiagnosticText $_.Exception.Message
        }
    }
    if ($userId -and ('XmCameraDiagnostics' -as [type])) {
        try { $null = [XmCameraDiagnostics]::Call('account.forget', (@{ user_id = $userId } | ConvertTo-Json -Compress)) } catch { }
    }
    $token = $null
    $login = $null
    $config = $null
    Complete-DiagnosticArchive
}