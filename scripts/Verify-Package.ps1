<#
.SYNOPSIS
Checks that a staged AetherCore package is complete and free of development files.

.DESCRIPTION
The failure this exists to catch is a package that is missing one file which every
developer's machine happens to have anyway - from the source tree, a build directory, or
a system-wide install. That kind of gap is invisible until someone who only downloaded
the editor tries to use it, so it is checked here, mechanically, against the staged
payload alone.

Run it against the output of `cmake --install --component Runtime --prefix <dir>`. The
release workflow runs it between staging and packaging, so a bad package fails the build
instead of shipping.

.PARAMETER PayloadDir
The staged package directory to check.

.PARAMETER RequireBundledRuntime
Require a bundled .NET runtime under dotnet/. The release workflow sets this; a local
`cmake --install` has no runtime staged and should not be failed for it.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$PayloadDir,
    [switch]$RequireBundledRuntime
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $PayloadDir)) {
    throw "Payload directory not found: $PayloadDir"
}
$root = (Resolve-Path -LiteralPath $PayloadDir).Path
Write-Host "Verifying package at $root"

$problems = [System.Collections.Generic.List[string]]::new()

function Require-File([string]$relative, [string]$why, [int]$minBytes = 1) {
    $full = Join-Path $root $relative
    if (-not (Test-Path -LiteralPath $full -PathType Leaf)) {
        $script:problems.Add("missing $relative - $why")
        return
    }
    $size = (Get-Item -LiteralPath $full).Length
    if ($size -lt $minBytes) {
        $script:problems.Add("$relative is $size bytes, expected at least $minBytes - $why")
    }
}

function Require-Dir([string]$relative, [string]$why) {
    if (-not (Test-Path -LiteralPath (Join-Path $root $relative) -PathType Container)) {
        $script:problems.Add("missing directory $relative - $why")
    }
}

# The applications themselves.
Require-File 'Launcher.exe' 'the project hub is the entry point users start from'
Require-File 'Editor.exe'   'the editor'
Require-File 'AetherGame.exe' 'the runtime the editor copies when publishing a game'
Require-File 'nethost.dll'  'CoreCLR cannot be bootstrapped without it'

# Engine content. engine.pak carries the compiled SPIR-V, so a truncated or placeholder
# pak fails at pipeline creation rather than at load, well away from the cause.
Require-File 'data/engine.pak' 'fonts, branding and every compiled shader live here' -minBytes 100000
Require-File 'data/config/EngineSettings.toml' 'the shipped settings layer'

# Managed assemblies the host loads at startup.
Require-File 'data/scripts/managed/AetherCore.dll' 'the C# SDK assembly'
Require-File 'data/scripts/managed/AetherCore.Interop.dll' 'the CoreCLR boot assembly'
Require-File 'data/scripts/managed/AetherCore.Interop.runtimeconfig.json' 'hostfxr initialises from this'

# What "New Project" needs. These are the files that used to be read out of the build
# machine's source tree, which is the whole reason this check exists.
Require-File 'data/templates/scenes/default.scene.toml' 'seeds a new 3D project'
Require-File 'data/templates/scenes/default2d.scene.toml' 'seeds a new 2D project'
Require-File 'data/sdk/managed/AetherCore/AetherCore.csproj' 'a generated game .csproj references it'
Require-File 'data/sdk/managed/Directory.Build.props' 'without it the SDK project has no TargetFramework'

if ($RequireBundledRuntime) {
    Require-Dir 'dotnet/host/fxr' 'hostfxr is resolved from the bundled runtime'
    Require-Dir 'dotnet/shared/Microsoft.NETCore.App' 'the runtime itself'
}

# Development leftovers. Scoped to the root directory on purpose: the managed .pdb files
# under data/scripts/managed are small, intentional, and what makes stepping through a
# project's own scripts work.
foreach ($unwanted in @('*.pdb', '*.lib', '*.exp', '*.ilk', 'GFSDK_Aftermath*.dll', '*.rc', 'cmake_install.cmake')) {
    foreach ($found in Get-ChildItem -LiteralPath $root -Filter $unwanted -File -ErrorAction SilentlyContinue) {
        $problems.Add("development file shipped: $($found.Name)")
    }
}
if (Test-Path -LiteralPath (Join-Path $root 'CMakeFiles')) {
    $problems.Add('development directory shipped: CMakeFiles')
}

$sizeMb = [math]::Round(((Get-ChildItem -LiteralPath $root -Recurse -File | Measure-Object Length -Sum).Sum / 1MB), 1)
$fileCount = (Get-ChildItem -LiteralPath $root -Recurse -File).Count

if ($problems.Count -gt 0) {
    Write-Host ''
    Write-Host "Package verification FAILED ($($problems.Count) problem(s)):" -ForegroundColor Red
    foreach ($p in $problems) { Write-Host "  - $p" -ForegroundColor Red }
    exit 1
}

Write-Host "Package OK: $fileCount files, $sizeMb MB" -ForegroundColor Green
exit 0
