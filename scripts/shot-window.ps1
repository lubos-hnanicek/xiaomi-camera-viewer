<#
.SYNOPSIS
    Screenshots the running app.

.DESCRIPTION
    A look at what actually got drawn, which is quicker than clicking through by
    hand and is the only way to check a layout without asking someone to look.

.PARAMETER Crop
    Which part to save alongside the full window: the caption, the pan and tilt
    pad in the bottom-left corner, or nothing.

.PARAMETER FocusTile
    Press F once the app is up, which focuses a single camera and is what makes
    the pan and tilt pad appear.
#>
[CmdletBinding()]
param(
    [ValidateSet('caption', 'pad', 'none')]
    [string]$Crop = 'caption',

    [switch]$FocusTile,

    [string]$Out = "$env:TEMP\xiaomi-viewer.png",

    # Time given to sign in from the saved token and get a first frame.
    [int]$WarmupSeconds = 14
)

$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class Shot {
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr FindWindowW(string cls, string title);
    [DllImport("user32.dll")]
    public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")]
    public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")]
    public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")]
    public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    public struct RECT { public int Left, Top, Right, Bottom; }
    public struct POINT { public int X, Y; }
}
"@

$exe = Join-Path (Split-Path -Parent $PSScriptRoot) 'build\msvc\RelWithDebInfo\XiaomiViewer.exe'
if (-not (Test-Path $exe)) { throw "Not built: $exe" }

Get-Process XiaomiViewer -ErrorAction SilentlyContinue | ForEach-Object { $_.Kill(); $_.WaitForExit() }
Start-Sleep -Milliseconds 500

Write-Host "==> starting the app"
Start-Process $exe | Out-Null

# Both the class and the title, because PowerShell marshals $null for a string
# parameter as an empty string rather than a null pointer, and FindWindow then
# hunts for a window whose title is exactly empty.
$window = [IntPtr]::Zero
for ($i = 0; $i -lt 60 -and $window -eq [IntPtr]::Zero; $i++) {
    Start-Sleep -Milliseconds 250
    $window = [Shot]::FindWindowW('XiaomiViewerWindow', 'Xiaomi Camera Viewer')
}
if ($window -eq [IntPtr]::Zero) { throw "The window never appeared." }

Write-Host "==> waiting $WarmupSeconds s for a camera to come up"
Start-Sleep -Seconds $WarmupSeconds

# 3 is SW_MAXIMIZE, so a tile is large enough for the pad to be drawn at all.
[Shot]::ShowWindow($window, 3) | Out-Null
[Shot]::SetForegroundWindow($window) | Out-Null
Start-Sleep -Milliseconds 800

if ($FocusTile) {
    Write-Host "==> focusing a tile with F"
    [System.Windows.Forms.SendKeys]::SendWait('f')
    Start-Sleep -Seconds 3
}

$rect = New-Object Shot+RECT
[Shot]::GetClientRect($window, [ref]$rect) | Out-Null
$topLeft = New-Object Shot+POINT
[Shot]::ClientToScreen($window, [ref]$topLeft) | Out-Null

$w = $rect.Right - $rect.Left
$h = $rect.Bottom - $rect.Top
$bmp = New-Object System.Drawing.Bitmap $w, $h
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($topLeft.X, $topLeft.Y, 0, 0, $bmp.Size)
$g.Dispose()

$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
Write-Host "==> full window: $Out"

if ($Crop -ne 'none') {
    # Cropped so the detail is legible instead of being a speck in a 4K shot.
    $area = if ($Crop -eq 'caption') {
        New-Object System.Drawing.Rectangle ([Math]::Max(0, $w - 700)), 0, ([Math]::Min(700, $w)), 60
    } else {
        New-Object System.Drawing.Rectangle 0, ($h - [Math]::Min(420, $h)), ([Math]::Min(420, $w)), ([Math]::Min(420, $h))
    }

    # Not $crop: PowerShell variable names ignore case, so that would assign a
    # bitmap to the -Crop parameter and fail its validation.
    $cropped = $bmp.Clone($area, $bmp.PixelFormat)
    $cropPath = [IO.Path]::ChangeExtension($Out, $null) + "-$Crop.png"
    $cropped.Save($cropPath, [System.Drawing.Imaging.ImageFormat]::Png)
    $cropped.Dispose()
    Write-Host "==> $Crop : $cropPath"
}

$bmp.Dispose()

Get-Process XiaomiViewer -ErrorAction SilentlyContinue | ForEach-Object { $_.Kill(); $_.WaitForExit() }
