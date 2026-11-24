#Requires -Version 7.0
[CmdletBinding()]
param()
. (Join-Path $PSScriptRoot 'BuildCommon.ps1')
Initialize-WindowsToolchain
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
