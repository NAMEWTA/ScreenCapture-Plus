#Requires -Version 7.0
[CmdletBinding()]
param()
. (Join-Path $PSScriptRoot 'BuildCommon.ps1')

New-Item $DependencyRoot -ItemType Directory -Force | Out-Null
foreach ($name in @('ling', 'gifski')) {
    $dependency = $DependencyLock.$name
    if ($dependency.commit -notmatch '^[0-9a-f]{40}$') { throw "Invalid pinned commit for $name." }
    $destination = Join-Path $DependencyRoot $name
    if (-not (Test-Path (Join-Path $destination '.git'))) {
        if ((Test-Path $destination) -and @(Get-ChildItem $destination -Force).Count) {
            throw "$destination is not an empty Git checkout; move it aside before restoring."
        }
        & git init $destination
        Assert-NativeSuccess "Initializing $name"
        & git -C $destination remote add origin $dependency.repository
        Assert-NativeSuccess "Adding the $name remote"
    }
    $origin = & git -C $destination remote get-url origin
    Assert-NativeSuccess "Reading the $name remote"
    if ($origin -ne $dependency.repository) { throw "Unexpected remote in $destination." }
    $changes = & git -C $destination status --porcelain --untracked-files=no
    Assert-NativeSuccess "Checking $name modifications"
    if ($changes) { throw "$destination contains modified source; preserve those changes before restoring dependencies." }
    $head = & git -C $destination rev-parse --verify HEAD 2>$null
    if ($LASTEXITCODE -ne 0 -or $head -ne $dependency.commit) {
        & git -C $destination fetch --no-tags --depth=1 origin $dependency.commit
        Assert-NativeSuccess "Fetching pinned $name"
        & git -C $destination checkout --detach $dependency.commit
        Assert-NativeSuccess "Checking out pinned $name"
    }
    $head = & git -C $destination rev-parse HEAD
    Assert-NativeSuccess "Verifying $name"
    if ($head -ne $dependency.commit) { throw "Pinned revision verification failed for $name." }
    Write-Host "${name}: $head"
}
