#Requires -Version 7.0
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')] [string] $Configuration = 'Release',
    [switch] $Gui,
    [string] $RealPackageDirectory,
    [switch] $RequireRealPackage
)
. (Join-Path $PSScriptRoot 'BuildCommon.ps1')
Initialize-WindowsToolchain
$project = Join-Path $RepoRoot 'build/windows/LegacyUpdateProbe.vcxproj'
& MSBuild.exe $project /m /nologo /verbosity:minimal "/p:Configuration=$Configuration" /p:Platform=x64 `
    "/bl:$(Join-Path $ArtifactsRoot "legacy-update-$Configuration.binlog")"
Assert-NativeSuccess 'Building the production WinRT bridge transport probe'
$nativeExecutable = Join-Path $ArtifactsRoot 'bin/x64/Release/huhucapture.exe'
if (-not (Test-Path $nativeExecutable -PathType Leaf)) { throw 'Build the actual Release Windows product before bridge validation.' }
$output = Join-Path $ArtifactsRoot "tests/windows/legacy-update/$Configuration/run-$([guid]::NewGuid().ToString('N'))"
$probe = Join-Path $ArtifactsRoot "tests/windows/legacy-update/$Configuration/LegacyUpdateProbe.exe"
$arguments = @((Join-Path $PSScriptRoot 'test-legacy-update.py'), '--probe', $probe,
    '--legacy-executable', $nativeExecutable, '--output', $output, '--configuration', $Configuration)
if ($RealPackageDirectory) { $arguments += @('--real-package-directory', $RealPackageDirectory) }
if ($RequireRealPackage) { $arguments += '--require-real-package' }
if ($Gui) {
    $guiProject = Join-Path $RepoRoot 'build/windows/GuiIntegrationTests.vcxproj'
    & MSBuild.exe $guiProject /m /nologo /verbosity:minimal /p:Configuration=Release /p:Platform=x64 `
        "/bl:$(Join-Path $ArtifactsRoot 'legacy-update-gui.binlog')"
    Assert-NativeSuccess 'Linking real About controls and application-owned bridge service'
    $guiDirectory = Join-Path $ArtifactsRoot 'tests/windows/gui'
    [IO.File]::WriteAllText((Join-Path $guiDirectory 'config.json'), '')
    # Exercise actual six-language files, including the four external languages
    # used by the retained Windows application, rather than English fallback.
    $languageDirectory = Join-Path $guiDirectory 'Lang'
    New-Item $languageDirectory -ItemType Directory -Force | Out-Null
    Get-ChildItem (Join-Path $RepoRoot 'Lang') -Filter '*.json' -File | Copy-Item -Destination $languageDirectory -Force
    $arguments += @('--gui-probe', (Join-Path $guiDirectory 'GuiIntegrationTests.exe'))
}
& python @arguments
Assert-NativeSuccess 'Running actual HTTP/file WinRT bridge checks (scope recorded in result.json)'
