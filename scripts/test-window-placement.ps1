<#
.SYNOPSIS
    Checks that the window opens back where it was closed.

.DESCRIPTION
    Drives the real window: moves it, closes the app, and looks at both the
    saved config and where the window comes back. Also plants a position on a
    monitor that does not exist, which the app is supposed to ignore.

    The config is backed up and put back afterwards.
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Windows.Forms

Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class Wnd {
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr FindWindowW(string cls, string title);
    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")]
    public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")]
    public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")]
    public static extern bool IsZoomed(IntPtr h);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
    public struct RECT { public int Left, Top, Right, Bottom; }
}
"@

$exe = Join-Path (Split-Path -Parent $PSScriptRoot) 'build\msvc\RelWithDebInfo\XiaomiViewer.exe'
if (-not (Test-Path $exe)) { throw "Not built: $exe" }

$configPath = Join-Path $env:APPDATA 'XiaomiViewer\config.json'
$backup = "$configPath.placement-test-backup"
if (Test-Path $configPath) { Copy-Item $configPath $backup -Force }

$failures = @()
function Check([string]$what, [bool]$ok, [string]$detail) {
    if ($ok) {
        Write-Host ("    PASS  {0}" -f $what) -ForegroundColor Green
    } else {
        Write-Host ("    FAIL  {0}: {1}" -f $what, $detail) -ForegroundColor Red
        $script:failures += $what
    }
    if ($ok -and $detail) { Write-Host ("          {0}" -f $detail) -ForegroundColor DarkGray }
}

function Start-App {
    Start-Process $exe | Out-Null
    $h = [IntPtr]::Zero
    for ($i = 0; $i -lt 80 -and $h -eq [IntPtr]::Zero; $i++) {
        Start-Sleep -Milliseconds 250
        $h = [Wnd]::FindWindowW('XiaomiViewerWindow', 'Xiaomi Camera Viewer')
    }
    if ($h -eq [IntPtr]::Zero) { throw "The window never appeared." }
    Start-Sleep -Milliseconds 900
    return $h
}

function Stop-App([IntPtr]$h) {
    # WM_CLOSE, so the app shuts down the way it does for a user and gets as far
    # as writing its config.
    [Wnd]::PostMessageW($h, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    $p = Get-Process XiaomiViewer -ErrorAction SilentlyContinue
    if ($p) { $p.WaitForExit(20000) | Out-Null }
    Start-Sleep -Milliseconds 400
}

function Get-Saved {
    (Get-Content $configPath -Raw | ConvertFrom-Json).window
}

function Rect([IntPtr]$h) {
    $r = New-Object Wnd+RECT
    [Wnd]::GetWindowRect($h, [ref]$r) | Out-Null
    return $r
}

Get-Process XiaomiViewer -ErrorAction SilentlyContinue | ForEach-Object { $_.Kill(); $_.WaitForExit() }

try {
    Write-Host "==> a moved window comes back where it was" -ForegroundColor Cyan
    $h = Start-App
    # SWP_NOZORDER | SWP_NOACTIVATE
    [Wnd]::SetWindowPos($h, [IntPtr]::Zero, 220, 160, 1120, 740, 0x0014) | Out-Null
    Start-Sleep -Milliseconds 700
    $before = Rect $h
    Stop-App $h

    $saved = Get-Saved
    Check "the position is written to the config" ($null -ne $saved) `
        ("saved {0},{1} {2}x{3} maximized={4}" -f $saved.x, $saved.y, $saved.width, $saved.height, $saved.maximized)

    $h = Start-App
    $after = Rect $h
    Check "the window reopens at the same place" `
        (($before.Left -eq $after.Left) -and ($before.Top -eq $after.Top) -and
         (($before.Right - $before.Left) -eq ($after.Right - $after.Left)) -and
         (($before.Bottom - $before.Top) -eq ($after.Bottom - $after.Top))) `
        ("was {0},{1} {2}x{3}; came back {4},{5} {6}x{7}" -f
            $before.Left, $before.Top, ($before.Right - $before.Left), ($before.Bottom - $before.Top),
            $after.Left, $after.Top, ($after.Right - $after.Left), ($after.Bottom - $after.Top))

    Write-Host "==> maximized survives a restart" -ForegroundColor Cyan
    [Wnd]::ShowWindow($h, 3) | Out-Null   # SW_MAXIMIZE
    Start-Sleep -Milliseconds 700
    Stop-App $h

    $saved = Get-Saved
    Check "the config records it as maximized" ($saved.maximized -eq $true) `
        ("restore rectangle kept as {0},{1} {2}x{3}" -f $saved.x, $saved.y, $saved.width, $saved.height)

    $h = Start-App
    Check "it opens maximized" ([Wnd]::IsZoomed($h)) ""

    # Un-maximize so the restore rectangle is what the next test starts from.
    [Wnd]::ShowWindow($h, 9) | Out-Null   # SW_RESTORE
    Start-Sleep -Milliseconds 500
    $restored = Rect $h
    Check "un-maximizing lands back on the saved rectangle" `
        (($restored.Left -eq $before.Left) -and ($restored.Top -eq $before.Top)) `
        ("{0},{1}" -f $restored.Left, $restored.Top)
    Stop-App $h

    Write-Host "==> a position on a monitor that is gone is ignored" -ForegroundColor Cyan
    $json = Get-Content $configPath -Raw | ConvertFrom-Json
    $json.window.x = -6000
    $json.window.y = -6000
    $json.window.maximized = $false
    $json | ConvertTo-Json -Depth 10 | Set-Content $configPath -Encoding UTF8

    $h = Start-App
    $fallback = Rect $h
    Check "the window is not off in the void" (($fallback.Left -gt -2000) -and ($fallback.Top -gt -2000)) `
        ("opened at {0},{1}" -f $fallback.Left, $fallback.Top)
    Stop-App $h

    Write-Host "==> half off the bottom-right corner is still allowed" -ForegroundColor Cyan
    $screen = [System.Windows.Forms.Screen]::PrimaryScreen.WorkingArea
    $json = Get-Content $configPath -Raw | ConvertFrom-Json
    # Three quarters of the way across, so a 1120-wide window hangs off the edge
    # but keeps well over half of itself on the monitor.
    $json.window.x = [int]($screen.Width - 900)
    $json.window.y = 100
    $json | ConvertTo-Json -Depth 10 | Set-Content $configPath -Encoding UTF8

    $h = Start-App
    $edge = Rect $h
    Check "a mostly-visible window keeps its position" ($edge.Left -eq $json.window.x) `
        ("asked for {0}, opened at {1}" -f $json.window.x, $edge.Left)
    Stop-App $h
}
finally {
    Get-Process XiaomiViewer -ErrorAction SilentlyContinue | ForEach-Object { $_.Kill(); $_.WaitForExit() }
    if (Test-Path $backup) { Move-Item $backup $configPath -Force }
}

Write-Host ""
if ($failures.Count -eq 0) {
    Write-Host "all checks passed" -ForegroundColor Green
} else {
    Write-Host ("{0} check(s) failed" -f $failures.Count) -ForegroundColor Red
    exit 1
}
