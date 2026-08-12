<#
.SYNOPSIS
    Checks that the release binaries carry consistent version metadata.

.DESCRIPTION
    Reads the product name and version from the project's own definitions and
    verifies that XiaomiViewer.exe and xmbridge.dll agree with them. The FFmpeg
    DLLs are upstream binaries and therefore outside this project's metadata.

.PARAMETER Configuration
    Which build output to check. Ignored if -Path is given.

.PARAMETER Path
    Directory holding the binaries, for checking a staged copy instead.
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'RelWithDebInfo', 'Release')]
    [string]$Configuration = 'RelWithDebInfo',

    [string]$Path
)

$ErrorActionPreference = 'Stop'

$RepoRoot = Split-Path -Parent $PSScriptRoot
if (-not $Path) {
    $Path = Join-Path $RepoRoot "build\msvc\$Configuration"
}
if (-not (Test-Path $Path)) {
    throw "Nothing to check at $Path."
}

$expectedVersion = & (Join-Path $PSScriptRoot 'get-version.ps1')

$cmakeLists = Get-Content (Join-Path $RepoRoot 'CMakeLists.txt') -Raw
if ($cmakeLists -notmatch '(?m)^\s*set\(XV_PRODUCT_NAME\s+"([^"]+)"\)') {
    throw "No XV_PRODUCT_NAME found in CMakeLists.txt"
}
$expectedProduct = $Matches[1]

Write-Host "==> checking $Path" -ForegroundColor Cyan
Write-Host "    product '$expectedProduct', version $expectedVersion"

$failures = 0

function Test-Value($Name, $Actual, $Expected, $File) {
    if ($Actual -eq $Expected) {
        Write-Host "  PASS  $File $Name is '$Actual'" -ForegroundColor Green
    } else {
        Write-Host "  FAIL  $File $Name is '$Actual', expected '$Expected'" -ForegroundColor Red
        $script:failures++
    }
}

foreach ($name in @('XiaomiViewer.exe', 'xmbridge.dll')) {
    $file = Join-Path $Path $name
    if (-not (Test-Path $file)) {
        Write-Host "  FAIL  $name is missing" -ForegroundColor Red
        $failures++
        continue
    }

    $info = (Get-Item $file).VersionInfo
    Test-Value 'ProductName' $info.ProductName $expectedProduct $name
    Test-Value 'ProductVersion' $info.ProductVersion $expectedVersion $name
    Test-Value 'FileVersion' $info.FileVersion $expectedVersion $name
}

Write-Host ""
if ($failures -gt 0) {
    Write-Host "$failures check(s) failed" -ForegroundColor Red
    exit 1
}

Write-Host "metadata is consistent" -ForegroundColor Green
