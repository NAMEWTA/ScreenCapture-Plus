#Requires -Version 7.0
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')][string] $Configuration = 'Release',
    [string] $Version = '',
    [switch] $SkipRestore
)
. (Join-Path $PSScriptRoot 'BuildCommon.ps1')
$productVersion = Get-ProductVersion
if ($Version -and ((Get-ReleaseVersion $Version) -join '.') -cne "$productVersion.0") {
    throw "Release $Version does not match build/version.json ($productVersion)."
}
Initialize-WindowsToolchain
if (-not $SkipRestore) { & (Join-Path $PSScriptRoot 'restore.ps1') }

if (-not (Get-Command rustup -ErrorAction SilentlyContinue)) {
    throw 'Install Rust using rustup (https://rustup.rs), then run this script again.'
}
& rustup toolchain install $DependencyLock.rustToolchain --profile minimal --target $DependencyLock.rustTarget
Assert-NativeSuccess 'Installing the pinned Rust toolchain'

$dependencyArtifacts = Join-Path $ArtifactsRoot 'dependencies'
New-Item $dependencyArtifacts -ItemType Directory -Force | Out-Null
$nativeProps = Join-Path $dependencyArtifacts 'gifski-native.props'
$nativeLog = Join-Path $dependencyArtifacts 'gifski-build.log'
$previousRustFlags = $env:RUSTFLAGS
try {
    $env:RUSTFLAGS = '-C target-feature=+crt-static'
    & cargo "+$($DependencyLock.rustToolchain)" rustc --locked --release --lib --crate-type staticlib `
        --manifest-path (Join-Path $DependencyRoot 'gifski/Cargo.toml') `
        --target $DependencyLock.rustTarget --target-dir (Join-Path $ArtifactsRoot 'cargo') `
        --no-default-features --features gifsicle -- --print native-static-libs 2>&1 | Tee-Object -FilePath $nativeLog
    Assert-NativeSuccess 'Building the static GIF encoder'
} finally {
    $env:RUSTFLAGS = $previousRustFlags
}
$nativeMatch = Select-String -Path $nativeLog -Pattern 'native-static-libs:\s*(.+)$' | Select-Object -Last 1
if ($nativeMatch) {
    # rustc emits both library filenames and MSVC /defaultlib:name directives.
    # Translate the latter to filenames while preserving the reported link order.
    $libraries = foreach ($entry in ($nativeMatch.Matches[0].Groups[1].Value.Trim() -split '\s+')) {
        if ($entry -match '^/defaultlib:([A-Za-z0-9_.-]+)$') {
            $library = $Matches[1]
            if (-not $library.EndsWith('.lib', [StringComparison]::OrdinalIgnoreCase)) { $library += '.lib' }
            $library
        } elseif ($entry -match '^[A-Za-z0-9_.-]+\.lib$') {
            $entry
        } else {
            throw "Unexpected Rust native link directive: $entry"
        }
    }
    $nativeLibraries = $libraries -join ';'
    @"
<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <ItemDefinitionGroup><Link><AdditionalDependencies>$nativeLibraries;%(AdditionalDependencies)</AdditionalDependencies></Link></ItemDefinitionGroup>
</Project>
"@ | Set-Content $nativeProps -Encoding utf8NoBOM
} elseif (-not (Test-Path $nativeProps)) {
    throw 'Rust did not report the native libraries needed to link gifski.'
}

$arguments = @((Join-Path $RepoRoot 'huhucapture.slnx'), '/m', '/nologo', '/verbosity:minimal',
    "/p:Configuration=$Configuration", '/p:Platform=x64', "/bl:$ArtifactsRoot/build-$Configuration.binlog")
& MSBuild.exe @arguments
Assert-NativeSuccess 'Building huhucapture'
$executable = Join-Path $ArtifactsRoot "bin/x64/$Configuration/huhucapture.exe"
if (-not (Test-Path $executable)) { throw "Expected executable was not produced: $executable" }
Write-Host "Built $executable"
