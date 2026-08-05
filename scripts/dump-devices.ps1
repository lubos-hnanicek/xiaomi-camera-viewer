<#
.SYNOPSIS
    Prints the Mi cloud device list, matched against the local ARP table.

.DESCRIPTION
    A diagnostic for the case where the cloud reports no LAN address for a
    camera. The device list carries a MAC even when it carries no usable IP, so
    matching that MAC against the ARP table finds the camera on the network.

    Reuses the saved session in %APPDATA%\XiaomiViewer\config.json, so it needs
    no credentials, but it must run as the same Windows user that signed in.
#>
[CmdletBinding()]
param(
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

$source = @"
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class XmDump
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
        if (module == IntPtr.Zero)
            throw new Exception("LoadLibrary failed: " + Marshal.GetLastWin32Error());
    }

    public static string Call(string method, string request)
    {
        IntPtr proc = GetProcAddress(module, "xmb_call");
        if (proc == IntPtr.Zero) throw new Exception("missing export xmb_call");
        CallFn fn = (CallFn)Marshal.GetDelegateForFunctionPointer(proc, typeof(CallFn));

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
}
"@

Add-Type -TypeDefinition $source -Language CSharp
[XmDump]::Load($DllPath)

$login = @{
    user_id = $config.account.user_id
    region  = $config.account.region
    token   = $token
} | ConvertTo-Json -Compress

Write-Host "==> restoring session for $($config.account.user_id) (region $($config.account.region))" -ForegroundColor Cyan
$res = [XmDump]::Call('login.token', $login) | ConvertFrom-Json
if (-not $res.ok) { throw "login.token failed: $($res.error)" }

$res = [XmDump]::Call('device.list', (@{ user_id = $config.account.user_id } | ConvertTo-Json -Compress)) | ConvertFrom-Json
if (-not $res.ok) { throw "device.list failed: $($res.error)" }

# Normalise the ARP table to "aabbccddeeff" so cloud MACs can be looked up.
$arp = @{}
foreach ($line in (arp -a)) {
    if ($line -match '\s*(\d+\.\d+\.\d+\.\d+)\s+([0-9a-fA-F-]{17})\s') {
        $arp[($matches[2] -replace '-', '').ToLower()] = $matches[1]
    }
}

Write-Host ""
$res.devices | ForEach-Object {
    $key = ($_.mac -replace '[:-]', '').ToLower()
    $onLan = $arp[$key]

    [PSCustomObject]@{
        Name     = $_.name
        Model    = $_.model
        Did      = $_.did
        CloudIP  = $_.ip
        MAC      = $_.mac
        ArpIP    = if ($onLan) { $onLan } else { '-' }
    }
} | Format-Table -AutoSize
