<#
.SYNOPSIS
    Stages and archives a redistributable copy of the application.

.DESCRIPTION
    Two halves of one job, separable on purpose. A release is code signed, and a
    signature has to be applied to the executable and the DLL *before* they are
    zipped -- signing the archive afterwards would leave the files that Windows
    actually checks unsigned. So the release workflow stages, hands the binaries
    to SignPath, drops the signed ones back into the staging directory, and only
    then archives:

        package.ps1 -Stage        # build output -> dist/XiaomiViewer-<config>/
        ... sign the binaries in place ...
        package.ps1 -Archive      # that directory -> dist/*.zip

    Run with neither switch, both happen in order, which is what a local build
    wants and what build.ps1 -Package does.

.PARAMETER Configuration
    Which build output to package. Debug stages but is never archived: it is not
    something to hand to anyone.

.PARAMETER Stage
    Copy the executable, its DLLs and the licence documents into dist/.

.PARAMETER Archive
    Write the release and symbols zips from the staged directory.

.PARAMETER RequireSymbols
    Treat a missing .pdb as an error. A local build has no use for the symbols
    archive, but a published release without it can never be given one later:
    the .pdb only matches the exact binary it was built with.
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'RelWithDebInfo', 'Release')]
    [string]$Configuration = 'RelWithDebInfo',

    [switch]$Stage,

    [switch]$Archive,

    [switch]$RequireSymbols
)

$ErrorActionPreference = 'Stop'

$RepoRoot = Split-Path -Parent $PSScriptRoot
$OutputDir = Join-Path $RepoRoot "build\msvc\$Configuration"
$StageDir = Join-Path $RepoRoot "dist\XiaomiViewer-$Configuration"

if (-not $Stage -and -not $Archive) {
    $Stage = $true
    $Archive = $true
}

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

# --- Stage ------------------------------------------------------------------

if ($Stage) {
    if (-not (Test-Path (Join-Path $OutputDir 'XiaomiViewer.exe'))) {
        throw "No XiaomiViewer.exe in $OutputDir. Build first with scripts/build.ps1."
    }

    Write-Step "Staging $StageDir"

    # Emptied and reused rather than deleted and recreated. A directory that is
    # open in Explorer, or that some shell is sitting in, cannot be removed, and
    # that is not a good enough reason to fail a build.
    #
    # A freshly written executable is often still held briefly by the on-access
    # virus scanner, so give the lock a few seconds to clear before giving up.
    if (Test-Path $StageDir) {
        $attempt = 0
        while ($true) {
            try {
                Get-ChildItem -Force $StageDir | Remove-Item -Recurse -Force -ErrorAction Stop
                break
            } catch {
                if (++$attempt -ge 5) { throw }
                Start-Sleep -Seconds 2
            }
        }
    }
    New-Item -ItemType Directory -Force -Path $StageDir | Out-Null

    Copy-Item (Join-Path $OutputDir 'XiaomiViewer.exe') $StageDir
    Copy-Item (Join-Path $OutputDir '*.dll') $StageDir -ErrorAction SilentlyContinue

    # LICENSE.LGPL-2.1.txt has to travel with the FFmpeg DLLs, not just be
    # referenced from the notices, for the LGPL to be satisfied.
    foreach ($doc in @('README.md', 'LICENSE', 'THIRD-PARTY-NOTICES.md', 'LICENSE.LGPL-2.1.txt')) {
        $path = Join-Path $RepoRoot $doc
        if (Test-Path $path) { Copy-Item $path $StageDir }
    }

    Write-Step "Packaged $StageDir"
}

# --- Archive ----------------------------------------------------------------

# Release and RelWithDebInfo of the same version write the same files on purpose:
# only one of them is the release, and it is the one that was packaged last.
if ($Archive) {
    if ($Configuration -eq 'Debug') {
        Write-Step "Debug builds stop at the staged folder; no archive written"
        return
    }

    if (-not (Test-Path $StageDir)) {
        throw "Nothing staged at $StageDir. Run package.ps1 -Stage first."
    }

    $version = & (Join-Path $PSScriptRoot 'get-version.ps1')
    $root = "XiaomiViewer-$version"

    Write-Archive -Path (Join-Path $RepoRoot "dist\$root-win-x64.zip") -Root $root `
        -Base $StageDir -Files (Get-ChildItem -File -Recurse $StageDir)

    # Symbols go in their own archive. The .pdb compresses to more than a quarter
    # of the download and nobody running the app needs it, but a crash report is
    # unreadable without the one matching the build it came from, so it has to be
    # kept and published rather than simply dropped.
    $symbols = Get-Item (Join-Path $OutputDir 'XiaomiViewer.pdb') -ErrorAction SilentlyContinue
    if ($symbols) {
        Write-Archive -Path (Join-Path $RepoRoot "dist\$root-win-x64-symbols.zip") -Root $root `
            -Base $symbols.DirectoryName -Files @($symbols)
    } elseif ($RequireSymbols) {
        throw "No XiaomiViewer.pdb in $OutputDir. Build the RelWithDebInfo configuration for a release."
    } else {
        Write-Warning "No XiaomiViewer.pdb in $OutputDir; the symbols archive was not written."
    }
}
