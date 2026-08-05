<#
.SYNOPSIS
    Checks that both lenses of a dual-lens camera stream at the same time.

.DESCRIPTION
    Adds the second lens of the given camera to the grid alongside the first and
    reports what each tile delivered. Running both together is the case that
    matters: a camera may serve either lens alone and still refuse two sessions.

    The real config is backed up and restored, including if this is interrupted.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Did,
    [int]$Seconds = 20
)

$ErrorActionPreference = 'Stop'

$repoRoot   = Split-Path -Parent $PSScriptRoot
$exe        = Join-Path $repoRoot 'dist\XiaomiViewer-RelWithDebInfo\XiaomiViewer.exe'
$configPath = Join-Path $env:APPDATA 'XiaomiViewer\config.json'
$logPath    = Join-Path $env:APPDATA 'XiaomiViewer\xiaomi-viewer.log'
$backupPath = "$configPath.lens-backup"

if (-not (Test-Path $exe))        { throw "Not built: $exe" }
if (-not (Test-Path $configPath)) { throw "No config at $configPath" }

Copy-Item $configPath $backupPath -Force

try {
    $config = Get-Content $backupPath -Raw | ConvertFrom-Json

    $primary = $config.cameras | Where-Object { $_.did -eq $Did -and $_.channel -eq '' }
    if (-not $primary) { throw "No camera with did $Did and the default channel in the config." }

    foreach ($cam in $config.cameras) { $cam.enabled = ($cam.did -eq $Did) }

    if (-not ($config.cameras | Where-Object { $_.did -eq $Did -and $_.channel -eq '1' })) {
        # Only the channel differs, exactly as "Add lens 2" does: the lens
        # suffix comes from the label, not from a renamed copy.
        $second = $primary | ConvertTo-Json -Depth 10 | ConvertFrom-Json
        $second.channel = '1'
        $config.cameras += $second
    }

    $config | ConvertTo-Json -Depth 10 | Set-Content $configPath -Encoding UTF8
    Remove-Item $logPath -ErrorAction SilentlyContinue

    Write-Host "==> streaming both lenses for $Seconds seconds" -ForegroundColor Cyan
    $proc = Start-Process $exe -PassThru
    Start-Sleep -Seconds $Seconds
    if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force }
    Start-Sleep -Seconds 2

    Get-Content $logPath | Where-Object {
        $_ -match 'connected over|first keyframe|pipeline ready|could not open|session ended'
    }
} finally {
    Copy-Item $backupPath $configPath -Force
    Remove-Item $backupPath -Force
    Write-Host "`nconfig restored" -ForegroundColor DarkGray
}
