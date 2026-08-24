<#
.SYNOPSIS
    Finds, and optionally downloads, the firmware image a camera would install.

.DESCRIPTION
    The camera's own firmware is the one published artifact that contains the
    camera's side of the MISS protocol. Everything else about the undocumented
    commands has had to be inferred from what a camera does or does not answer;
    the code that decides those answers is in the image, and the JSON keys it
    parses are ordinary strings inside it.

    Xiaomi does not document a firmware endpoint, so this asks several candidates
    and reports whatever each returns. `cloud.raw` signs an arbitrary IoT API
    call, which is what makes trying them cheap.

    Nothing here installs anything. The upgrade endpoints are deliberately not
    among the candidates: every path asked is a query.

    Reuses the saved session in %APPDATA%\XiaomiViewer\config.json, so it needs
    no credentials, but it must run as the same Windows user that signed in.

.PARAMETER Did
    Which camera to ask about. Defaults to every camera on the account.

.PARAMETER Download
    Fetch any image found into the directory given by -OutDir.
#>
[CmdletBinding()]
param(
    [string]$Did,
    [switch]$Download,
    [string]$OutDir,
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

if (-not $OutDir) {
    $OutDir = Join-Path (Split-Path -Parent $PSScriptRoot) 'build\firmware'
}

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

public static class XmFw
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
        IntPtr p = GetProcAddress(module, "xmb_call");
        if (p == IntPtr.Zero) throw new Exception("missing export xmb_call");
        CallFn fn = (CallFn)Marshal.GetDelegateForFunctionPointer(p, typeof(CallFn));

        byte[] buffer = new byte[1048576];
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
"@ -Language CSharp

[XmFw]::Load($DllPath)

$userId = $config.account.user_id

$login = @{ user_id = $userId; region = $config.account.region; token = $token } | ConvertTo-Json -Compress
Write-Host "==> restoring session for $userId (region $($config.account.region))" -ForegroundColor Cyan
$res = [XmFw]::Call('login.token', $login) | ConvertFrom-Json
if (-not $res.ok) { throw "login.token failed: $($res.error)" }

$res = [XmFw]::Call('device.list', (@{ user_id = $userId } | ConvertTo-Json -Compress)) | ConvertFrom-Json
if (-not $res.ok) { throw "device.list failed: $($res.error)" }

$targets = if ($Did) {
    $res.devices | Where-Object { $_.did -eq $Did }
} else {
    $res.devices
}
if (-not $targets) { throw "No matching camera in the account." }

function Invoke-Raw([string]$path, [hashtable]$params) {
    $req = @{
        user_id = $userId
        path    = $path
        params  = ($params | ConvertTo-Json -Compress -Depth 5)
    } | ConvertTo-Json -Compress

    try {
        $r = [XmFw]::Call('cloud.raw', $req) | ConvertFrom-Json
        if (-not $r.ok) { return @{ Ok = $false; Text = $r.error } }
        return @{ Ok = $true; Result = $r.result; Text = ($r.result | ConvertTo-Json -Compress -Depth 10) }
    } catch {
        return @{ Ok = $false; Text = "$_" }
    }
}

# Query endpoints only. Anything that would start an upgrade is left out on
# purpose: a camera that flashes an image mid-probe is a brick waiting to happen.
$candidates = @(
    @{ Path = '/home/checkversion';       Params = @{} }
    @{ Path = '/v2/device/latest_ver';    Params = @{} }
    @{ Path = '/home/devupgrade_status';  Params = @{} }
)

$found = @()

foreach ($device in $targets) {
    Write-Host ""
    Write-Host "==> $($device.name) [$($device.model)] did=$($device.did)" -ForegroundColor Cyan

    foreach ($candidate in $candidates) {
        $params = $candidate.Params.Clone()
        $params['did'] = $device.did
        $params['pid'] = 0

        $answer = Invoke-Raw $candidate.Path $params
        if (-not $answer.Ok) {
            Write-Host ("  {0,-26} {1}" -f $candidate.Path, $answer.Text) -ForegroundColor DarkGray
            continue
        }

        Write-Host ("  {0,-26} {1}" -f $candidate.Path, $answer.Text) -ForegroundColor Green

        # Whatever the shape, a firmware image announces itself as a URL. Rather
        # than guess which member holds it, take any that looks like one.
        foreach ($url in [regex]::Matches($answer.Text, 'https?://[^"\\ ]+')) {
            if ($url.Value -match '\.(bin|gz|tar|img|pkg|swu|ota)(\?|$)') {
                Write-Host ("      image: {0}" -f $url.Value) -ForegroundColor Yellow
                $found += [PSCustomObject]@{ Model = $device.model; Url = $url.Value }
            }
        }
    }
}

if (-not $found) {
    Write-Host ""
    Write-Host "==> no firmware URL in any answer" -ForegroundColor Yellow
    return
}

Write-Host ""
$found = $found | Sort-Object Url -Unique
$found | Format-Table -AutoSize

if (-not $Download) {
    Write-Host "==> re-run with -Download to fetch these" -ForegroundColor DarkGray
    return
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
foreach ($image in $found) {
    $name = "{0}_{1}" -f $image.Model, (Split-Path -Leaf ($image.Url -split '\?')[0])
    $path = Join-Path $OutDir $name
    Write-Host "==> downloading $name" -ForegroundColor Cyan
    curl.exe -sL $image.Url -o $path
    $size = (Get-Item $path).Length
    Write-Host ("    {0} bytes -> {1}" -f $size, $path) -ForegroundColor DarkGray
}
