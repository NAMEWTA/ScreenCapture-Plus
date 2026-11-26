#Requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string] $SourceCommit,
    [Parameter(Mandatory)] [string] $WindowsRun,
    [Parameter(Mandatory)] [string] $PackageDirectory,
    [Parameter(Mandatory)] [string] $EvidenceDirectory
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
# P11a's sole transition exception is explicit and independent of the Qt
# corresponding-source gate. This does not restore either automatic publisher.
& python (Join-Path $PSScriptRoot 'publish-bridge-release.py') publish `
    --source-commit $SourceCommit --windows-run $WindowsRun `
    --package $PackageDirectory --evidence $EvidenceDirectory
if ($LASTEXITCODE -ne 0) { throw "The bridge-only publisher refused publication (exit $LASTEXITCODE)." }
