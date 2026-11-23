[CmdletBinding()]
param([Parameter(Mandatory)][string]$HelperScript)

$ErrorActionPreference = 'Stop'
. $HelperScript

function Assert($Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function Assert-Throws([scriptblock]$Action, [string]$Pattern) {
    try { & $Action } catch {
        if ($_.Exception.Message -match $Pattern) { return }
        throw
    }
    throw "Expected failure matching: $Pattern"
}

function Read-TestStatus([string]$Path) {
    $stream = [IO.File]::Open($Path, 'Open', 'Read', ([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
    $reader = [IO.StreamReader]::new($stream)
    try { return $reader.ReadToEnd() | ConvertFrom-Json } finally { $reader.Dispose() }
}

function Wait-Status([string]$Directory, [string]$State, $Process) {
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    while ([DateTime]::UtcNow -lt $deadline) {
        $path = Join-Path $Directory 'status.json'
        if (Test-Path -LiteralPath $path) {
            $status = Read-TestStatus $path
            if ($status.state -eq $State) { return $status }
            if ($status.state -eq 'failed') { throw "Helper failed: $($status.error)" }
        }
        if ($Process.HasExited) {
            if (Test-Path -LiteralPath $path) {
                $status = Read-TestStatus $path
                if ($status.state -eq $State) { return $status }
            }
            $errorPath = "$Directory-error.log"
            $details = if (Test-Path -LiteralPath $errorPath) { Get-Content -LiteralPath $errorPath -Raw } else { '' }
            throw "Helper exited before $State (exit $($Process.ExitCode)). $details"
        }
        Start-Sleep -Milliseconds 50
    }
    throw "Timed out waiting for $State."
}

function New-TestArchive([string]$Path, [string[]]$Names) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::Open($Path, 'Create')
    try {
        foreach ($name in $Names) {
            $entry = $zip.CreateEntry($name)
            $writer = [IO.StreamWriter]::new($entry.Open())
            $writer.Write('new')
            $writer.Dispose()
        }
    } finally {
        $zip.Dispose()
    }
}

$testRoot = Join-Path ([IO.Path]::GetTempPath()) ("XiaomiViewer update tests " + [char]0x017E + ' ' + [guid]::NewGuid().ToString())
[IO.Directory]::CreateDirectory($testRoot) | Out-Null
$processes = [Collections.Generic.List[Diagnostics.Process]]::new()
try {
    Assert ((ConvertTo-UpdateVersion 'v0.7.10') -gt (ConvertTo-UpdateVersion '0.7.9')) 'Versions were compared lexically.'
    Assert ((ConvertTo-UpdateVersion '1.0.0') -gt (ConvertTo-UpdateVersion '0.99.99')) 'Major version ordering failed.'
    foreach ($version in @('v1.2.3-beta', 'v1.2', '1.2.3.4', 'v01.2.3', '../1.2.3')) {
        Assert-Throws { ConvertTo-UpdateVersion $version } 'Unrecognized'
    }

    $script:FixtureRelease = [pscustomobject]@{
        tag_name = 'v9.8.7'; draft = $false; prerelease = $false; body = "Changelog`n- Test change"
        assets = @([pscustomobject]@{
            name = 'XiaomiViewer-win-arm64.zip'; state = 'uploaded'; size = 100
            digest = 'sha256:' + ('a' * 64)
            browser_download_url = 'https://github.com/lubos-hnanicek/xiaomi-camera-viewer/releases/download/v9.8.7/XiaomiViewer-win-arm64.zip'
        })
    }
    function Invoke-RestMethod { return $script:FixtureRelease }
    $release = Get-UpdateRelease 'arm64'
    Assert ($release.Version -eq '9.8.7' -and $release.Changelog.Contains('Test change')) 'Release metadata was lost.'
    Assert-Throws { Get-UpdateRelease 'x64' } 'exactly one'
    $asset = $script:FixtureRelease.assets[0]
    $asset.digest = $null
    Assert-Throws { Get-UpdateRelease 'arm64' } 'SHA-256'
    $asset.digest = 'sha256:' + ('a' * 64)
    $asset.browser_download_url = 'https://example.com/package.zip'
    Assert-Throws { Get-UpdateRelease 'arm64' } 'unexpected download URL'
    $script:FixtureRelease.prerelease = $true
    Assert-Throws { Get-UpdateRelease 'arm64' } 'stable'

    $script:UpdateParent = [Diagnostics.Process]::GetCurrentProcess()
    $WorkDirectory = $testRoot
    $prefix = 'XiaomiViewer-9.8.7-win-x64/'
    $goodZip = Join-Path $testRoot 'good.zip'
    New-TestArchive $goodZip @("${prefix}a.dll", "${prefix}new.dll")
    $sha256 = [Security.Cryptography.SHA256]::Create()
    try {
        $expectedHash = [BitConverter]::ToString($sha256.ComputeHash([IO.File]::ReadAllBytes($goodZip))).Replace('-', '')
    } finally { $sha256.Dispose() }
    $archiveRelease = [pscustomobject]@{
        Size = (Get-Item -LiteralPath $goodZip).Length
        Hash = $expectedHash
    }
    Test-UpdateArchive $archiveRelease $goodZip
    $archiveRelease.Hash = '0' * 64
    Assert-Throws { Test-UpdateArchive $archiveRelease $goodZip } 'SHA-256 verification'
    $archiveRelease.Size += 1
    Assert-Throws { Test-UpdateArchive $archiveRelease $goodZip } 'incomplete'
    $stage = Join-Path $testRoot 'stage'
    Expand-UpdateArchive $goodZip $stage '9.8.7' 'x64'
    Assert ([IO.File]::ReadAllText((Join-Path $stage 'a.dll')) -eq 'new') 'Package was not extracted.'
    Assert-Throws { Test-UpdatePayload $stage '9.8.7' 'x64' } 'missing XiaomiViewer.exe'
    $payload = Join-Path $testRoot 'payload'
    [IO.Directory]::CreateDirectory($payload) | Out-Null
    $systemExecutable = [Diagnostics.Process]::GetCurrentProcess().MainModule.FileName
    Copy-Item -LiteralPath $systemExecutable -Destination (Join-Path $payload 'XiaomiViewer.exe')
    Copy-Item -LiteralPath $systemExecutable -Destination (Join-Path $payload 'XiaomiViewerUpdater.exe')
    foreach ($name in @('xmbridge.dll', 'ProggyVector-Regular.ttf', 'update-program.ps1')) {
        [IO.File]::WriteAllText((Join-Path $payload $name), '')
    }
    $systemVersion = [Diagnostics.FileVersionInfo]::GetVersionInfo($systemExecutable).ProductVersion
    $systemArchitecture = [Runtime.InteropServices.RuntimeInformation]::ProcessArchitecture.ToString().ToLowerInvariant()
    $otherArchitecture = if ($systemArchitecture -eq 'arm64') { 'x64' } else { 'arm64' }
    Test-UpdatePayload $payload $systemVersion $systemArchitecture
    Assert-Throws { Test-UpdatePayload $payload '9.8.7' $systemArchitecture } 'version does not match'
    Assert-Throws { Test-UpdatePayload $payload $systemVersion $otherArchitecture } 'wrong architecture'
    $badNames = @("${prefix}../outside.txt", "${prefix}a.dll:stream", "${prefix}NUL.txt", "${prefix}nested/file.dll")
    for ($i = 0; $i -lt $badNames.Count; ++$i) {
        $zip = Join-Path $testRoot "bad-$i.zip"
        New-TestArchive $zip @($badNames[$i])
        Assert-Throws { Expand-UpdateArchive $zip (Join-Path $testRoot "bad-$i") '9.8.7' 'x64' } 'Unsafe filename'
    }
    $duplicateZip = Join-Path $testRoot 'duplicate.zip'
    New-TestArchive $duplicateZip @("${prefix}a.dll", "${prefix}A.dll")
    Assert-Throws { Expand-UpdateArchive $duplicateZip (Join-Path $testRoot 'duplicate') '9.8.7' 'x64' } 'Unsafe filename'
    Assert-Throws { Expand-UpdateArchive $goodZip (Join-Path $testRoot 'wrong-arch') '9.8.7' 'arm64' } 'Unexpected directory'
    Assert (-not [IO.File]::Exists((Join-Path $testRoot 'outside.txt'))) 'Archive escaped the staging directory.'

    # Exercise the real helper's cross-process consent, progress and exit handshake
    # with a synthetic release and transfer. No network or running viewer is used.
    $wrapperText = @'
param([string]$TestTarget, [int]$TestParent, [string]$TestVersion)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'update-program.ps1')
$ApplicationDirectory = $TestTarget
$WorkDirectory = $PSScriptRoot
$CurrentVersion = $TestVersion
$Architecture = 'x64'
$ParentProcessId = $TestParent
function Get-UpdateRelease {
    return [pscustomobject]@{ Version = '9.8.7'; Changelog = "Release notes`n- A real change"; Size = 100 }
}
function Save-UpdateArchive($Release, [string]$Path) {
    [IO.File]::WriteAllText((Join-Path $WorkDirectory 'download.started'), 'yes')
    $script:UpdateStatus.downloaded = 50L
    Write-UpdateStatus 'downloading' 'Fixture download'
    Wait-UpdateSignal 'finish-download.request'
    $script:UpdateStatus.downloaded = 100L
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::Open($Path, 'Create')
    try {
        foreach ($name in @('a.dll', 'new.dll')) {
            $entry = $zip.CreateEntry("XiaomiViewer-9.8.7-win-x64/$name")
            $writer = [IO.StreamWriter]::new($entry.Open())
            $writer.Write('updated')
            $writer.Dispose()
        }
    } finally { $zip.Dispose() }
}
function Test-UpdatePayload {}
Invoke-ProgramUpdate
'@
    $powershell = Join-Path ([Environment]::GetFolderPath('System')) 'WindowsPowerShell\v1.0\powershell.exe'
    foreach ($scenario in @('up-to-date', 'decline', 'cancel-download', 'cancel-ready', 'prepared')) {
        $run = Join-Path $testRoot ("XiaomiViewer-update-{" + [guid]::NewGuid().ToString() + '}')
        [IO.Directory]::CreateDirectory($run) | Out-Null
        Copy-Item -LiteralPath $HelperScript -Destination (Join-Path $run 'update-program.ps1')
        $wrapper = Join-Path $run 'fixture.ps1'
        [IO.File]::WriteAllText($wrapper, $wrapperText)
        $app = Join-Path $testRoot $scenario
        [IO.Directory]::CreateDirectory($app) | Out-Null
        [IO.File]::WriteAllText((Join-Path $app 'a.dll'), 'original')
        [IO.File]::WriteAllText((Join-Path $app 'unrelated.txt'), 'keep')
        $parentId = $PID
        $testVersion = if ($scenario -eq 'up-to-date') { '9.8.7' } else { '0.7.1' }
        $child = Start-Process $powershell -ArgumentList "-NoProfile -NonInteractive -ExecutionPolicy Bypass -File `"$wrapper`" -TestTarget `"$app`" -TestParent $parentId -TestVersion $testVersion" `
            -RedirectStandardError "$run-error.log" -PassThru -WindowStyle Hidden
        $null = $child.Handle
        $processes.Add($child)
        if ($scenario -eq 'up-to-date') {
            $null = Wait-Status $run 'up_to_date' $child
            Assert ($child.WaitForExit(20000) -and $child.ExitCode -eq 0) 'Up-to-date check did not finish.'
            Assert (-not [IO.File]::Exists((Join-Path $run 'download.started'))) 'An up-to-date check downloaded a package.'
            continue
        }
        $status = Wait-Status $run 'available' $child
        Assert ($status.changelog.Contains('A real change')) 'Confirmation lost the changelog.'
        Start-Sleep -Milliseconds 300
        Assert (-not [IO.File]::Exists((Join-Path $run 'download.started'))) 'Download started before confirmation.'
        if ($scenario -ne 'decline') {
            [IO.File]::WriteAllText((Join-Path $run 'confirm.request'), '')
            $status = Wait-Status $run 'downloading' $child
            Assert ($status.downloaded -eq 50 -and $status.total -eq 100) 'Download progress was not reported.'
        }
        if ($scenario -eq 'prepared' -or $scenario -eq 'cancel-ready') {
            [IO.File]::WriteAllText((Join-Path $run 'finish-download.request'), '')
            $null = Wait-Status $run 'ready' $child
        }
        if ($scenario -eq 'prepared') {
            [IO.File]::WriteAllText((Join-Path $run 'apply.request'), '')
            Assert ($child.WaitForExit(20000) -and $child.ExitCode -eq 0) 'Update helper did not hand off to the updater.'
            Assert ([IO.File]::ReadAllText((Join-Path $app 'a.dll')) -eq 'original') 'PowerShell helper modified application files.'
            Assert ([IO.Directory]::Exists((Join-Path $run 'stage'))) 'Prepared package was removed before the standalone updater could consume it.'
        } else {
            [IO.File]::WriteAllText((Join-Path $run 'cancel.request'), '')
            Assert ($child.WaitForExit(20000) -and $child.ExitCode -eq 0) 'Cancellation did not complete.'
            Assert (-not [IO.Directory]::Exists($run)) 'Cancellation did not clean its temporary files.'
            Assert ([IO.File]::ReadAllText((Join-Path $app 'a.dll')) -eq 'original') 'Cancelled update modified application files.'
        }
    }
    Write-Host 'Release validation, safe extraction, consent, progress, cancellation and standalone-updater handoff passed.'
} finally {
    foreach ($process in $processes) {
        if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force }
        $process.Dispose()
    }
    if ($script:UpdateParent) { $script:UpdateParent.Dispose() }
    Remove-Item -LiteralPath $testRoot -Recurse -Force
}
