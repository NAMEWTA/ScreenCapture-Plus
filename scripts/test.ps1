#Requires -Version 7.0
[CmdletBinding()]
param()
. (Join-Path $PSScriptRoot 'BuildCommon.ps1')
Initialize-WindowsToolchain
# Validate the guarded publication entry point during PR checks as well. Parsing
# never executes release operations, which remain restricted to the main job.
foreach ($script in Get-ChildItem $PSScriptRoot -Filter '*.ps1' -File) {
    $tokens = $null
    $parseErrors = $null
    [Management.Automation.Language.Parser]::ParseFile($script.FullName, [ref]$tokens, [ref]$parseErrors) | Out-Null
    if ($parseErrors.Count -ne 0) { throw "Invalid PowerShell syntax in $($script.Name): $($parseErrors -join '; ')" }
}
Write-Host 'PowerShell build, test, and release scripts parsed successfully.'
$testDirectory = Join-Path $ArtifactsRoot 'tests'
New-Item $testDirectory -ItemType Directory -Force | Out-Null
$tests = @(Get-ChildItem (Join-Path $RepoRoot 'tests') -Filter '*.cpp' -File | Sort-Object Name)
if ($tests.Count -eq 0) { throw 'No C++ regression tests were found.' }
foreach ($test in $tests) {
    $executable = Join-Path $testDirectory "$($test.BaseName).exe"
    $object = Join-Path $testDirectory "$($test.BaseName).obj"
    & cl.exe /nologo /std:c++20 /EHsc /utf-8 /W4 /MT /O2 "/I$(Join-Path $RepoRoot 'src')" `
        $test.FullName "/Fo$object" "/Fe$executable"
    Assert-NativeSuccess "Compiling $($test.Name)"
    & $executable
    Assert-NativeSuccess "Running $($test.Name)"
}
