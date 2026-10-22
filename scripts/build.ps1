<#
.SYNOPSIS
    Builds Xiaomi Camera Viewer end to end.

.DESCRIPTION
    Locates Visual Studio 2026, enters its x64 or ARM64 developer environment, then
    configures and builds with the bundled CMake and Ninja. The Go bridge DLL is
    built as a dependency of the executable, so this one script produces a
    complete, runnable output directory.

.PARAMETER Configuration
    Debug, RelWithDebInfo (default) or Release.

.PARAMETER Architecture
    x64 (default) or arm64.

.PARAMETER Package
    Also stage a redistributable folder under dist/ and zip it, by handing off to
    package.ps1.

.PARAMETER TestsOnly
    Configure, build and run the hardware-independent native CTest targets
    without building the application or Go bridge.

.PARAMETER Jobs
    Maximum parallel compilations. Capped to keep memory use sane on machines
    where a wide C++ build would otherwise exhaust the commit limit.
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'RelWithDebInfo', 'Release')]
    [string]$Configuration = 'RelWithDebInfo',

    [ValidateSet('x64', 'arm64')]
    [string]$Architecture = 'x64',

    [switch]$Package,

    [switch]$TestsOnly,

    [ValidateRange(1, 64)]
    [int]$Jobs = 16
)

$ErrorActionPreference = 'Stop'

$RepoRoot = Split-Path -Parent $PSScriptRoot
$BuildDir = Join-Path $RepoRoot $(if ($Architecture -eq 'x64') { 'build\msvc' } else { 'build\msvc-arm64' })

function Write-Step($Message) {
    Write-Host "==> $Message" -ForegroundColor Cyan
}

# --- Visual Studio environment ----------------------------------------------

$vcComponent = if ($Architecture -eq 'x64') {
    'Microsoft.VisualStudio.Component.VC.Tools.x86.x64'
} else {
    'Microsoft.VisualStudio.Component.VC.Tools.ARM64'
}
$vcvarsName = if ($Architecture -eq 'x64') { 'vcvars64.bat' } else { 'vcvarsamd64_arm64.bat' }

$vswhere = @(
    (Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'),
    (Join-Path $env:ProgramFiles 'Microsoft Visual Studio\Installer\vswhere.exe')
) | Where-Object { Test-Path $_ } | Select-Object -First 1

if ($vswhere) {
    $vsPath = & $vswhere -latest -products * `
        -requires $vcComponent `
        -property installationPath
    if (-not $vsPath) {
        throw "No Visual Studio installation with the C++ toolset was found."
    }
} else {
    # Some current Visual Studio installations omit vswhere even though the
    # vcvars scripts are present. Use the newest matching installation as a
    # local fallback; hosted runners still take the component-aware path above.
    $vcvars = Get-ChildItem "$env:ProgramFiles\Microsoft Visual Studio" -Recurse -Filter $vcvarsName -ErrorAction SilentlyContinue |
              Sort-Object FullName -Descending | Select-Object -First 1
    if (-not $vcvars) {
        throw "vswhere.exe and $vcvarsName were not found. Visual Studio with the C++ toolset is required."
    }
    $vsPath = $vcvars.Directory.Parent.Parent.Parent.FullName
}

Write-Step "Visual Studio: $vsPath"

# vcvars only exports into its own process, so run it in cmd and import the
# resulting environment back here.
$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\$vcvarsName"
if (-not (Test-Path $vcvars)) {
    throw "$vcvarsName not found under $vsPath"
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

$imguiHeader = Join-Path $RepoRoot 'third_party\imgui\imgui.h'
$ffmpegHeader = Join-Path $RepoRoot "third_party\ffmpeg\$Architecture\include\libavcodec\avcodec.h"
$uiFont = Join-Path $RepoRoot 'third_party\proggyvector\ProggyVector-Regular.ttf'
if (-not (Test-Path $imguiHeader) -or -not (Test-Path $ffmpegHeader) -or -not (Test-Path $uiFont)) {
    Write-Step "Fetching dependencies"
    & (Join-Path $PSScriptRoot 'fetch-deps.ps1') -Architecture $Architecture
}

# --- Configure and build ----------------------------------------------------

Write-Step "Configuring ($Configuration)"
& $cmake -S $RepoRoot -B $BuildDir -G 'Ninja Multi-Config' `
    "-DCMAKE_MAKE_PROGRAM=$ninja" `
    '-U' 'CMAKE_AR' `
    '-U' 'CMAKE_DLLTOOL' `
    '-U' 'CMAKE_LINKER' `
    '-U' 'CMAKE_NM' `
    '-U' 'CMAKE_OBJCOPY' `
    '-U' 'CMAKE_OBJDUMP' `
    '-U' 'CMAKE_RANLIB' `
    '-U' 'CMAKE_STRIP' `
    '-DCMAKE_C_COMPILER=cl.exe' `
    '-DCMAKE_CXX_COMPILER=cl.exe' `
    "-DXV_ARCHITECTURE=$Architecture" `
    '-DCMAKE_CONFIGURATION_TYPES=Debug;RelWithDebInfo;Release'
if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed" }

if ($TestsOnly) {
    Write-Step "Building native tests with at most $Jobs parallel compilations"
    & $cmake --build $BuildDir --config $Configuration `
        --target recording_timeline_tests camera_channel_tests --parallel $Jobs
    if ($LASTEXITCODE -ne 0) { throw "Test build failed" }

    Write-Step "Running native tests"
    & ctest --test-dir $BuildDir -C $Configuration --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw "Native tests failed" }
    return
}

Write-Step "Building with at most $Jobs parallel compilations"
& $cmake --build $BuildDir --config $Configuration --parallel $Jobs
if ($LASTEXITCODE -ne 0) { throw "Build failed" }

$outputDir = Join-Path $BuildDir $Configuration
Write-Step "Built $outputDir\XiaomiViewer.exe ($Architecture)"

# --- Package ----------------------------------------------------------------

# The app is handed out as a zip that is unpacked and run, so make the zip here
# rather than leave the last step of a local build to be done by hand.
if ($Package) {
    & (Join-Path $PSScriptRoot 'package.ps1') -Architecture $Architecture -Configuration $Configuration
}
