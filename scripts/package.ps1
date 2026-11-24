#Requires -Version 7.0
[CmdletBinding()]
param([string] $Version = '')
. (Join-Path $PSScriptRoot 'BuildCommon.ps1')
Initialize-WindowsToolchain
$binaryDirectory = Join-Path $ArtifactsRoot 'bin/x64/Release'
$executable = Join-Path $binaryDirectory 'huhucapture.exe'
if (-not (Test-Path $executable)) { throw 'Build the Release configuration before packaging.' }
$versionInfo = [Diagnostics.FileVersionInfo]::GetVersionInfo($executable)
$fileVersion = $versionInfo.FileVersion
$expectedIdentity = @{
    ProductName = 'huhucapture'
    FileDescription = 'huhucapture'
    OriginalFilename = 'huhucapture.exe'
    CompanyName = 'NAMEWTA'
}
foreach ($property in $expectedIdentity.Keys) {
    if ($versionInfo.$property -cne $expectedIdentity[$property]) {
        throw "Executable identity $property '$($versionInfo.$property)' does not match '$($expectedIdentity[$property])'."
    }
}
if ($Version) {
    $expected = (Get-ReleaseVersion $Version) -join '.'
    if ($fileVersion -ne $expected) { throw "Executable version $fileVersion does not match release $Version ($expected)." }
} else {
    $Version = 'v' + (($fileVersion -split '\.')[0..2] -join '.')
}

# A single executable must not acquire a dependency on the Visual C++ redistributable
# or on separate copies of our three static libraries.
$imports = & dumpbin.exe /nologo /dependents $executable
Assert-NativeSuccess 'Inspecting executable dependencies'
if (($imports -join "`n") -match '(?im)\b(?:vcruntime\d\w*|msvcp\d\w*|gifski|ling|yoga)\.dll\b') {
    throw 'The executable unexpectedly requires an external C++ runtime or application DLL.'
}

$packageRoot = Join-Path $ArtifactsRoot 'package'
$stage = Join-Path $ArtifactsRoot 'package-stage'
# These two directories contain generated build products only.
foreach ($directory in @($packageRoot, $stage)) {
    if (Test-Path $directory) { Remove-Item $directory -Recurse -Force }
    New-Item $directory -ItemType Directory -Force | Out-Null
}
$portable = Join-Path $stage 'portable'
$licenses = Join-Path $portable 'licenses'
New-Item $licenses -ItemType Directory -Force | Out-Null
Copy-Item $executable (Join-Path $packageRoot 'huhucapture.exe')
Copy-Item $executable (Join-Path $portable 'huhucapture.exe')
Copy-Item (Join-Path $RepoRoot 'Lang') $portable -Recurse
Copy-Item (Join-Path $RepoRoot 'LICENSE') (Join-Path $licenses 'huhucapture-LICENSE.txt')
Copy-Item (Join-Path $DependencyRoot 'ling/LICENSE') (Join-Path $licenses 'Ling-LICENSE.txt')
Copy-Item (Join-Path $DependencyRoot 'gifski/LICENSE') (Join-Path $licenses 'gifski-LICENSE.txt')
Copy-Item (Join-Path $RepoRoot 'third_party/licenses/Yoga-LICENSE.txt') $licenses
foreach ($dependency in @('quirc', 'cgif')) {
    Copy-Item (Join-Path $RepoRoot "third_party/$dependency/LICENSE") (Join-Path $licenses "$dependency-LICENSE.txt")
}
Copy-Item (Join-Path $RepoRoot 'third_party/NOTICE.md') $portable

$commit = & git -C $RepoRoot rev-parse HEAD
Assert-NativeSuccess 'Reading the source revision'
$metadata = [ordered]@{
    repository = 'NAMEWTA/huhucapture'
    commit = $commit.Trim()
    tag = $Version
    executableVersion = $fileVersion
    architecture = 'x64'
    configuration = 'Release'
    toolset = 'MSVC v145'
    runtime = 'static CRT (/MT)'
    dependencies = $DependencyLock
    executableSha256 = (Get-FileHash $executable -Algorithm SHA256).Hash.ToLowerInvariant()
}
$metadata | ConvertTo-Json -Depth 10 | Set-Content (Join-Path $packageRoot 'build-info.json') -Encoding utf8NoBOM
Copy-Item (Join-Path $packageRoot 'build-info.json') $portable
Compress-Archive -Path (Join-Path $portable '*') -DestinationPath (Join-Path $packageRoot "huhucapture-$Version-windows-x64.zip")

$symbols = @(Get-ChildItem $binaryDirectory -Filter '*.pdb' -File)
if ($symbols.Count -eq 0) { throw 'The Release symbols are missing.' }
Compress-Archive -Path $symbols.FullName -DestinationPath (Join-Path $packageRoot "huhucapture-$Version-windows-x64-symbols.zip")
$checksums = Get-ChildItem $packageRoot -File | Sort-Object Name | ForEach-Object {
    '{0}  {1}' -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant(), $_.Name
}
$checksums | Set-Content (Join-Path $packageRoot 'SHA256SUMS.txt') -Encoding utf8NoBOM
Write-Host "Packaged $packageRoot"
