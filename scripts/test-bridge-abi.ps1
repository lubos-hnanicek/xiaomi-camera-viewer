<#
.SYNOPSIS
    Exercises the xmbridge.dll C ABI from outside Go.

.DESCRIPTION
    The Go unit tests cover the dispatch logic but stop at the cgo boundary.
    This drives the exported functions the way the C++ app does, so the buffer
    sizing convention, the handle table and the error paths are all checked
    against the real DLL. No Mi account is needed: every case here is expected
    to fail cleanly rather than reach the network.
#>
[CmdletBinding()]
param(
    [string]$DllPath
)

$ErrorActionPreference = 'Stop'

# Resolved here rather than as a parameter default: Windows PowerShell does not
# populate $PSScriptRoot while binding parameters.
if (-not $DllPath) {
    $DllPath = Join-Path (Split-Path -Parent $PSScriptRoot) 'build\msvc\RelWithDebInfo\xmbridge.dll'
}

if (-not (Test-Path $DllPath)) {
    throw "xmbridge.dll not found at $DllPath. Build first with scripts/build.ps1."
}

$DllPath = (Resolve-Path $DllPath).Path
Write-Host "==> testing $DllPath" -ForegroundColor Cyan

# The DLL is loaded by absolute path so the test does not depend on the working
# directory or on a copy already being on PATH.
$source = @"
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class XmBridge
{
    [DllImport("kernel32", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern IntPtr LoadLibrary(string path);

    [DllImport("kernel32", SetLastError = true)]
    private static extern IntPtr GetProcAddress(IntPtr module, string name);

    private delegate IntPtr VersionFn();
    private delegate int CallFn(string method, string request, byte[] outBuf, int cap);
    private delegate IntPtr StreamOpenFn(string request, byte[] outBuf, int cap);
    private delegate void StreamCloseFn(IntPtr handle);
    private delegate int StreamCommandFn(IntPtr handle, string request, byte[] outBuf, int cap);

    private static IntPtr module;

    public static void Load(string path)
    {
        module = LoadLibrary(path);
        if (module == IntPtr.Zero)
            throw new Exception("LoadLibrary failed: " + Marshal.GetLastWin32Error());
    }

    public static bool HasExport(string name)
    {
        return GetProcAddress(module, name) != IntPtr.Zero;
    }

    private static T Fn<T>(string name) where T : class
    {
        IntPtr proc = GetProcAddress(module, name);
        if (proc == IntPtr.Zero)
            throw new Exception("missing export: " + name);
        return Marshal.GetDelegateForFunctionPointer(proc, typeof(T)) as T;
    }

    public static string Version()
    {
        return Marshal.PtrToStringAnsi(Fn<VersionFn>("xmb_version")());
    }

    // Returns the length the bridge reported, and the bytes it wrote.
    public static int Call(string method, string request, int capacity, out string response)
    {
        byte[] buffer = new byte[Math.Max(capacity, 1)];
        int needed = Fn<CallFn>("xmb_call")(method, request, buffer, capacity);
        int copied = (needed >= 0 && needed <= capacity) ? needed : 0;
        response = Encoding.UTF8.GetString(buffer, 0, copied);
        return needed;
    }

    public static IntPtr StreamOpen(string request, out string response)
    {
        byte[] buffer = new byte[8192];
        IntPtr handle = Fn<StreamOpenFn>("xmb_stream_open")(request, buffer, buffer.Length);
        int end = Array.IndexOf(buffer, (byte)0);
        if (end < 0) end = buffer.Length;
        response = Encoding.UTF8.GetString(buffer, 0, end);
        return handle;
    }

    public static int StreamCommand(IntPtr handle, string request, out string response)
    {
        byte[] buffer = new byte[8192];
        int needed = Fn<StreamCommandFn>("xmb_stream_command")(handle, request, buffer, buffer.Length);
        int copied = (needed >= 0 && needed <= buffer.Length) ? needed : 0;
        response = Encoding.UTF8.GetString(buffer, 0, copied);
        return needed;
    }

    public static void StreamClose(IntPtr handle)
    {
        Fn<StreamCloseFn>("xmb_stream_close")(handle);
    }
}
"@

Add-Type -TypeDefinition $source -Language CSharp

$script:failures = 0

function Assert-True($condition, $description) {
    if ($condition) {
        Write-Host "  PASS  $description" -ForegroundColor Green
    } else {
        Write-Host "  FAIL  $description" -ForegroundColor Red
        $script:failures++
    }
}

[XmBridge]::Load($DllPath)

Write-Host "-- exports"
foreach ($name in @('xmb_version', 'xmb_call', 'xmb_stream_open', 'xmb_stream_read',
                    'xmb_stream_command', 'xmb_stream_close')) {
    Assert-True ([XmBridge]::HasExport($name)) "$name is exported"
}

Write-Host "-- version"
$version = [XmBridge]::Version()
Assert-True (-not [string]::IsNullOrEmpty($version)) "xmb_version returns '$version'"

Write-Host "-- control plane"
$response = ''
$needed = [XmBridge]::Call('nonsense.method', '{}', 4096, [ref]$response)
Assert-True ($needed -gt 0) "unknown method reports a response length ($needed)"
Assert-True ($response -match '"ok":false') "unknown method is rejected"
Assert-True ($response -match 'nonsense.method') "the error names the offending method"

$response = ''
$needed = [XmBridge]::Call('account.forget', '{"user_id":"nobody"}', 4096, [ref]$response)
Assert-True ($response -match '"ok":true') "account.forget succeeds for an unknown account"

$response = ''
$needed = [XmBridge]::Call('device.list', '{"user_id":"nobody"}', 4096, [ref]$response)
Assert-True ($response -match '"ok":false') "device.list fails when not signed in"

# The snprintf convention: too small a buffer must report the size needed and
# write nothing, and the retry with that size must then fit exactly.
Write-Host "-- buffer sizing"
$short = ''
$neededShort = [XmBridge]::Call('nonsense.method', '{}', 4, [ref]$short)
Assert-True ($neededShort -gt 4) "an undersized buffer reports the required size ($neededShort)"
Assert-True ($short.Length -eq 0) "nothing is written into an undersized buffer"

$exact = ''
$neededExact = [XmBridge]::Call('nonsense.method', '{}', $neededShort, [ref]$exact)
Assert-True ($neededExact -eq $neededShort) "the retry needs exactly the reported size"
Assert-True ($exact.Length -eq $neededShort) "the retry fills the buffer exactly"

Write-Host "-- media plane"
$response = ''
$handle = [XmBridge]::StreamOpen('{"user_id":"nobody","did":"","ip":""}', [ref]$response)
Assert-True ($handle -eq [IntPtr]::Zero) "opening a stream without did or ip returns no handle"
Assert-True ($response -match '"ok":false') "the failure is explained in the response"

$response = ''
$status = [XmBridge]::StreamCommand([IntPtr]::Zero, '{"method":"stats"}', [ref]$response)
Assert-True ($status -eq -4) "a command on a null handle returns XMB_ERR_INVALID_HANDLE ($status)"

# Closing a handle that was never opened must not fault.
[XmBridge]::StreamClose([IntPtr]::Zero)
Assert-True $true "closing a null handle is harmless"

Write-Host ""
if ($script:failures -gt 0) {
    Write-Host "$($script:failures) check(s) failed" -ForegroundColor Red
    exit 1
}

Write-Host "all ABI checks passed" -ForegroundColor Green
