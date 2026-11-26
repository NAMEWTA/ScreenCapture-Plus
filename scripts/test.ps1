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
    $extraObjects = @()
    $extraCompilerArguments = @()
    if ($test.BaseName -in @('media_frame_tests', 'recording_frame_tests', 'frame_queue_tests', 'frame_normalizer_tests')) {
        $frameObject = Join-Path $testDirectory 'MediaFrame.obj'
        & cl.exe /nologo /std:c++20 /EHsc /utf-8 /W4 /MT /O2 /c "/I$(Join-Path $RepoRoot 'src')" `
            (Join-Path $RepoRoot 'src/core/MediaFrame.cpp') "/Fo$frameObject"
        Assert-NativeSuccess 'Compiling shared media frame implementation'
        $extraObjects = @($frameObject)
    }
    if ($test.BaseName -eq 'recording_frame_tests') {
        $sourceObject = Join-Path $testDirectory 'FrameSource.obj'
        & cl.exe /nologo /std:c++20 /EHsc /utf-8 /W4 /MT /O2 /c "/I$(Join-Path $RepoRoot 'src')" `
            (Join-Path $RepoRoot 'src/recording/FrameSource.cpp') "/Fo$sourceObject"
        Assert-NativeSuccess 'Compiling shared recording frame source implementation'
        $extraObjects += $sourceObject
    }
    if ($test.BaseName -in @('frame_normalizer_tests', 'recording_frame_tests')) {
        $normalizerObject = Join-Path $testDirectory 'FrameNormalizer.obj'
        & cl.exe /nologo /std:c++20 /EHsc /utf-8 /W4 /MT /O2 /c "/I$(Join-Path $RepoRoot 'src')" `
            (Join-Path $RepoRoot 'src/services/media/FrameNormalizer.cpp') "/Fo$normalizerObject"
        Assert-NativeSuccess 'Compiling the shared bounded frame normalizer'
        $extraObjects += $normalizerObject
    }
    if ($test.BaseName -eq 'frame_queue_tests') {
        $queueObject = Join-Path $testDirectory 'FrameQueue.obj'
        & cl.exe /nologo /std:c++20 /EHsc /utf-8 /W4 /MT /O2 /c "/I$(Join-Path $RepoRoot 'src')" `
            (Join-Path $RepoRoot 'src/core/FrameQueue.cpp') "/Fo$queueObject"
        Assert-NativeSuccess 'Compiling shared bounded frame queue'
        $extraObjects += $queueObject
    }
    if ($test.BaseName -in @('scroll_capture_model_tests', 'scroll_accumulator_tests')) {
        $scrollObject = Join-Path $testDirectory 'ScrollCaptureModel.obj'
        & cl.exe /nologo /std:c++20 /EHsc /utf-8 /W4 /MT /O2 /c "/I$(Join-Path $RepoRoot 'src')" `
            (Join-Path $RepoRoot 'src/capture/ScrollCaptureModel.cpp') "/Fo$scrollObject"
        Assert-NativeSuccess 'Compiling shared scrolling capture model'
        $extraObjects += $scrollObject
    }
    if ($test.BaseName -eq 'qr_recognition_tests') {
        # Retain the existing native product's real decoder configuration while
        # the same core is also built by CMake. Each language uses its actual
        # compiler mode; the two platform text files need distinct object names.
        $extraCompilerArguments = @('/DQUIRC_MAX_REGIONS=65534', "/I$(Join-Path $RepoRoot 'third_party')", "/I$(Join-Path $RepoRoot 'tests')")
        foreach ($name in @('quirc', 'identify', 'decode', 'version_db')) {
            $decoderObject = Join-Path $testDirectory "qr_$name.obj"
            & cl.exe /nologo /std:c11 /TC /utf-8 /W4 /MT /O2 /c @extraCompilerArguments `
                (Join-Path $RepoRoot "third_party/quirc/$name.c") "/Fo$decoderObject"
            Assert-NativeSuccess "Compiling retained QR decoder $name"
            $extraObjects += $decoderObject
        }
        $qrSources = [ordered]@{
            qr_core = 'src/core/QrRecognition.cpp'
            qr_text = 'src/platform/QrText.cpp'
            qr_text_windows = 'src/platform/windows/QrText.cpp'
        }
        foreach ($source in $qrSources.GetEnumerator()) {
            $qrObject = Join-Path $testDirectory "$($source.Key).obj"
            & cl.exe /nologo /std:c++20 /EHsc /utf-8 /W4 /MT /O2 /c "/I$(Join-Path $RepoRoot 'src')" @extraCompilerArguments `
                (Join-Path $RepoRoot $source.Value) "/Fo$qrObject"
            Assert-NativeSuccess "Compiling shared QR adapter $($source.Key)"
            $extraObjects += $qrObject
        }
    }
    & cl.exe /nologo /std:c++20 /EHsc /utf-8 /W4 /MT /O2 "/I$(Join-Path $RepoRoot 'src')" `
        @extraCompilerArguments $test.FullName @extraObjects "/Fo$object" "/Fe$executable"
    Assert-NativeSuccess "Compiling $($test.Name)"
    & $executable
    Assert-NativeSuccess "Running $($test.Name)"
}
