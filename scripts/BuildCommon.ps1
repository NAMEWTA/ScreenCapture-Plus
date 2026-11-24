# Shared by the local build and GitHub Actions. Requires PowerShell 7.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$RepoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$ArtifactsRoot = Join-Path $RepoRoot 'artifacts'
$DependencyRoot = Join-Path $RepoRoot '.deps'
$DependencyLock = Get-Content (Join-Path $RepoRoot 'build/dependencies.lock.json') -Raw | ConvertFrom-Json

function Assert-NativeSuccess([string] $Operation) {
    if ($LASTEXITCODE -ne 0) {
        throw "$Operation failed with exit code $LASTEXITCODE."
    }
}

function Initialize-WindowsToolchain {
    if (-not $IsWindows) { throw 'The desktop application requires Windows and Visual Studio 2026.' }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    if (-not (Test-Path $vswhere)) { throw 'Install Visual Studio 2026 with Desktop development with C++ and ATL.' }
    $installation = & $vswhere -latest -products '*' -version '[18.0,19.0)' `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 Microsoft.VisualStudio.Component.VC.ATL `
        -property installationPath
    Assert-NativeSuccess 'Finding Visual Studio 2026'
    if (-not $installation) { throw 'Visual Studio 2026 with MSVC v145 and ATL was not found.' }
    & (Join-Path $installation 'Common7/Tools/Launch-VsDevShell.ps1') -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
    if (-not (Get-Command MSBuild.exe -ErrorAction SilentlyContinue)) { throw 'MSBuild is unavailable after initializing Visual Studio.' }
}

function Get-ReleaseVersion([string] $Value) {
    # Tags are data, never executable shell text. Keep all resource components within WORD.
    if ($Value -notmatch '^v?(\d+)\.(\d+)\.(\d+)(?:-[0-9A-Za-z][0-9A-Za-z.-]*)?$') {
        throw "Invalid version '$Value'. Use vMAJOR.MINOR.PATCH or vMAJOR.MINOR.PATCH-prerelease."
    }
    $parts = @([int]$Matches[1], [int]$Matches[2], [int]$Matches[3], 0)
    if ($parts.Where({ $_ -gt 65535 }).Count) { throw 'Windows version components must be between 0 and 65535.' }
    return $parts
}
