<#
.SYNOPSIS
    Prints the project version.

.DESCRIPTION
    The project() call in CMakeLists.txt is the single place the version is
    written by hand. Everything else -- the packaging scripts, the Go bridge
    build, the release workflow -- reads it from here, so a release cannot end up
    with a zip, an executable and a DLL that disagree about which version they
    are. SignPath rejects exactly that: an Open Source signing policy requires
    the product version to be identical across every signed binary in a build.
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'

$projectFile = Join-Path (Split-Path -Parent $PSScriptRoot) 'CMakeLists.txt'
if ((Get-Content $projectFile -Raw) -notmatch '(?m)^\s*VERSION\s+(\d+\.\d+\.\d+)\s*$') {
    throw "No project version found in $projectFile"
}

$Matches[1]
