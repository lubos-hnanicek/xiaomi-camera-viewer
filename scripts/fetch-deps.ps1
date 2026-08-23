<#
.SYNOPSIS
    Downloads the third-party dependencies that are not vendored into the repo.

.DESCRIPTION
    Fetches Dear ImGui (source), nlohmann/json (single header) and a prebuilt
    LGPL shared FFmpeg (headers + import libs + runtime DLLs) into third_party/.

    FFmpeg is taken as a prebuilt binary rather than built from source because
    building it under MSVC needs an MSYS2 shell and takes the better part of an
    hour; the BtbN LGPL shared builds already ship MSVC-compatible .lib files.

    Re-running is cheap: anything already present is skipped unless -Force.
#>
[CmdletBinding()]
param(
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$RepoRoot = Split-Path -Parent $PSScriptRoot
$ThirdParty = Join-Path $RepoRoot 'third_party'

# Pinned versions. Bump deliberately, not automatically.
#
# Pinned by content, not just by name: a signed release has to be reproducible
# from its source commit, and both a git tag and a GitHub release asset can be
# replaced after the fact. BtbN's "latest" release in particular is rewritten
# nightly, so the same commit built a week apart used to link a different FFmpeg.
# Every download is therefore checked against a hash recorded here, and the
# resolved ImGui commit against the one the tag pointed at when it was pinned.
# The docking branch, not master: multi-viewport support lives only there, and it
# is what lets the help and log windows be dragged out of the main window and
# become windows of their own. Same release as the master tag of the same name.
$ImGuiTag = 'v1.92.9b-docking'
$ImGuiCommit = 'b48d1afbe8ee8b238e2961dc363a949dd7304e23'

$JsonTag = 'v3.12.0'
$JsonSha256 = 'AAF127C04CB31C406E5B04A63F1AE89369FCCDE6D8FA7CDDA1ED4F32DFC5DE63'

$FFmpegRelease = 'autobuild-2026-07-31-14-10'
$FFmpegAsset = 'ffmpeg-n8.1.2-34-g9b6c8969e0-win64-lgpl-shared-8.1.zip'
$FFmpegSha256 = '97E1AF03208A4582C26D5F3E670AB51AF50B8D5788DA78231AAE218A7C917D56'
$FFmpegUrl = "https://github.com/BtbN/FFmpeg-Builds/releases/download/$FFmpegRelease/$FFmpegAsset"

New-Item -ItemType Directory -Force -Path $ThirdParty | Out-Null

function Write-Step($Message) {
    Write-Host "==> $Message" -ForegroundColor Cyan
}

function Assert-Sha256($Path, $Expected) {
    $actual = (Get-FileHash $Path -Algorithm SHA256).Hash
    if ($actual -ne $Expected) {
        throw "$(Split-Path -Leaf $Path) does not match its pinned hash.`n" +
              "  expected $Expected`n" +
              "  actual   $actual`n" +
              "Either the download was corrupted or the upstream asset was replaced. " +
              "Do not build a release from it until you know which."
    }
}

# --- Dear ImGui -------------------------------------------------------------
$ImGuiDir = Join-Path $ThirdParty 'imgui'
if ($Force -and (Test-Path $ImGuiDir)) {
    Remove-Item -Recurse -Force $ImGuiDir
}
if (Test-Path (Join-Path $ImGuiDir 'imgui.h')) {
    Write-Step "Dear ImGui already present, skipping"
} else {
    Write-Step "Cloning Dear ImGui $ImGuiTag"
    git clone --depth 1 --branch $ImGuiTag https://github.com/ocornut/imgui.git $ImGuiDir
    if ($LASTEXITCODE -ne 0) { throw "git clone of imgui failed" }

    $head = (git -C $ImGuiDir rev-parse HEAD).Trim()
    if ($head -ne $ImGuiCommit) {
        throw "Dear ImGui $ImGuiTag now points at $head, not the pinned $ImGuiCommit."
    }
}

# --- nlohmann/json ----------------------------------------------------------
$JsonDir = Join-Path $ThirdParty 'nlohmann'
$JsonHeader = Join-Path $JsonDir 'json.hpp'
if ($Force -and (Test-Path $JsonHeader)) {
    Remove-Item -Force $JsonHeader
}
if (Test-Path $JsonHeader) {
    Write-Step "nlohmann/json already present, skipping"
} else {
    Write-Step "Downloading nlohmann/json $JsonTag"
    New-Item -ItemType Directory -Force -Path $JsonDir | Out-Null
    Invoke-WebRequest "https://github.com/nlohmann/json/releases/download/$JsonTag/json.hpp" -OutFile $JsonHeader
    Assert-Sha256 $JsonHeader $JsonSha256
}

# --- FFmpeg -----------------------------------------------------------------
$FFmpegDir = Join-Path $ThirdParty 'ffmpeg'
if ($Force -and (Test-Path $FFmpegDir)) {
    Remove-Item -Recurse -Force $FFmpegDir
}
if (Test-Path (Join-Path $FFmpegDir 'include\libavcodec\avcodec.h')) {
    Write-Step "FFmpeg already present, skipping"
} else {
    Write-Step "Downloading FFmpeg ($FFmpegAsset, about 70 MB)"
    $zip = Join-Path $env:TEMP $FFmpegAsset
    Invoke-WebRequest $FFmpegUrl -OutFile $zip
    Assert-Sha256 $zip $FFmpegSha256

    Write-Step "Extracting FFmpeg"
    $staging = Join-Path $env:TEMP 'ffmpeg-staging'
    if (Test-Path $staging) { Remove-Item -Recurse -Force $staging }
    Expand-Archive $zip -DestinationPath $staging -Force

    # The archive has a single versioned top-level directory; flatten it.
    $inner = Get-ChildItem $staging -Directory | Select-Object -First 1
    if (-not $inner) { throw "unexpected FFmpeg archive layout" }
    New-Item -ItemType Directory -Force -Path $FFmpegDir | Out-Null
    foreach ($sub in @('include', 'lib', 'bin')) {
        $src = Join-Path $inner.FullName $sub
        if (Test-Path $src) {
            Copy-Item $src -Destination $FFmpegDir -Recurse -Force
        }
    }

    Remove-Item -Recurse -Force $staging
    Remove-Item -Force $zip
}

Write-Step "Dependencies ready under $ThirdParty"
