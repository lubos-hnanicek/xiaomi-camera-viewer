<#
.SYNOPSIS
    Reads the saved pan/tilt positions a camera holds.

.DESCRIPTION
    The app can move to a preset but has no way to create one, and the question
    is whether the camera would let it. This reads the fav-area service (siid 9)
    to see what a saved position looks like and how they are stored.

    Read-only: nothing is written and the camera does not move.
#>
[CmdletBinding()]
param(
    [string]$Did,
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
if (-not (Test-Path $configPath)) { throw "No config at $configPath. Sign in with the app first." }

$config = Get-Content $configPath -Raw | ConvertFrom-Json
$stored = $config.account.token
if (-not $stored -or -not $stored.StartsWith('dpapi:')) { throw "No saved token in the config." }

Add-Type -AssemblyName System.Security
$protected = [Convert]::FromBase64String($stored.Substring('dpapi:'.Length))
$plain = [System.Security.Cryptography.ProtectedData]::Unprotect(
    $protected, $null, [System.Security.Cryptography.DataProtectionScope]::CurrentUser)
$token = [System.Text.Encoding]::UTF8.GetString($plain)

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class XmFav
{
    [DllImport("kernel32", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern IntPtr LoadLibrary(string path);
    [DllImport("kernel32", SetLastError = true)]
    private static extern IntPtr GetProcAddress(IntPtr module, string name);

    private delegate int CallFn(string method, string request, byte[] outBuf, int cap);
    private static IntPtr module;

    public static void Load(string path)
    {
        module = LoadLibrary(path);
        if (module == IntPtr.Zero) throw new Exception("LoadLibrary failed: " + Marshal.GetLastWin32Error());
    }

    public static string Call(string method, string request)
    {
        IntPtr p = GetProcAddress(module, "xmb_call");
        if (p == IntPtr.Zero) throw new Exception("missing export xmb_call");
        CallFn fn = (CallFn)Marshal.GetDelegateForFunctionPointer(p, typeof(CallFn));
        byte[] buffer = new byte[262144];
        int needed = fn(method, request, buffer, buffer.Length);
        if (needed > buffer.Length) { buffer = new byte[needed]; needed = fn(method, request, buffer, buffer.Length); }
        if (needed < 0) throw new Exception(method + " failed with code " + needed);
        return Encoding.UTF8.GetString(buffer, 0, needed);
    }
}
"@ -Language CSharp

[XmFav]::Load($DllPath)

$userId = $config.account.user_id
$login = @{ user_id = $userId; region = $config.account.region; token = $token } | ConvertTo-Json -Compress
$res = [XmFav]::Call('login.token', $login) | ConvertFrom-Json
if (-not $res.ok) { throw "login.token failed: $($res.error)" }

$res = [XmFav]::Call('device.list', (@{ user_id = $userId } | ConvertTo-Json -Compress)) | ConvertFrom-Json
if (-not $res.ok) { throw "device.list failed: $($res.error)" }

$motorised = @(
    'isa.camera.hlc8',
    'isa.camera.hlc8a',
    'isa.camera.500dh',
    'isa.camera.hlmax',
    'isa.camera.700sa'
)
$targets = if ($Did) {
    $res.devices | Where-Object { $_.did -eq $Did }
} else {
    $res.devices | Where-Object { $motorised -contains $_.model }
}
if (-not $targets) { throw "No motorised camera found. Pass -Did explicitly." }

$props = @(
    @{ Name = 'fav-area (saved positions)'; Siid = 9; Piid = 1 },
    @{ Name = 'active-fav-area';            Siid = 9; Piid = 2 },
    @{ Name = 'hl-get-location';            Siid = 6; Piid = 12 }
)

foreach ($device in $targets) {
    Write-Host ""
    Write-Host "==> $($device.name) [$($device.model)] did=$($device.did)" -ForegroundColor Cyan

    foreach ($p in $props) {
        $req = @{ user_id = $userId; did = $device.did; props = @(@{ siid = $p.Siid; piid = $p.Piid }) } |
            ConvertTo-Json -Compress -Depth 5
        try {
            $r = [XmFav]::Call('miot.get', $req) | ConvertFrom-Json
            if (-not $r.ok) {
                Write-Host ("    {0,-28} error: {1}" -f $p.Name, $r.error) -ForegroundColor Yellow
                continue
            }
            $entry = $r.result | Select-Object -First 1
            if ($entry.code -ne 0) {
                Write-Host ("    {0,-28} unsupported (code {1})" -f $p.Name, $entry.code) -ForegroundColor DarkGray
                continue
            }
            Write-Host ("    {0,-28} {1}" -f $p.Name, $entry.value) -ForegroundColor Green
        } catch {
            Write-Host ("    {0,-28} failed: {1}" -f $p.Name, $_) -ForegroundColor Red
        }
    }
}

Write-Host ""
