#Requires -Version 7.0
[CmdletBinding()]
param([string] $PackageDirectory = (Join-Path $PSScriptRoot '../package'))
. (Join-Path $PSScriptRoot 'BuildCommon.ps1')

# This is the explicitly requested first fork release, not a general auto-release rule.
# Later versions keep using the release: published workflow. The notes file records
# release intent; it is only added to main after the feature changes are reviewed.
$tag = 'v0.0.1'
$repository = 'NAMEWTA/ScreenCapture-Plus'
$notes = Join-Path $RepoRoot "releases/$tag.md"
if ($env:GITHUB_EVENT_NAME -ne 'push' -or $env:GITHUB_REF -ne 'refs/heads/main' -or $env:GITHUB_REPOSITORY -cne $repository) {
    throw 'The first-release entry point is restricted to this repository and a main push.'
}
if (-not (Test-Path $notes -PathType Leaf)) {
    Write-Host 'No first-release declaration is present; keeping the verified workflow artifact only.'
    exit 0
}
if ([string]::IsNullOrWhiteSpace((Get-Content $notes -Raw))) { throw 'The release notes must not be empty.' }
if ($env:GITHUB_SHA -notmatch '^[0-9a-f]{40}$') { throw 'The workflow source revision is missing or invalid.' }
$sourceCommit = $env:GITHUB_SHA

# A successful listing is required; an authentication/network failure must never
# be interpreted as permission to create a release. Published releases are immutable
# to this entry point. A pre-existing draft requires explicit maintainer recovery.
$existingJson = & gh api "repos/$repository/releases?per_page=100" --paginate `
    --jq '.[] | select(.tag_name == "v0.0.1") | {draft, html_url}'
Assert-NativeSuccess 'Checking whether the first release already exists'
if ($existingJson) {
    $existing = $existingJson | ConvertFrom-Json
    if ($existing.draft) { throw "A draft $tag already exists. Inspect it before publishing; this job will not rewrite it." }
    Write-Host "The first release is already published: $($existing.html_url). Leaving it unchanged."
    exit 0
}

$currentMain = & gh api "repos/$repository/git/ref/heads/main" --jq '.object.sha'
Assert-NativeSuccess 'Checking the current main revision'
if ($currentMain.Trim() -cne $sourceCommit) {
    Write-Host 'A newer main revision exists; its own successful build will handle the first release.'
    exit 0
}
$checkoutCommit = & git -C $RepoRoot rev-parse HEAD
Assert-NativeSuccess 'Checking the checked-out revision'
if ($checkoutCommit.Trim() -cne $sourceCommit) { throw 'The release checkout does not match the tested source revision.' }

$existingTag = & gh api "repos/$repository/git/matching-refs/tags/$tag" `
    --jq '.[] | select(.ref == "refs/tags/v0.0.1") | .ref'
Assert-NativeSuccess 'Checking whether the first tag already exists'
if ($existingTag) { throw "Tag $tag already exists without a published release. This job never reuses or moves an existing tag." }

$package = (Resolve-Path $PackageDirectory).Path
$assetNames = @(
    'ScreenCapture.exe'
    "ScreenCapture-Plus-$tag-windows-x64.zip"
    "ScreenCapture-Plus-$tag-windows-x64-symbols.zip"
    'build-info.json'
    'SHA256SUMS.txt'
)
$actualNames = @(Get-ChildItem $package -File | Select-Object -ExpandProperty Name | Sort-Object)
if (($actualNames -join '|') -cne (($assetNames | Sort-Object) -join '|')) { throw 'The verified release asset set is incomplete or unexpected.' }
$metadata = Get-Content (Join-Path $package 'build-info.json') -Raw | ConvertFrom-Json
if ($metadata.repository -cne $repository -or $metadata.commit -cne $sourceCommit -or $metadata.tag -cne $tag -or
    $metadata.executableVersion -cne '0.0.1.0' -or $metadata.architecture -cne 'x64' -or $metadata.configuration -cne 'Release') {
    throw 'The build metadata does not match the authorized first release and tested commit.'
}
$checksumNames = @()
foreach ($line in Get-Content (Join-Path $package 'SHA256SUMS.txt')) {
    if ($line -notmatch '^([0-9a-f]{64})  ([A-Za-z0-9._-]+)$') { throw 'The checksum manifest contains an invalid entry.' }
    $expectedHash = $Matches[1]
    $name = $Matches[2]
    if ($name -ceq 'SHA256SUMS.txt' -or $name -cnotin $assetNames -or $name -cin $checksumNames) { throw 'The checksum manifest contains an unexpected or duplicate asset.' }
    $checksumNames += $name
    $actualHash = (Get-FileHash (Join-Path $package $name) -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualHash -cne $expectedHash) { throw "Release checksum mismatch: $name" }
}
if ($checksumNames.Count -ne $assetNames.Count - 1) { throw 'The checksum manifest is incomplete.' }
$executableHash = (Get-FileHash (Join-Path $package 'ScreenCapture.exe') -Algorithm SHA256).Hash.ToLowerInvariant()
if ($metadata.executableSha256 -cne $executableHash) { throw 'The executable does not match its build metadata.' }

# Atomically create the new tag. A check followed only by release --target would
# race another tag creator, because --target is ignored for a pre-existing tag.
# POST /git/refs fails if the ref already exists; it never moves an existing tag.
$createdCommit = & gh api --method POST "repos/$repository/git/refs" `
    -f "ref=refs/tags/$tag" -f "sha=$sourceCommit" --jq '.object.sha'
Assert-NativeSuccess 'Creating the first release tag at the tested revision'
if ($createdCommit.Trim() -cne $sourceCommit) { throw 'The created tag does not match the tested revision.' }
$tagCommit = & gh api "repos/$repository/git/ref/tags/$tag" --jq '.object.sha'
Assert-NativeSuccess 'Verifying the newly created tag'
if ($tagCommit.Trim() -cne $sourceCommit) { throw 'The release tag changed before publication.' }

# GitHub CLI creates a draft, uploads the supplied assets, and then publishes it.
# --verify-tag requires the exact tag created and checked above; --target also
# records the tested source revision in the release metadata.
# GITHUB_TOKEN release events do not trigger another workflow, so upload the tested
# assets here rather than relying on a second release: published build.
$assets = @($assetNames | ForEach-Object { Join-Path $package $_ })
& gh release create $tag @assets --repo $repository --target $sourceCommit `
    --verify-tag --title "ScreenCapture-Plus $tag" --notes-file $notes --latest
Assert-NativeSuccess 'Publishing the authorized first release'
