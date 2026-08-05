<#
.SYNOPSIS
    Builds the Go protocol bridge as a C-ABI DLL.

.DESCRIPTION
    cgo cannot use MSVC, so -buildmode=c-shared needs a GCC-compatible compiler.
    This locates a mingw-w64 gcc (winlibs, MSYS2 or one already on PATH) and
    builds bridge/ into xmbridge.dll plus its generated header.

    The resulting DLL is a plain C-ABI module, so the MSVC-built application
    links against it through the generated import library without caring that
    the other side was compiled by gcc.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$OutputDir,

    [ValidateSet('release', 'debug')]
    [string]$Configuration = 'release'
)

$ErrorActionPreference = 'Stop'

$RepoRoot = Split-Path -Parent $PSScriptRoot
$BridgeDir = Join-Path $RepoRoot 'bridge'

function Find-Tool([string]$Name, [string[]]$Candidates) {
    $onPath = Get-Command $Name -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }
    foreach ($c in $Candidates) {
        if (Test-Path $c) { return $c }
    }
    return $null
}

$go = Find-Tool 'go' @(
    "$env:ProgramFiles\Go\bin\go.exe",
    "$env:LOCALAPPDATA\Programs\Go\bin\go.exe"
)
if (-not $go) {
    throw "Go toolchain not found. Install it with: winget install --id GoLang.Go"
}

$gcc = Find-Tool 'gcc' @(
    "$env:ProgramFiles\WinLibs\mingw64\bin\gcc.exe",
    "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin\gcc.exe",
    "C:\msys64\mingw64\bin\gcc.exe",
    "C:\mingw64\bin\gcc.exe"
)
if (-not $gcc) {
    # WinGet installs into a hashed directory; search for it as a last resort.
    $wingetRoot = Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Packages'
    if (Test-Path $wingetRoot) {
        $found = Get-ChildItem $wingetRoot -Recurse -Filter 'gcc.exe' -ErrorAction SilentlyContinue |
                 Select-Object -First 1
        if ($found) { $gcc = $found.FullName }
    }
}
if (-not $gcc) {
    throw "mingw-w64 gcc not found. Install it with: winget install --id BrechtSanders.WinLibs.POSIX.UCRT"
}

Write-Host "==> go:  $go"
Write-Host "==> gcc: $gcc"

New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$OutputDir = (Resolve-Path $OutputDir).Path
$dllPath = Join-Path $OutputDir 'xmbridge.dll'

# Put gcc on PATH for the duration of the build so cgo can find the whole
# toolchain (gcc drives ld, as and the runtime libs from the same bin dir).
$gccBin = Split-Path -Parent $gcc
$env:PATH = "$gccBin;$env:PATH"
$env:CGO_ENABLED = '1'
$env:CC = $gcc
$env:GOOS = 'windows'
$env:GOARCH = 'amd64'

$ldflags = '-s -w'
if ($Configuration -eq 'debug') { $ldflags = '' }

Push-Location $BridgeDir
try {
    Write-Host "==> go mod tidy"
    & $go mod tidy
    if ($LASTEXITCODE -ne 0) { throw "go mod tidy failed" }

    Write-Host "==> go build -buildmode=c-shared"
    $buildArgs = @('build', '-buildmode=c-shared', '-trimpath')
    if ($ldflags) { $buildArgs += @('-ldflags', $ldflags) }
    $buildArgs += @('-o', $dllPath, '.')

    & $go @buildArgs
    if ($LASTEXITCODE -ne 0) { throw "go build failed" }
} finally {
    Pop-Location
}

if (-not (Test-Path $dllPath)) {
    throw "expected $dllPath to exist after build"
}

Write-Host "==> built $dllPath"
