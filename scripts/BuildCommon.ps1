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

function Get-ProductVersion {
    $source = Join-Path $RepoRoot 'build/version.json'
    $template = Join-Path $RepoRoot 'build/windows/Version.props.in'
    if ((Get-Item $source).Length -gt 4096) { throw 'build/version.json is oversized.' }
    $document = Get-Content $source -Raw -Encoding utf8 | ConvertFrom-Json -AsHashtable
    if ($document -isnot [System.Collections.IDictionary] -or $document.Count -ne 2 -or
        -not $document.Contains('schemaVersion') -or -not $document.Contains('version') -or
        $document['schemaVersion'] -isnot [long] -or $document['schemaVersion'] -ne 1 -or
        $document['version'] -isnot [string]) {
        throw 'build/version.json requires schemaVersion 1 and one version string.'
    }
    $value = $document['version']
    if ($value -cnotmatch '^(0|[1-9][0-9]{0,4})\.(0|[1-9][0-9]{0,4})\.(0|[1-9][0-9]{0,4})$') {
        throw 'build/version.json must contain one canonical MAJOR.MINOR.PATCH value.'
    }
    $parts = @([int]$Matches[1], [int]$Matches[2], [int]$Matches[3])
    if ($parts.Where({ $_ -gt 65535 }).Count) { throw 'build/version.json components must be between 0 and 65535.' }
    $substitutions = @{
        HUHU_PRODUCT_VERSION = $value
        HUHU_VERSION_MAJOR = [string]$parts[0]
        HUHU_VERSION_MINOR = [string]$parts[1]
        HUHU_VERSION_PATCH = [string]$parts[2]
        HUHU_VERSION_SOURCE_SHA256 = (Get-FileHash $source -Algorithm SHA256).Hash.ToLowerInvariant()
        HUHU_VERSION_TEMPLATE_SHA256 = (Get-FileHash $template -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    $expected = Get-Content $template -Raw -Encoding utf8
    foreach ($entry in $substitutions.GetEnumerator()) { $expected = $expected.Replace('@' + $entry.Key + '@', $entry.Value) }
    if ((Get-Content (Join-Path $RepoRoot 'VERSION') -Raw -Encoding utf8) -cne ($value + "`n") -or
        (Get-Content (Join-Path $RepoRoot 'build/windows/Version.props') -Raw -Encoding utf8) -cne $expected) {
        throw 'Generated VERSION or Version.props is stale; run python scripts/version_source.py --write.'
    }
    return $value
}

function Write-ProductVersionProps {
    $value = Get-ProductVersion
    $parts = $value.Split('.')
    $source = Join-Path $RepoRoot 'build/version.json'
    $sha256 = (Get-FileHash $source -Algorithm SHA256).Hash
    $template = Get-Content (Join-Path $RepoRoot 'build/windows/Version.props.in') -Raw -Encoding utf8
    $content = $template.Replace('@PRODUCT_VERSION@', $value).Replace('@SOURCE_SHA256@', $sha256)
    $content = $content.Replace('@VERSION_MAJOR@', $parts[0]).Replace('@VERSION_MINOR@', $parts[1]).Replace('@VERSION_PATCH@', $parts[2])
    $directory = Join-Path $ArtifactsRoot 'generated'
    New-Item $directory -ItemType Directory -Force | Out-Null
    $path = Join-Path $directory 'Version.props'
    if (-not (Test-Path $path) -or [IO.File]::ReadAllText($path) -cne $content) {
        [IO.File]::WriteAllText($path, $content, [Text.UTF8Encoding]::new($false))
    }
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
