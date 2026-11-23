<#
.SYNOPSIS
    Checks for a stable release and performs a confirmed portable update.
.DESCRIPTION
    Launched from the Help menu. No archive is downloaded before confirm.request.
    After preparation, apply.request authorizes replacement once the parent exits.
    The helper runs from its own temporary directory so it can replace itself.
#>
[CmdletBinding()]
param(
    [string]$ApplicationDirectory,
    [string]$WorkDirectory,
    [string]$CurrentVersion,
    [ValidateSet('x64', 'arm64')]
    [string]$Architecture = 'x64',
    [int]$ParentProcessId
)

$ErrorActionPreference = 'Stop'

function ConvertTo-UpdateVersion([string]$Text) {
    if ($Text -notmatch '^v?(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$') {
        throw "Unrecognized release version: $Text"
    }
    return [version]$Text.TrimStart('v')
}

function Get-UpdateRelease([string]$TargetArchitecture) {
    $release = Invoke-RestMethod -Uri 'https://api.github.com/repos/lubos-hnanicek/xiaomi-camera-viewer/releases/latest' `
        -Headers @{ Accept = 'application/vnd.github+json'; 'X-GitHub-Api-Version' = '2022-11-28' } `
        -UserAgent 'XiaomiViewer-Update' -TimeoutSec 30
    if ($release.draft -ne $false -or $release.prerelease -ne $false) {
        throw 'GitHub did not return a stable published release.'
    }
    $version = ConvertTo-UpdateVersion $release.tag_name
    $name = "XiaomiViewer-win-$TargetArchitecture.zip"
    $assets = @($release.assets | Where-Object { $_.name -ceq $name })
    if ($assets.Count -ne 1) { throw "This release does not contain exactly one $name package." }
    $asset = $assets[0]
    $url = "https://github.com/lubos-hnanicek/xiaomi-camera-viewer/releases/download/v$version/$name"
    if ($asset.browser_download_url -cne $url -or $asset.state -ne 'uploaded') {
        throw 'The release package has an unexpected download URL or is not fully uploaded.'
    }
    if ($asset.digest -cnotmatch '^sha256:[0-9a-f]{64}$' -or
        $asset.size -le 0 -or $asset.size -gt 512MB) {
        throw 'The release package has no valid SHA-256 digest or has an invalid size.'
    }
    return [pscustomobject]@{
        Version = $version.ToString(3)
        Changelog = [string]$release.body
        Url = $url
        Size = [long]$asset.size
        Hash = $asset.digest.Substring(7)
    }
}

function Write-UpdateStatus([string]$State, [string]$Step, [string]$ErrorMessage = '') {
    $script:UpdateStatus.state = $State
    $script:UpdateStatus.step = $Step
    $script:UpdateStatus.error = $ErrorMessage
    $text = $script:UpdateStatus | ConvertTo-Json -Compress
    $temporary = Join-Path $WorkDirectory 'status.tmp'
    $result = Join-Path $WorkDirectory 'status.json'
    [IO.File]::WriteAllText($temporary, $text, [Text.UTF8Encoding]::new($false))
    if ([IO.File]::Exists($result)) {
        [IO.File]::Replace($temporary, $result, [NullString]::Value)
    } else {
        [IO.File]::Move($temporary, $result)
    }
}

function Test-UpdateCancellation {
    if ([IO.File]::Exists((Join-Path $WorkDirectory 'cancel.request')) -or $script:UpdateParent.HasExited) {
        throw [OperationCanceledException]::new('Update cancelled.')
    }
}

function Wait-UpdateSignal([string]$Name) {
    while (-not [IO.File]::Exists((Join-Path $WorkDirectory $Name))) {
        Test-UpdateCancellation
        Start-Sleep -Milliseconds 100
    }
    if ($Name -ceq 'apply.request') {
        if ([IO.File]::Exists((Join-Path $WorkDirectory 'cancel.request'))) {
            throw [OperationCanceledException]::new('Update cancelled.')
        }
    } else {
        Test-UpdateCancellation
    }
}

function Save-UpdateArchive($Release, [string]$Path) {
    Test-UpdateCancellation
    Write-UpdateStatus 'downloading' 'Downloading application package'
    $request = [Net.HttpWebRequest]::Create($Release.Url)
    $request.UserAgent = 'XiaomiViewer-Update'
    $request.Timeout = 30000
    $request.ReadWriteTimeout = 15000
    $response = $null
    $inputStream = $null
    $outputStream = $null
    try {
        $response = $request.GetResponse()
        if ($response.ResponseUri.Scheme -ne 'https') { throw 'The download redirected away from HTTPS.' }
        if ($response.ContentLength -ge 0 -and $response.ContentLength -ne $Release.Size) {
            throw 'The download size does not match the release metadata.'
        }
        $inputStream = $response.GetResponseStream()
        $outputStream = [IO.File]::Open($Path, 'CreateNew', 'Write', 'None')
        $buffer = New-Object byte[] 65536
        $clock = [Diagnostics.Stopwatch]::StartNew()
        $lastReport = 0L
        while (($count = $inputStream.Read($buffer, 0, $buffer.Length)) -gt 0) {
            Test-UpdateCancellation
            if ($clock.Elapsed.TotalMinutes -gt 15) { throw 'The update download timed out.' }
            $script:UpdateStatus.downloaded += $count
            if ($script:UpdateStatus.downloaded -gt $Release.Size) { throw 'The download exceeds the expected size.' }
            $outputStream.Write($buffer, 0, $count)
            if ($clock.ElapsedMilliseconds - $lastReport -ge 200) {
                Write-UpdateStatus 'downloading' 'Downloading application package'
                $lastReport = $clock.ElapsedMilliseconds
            }
        }
        if ($script:UpdateStatus.downloaded -ne $Release.Size) { throw 'The update download is incomplete.' }
    } finally {
        if ($outputStream) { $outputStream.Dispose() }
        if ($inputStream) { $inputStream.Dispose() }
        if ($response) { $response.Dispose() }
    }
    Test-UpdateCancellation
    Write-UpdateStatus 'downloading' 'Verifying SHA-256 digest'
    Test-UpdateArchive $Release $Path
}

function Test-UpdateArchive($Release, [string]$Path) {
    if ((Get-Item -LiteralPath $Path).Length -ne $Release.Size) {
        throw 'The update download is incomplete.'
    }
    $stream = [IO.File]::OpenRead($Path)
    $sha256 = [Security.Cryptography.SHA256]::Create()
    try {
        $hash = [BitConverter]::ToString($sha256.ComputeHash($stream)).Replace('-', '')
    } finally {
        $sha256.Dispose()
        $stream.Dispose()
    }
    if ($hash -ine $Release.Hash) {
        throw 'The update package failed SHA-256 verification. No application files were changed.'
    }
}

function Expand-UpdateArchive([string]$Path, [string]$Destination, [string]$Version, [string]$TargetArchitecture) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Directory]::CreateDirectory($Destination) | Out-Null
    $root = "XiaomiViewer-$Version-win-$TargetArchitecture/"
    $names = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $zip = [IO.Compression.ZipFile]::OpenRead($Path)
    $expanded = 0L
    try {
        foreach ($entry in $zip.Entries) {
            Test-UpdateCancellation
            $name = $entry.FullName.Replace('\', '/')
            if (-not $name.StartsWith($root, [StringComparison]::Ordinal)) { throw 'Unexpected directory in update package.' }
            $name = $name.Substring($root.Length)
            if ($name -eq '' -and $entry.Length -eq 0) { continue }
            # Release packages are flat. Reject traversal, streams, duplicate names and symlinks.
            if ($name -eq '' -or $name.Contains('/') -or $name.IndexOfAny([IO.Path]::GetInvalidFileNameChars()) -ge 0 -or
                $name -match '[. ]$|^(CON|PRN|AUX|NUL|COM[0-9]|LPT[0-9])(\.|$)' -or
                (($entry.ExternalAttributes -shr 16) -band 0xF000) -eq 0xA000 -or
                -not $names.Add($name)) {
                throw 'Unsafe filename in update package.'
            }
            $expanded += $entry.Length
            if ($expanded -gt 1GB) { throw 'The expanded update package is too large.' }
            [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, (Join-Path $Destination $name), $false)
        }
    } finally {
        $zip.Dispose()
    }
}

function Test-UpdatePayload([string]$Directory, [string]$Version, [string]$TargetArchitecture) {
    foreach ($name in @('XiaomiViewer.exe', 'XiaomiViewerUpdater.exe', 'xmbridge.dll',
            'ProggyVector-Regular.ttf', 'update-program.ps1')) {
        if (-not [IO.File]::Exists((Join-Path $Directory $name))) { throw "The update package is missing $name." }
    }
    foreach ($name in @('XiaomiViewer.exe', 'XiaomiViewerUpdater.exe')) {
        $executable = Join-Path $Directory $name
        $info = [Diagnostics.FileVersionInfo]::GetVersionInfo($executable)
        if ($info.ProductVersion -ne $Version) { throw "$name version does not match the release." }
        Assert-UpdatePe $executable $TargetArchitecture
    }
}

function Assert-UpdatePe([string]$Executable, [string]$TargetArchitecture) {
    $reader = [IO.BinaryReader]::new([IO.File]::OpenRead($Executable))
    try {
        if ($reader.ReadUInt16() -ne 0x5A4D) { throw 'The update executable is not a Windows binary.' }
        $reader.BaseStream.Position = 0x3C
        $offset = $reader.ReadUInt32()
        $reader.BaseStream.Position = $offset
        if ($reader.ReadUInt32() -ne 0x00004550) { throw 'The update executable has an invalid PE header.' }
        $expected = if ($TargetArchitecture -eq 'arm64') { 0xAA64 } else { 0x8664 }
        if ($reader.ReadUInt16() -ne $expected) { throw 'The update executable has the wrong architecture.' }
    } finally {
        $reader.Dispose()
    }
}

function Test-UpdateTarget([string]$Directory, [string]$Stage) {
    if ((Get-Item -LiteralPath $Directory).Attributes -band [IO.FileAttributes]::ReparsePoint) {
        throw 'In-place updates are not supported in a linked application directory.'
    }
    foreach ($file in Get-ChildItem -LiteralPath $Stage -File) {
        $target = Join-Path $Directory $file.Name
        if (Test-Path -LiteralPath $target) {
            $item = Get-Item -LiteralPath $target
            if ($item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
                throw "Cannot replace $($file.Name). The target is a directory or link."
            }
        }
    }
}

function Invoke-ProgramUpdate {
    $script:UpdateStatus = [ordered]@{
        state = 'checking'; step = ''; error = ''; version = ''; changelog = ''
        downloaded = 0L; total = 0L
    }
    try {
        $work = [IO.Path]::GetFullPath($WorkDirectory).TrimEnd('\')
        if ((Split-Path -Leaf $work) -notmatch '^XiaomiViewer-update-\{[0-9a-f-]{36}\}$' -or
            $work -ine $PSScriptRoot) {
            throw 'The update helper must run from its dedicated temporary directory.'
        }
        $script:UpdateParent = [Diagnostics.Process]::GetProcessById($ParentProcessId)
        $null = $script:UpdateParent.Handle
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        Write-UpdateStatus 'checking' 'Checking GitHub for a newer stable release'
        $current = ConvertTo-UpdateVersion $CurrentVersion
        $release = Get-UpdateRelease $Architecture
        Test-UpdateCancellation
        $script:UpdateStatus.version = $release.Version
        $script:UpdateStatus.changelog = $release.Changelog
        $script:UpdateStatus.total = $release.Size
        if ((ConvertTo-UpdateVersion $release.Version) -le $current) {
            Write-UpdateStatus 'up_to_date' 'You are running the latest version'
            return
        }
        Write-UpdateStatus 'available' 'A newer stable release is available'
        Wait-UpdateSignal 'confirm.request'
        $archive = Join-Path $WorkDirectory 'package.zip'
        Save-UpdateArchive $release $archive
        Write-UpdateStatus 'downloading' 'Unpacking and validating application files'
        $stage = Join-Path $WorkDirectory 'stage'
        Expand-UpdateArchive $archive $stage $release.Version $Architecture
        Test-UpdatePayload $stage $release.Version $Architecture
        Test-UpdateTarget $ApplicationDirectory $stage
        Test-UpdateCancellation
        Write-UpdateStatus 'ready' 'Update prepared; waiting for the viewer to close'
        Wait-UpdateSignal 'apply.request'
        Write-UpdateStatus 'installing' 'Standalone updater is replacing application files'
    } catch [OperationCanceledException] {
        Write-UpdateStatus 'cancelled' 'Update cancelled; no application files were changed'
    } catch {
        $message = $_.Exception.Message
        Write-UpdateStatus 'failed' '' $message
        throw
    } finally {
        if ($script:UpdateParent) {
            if ($script:UpdateStatus.state -eq 'cancelled') {
                try { Remove-Item -LiteralPath $work -Recurse -Force } catch {
                    Write-Warning "Could not remove cancelled update files: $($_.Exception.Message)"
                }
            }
            $script:UpdateParent.Dispose()
        }
    }
}

if ($MyInvocation.InvocationName -ne '.') {
    Invoke-ProgramUpdate
}
