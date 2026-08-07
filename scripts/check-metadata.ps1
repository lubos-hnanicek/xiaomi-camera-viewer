<#
.SYNOPSIS
    Checks the binaries that get code signed for the metadata a signature needs.

.DESCRIPTION
    SignPath's Open Source terms require every signed binary to carry a product
    name matching the project and a product version identical across the release,
    and enforce it with file metadata restrictions on the signing policy. A binary
    that does not comply is not rejected at build time -- it is rejected when the
    signing request is submitted, which is the worst possible moment to find out.

    So the same conditions are checked here, on every build, from the project's
    own definitions rather than from a second copy of the expected values.

    The FFmpeg DLLs are deliberately not checked and deliberately not signed:
    they are upstream binaries, and SignPath's terms allow shipping them unsigned
    inside a signed package but not signing them with the project's certificate.

.PARAMETER Configuration
    Which build output to check. Ignored if -Path is given.

.PARAMETER Path
    Directory holding the binaries, for checking a staged or signed copy instead.

.PARAMETER RequireSignature
    Also require a valid Authenticode signature. Used after signing, to catch a
    signing request that reported success but returned something unsigned.
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'RelWithDebInfo', 'Release')]
    [string]$Configuration = 'RelWithDebInfo',

    [string]$Path,

    [switch]$RequireSignature
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

    if ($RequireSignature) {
        $signature = Get-AuthenticodeSignature $file
        if ($signature.Status -eq 'Valid') {
            Write-Host "  PASS  $name is signed by $($signature.SignerCertificate.Subject)" -ForegroundColor Green
        } else {
            Write-Host "  FAIL  $name signature is $($signature.Status)" -ForegroundColor Red
            $failures++
        }
    }
}

Write-Host ""
if ($failures -gt 0) {
    Write-Host "$failures check(s) failed" -ForegroundColor Red
    exit 1
}

Write-Host "metadata is consistent" -ForegroundColor Green
