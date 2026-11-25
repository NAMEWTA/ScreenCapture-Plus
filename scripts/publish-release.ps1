#Requires -Version 7.0
[CmdletBinding()]
param([string] $PackageDirectory = (Join-Path $PSScriptRoot '../package'))
. (Join-Path $PSScriptRoot 'BuildCommon.ps1')

# Release intent is an explicit, reviewed declaration. Ordinary main commits do
# not invent a version; already-published declarations leave that release intact.
$repository = 'NAMEWTA/huhucapture'
$declarationPath = Join-Path $RepoRoot 'releases/publish.json'
if ($env:GITHUB_EVENT_NAME -ne 'push' -or $env:GITHUB_REF -ne 'refs/heads/main' -or $env:GITHUB_REPOSITORY -cne $repository) {
    throw 'Declared releases are restricted to this repository and a main push.'
}
if (-not (Test-Path $declarationPath -PathType Leaf)) {
    Write-Host 'No release declaration is present; keeping the verified workflow artifact only.'
    exit 0
}
$declaration = Get-Content $declarationPath -Raw | ConvertFrom-Json -AsHashtable
$keys = @($declaration.Keys | Sort-Object)
if (($keys -join '|') -cne 'notes|schemaVersion|tag' -or $declaration.schemaVersion -ne 1) {
    throw 'The release declaration must contain only schemaVersion (1), tag, and notes.'
}
$tag = $declaration.tag
# This automatic route publishes stable updater-compatible versions only. A strict
# canonical tag also makes the Git ref, asset names and jq filters unambiguous.
if ($tag -isnot [string] -or $tag -cnotmatch '^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$') {
    throw 'The declared tag must be a stable canonical vMAJOR.MINOR.PATCH version.'
}
$expectedVersion = (Get-ReleaseVersion $tag) -join '.'
if ($expectedVersion -cne "$(Get-ProductVersion).0") {
    throw 'The declared release does not match the repository VERSION.'
}
if ($declaration.notes -isnot [string] -or $declaration.notes -cne "releases/$tag.md") {
    throw 'The declared notes must be the matching releases/<tag>.md file.'
}
$notes = Join-Path $RepoRoot $declaration.notes
if (-not (Test-Path $notes -PathType Leaf) -or [string]::IsNullOrWhiteSpace((Get-Content $notes -Raw))) {
    throw 'The declared release notes must exist and must not be empty.'
}
if ($env:GITHUB_SHA -notmatch '^[0-9a-f]{40}$') { throw 'The workflow source revision is missing or invalid.' }
$sourceCommit = $env:GITHUB_SHA

# A successful listing is required; an authentication/network failure must never
# be interpreted as permission to create a release. Published releases are immutable
# to this entry point. A pre-existing draft requires explicit maintainer recovery.
$releaseFilter = '.[] | select(.tag_name == "' + $tag + '") | {draft, html_url}'
$existingJson = & gh api "repos/$repository/releases?per_page=100" --paginate --jq $releaseFilter
Assert-NativeSuccess 'Checking whether the declared release already exists'
if ($existingJson) {
    $existing = $existingJson | ConvertFrom-Json
    if ($existing.draft) { throw "A draft $tag already exists. Inspect it before publishing; this job will not rewrite it." }
    Write-Host "Release $tag is already published: $($existing.html_url). Leaving it unchanged."
    exit 0
}

$currentMain = & gh api "repos/$repository/git/ref/heads/main" --jq '.object.sha'
Assert-NativeSuccess 'Checking the current main revision'
if ($currentMain.Trim() -cne $sourceCommit) {
    Write-Host 'A newer main revision exists; its own successful build will handle the declared release.'
    exit 0
}
$checkoutCommit = & git -C $RepoRoot rev-parse HEAD
Assert-NativeSuccess 'Checking the checked-out revision'
if ($checkoutCommit.Trim() -cne $sourceCommit) { throw 'The release checkout does not match the tested source revision.' }

$tagFilter = '.[] | select(.ref == "refs/tags/' + $tag + '") | .ref'
$existingTag = & gh api "repos/$repository/git/matching-refs/tags/$tag" --jq $tagFilter
Assert-NativeSuccess 'Checking whether the declared tag already exists'
if ($existingTag) { throw "Tag $tag already exists without a published release. This job never reuses or moves an existing tag." }

$package = (Resolve-Path $PackageDirectory).Path
$assetNames = @(
    'huhucapture.exe'
    "huhucapture-$tag-windows-x64.zip"
    "huhucapture-$tag-windows-x64-symbols.zip"
    'build-info.json'
    'SHA256SUMS.txt'
)
$actualNames = @(Get-ChildItem $package -File | Select-Object -ExpandProperty Name | Sort-Object)
if (($actualNames -join '|') -cne (($assetNames | Sort-Object) -join '|')) { throw 'The verified release asset set is incomplete or unexpected.' }
$metadata = Get-Content (Join-Path $package 'build-info.json') -Raw | ConvertFrom-Json
if ($metadata.repository -cne $repository -or $metadata.commit -cne $sourceCommit -or $metadata.tag -cne $tag -or
    $metadata.executableVersion -cne $expectedVersion -or $metadata.architecture -cne 'x64' -or $metadata.configuration -cne 'Release') {
    throw 'The build metadata does not match the declared release and tested commit.'
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
$executableHash = (Get-FileHash (Join-Path $package 'huhucapture.exe') -Algorithm SHA256).Hash.ToLowerInvariant()
if ($metadata.executableSha256 -cne $executableHash) { throw 'The executable does not match its build metadata.' }

# Do the final main check after hashing, immediately before publishing a new ref.
$currentMain = & gh api "repos/$repository/git/ref/heads/main" --jq '.object.sha'
Assert-NativeSuccess 'Rechecking main before creating the release tag'
if ($currentMain.Trim() -cne $sourceCommit) {
    Write-Host 'A newer main revision exists; keeping this run as verified artifacts only.'
    exit 0
}

# Atomically create the new tag. A check followed only by release --target would
# race another tag creator, because --target is ignored for a pre-existing tag.
# POST /git/refs fails if the ref already exists; it never moves an existing tag.
$createdCommit = & gh api --method POST "repos/$repository/git/refs" `
    -f "ref=refs/tags/$tag" -f "sha=$sourceCommit" --jq '.object.sha'
Assert-NativeSuccess 'Creating the declared release tag at the tested revision'
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
    --verify-tag --title "huhucapture $tag" --notes-file $notes --latest
Assert-NativeSuccess 'Publishing the declared release'
