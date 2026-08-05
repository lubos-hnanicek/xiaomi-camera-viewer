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
$ImGuiTag = 'v1.92.9b'
$JsonTag = 'v3.12.0'
$FFmpegAsset = 'ffmpeg-n8.1-latest-win64-lgpl-shared-8.1.zip'
$FFmpegUrl = "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/$FFmpegAsset"

New-Item -ItemType Directory -Force -Path $ThirdParty | Out-Null

function Write-Step($Message) {
    Write-Host "==> $Message" -ForegroundColor Cyan
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
}

# --- FFmpeg -----------------------------------------------------------------
$FFmpegDir = Join-Path $ThirdParty 'ffmpeg'
if ($Force -and (Test-Path $FFmpegDir)) {
    Remove-Item -Recurse -Force $FFmpegDir
}
if (Test-Path (Join-Path $FFmpegDir 'include\libavcodec\avcodec.h')) {
    Write-Step "FFmpeg already present, skipping"
} else {
    Write-Step "Downloading FFmpeg ($FFmpegAsset, about 40 MB)"
    $zip = Join-Path $env:TEMP $FFmpegAsset
    Invoke-WebRequest $FFmpegUrl -OutFile $zip

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
