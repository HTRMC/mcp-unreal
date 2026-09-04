<#
.SYNOPSIS
    Packages the McpLink plugins into precompiled, redistributable binaries.

.DESCRIPTION
    Runs the engine's own RunUAT BuildPlugin on each McpLink* plugin, which
    compiles the editor modules and lays out a distributable plugin folder
    (Source + Binaries + generated headers). Because the result ships compiled
    DLLs, a Blueprint-only project can enable it without Visual Studio or a
    C++ conversion — that is the whole point of this script.

    The binaries are tied to the engine they were built with, so the produced
    zip is stamped with the engine version and only supports that 5.x line.

    GitHub-hosted CI runners have no Unreal Engine installed, so this step runs
    on a machine with the engine (or a self-hosted runner) and the zip is
    attached to the release with tools/publish-release.ps1.

.EXAMPLE
    pwsh -File tools/package-plugin.ps1
    pwsh -File tools/package-plugin.ps1 -EngineRoot "D:\Program Files\Epic Games\UE_5.8"
#>
[CmdletBinding()]
param(
    # UE install root (the folder containing Engine/). Falls back to
    # UE_ENGINE_ROOT, then to the Epic Launcher's install manifest.
    [string]$EngineRoot,

    # Where the packaged plugins and the zip are written.
    [string]$OutDir,

    # Platforms to compile runtime modules for. All McpLink modules are
    # Editor-type, so this mostly controls the host editor build.
    [string[]]$Platforms = @('Win64'),

    # Package only these plugins (default: all of them).
    [string[]]$Only,

    # Leave the packaged folders without producing a zip.
    [switch]$SkipZip,

    # Keep the .pdb debug symbols. They are ~95% of the packaged size and are
    # of no use to someone consuming the binaries, so they are dropped by
    # default; build from source if you need to debug into the plugin.
    [switch]$KeepSymbols
)

$ErrorActionPreference = 'Stop'

function Resolve-EngineRoot {
    param([string]$Explicit)

    foreach ($candidate in @($Explicit, $env:UE_ENGINE_ROOT)) {
        if ($candidate -and (Test-Path (Join-Path $candidate 'Engine/Build/Build.version'))) {
            return (Resolve-Path $candidate).Path
        }
    }

    $manifest = Join-Path $env:ProgramData 'Epic/UnrealEngineLauncher/LauncherInstalled.dat'
    if (Test-Path $manifest) {
        $installs = (Get-Content $manifest -Raw | ConvertFrom-Json).InstallationList |
            Where-Object { $_.AppName -like 'UE_*' } |
            Sort-Object -Property AppName -Descending
        foreach ($install in $installs) {
            if (Test-Path (Join-Path $install.InstallLocation 'Engine/Build/Build.version')) {
                return $install.InstallLocation
            }
        }
    }

    throw 'No Unreal Engine install found. Pass -EngineRoot or set UE_ENGINE_ROOT.'
}

$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $OutDir) { $OutDir = Join-Path $repoRoot 'dist' }

$EngineRoot = Resolve-EngineRoot -Explicit $EngineRoot
$buildVersion = Get-Content (Join-Path $EngineRoot 'Engine/Build/Build.version') -Raw | ConvertFrom-Json
$engineVersion = "$($buildVersion.MajorVersion).$($buildVersion.MinorVersion).$($buildVersion.PatchVersion)"
$engineLine = "$($buildVersion.MajorVersion).$($buildVersion.MinorVersion)"

Write-Host "Engine     $EngineRoot ($engineVersion)"

if ($engineLine -ne '5.8') {
    Write-Warning "McpLink targets UE 5.8; packaging against $engineLine will most likely fail to compile."
}

$uat = Join-Path $EngineRoot 'Engine/Build/BatchFiles/RunUAT.bat'
if (-not (Test-Path $uat)) { throw "RunUAT.bat not found at $uat" }

# McpLink is the base plugin; the interop plugins depend on it, so it is
# packaged first and then passed to the others as a build dependency.
$basePlugin = Join-Path $repoRoot 'plugin/McpLink/McpLink.uplugin'
if (-not (Test-Path $basePlugin)) { throw "McpLink.uplugin not found at $basePlugin" }

$pluginVersion = (Get-Content $basePlugin -Raw | ConvertFrom-Json).VersionName
Write-Host "McpLink    $pluginVersion"

$pluginDirs = Get-ChildItem -Path (Join-Path $repoRoot 'plugin') -Directory |
    Where-Object { Test-Path (Join-Path $_.FullName "$($_.Name).uplugin") } |
    Sort-Object -Property @{ Expression = { $_.Name -ne 'McpLink' } }, Name

if ($Only) {
    $pluginDirs = $pluginDirs | Where-Object { $Only -contains $_.Name }
    if (-not $pluginDirs) { throw "none of -Only ($($Only -join ', ')) matched a plugin" }
}

$stageDir = Join-Path $OutDir 'plugins'
if (Test-Path $stageDir) { Remove-Item $stageDir -Recurse -Force }
New-Item -ItemType Directory -Path $stageDir -Force | Out-Null

$platformArg = $Platforms -join '+'

foreach ($dir in $pluginDirs) {
    $name = $dir.Name
    $uplugin = Join-Path $dir.FullName "$name.uplugin"
    $packageDir = Join-Path $stageDir $name

    Write-Host ''
    Write-Host "==> packaging $name for $platformArg"

    $uatArgs = @(
        'BuildPlugin'
        "-Plugin=$uplugin"
        "-Package=$packageDir"
        "-TargetPlatforms=$platformArg"
    )
    # Interop plugins compile against McpLink's headers.
    if ($name -ne 'McpLink') { $uatArgs += "-Dependencies=$basePlugin" }

    & $uat @uatArgs
    if ($LASTEXITCODE -ne 0) { throw "BuildPlugin failed for $name (exit $LASTEXITCODE)" }

    # BuildPlugin leaves its scratch host project behind; it is not part of the
    # redistributable.
    $hostProject = Join-Path $packageDir 'HostProject'
    if (Test-Path $hostProject) { Remove-Item $hostProject -Recurse -Force }

    $binaries = Join-Path $packageDir 'Binaries'
    if (-not (Test-Path $binaries)) {
        throw "$name packaged without Binaries/ — a Blueprint-only project could not use it."
    }

    if (-not $KeepSymbols) {
        $symbols = Get-ChildItem $packageDir -Recurse -Filter '*.pdb' -File
        if ($symbols) {
            $freed = [math]::Round((($symbols | Measure-Object Length -Sum).Sum / 1MB), 1)
            $symbols | Remove-Item -Force
            Write-Host "    dropped $($symbols.Count) pdb files ($freed MB)"
        }
    }

    $dlls = @(Get-ChildItem $binaries -Recurse -Filter '*.dll' -File)
    Write-Host "    $($dlls.Count) module binaries"
}

Write-Host ''
Write-Host "Packaged to $stageDir"

if ($SkipZip) { return }

$zipName = "McpLink-$pluginVersion-UE$engineLine-$($Platforms -join '-').zip"
$zipPath = Join-Path $OutDir $zipName
if (Test-Path $zipPath) { Remove-Item $zipPath -Force }

Compress-Archive -Path (Join-Path $stageDir '*') -DestinationPath $zipPath -CompressionLevel Optimal

$hash = (Get-FileHash $zipPath -Algorithm SHA256).Hash.ToLower()
Set-Content -Path "$zipPath.sha256" -Value "$hash  $zipName"

Write-Host ''
Write-Host "Zip        $zipPath"
Write-Host "SHA256     $hash"
Write-Host ''
Write-Host 'Users unzip this into <YourProject>/Plugins/ (or <Engine>/Plugins/Marketplace/).'
Write-Host "Built against UE $engineVersion — it will only load on the $engineLine line."
