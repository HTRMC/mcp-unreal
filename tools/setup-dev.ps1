<#
.SYNOPSIS
    Prepares test-project/ for development by linking the plugins into it.

.DESCRIPTION
    test-project/Plugins/McpLink* are NTFS directory junctions pointing at
    plugin/McpLink*, so the editor loads the sources you are editing without a
    second copy. Junctions are not committed (git would store them as symlinks
    or plain files and they would break on clone), so a fresh checkout has to
    recreate them — that is what this script does.

    Run once after cloning:  pwsh -File tools/setup-dev.ps1
#>
[CmdletBinding()]
param(
    # Recreate junctions that already exist.
    [switch]$Force
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$pluginRoot = Join-Path $repoRoot 'plugin'
$targetRoot = Join-Path $repoRoot 'test-project/Plugins'

if (-not (Test-Path $pluginRoot)) {
    throw "plugin/ not found under $repoRoot — run this from a full checkout."
}

New-Item -ItemType Directory -Path $targetRoot -Force | Out-Null

$plugins = Get-ChildItem -Path $pluginRoot -Directory | Where-Object {
    Test-Path (Join-Path $_.FullName "$($_.Name).uplugin")
}

if (-not $plugins) { throw "no plugins found under $pluginRoot" }

foreach ($plugin in $plugins) {
    $link = Join-Path $targetRoot $plugin.Name

    if (Test-Path $link) {
        $existing = Get-Item $link -Force
        $isLink = $null -ne $existing.LinkType
        if (-not $Force -and $isLink) {
            Write-Host "ok       $($plugin.Name) -> $($existing.Target)"
            continue
        }
        if (-not $isLink) {
            throw "$link exists and is a real directory, not a junction — move it aside first."
        }
        Remove-Item $link -Force -Recurse
    }

    New-Item -ItemType Junction -Path $link -Target $plugin.FullName | Out-Null
    Write-Host "linked   $($plugin.Name) -> $($plugin.FullName)"
}

Write-Host ''
Write-Host "test-project/Plugins is ready. Build the editor target with:"
Write-Host '  & "$env:UE_ENGINE_ROOT\Engine\Build\BatchFiles\Build.bat" McpTestEditor Win64 Development -Project="' -NoNewline
Write-Host (Join-Path $repoRoot 'test-project/McpTest.uproject') -NoNewline
Write-Host '" -WaitMutex'
