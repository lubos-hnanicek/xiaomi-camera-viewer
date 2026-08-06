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

# Zips files under one named directory rather than at the root, so unpacking
# cannot spill them into whatever directory the archive was opened in, and so
# that unpacking the symbols over the application puts the .pdb beside the
# executable, which is where a debugger goes looking for it.
function Write-Archive($Path, $Root, $Base, $Files) {
    if (Test-Path $Path) { Remove-Item $Path -Force }

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [System.IO.Compression.ZipFile]::Open($Path, 'Create')
    try {
        foreach ($file in $Files) {
            $relative = $file.FullName.Substring($Base.Length + 1).Replace('\', '/')
            [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
                $archive, $file.FullName, "$Root/$relative",
                [System.IO.Compression.CompressionLevel]::Optimal) | Out-Null
        }
    } finally {
        $archive.Dispose()
    }

    Write-Step "Wrote $Path ($([int]((Get-Item $Path).Length / 1MB)) MB)"
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

    # LICENSE.LGPL-2.1.txt has to travel with the FFmpeg DLLs, not just be
    # referenced from the notices, for the LGPL to be satisfied.
    foreach ($doc in @('README.md', 'LICENSE', 'THIRD-PARTY-NOTICES.md', 'LICENSE.LGPL-2.1.txt')) {
        $path = Join-Path $RepoRoot $doc
        if (Test-Path $path) { Copy-Item $path $dist }
    }

    Write-Step "Packaged $dist"

    # The app is handed out as a zip that is unpacked and run, so make the zip
    # here rather than leave the last step of a release to be done by hand.
    #
    # A Debug build is not something to hand to anyone, so it stops at the
    # staged folder. Release and RelWithDebInfo of the same version write the
    # same files on purpose: only one of them is the release, and it is the one
    # that was packaged last.
    if ($Configuration -ne 'Debug') {
        $projectFile = Join-Path $RepoRoot 'CMakeLists.txt'
        if ((Get-Content $projectFile -Raw) -notmatch '(?m)^\s*VERSION\s+(\d+\.\d+\.\d+)') {
            throw "No project version found in $projectFile"
        }
        $version = $Matches[1]
        $root = "XiaomiViewer-$version"

        Write-Archive -Path (Join-Path $RepoRoot "dist\$root-win-x64.zip") -Root $root `
            -Base $dist -Files (Get-ChildItem -File -Recurse $dist)

        # Symbols go in their own archive. The .pdb compresses to more than a
        # quarter of the download and nobody running the app needs it, but a
        # crash report is unreadable without the one matching the build it came
        # from, so it has to be kept and published rather than simply dropped.
        $symbols = Get-Item (Join-Path $outputDir 'XiaomiViewer.pdb') -ErrorAction SilentlyContinue
        if ($symbols) {
            Write-Archive -Path (Join-Path $RepoRoot "dist\$root-win-x64-symbols.zip") -Root $root `
                -Base $symbols.DirectoryName -Files @($symbols)
        } else {
            Write-Warning "No XiaomiViewer.pdb in $outputDir; the symbols archive was not written."
        }
    }
}
