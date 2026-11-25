#Requires -Version 7.0
[CmdletBinding()]
param()
. (Join-Path $PSScriptRoot 'BuildCommon.ps1')
Initialize-WindowsToolchain
$testDirectory = Join-Path $ArtifactsRoot 'tests/windows/update-bridge'
New-Item $testDirectory -ItemType Directory -Force | Out-Null
$project = Join-Path $RepoRoot 'build/windows/WindowsUpdateDownloadTests.vcxproj'
& MSBuild.exe $project /m /nologo /verbosity:minimal /p:Configuration=Release /p:Platform=x64 `
    "/bl:$(Join-Path $ArtifactsRoot 'windows-update-bridge-tests.binlog')"
Assert-NativeSuccess 'Building the real WinRT update bridge without Qt or Ling'
& python (Join-Path $PSScriptRoot 'test-windows-update-bridge.py') `
    --client (Join-Path $testDirectory 'WindowsUpdateDownloadTests.exe') `
    --evidence (Join-Path $testDirectory 'evidence')
Assert-NativeSuccess 'Validating real HTTP streaming, package provenance and joined cancellation'
