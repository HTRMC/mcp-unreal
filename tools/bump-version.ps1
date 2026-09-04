<#
.SYNOPSIS
    Sets the release version across every file that carries one.

.DESCRIPTION
    A release version lives in Cargo.toml, in each plugin's .uplugin, and as a
    CHANGELOG heading. The release workflow and publish-release.ps1 both refuse
    to run when those disagree, so this script sets them together and moves the
    CHANGELOG's Unreleased entries into the new section.

.EXAMPLE
    pwsh -File tools/bump-version.ps1 0.2.0
    pwsh -File tools/bump-version.ps1 0.2.0 -WhatIf
#>
[CmdletBinding(SupportsShouldProcess)]
param(
    # New version, without the leading "v".
    [Parameter(Mandatory)]
    [ValidatePattern('^\d+\.\d+\.\d+$')]
    [string]$Version
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$today = (Get-Date).ToString('yyyy-MM-dd')

function Set-FileText {
    param([string]$Path, [string]$Text)
    if ($PSCmdlet.ShouldProcess($Path, 'update version')) {
        # UTF-8 without BOM, LF preserved as authored.
        [System.IO.File]::WriteAllText($Path, $Text)
    }
}

# --- Cargo.toml -------------------------------------------------------------
$cargoPath = Join-Path $repoRoot 'Cargo.toml'
$cargo = Get-Content $cargoPath -Raw
$updated = [regex]::Replace($cargo, '(?m)^version = "\d+\.\d+\.\d+"$', "version = `"$Version`"", 1)
if ($updated -eq $cargo) { throw "no version line found in $cargoPath" }
Set-FileText -Path $cargoPath -Text $updated
Write-Host "Cargo.toml        -> $Version"

# --- plugin descriptors -----------------------------------------------------
$descriptors = Get-ChildItem (Join-Path $repoRoot 'plugin') -Filter '*.uplugin' -Recurse -File
if (-not $descriptors) { throw 'no .uplugin files found' }

foreach ($descriptor in $descriptors) {
    $text = Get-Content $descriptor.FullName -Raw
    $next = [regex]::Replace($text, '"VersionName": "[^"]*"', "`"VersionName`": `"$Version`"", 1)
    if ($next -eq $text) { throw "no VersionName in $($descriptor.FullName)" }
    Set-FileText -Path $descriptor.FullName -Text $next
    Write-Host "$($descriptor.Name.PadRight(17)) -> $Version"
}

# --- CHANGELOG --------------------------------------------------------------
$changelogPath = Join-Path $repoRoot 'CHANGELOG.md'
$changelog = Get-Content $changelogPath -Raw

if ($changelog -match "(?m)^## \[$([regex]::Escape($Version))\]") {
    Write-Warning "CHANGELOG.md already has a [$Version] section — leaving it alone."
}
else {
    # Everything under "## [Unreleased]" becomes the new release section.
    $pattern = '(?ms)^## \[Unreleased\]\s*\r?\n(.*?)(?=^## \[|\z)'
    $match = [regex]::Match($changelog, $pattern)
    if (-not $match.Success) { throw 'no "## [Unreleased]" section in CHANGELOG.md' }

    $carried = $match.Groups[1].Value.Trim()
    if (-not $carried) {
        Write-Warning 'the Unreleased section is empty — write the release notes before tagging.'
    }

    $replacement = "## [Unreleased]`n`n## [$Version] - $today`n`n$carried`n`n"
    $changelog = $changelog.Remove($match.Index, $match.Length).Insert($match.Index, $replacement)

    # Refresh the link definitions at the bottom.
    $repoUrl = 'https://github.com/HTRMC/mcp-unreal'
    $changelog = [regex]::Replace(
        $changelog,
        '(?m)^\[Unreleased\]: .*$',
        "[Unreleased]: $repoUrl/compare/v$Version...HEAD")
    if ($changelog -notmatch "(?m)^\[$([regex]::Escape($Version))\]:") {
        $changelog = $changelog.TrimEnd() + "`n[$Version]: $repoUrl/releases/tag/v$Version`n"
    }

    Set-FileText -Path $changelogPath -Text $changelog
    Write-Host "CHANGELOG.md      -> [$Version] - $today"
}

Write-Host ''
Write-Host 'Review the diff, then:'
Write-Host "  git commit -am `"Release v$Version`""
Write-Host "  git tag v$Version && git push origin main v$Version"
Write-Host '  pwsh -File tools/publish-release.ps1 -Publish'
