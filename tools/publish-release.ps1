<#
.SYNOPSIS
    Attaches the precompiled McpLink plugin to a GitHub release.

.DESCRIPTION
    The Release workflow builds the mcp-unreal server for every platform and
    drafts the release, but it cannot build the UE plugin: GitHub-hosted runners
    have no Unreal Engine. This script fills that gap from a machine that does —
    it packages the plugin and uploads the zip to the release for a tag.

    Typical flow for a release:
        1. bump the version in Cargo.toml and the .uplugin files
        2. write the CHANGELOG.md section
        3. git tag v0.1.0 && git push origin v0.1.0     (drafts the release)
        4. pwsh -File tools/publish-release.ps1 -Publish

.EXAMPLE
    pwsh -File tools/publish-release.ps1
    pwsh -File tools/publish-release.ps1 -Tag v0.1.0 -Publish
#>
[CmdletBinding()]
param(
    # Release tag to upload to. Defaults to the most recent tag.
    [string]$Tag,

    # Passed through to package-plugin.ps1.
    [string]$EngineRoot,

    # Reuse whatever is already in dist/ instead of rebuilding the plugin.
    [switch]$SkipPackage,

    # Take the release out of draft once the plugin zip is attached.
    [switch]$Publish
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot

foreach ($tool in 'git', 'gh') {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        throw "$tool is not on PATH."
    }
}

Push-Location $repoRoot
try {
    if (-not $Tag) {
        $Tag = (git describe --tags --abbrev=0 2>$null)
        if (-not $Tag) { throw 'No tags found. Pass -Tag, or create one first.' }
    }
    Write-Host "Tag        $Tag"

    # The tag drives what users install, so refuse to publish a plugin built
    # from a different version than the tag claims.
    $pluginVersion = (Get-Content (Join-Path $repoRoot 'plugin/McpLink/McpLink.uplugin') -Raw |
        ConvertFrom-Json).VersionName
    if ($Tag.TrimStart('v') -ne $pluginVersion) {
        throw "tag $Tag does not match McpLink.uplugin VersionName $pluginVersion — bump one of them."
    }

    if (-not $SkipPackage) {
        $packageArgs = @{}
        if ($EngineRoot) { $packageArgs['EngineRoot'] = $EngineRoot }
        & (Join-Path $PSScriptRoot 'package-plugin.ps1') @packageArgs
        if ($LASTEXITCODE -ne 0) { throw "packaging failed (exit $LASTEXITCODE)" }
    }

    $assets = @(Get-ChildItem (Join-Path $repoRoot 'dist') -Filter 'McpLink-*.zip' -File)
    if (-not $assets) { throw 'no McpLink-*.zip in dist/ — run without -SkipPackage.' }

    foreach ($asset in $assets) {
        $size = [math]::Round($asset.Length / 1MB, 1)
        Write-Host "Uploading  $($asset.Name) ($size MB)"
    }

    gh release view $Tag > $null 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "release $Tag does not exist yet — push the tag and let the Release workflow draft it first."
    }

    gh release upload $Tag @($assets.FullName) --clobber
    if ($LASTEXITCODE -ne 0) { throw "gh release upload failed (exit $LASTEXITCODE)" }

    if ($Publish) {
        gh release edit $Tag --draft=false
        if ($LASTEXITCODE -ne 0) { throw "gh release edit failed (exit $LASTEXITCODE)" }
        Write-Host "Published  $Tag"
    }
    else {
        Write-Host ''
        Write-Host "Assets attached. The release is still a draft — publish it with:"
        Write-Host "  gh release edit $Tag --draft=false"
    }
}
finally {
    Pop-Location
}
