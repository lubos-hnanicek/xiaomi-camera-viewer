<#
.SYNOPSIS
    Builds Xiaomi Camera Viewer end to end.

.DESCRIPTION
    Locates Visual Studio 2026, enters its x64 developer environment, then
    configures and builds with the bundled CMake and Ninja. The Go bridge DLL is
    built as a dependency of the executable, so this one script produces a
    complete, runnable output directory.

.PARAMETER Configuration
    Debug, RelWithDebInfo (default) or Release.

.PARAMETER Package
    Also stage a redistributable folder under dist/.

.PARAMETER Jobs
    Maximum parallel compilations. Capped to keep memory use sane on machines
    where a wide C++ build would otherwise exhaust the commit limit.
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'RelWithDebInfo', 'Release')]
    [string]$Configuration = 'RelWithDebInfo',

    [switch]$Package,

    [ValidateRange(1, 64)]
    [int]$Jobs = 16
)

$ErrorActionPreference = 'Stop'

$RepoRoot = Split-Path -Parent $PSScriptRoot
$BuildDir = Join-Path $RepoRoot 'build\msvc'

function Write-Step($Message) {
    Write-Host "==> $Message" -ForegroundColor Cyan
}

# --- Visual Studio environment ----------------------------------------------

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) {
    throw "vswhere.exe not found. Visual Studio 2017 or newer is required."
}

$vsPath = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath
if (-not $vsPath) {
    throw "No Visual Studio installation with the C++ toolset was found."
}

Write-Step "Visual Studio: $vsPath"

# vcvars only exports into its own process, so run it in cmd and import the
# resulting environment back here.
$vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) {
    throw "vcvars64.bat not found under $vsPath"
}

$captured = & cmd.exe /c "`"$vcvars`" >nul 2>&1 && set"
foreach ($line in $captured) {
    if ($line -match '^([^=]+)=(.*)$') {
        Set-Item -Path "Env:$($Matches[1])" -Value $Matches[2] -ErrorAction SilentlyContinue
    }
}

$cmakeRoot = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake'
$cmake = Join-Path $cmakeRoot 'CMake\bin\cmake.exe'
$ninja = Join-Path $cmakeRoot 'Ninja\ninja.exe'

if (-not (Test-Path $cmake)) {
    $fallback = Get-Command cmake -ErrorAction SilentlyContinue
    $cmake = if ($fallback) { $fallback.Source } else { $null }
}
if (-not $cmake) { throw "cmake.exe not found." }

if (-not (Test-Path $ninja)) {
    $fallback = Get-Command ninja -ErrorAction SilentlyContinue
    $ninja = if ($fallback) { $fallback.Source } else { $null }
}
if (-not $ninja) { throw "ninja.exe not found." }

Write-Step "cmake: $cmake"
Write-Step "ninja: $ninja"

# --- Dependencies -----------------------------------------------------------

if (-not (Test-Path (Join-Path $RepoRoot 'third_party\imgui\imgui.h'))) {
    Write-Step "Fetching dependencies"
    & (Join-Path $PSScriptRoot 'fetch-deps.ps1')
}

# --- Configure and build ----------------------------------------------------

Write-Step "Configuring ($Configuration)"
& $cmake -S $RepoRoot -B $BuildDir -G 'Ninja Multi-Config' `
    "-DCMAKE_MAKE_PROGRAM=$ninja" `
    '-DCMAKE_C_COMPILER=cl.exe' `
    '-DCMAKE_CXX_COMPILER=cl.exe' `
    '-DCMAKE_CONFIGURATION_TYPES=Debug;RelWithDebInfo;Release'
if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed" }

Write-Step "Building with at most $Jobs parallel compilations"
& $cmake --build $BuildDir --config $Configuration --parallel $Jobs
if ($LASTEXITCODE -ne 0) { throw "Build failed" }

$outputDir = Join-Path $BuildDir $Configuration
Write-Step "Built $outputDir\XiaomiViewer.exe"

# --- Package ----------------------------------------------------------------

if ($Package) {
    $dist = Join-Path $RepoRoot "dist\XiaomiViewer-$Configuration"
    Write-Step "Staging $dist"

    # Emptied and reused rather than deleted and recreated. A directory that is
    # open in Explorer, or that some shell is sitting in, cannot be removed, and
    # that is not a good enough reason to fail a build.
    #
    # A freshly written executable is often still held briefly by the on-access
    # virus scanner, so give the lock a few seconds to clear before giving up.
    if (Test-Path $dist) {
        $attempt = 0
        while ($true) {
            try {
                Get-ChildItem -Force $dist | Remove-Item -Recurse -Force -ErrorAction Stop
                break
            } catch {
                if (++$attempt -ge 5) { throw }
                Start-Sleep -Seconds 2
            }
        }
    }
    New-Item -ItemType Directory -Force -Path $dist | Out-Null

    Copy-Item (Join-Path $outputDir 'XiaomiViewer.exe') $dist
    Copy-Item (Join-Path $outputDir '*.dll') $dist -ErrorAction SilentlyContinue

    foreach ($doc in @('README.md', 'LICENSE', 'THIRD-PARTY-NOTICES.md')) {
        $path = Join-Path $RepoRoot $doc
        if (Test-Path $path) { Copy-Item $path $dist }
    }

    if ($Configuration -ne 'Debug') {
        # Ship symbols alongside so a crash report from a user is actionable.
        Copy-Item (Join-Path $outputDir 'XiaomiViewer.pdb') $dist -ErrorAction SilentlyContinue
    }

    Write-Step "Packaged $dist"
}
