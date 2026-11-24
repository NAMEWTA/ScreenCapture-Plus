#Requires -Version 7.0
[CmdletBinding()]
param()
. (Join-Path $PSScriptRoot 'BuildCommon.ps1')
if (-not $IsWindows) { throw 'The startup smoke check requires Windows.' }
$executable = Join-Path $ArtifactsRoot 'bin/x64/Release/huhucapture.exe'
if (-not (Test-Path $executable)) { throw 'Build the Release executable before running the startup smoke check.' }
$smokeDirectory = Join-Path $ArtifactsRoot ('smoke-' + [Guid]::NewGuid().ToString('N'))
$process = $null
try {
    New-Item $smokeDirectory -ItemType Directory -Force | Out-Null
    $smokeExecutable = Join-Path $smokeDirectory 'huhucapture.exe'
    Copy-Item $executable $smokeExecutable
    # Exercise portable config and embedded languages without an installed Lang directory.
    New-Item (Join-Path $smokeDirectory 'config.json') -ItemType File | Out-Null
    $process = Start-Process -FilePath $smokeExecutable -ArgumentList '--enter=tray' `
        -WorkingDirectory $smokeDirectory -PassThru
    if ($process.WaitForExit(3000)) {
        throw "Tray startup exited prematurely with code $($process.ExitCode)."
    }
    Write-Host 'Single-executable tray startup remained alive for 3 seconds.'
} finally {
    if ($process) {
        if (-not $process.HasExited) {
            $process.Kill()
            if (-not $process.WaitForExit(5000)) { throw 'The smoke-check process did not stop.' }
        }
        $process.Dispose()
    }
    if (Test-Path $smokeDirectory) { Remove-Item $smokeDirectory -Recurse -Force }
}
