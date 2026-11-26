#Requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string] $Executable,
    [Parameter(Mandatory)] [string] $Version
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if (-not $IsWindows) { throw 'The final bridge PE resource gate requires actual Windows.' }
if ($Version -cnotmatch '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$') {
    throw 'Expected the canonical single product version.'
}
$file = Get-Item -LiteralPath $Executable
if ($file.PSIsContainer -or $file.LinkType -or $file.Length -le 0 -or $file.Length -gt 64MB) {
    throw 'The bridge must remain an unlinked standalone executable within the old updater budget.'
}
$versionInfo = [Diagnostics.FileVersionInfo]::GetVersionInfo($file.FullName)
if ($versionInfo.FileVersion -cne "$Version.0" -or $versionInfo.ProductName -cne 'huhucapture' -or
    $versionInfo.FileDescription -cne 'huhucapture' -or $versionInfo.OriginalFilename -cne 'huhucapture.exe' -or
    $versionInfo.CompanyName -cne 'NAMEWTA') {
    throw 'Native bridge PE version/resource identity does not match the reviewed source.'
}
$stream = [IO.File]::OpenRead($file.FullName)
$reader = [IO.BinaryReader]::new($stream)
try {
    if ($stream.Length -lt 64 -or $reader.ReadUInt16() -ne 0x5a4d) { throw 'Missing actual DOS header.' }
    $stream.Position = 60
    $offset = $reader.ReadUInt32()
    if ($offset -lt 64 -or $offset -gt 1MB -or $offset + 26 -gt $stream.Length) { throw 'Invalid PE header extent.' }
    $stream.Position = $offset
    if ($reader.ReadUInt32() -ne 0x4550 -or $reader.ReadUInt16() -ne 0x8664) { throw 'Bridge is not an AMD64 PE image.' }
    $stream.Position = $offset + 22
    $characteristics = $reader.ReadUInt16()
    if (($characteristics -band 2) -eq 0 -or ($characteristics -band 0x2000) -ne 0 -or $reader.ReadUInt16() -ne 0x20b) {
        throw 'Bridge must be an executable PE32+ image, not a DLL or disguised package.'
    }
} finally {
    $reader.Dispose()
    $stream.Dispose()
}
Write-Host 'Verified actual native Windows bridge version, brand and executable PE characteristics.'
