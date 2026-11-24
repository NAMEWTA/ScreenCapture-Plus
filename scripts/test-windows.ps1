#Requires -Version 7.0
[CmdletBinding()]
param([switch] $Gui)
. (Join-Path $PSScriptRoot 'BuildCommon.ps1')
Initialize-WindowsToolchain
$testDirectory = Join-Path $ArtifactsRoot 'tests/windows'
if ($Gui) {
    $guiDirectory = Join-Path $testDirectory 'gui'
    $screens = Join-Path $guiDirectory 'screens'
    New-Item $screens -ItemType Directory -Force | Out-Null
    $project = Join-Path $RepoRoot 'build/windows/GuiIntegrationTests.vcxproj'
    & MSBuild.exe $project /m /nologo /verbosity:minimal /p:Configuration=Release /p:Platform=x64 `
        "/bl:$(Join-Path $ArtifactsRoot 'gui-integration-tests.binlog')"
    Assert-NativeSuccess 'Linking the production application into the native GUI integration harness'
    [IO.File]::WriteAllText((Join-Path $guiDirectory 'config.json'), '')
    $start = [Diagnostics.ProcessStartInfo]::new((Join-Path $guiDirectory 'GuiIntegrationTests.exe'))
    $start.WorkingDirectory = $guiDirectory
    $start.UseShellExecute = $false
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.ArgumentList.Add($screens)
    $start.ArgumentList.Add('--enter=tray')
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $start
    $timedOut = $false
    $started = $false
    try {
        $started = $process.Start()
        if (-not $started) { throw 'The native GUI harness did not start.' }
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit(45000)) {
            $timedOut = $true
            $process.Kill()
            if (-not $process.WaitForExit(5000)) { throw 'The GUI harness could not be stopped after its hard timeout.' }
        }
        $report = $stdout.GetAwaiter().GetResult() + $stderr.GetAwaiter().GetResult()
        $report | Set-Content (Join-Path $guiDirectory 'test-report.txt') -Encoding utf8NoBOM
        Write-Host $report
        $exitCode = $process.ExitCode
    } finally {
        if ($started -and -not $process.HasExited) { $process.Kill() }
        $process.Dispose()
    }
    if ($timedOut) { throw 'The real GUI checks exceeded the 45-second hard timeout; inspect the retained GUI report.' }
    if ($exitCode -eq 77) { throw 'GUI checks are unavailable on this Runner: see the explicit graphics/desktop capability diagnostic. They did not pass.' }
    if ($exitCode -ne 0) { throw "Native GUI integration failed with exit code $exitCode." }
    return
}
$renderDirectory = Join-Path $testDirectory 'renders'
New-Item $renderDirectory -ItemType Directory -Force | Out-Null
$project = Join-Path $RepoRoot 'build/windows/AnnotationRenderTests.vcxproj'
& MSBuild.exe $project /m /nologo /verbosity:minimal /p:Configuration=Release /p:Platform=x64 `
    "/bl:$(Join-Path $ArtifactsRoot 'annotation-render-tests.binlog')"
Assert-NativeSuccess 'Building the native annotation renderer integration tests'
$executable = Join-Path $testDirectory 'AnnotationRenderTests.exe'
& $executable $renderDirectory 2>&1 | Tee-Object (Join-Path $testDirectory 'test-report.txt')
Assert-NativeSuccess 'Running Direct2D/DirectWrite annotation export checks on WARP'
