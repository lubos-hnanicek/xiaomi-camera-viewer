<#
.SYNOPSIS
    Checks that both lenses of a dual-lens camera stream at the same time.

.DESCRIPTION
    Adds the second lens of the given camera to the grid alongside the first and
    checks that both tiles decode while reporting the same remote endpoint.
    Matching endpoints prove that the two logical handles share one physical
    camera session.

    The real config is backed up and restored, including if this is interrupted.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Did,
    [int]$Seconds = 20
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $repoRoot 'build\msvc\RelWithDebInfo\XiaomiViewer.exe'
$configPath = Join-Path $env:APPDATA 'XiaomiViewer\config.json'
$logPath = Join-Path $env:APPDATA 'XiaomiViewer\xiaomi-viewer.log'
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

    $lines = @(Get-Content $logPath)
    $interesting = @($lines | Where-Object {
        $_ -match 'connected over|first keyframe|pipeline ready|could not open|session ended'
    })
    $interesting

    $connections = @($lines | Select-String -Pattern 'connected over \S+ to (\S+) \(vendor')
    $endpoints = @($connections | ForEach-Object { $_.Matches[0].Groups[1].Value } |
        Sort-Object -Unique)
    $keyframes = @($lines | Select-String -Pattern 'first keyframe').Count

    if ($connections.Count -ne 2) {
        throw "Expected two logical connections, found $($connections.Count)."
    }
    if ($endpoints.Count -ne 1) {
        throw "The two lenses used different physical endpoints: $($endpoints -join ', ')"
    }
    if ($keyframes -ne 2) {
        throw "Expected two decoded keyframes, found $keyframes."
    }

    Write-Host "`nshared endpoint confirmed: $($endpoints[0])" -ForegroundColor Green
} finally {
    Copy-Item $backupPath $configPath -Force
    Remove-Item $backupPath -Force
    Write-Host "`nconfig restored" -ForegroundColor DarkGray
}
