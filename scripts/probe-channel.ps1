<#
.SYNOPSIS
    Reports what each lens of a multi-lens camera delivers.

.DESCRIPTION
    Multi-lens Xiaomi cameras appear once in the device list but stream a second
    lens on another channel. This runs the app once per channel with only that
    camera enabled and reports the resolution that came back, so a model can be
    confirmed as dual-lens rather than assumed.

    The real config is backed up and restored, including if this is interrupted.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Did,
    [string[]]$Channels = @('', '1'),
    [string]$Quality = 'hd',
    [int]$SecondsPerRun = 14
)

$ErrorActionPreference = 'Stop'

$repoRoot   = Split-Path -Parent $PSScriptRoot
$exe        = Join-Path $repoRoot 'dist\XiaomiViewer-RelWithDebInfo\XiaomiViewer.exe'
$configPath = Join-Path $env:APPDATA 'XiaomiViewer\config.json'
$logPath    = Join-Path $env:APPDATA 'XiaomiViewer\xiaomi-viewer.log'
$backupPath = "$configPath.probe-backup"

if (-not (Test-Path $exe))        { throw "Not built: $exe" }
if (-not (Test-Path $configPath)) { throw "No config at $configPath" }

Copy-Item $configPath $backupPath -Force
$results = @()

try {
    foreach ($ch in $Channels) {
        $config = Get-Content $backupPath -Raw | ConvertFrom-Json

        $found = $false
        foreach ($cam in $config.cameras) {
            if ($cam.did -eq $Did) {
                $cam.enabled = $true
                $cam.channel = $ch
                $cam.quality = $Quality
                $found = $true
            } else {
                $cam.enabled = $false
            }
        }
        if (-not $found) { throw "No camera with did $Did in the config." }

        $config | ConvertTo-Json -Depth 10 | Set-Content $configPath -Encoding UTF8
        Remove-Item $logPath -ErrorAction SilentlyContinue

        $shown = if ($ch -eq '') { '(default)' } else { $ch }
        Write-Host "==> channel $shown" -ForegroundColor Cyan

        $proc = Start-Process $exe -PassThru
        Start-Sleep -Seconds $SecondsPerRun
        if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force }
        Start-Sleep -Seconds 2

        $log = if (Test-Path $logPath) { Get-Content $logPath -Raw } else { '' }

        $results += [PSCustomObject]@{
            Channel    = $shown
            Resolution = if ($log -match 'video output pipeline ready at (\d+x\d+)') { $matches[1] } else { '-' }
            Codec      = if ($log -match 'first keyframe, codec=(\S+)') { $matches[1] } else { '-' }
            Drops      = ([regex]::Matches($log, 'session ended')).Count
            Error      = if ($log -match 'could not open stream: (.+)') { $matches[1].Trim() } else { '' }
        }
    }
} finally {
    Copy-Item $backupPath $configPath -Force
    Remove-Item $backupPath -Force
}

Write-Host ""
$results | Format-Table -AutoSize
