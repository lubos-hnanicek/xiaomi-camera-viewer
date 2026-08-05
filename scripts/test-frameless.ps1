<#
.SYNOPSIS
    Checks the custom window frame.

.DESCRIPTION
    The window draws its own caption, which means it has to answer for
    everything Windows used to: where the resize edges are, which strip drags
    the window, and how big the client area is when maximized. This asks the
    window directly with WM_NCHITTEST and measures the rest, then clicks the
    minimize and close buttons for real.
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'

Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class Frame {
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr FindWindowW(string cls, string title);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr SendMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")]
    public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")]
    public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport("user32.dll")]
    public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")]
    public static extern bool IsZoomed(IntPtr h);
    [DllImport("user32.dll")]
    public static extern bool IsIconic(IntPtr h);
    [DllImport("user32.dll")]
    public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")]
    public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")]
    public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, UIntPtr extra);
    public struct RECT { public int Left, Top, Right, Bottom; }
    public struct POINT { public int X, Y; }
}
"@

Add-Type -AssemblyName System.Windows.Forms

$exe = Join-Path (Split-Path -Parent $PSScriptRoot) 'build\msvc\RelWithDebInfo\XiaomiViewer.exe'
if (-not (Test-Path $exe)) { throw "Not built: $exe" }

$configPath = Join-Path $env:APPDATA 'XiaomiViewer\config.json'
$backup = "$configPath.frameless-backup"
if (Test-Path $configPath) { Copy-Item $configPath $backup -Force }

$failures = @()
function Check([string]$what, [bool]$ok, [string]$detail) {
    if ($ok) {
        Write-Host ("    PASS  {0}" -f $what) -ForegroundColor Green
        if ($detail) { Write-Host ("          {0}" -f $detail) -ForegroundColor DarkGray }
    } else {
        Write-Host ("    FAIL  {0}: {1}" -f $what, $detail) -ForegroundColor Red
        $script:failures += $what
    }
}

# The hit test codes this cares about.
$HT = @{
    1 = 'HTCLIENT'; 2 = 'HTCAPTION'; 9 = 'HTMAXBUTTON'; 10 = 'HTLEFT'; 11 = 'HTRIGHT'
    12 = 'HTTOP'; 13 = 'HTTOPLEFT'; 14 = 'HTTOPRIGHT'; 15 = 'HTBOTTOM'
    16 = 'HTBOTTOMLEFT'; 17 = 'HTBOTTOMRIGHT'
}
function Name([int]$code) { if ($HT.ContainsKey($code)) { $HT[$code] } else { "code $code" } }

Get-Process XiaomiViewer -ErrorAction SilentlyContinue | ForEach-Object { $_.Kill(); $_.WaitForExit() }
Start-Sleep -Milliseconds 400

$process = Start-Process $exe -PassThru
$window = [IntPtr]::Zero
for ($i = 0; $i -lt 80 -and $window -eq [IntPtr]::Zero; $i++) {
    Start-Sleep -Milliseconds 250
    $window = [Frame]::FindWindowW('XiaomiViewerWindow', 'Xiaomi Camera Viewer')
}
if ($window -eq [IntPtr]::Zero) { throw "The window never appeared." }
Start-Sleep -Seconds 3

function Client { $r = New-Object Frame+RECT; [Frame]::GetClientRect($window, [ref]$r) | Out-Null; $r }
function Window { $r = New-Object Frame+RECT; [Frame]::GetWindowRect($window, [ref]$r) | Out-Null; $r }
function Origin {
    $p = New-Object Frame+POINT
    [Frame]::ClientToScreen($window, [ref]$p) | Out-Null
    $p
}

# WM_NCHITTEST takes screen coordinates packed into lParam.
function Probe([int]$clientX, [int]$clientY) {
    $o = Origin
    $l = (($o.Y + $clientY) -shl 16) -bor (($o.X + $clientX) -band 0xFFFF)
    [int][Frame]::SendMessageW($window, 0x0084, [IntPtr]::Zero, [IntPtr]$l)
}

function Click([int]$clientX, [int]$clientY) {
    $o = Origin
    [Frame]::SetForegroundWindow($window) | Out-Null
    [Frame]::SetCursorPos($o.X + $clientX, $o.Y + $clientY) | Out-Null
    Start-Sleep -Milliseconds 350
    [Frame]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)  # LEFTDOWN
    Start-Sleep -Milliseconds 80
    [Frame]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)  # LEFTUP
    Start-Sleep -Milliseconds 500
}

try {
    Write-Host "==> the window has no frame of its own" -ForegroundColor Cyan
    $w = Window
    $c = Client
    Check "the client area covers the whole window" `
        ((($w.Right - $w.Left) -eq ($c.Right - $c.Left)) -and
         (($w.Bottom - $w.Top) -eq ($c.Bottom - $c.Top))) `
        ("window {0}x{1}, client {2}x{3}" -f ($w.Right - $w.Left), ($w.Bottom - $w.Top),
            ($c.Right - $c.Left), ($c.Bottom - $c.Top))

    Write-Host "==> where the mouse lands" -ForegroundColor Cyan
    $midX = [int](($c.Right - $c.Left) / 2)
    $midY = [int](($c.Bottom - $c.Top) / 2)

    # How tall the caption is, found by asking rather than assuming.
    $captionHeight = 0
    for ($y = 1; $y -lt 80; $y++) {
        if ((Probe $midX $y) -eq 2) { $captionHeight = $y }
    }
    Check "there is a draggable caption strip" ($captionHeight -gt 8) `
        ("caption is $captionHeight px tall")

    $capY = [int]($captionHeight / 2)
    Check "the middle of the bar drags the window" ((Probe $midX $capY) -eq 2) `
        (Name (Probe $midX $capY))
    Check "the menus keep the mouse" ((Probe 20 $capY) -eq 1) (Name (Probe 20 $capY))
    Check "the content area is content" ((Probe $midX $midY) -eq 1) (Name (Probe $midX $midY))

    Write-Host "==> the resize edges are still there" -ForegroundColor Cyan
    $right = $c.Right - 2
    $bottom = $c.Bottom - 2
    Check "left edge"   ((Probe 1 $midY) -eq 10) (Name (Probe 1 $midY))
    Check "right edge"  ((Probe $right $midY) -eq 11) (Name (Probe $right $midY))
    Check "top edge"    ((Probe $midX 1) -eq 12) (Name (Probe $midX 1))
    Check "bottom edge" ((Probe $midX $bottom) -eq 15) (Name (Probe $midX $bottom))
    Check "bottom right corner" ((Probe $right $bottom) -eq 17) (Name (Probe $right $bottom))

    Write-Host "==> the caption buttons" -ForegroundColor Cyan
    # Find the maximize button by scanning in from the right edge.
    $maxFrom = -1
    $maxTo = -1
    for ($x = $c.Right - 1; $x -gt $c.Right - 400; $x--) {
        if ((Probe $x $capY) -eq 9) {
            if ($maxTo -lt 0) { $maxTo = $x }
            $maxFrom = $x
        }
    }
    Check "the maximize button is handed to Windows for snap layouts" ($maxFrom -gt 0) `
        ("HTMAXBUTTON from x=$maxFrom to x=$maxTo")

    $buttonWidth = $maxTo - $maxFrom + 1
    $minimizeX = $maxFrom - [int]($buttonWidth / 2)
    $closeX = $maxTo + [int]($buttonWidth / 2)
    Check "minimize and close are the app's to handle" `
        (((Probe $minimizeX $capY) -eq 1) -and ((Probe $closeX $capY) -eq 1)) `
        ("minimize {0}, close {1}" -f (Name (Probe $minimizeX $capY)), (Name (Probe $closeX $capY)))

    Write-Host "==> maximizing fits the work area" -ForegroundColor Cyan
    [Frame]::ShowWindow($window, 3) | Out-Null   # SW_MAXIMIZE
    Start-Sleep -Milliseconds 900
    $o = Origin
    $c = Client
    $work = [System.Windows.Forms.Screen]::FromHandle($window).WorkingArea
    Check "the maximized client area is exactly the work area" `
        (($o.X -eq $work.X) -and ($o.Y -eq $work.Y) -and
         (($c.Right - $c.Left) -eq $work.Width) -and (($c.Bottom - $c.Top) -eq $work.Height)) `
        ("client {0},{1} {2}x{3}; work area {4},{5} {6}x{7}" -f $o.X, $o.Y,
            ($c.Right - $c.Left), ($c.Bottom - $c.Top), $work.X, $work.Y, $work.Width, $work.Height)

    Check "no resize edge on a maximized window" ((Probe 1 $midY) -eq 1) (Name (Probe 1 $midY))

    Write-Host "==> double-clicking the caption restores it" -ForegroundColor Cyan
    # WM_NCLBUTTONDBLCLK with HTCAPTION, which is what a real double-click sends.
    [Frame]::SendMessageW($window, 0x00A3, [IntPtr]2, [IntPtr]0) | Out-Null
    Start-Sleep -Milliseconds 900
    Check "the window is no longer maximized" (-not [Frame]::IsZoomed($window)) ""

    Write-Host "==> clicking the buttons" -ForegroundColor Cyan
    $c = Client
    $capY = [int]($captionHeight / 2)
    Click $minimizeX $capY
    Check "minimize minimizes" ([Frame]::IsIconic($window)) ""

    [Frame]::ShowWindow($window, 9) | Out-Null   # SW_RESTORE
    Start-Sleep -Milliseconds 800

    Click $closeX $capY
    $process.WaitForExit(8000) | Out-Null
    Check "close closes" ($process.HasExited) ""
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
