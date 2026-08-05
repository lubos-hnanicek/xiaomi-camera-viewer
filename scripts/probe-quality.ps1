<#
.SYNOPSIS
    Reports the resolution each stream quality profile yields for one camera.

.DESCRIPTION
    Xiaomi's numeric quality profiles mean different things on different models
    and the mapping is not documented anywhere, so the only reliable way to pick
    a default is to ask the camera. This runs the app once per profile with only
    the chosen camera enabled and reports what came back.

    The real config is backed up and restored, including if this is interrupted.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Did,
    [string[]]$Qualities = @('0', '1', '2', '3', '4', '5'),
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
Write-Host "config backed up to $backupPath" -ForegroundColor DarkGray

$results = @()

try {
    foreach ($q in $Qualities) {
        $config = Get-Content $backupPath -Raw | ConvertFrom-Json

        $found = $false
        foreach ($cam in $config.cameras) {
            if ($cam.did -eq $Did) {
                $cam.enabled = $true
                $cam.quality = $q
                $found = $true
            } else {
                $cam.enabled = $false
            }
        }
        if (-not $found) { throw "No camera with did $Did in the config." }

        $config | ConvertTo-Json -Depth 10 | Set-Content $configPath -Encoding UTF8
        Remove-Item $logPath -ErrorAction SilentlyContinue

        Write-Host "==> quality $q" -ForegroundColor Cyan
        $proc = Start-Process $exe -PassThru
        Start-Sleep -Seconds $SecondsPerRun
        if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force }
        Start-Sleep -Seconds 2

        $log = if (Test-Path $logPath) { Get-Content $logPath -Raw } else { '' }

        $res = if ($log -match 'video output pipeline ready at (\d+x\d+)') { $matches[1] } else { '-' }
        $codec = if ($log -match 'first keyframe, codec=(\S+)') { $matches[1] } else { '-' }
        $proto = if ($log -match 'connected over (\S+) to') { $matches[1] } else { '-' }
        $ended = ([regex]::Matches($log, 'session ended')).Count
        $err = if ($log -match 'could not open stream: (.+)') { $matches[1].Trim() } else { '' }

        $results += [PSCustomObject]@{
            Quality    = $q
            Resolution = $res
            Codec      = $codec
            Transport  = $proto
            Drops      = $ended
            Error      = $err
        }

        Write-Host "    $res  $codec  $proto  drops=$ended $err" -ForegroundColor DarkGray
    }
} finally {
    Copy-Item $backupPath $configPath -Force
    Remove-Item $backupPath -Force
    Write-Host "config restored" -ForegroundColor DarkGray
}

Write-Host ""
$results | Format-Table -AutoSize
