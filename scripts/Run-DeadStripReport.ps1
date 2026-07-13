<#
.SYNOPSIS
  Toggle AETHERCORE_DEAD_STRIP_REPORT on, build App + AssetPacker, parse the
  link maps, then toggle the option back off. Keeps the 240MB+ maps from
  regenerating on every normal `cmake --build` between audits.

.DESCRIPTION
  Reconfigures twice (ON, then OFF) and runs the Python parser in between.
  Maps persist after toggling OFF; pass -ReportOnly to re-parse without
  reconfiguring or rebuilding.

  Only App and AssetPacker are built. Engine is a transitive dep via App's
  target_link_libraries, so the Engine map (if we wanted one) is included
  in App.map by the linker.

.PARAMETER Preset
  CMake configure preset to use (e.g. vs2022-msvc, vs2022-clang, default,
  linux-clang). Default: 'default'.

.PARAMETER ReportOnly
  Skip the reconfigure + build. Just re-run the Python parser against the
  existing .map files in the build dir. Useful after editing .cpp files
  (e.g. deleting a candidate) to refresh the report.

.PARAMETER OutDir
  Where to write the report files. Default: <build-dir>/dead-strip-report/.

.EXAMPLE
  # Full audit: toggle on, build, parse, toggle off.
  .\scripts\Run-DeadStripReport.ps1 -Preset vs2022-msvc

  # Just re-parse after a source edit, no rebuild.
  .\scripts\Run-DeadStripReport.ps1 -Preset vs2022-msvc -ReportOnly

  # Linux build
  .\scripts\Run-DeadStripReport.ps1 -Preset linux-clang

  # Custom output location
  .\scripts\Run-DeadStripReport.ps1 -Preset default -OutDir D:/reports/aether
#>
[CmdletBinding()]
param(
    [string]$Preset = 'default',
    [switch]$ReportOnly,
    [string]$OutDir
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path "$PSScriptRoot/..").Path
$parserScript = Join-Path $repoRoot 'scripts/dead-strip-report.py'

# CMakePresets.json nests every preset under build/: 'build/default' for the
# 'default' preset and 'build/<preset>' for everything else.
function Get-BuildDir {
    param([string]$PresetName)
    if ($PresetName -eq 'default') {
        return Join-Path $repoRoot 'build/default'
    }
    return Join-Path $repoRoot "build/$PresetName"
}

function Invoke-Step {
    param(
        [string]$Title,
        [string]$Command,
        [string[]]$Args
    )
    Write-Host ""
    Write-Host "[$Title]" -ForegroundColor Cyan
    Write-Host "  $Command $($Args -join ' ')"
    & $Command @Args
    if ($LASTEXITCODE -ne 0) {
        throw "$Command failed with exit code $LASTEXITCODE"
    }
}

$buildDir = Get-BuildDir $Preset
if (-not $OutDir) {
    $OutDir = Join-Path $buildDir 'dead-strip-report'
}

if (-not (Test-Path $parserScript)) {
    throw "Parser script not found: $parserScript"
}

if ($ReportOnly) {
    if (-not (Test-Path $buildDir)) {
        throw "Build dir not found: $buildDir. Run without -ReportOnly first."
    }
    Invoke-Step '1/1' 'python' @(
        $parserScript,
        '--build-dir', $buildDir,
        '--source-dir', $repoRoot,
        '--out-dir', $OutDir
    )
    Write-Host ""
    Write-Host "Reports refreshed in: $OutDir" -ForegroundColor Green
    Write-Host "Open: $OutDir\summary.md" -ForegroundColor Green
    return
}

# 1. Reconfigure with option ON. This is what wires /MAP into the App and
# AssetPacker link commands. Reconfiguration is required because the option
# affects target_link_options; a plain rebuild wouldn't pick it up.
Invoke-Step '1/4' 'cmake' @('--preset', $Preset, '-DAETHERCORE_DEAD_STRIP_REPORT=ON')

# 2. Build only the two executables. Engine is pulled in transitively as a
# static lib via App's target_link_libraries, so the Engine code is reflected
# in App.map.
Invoke-Step '2/4' 'cmake' @('--build', '--preset', $Preset, '--target', 'App', 'AssetPacker')

# 3. Run the parser. Uses the actual binary dir resolved from the preset, not
# the script's guess, so a custom binaryDir in CMakePresets.json would still
# work (we currently hardcode build/<preset>/, which matches all current presets).
if (-not (Test-Path $buildDir)) {
    throw "Expected build dir not found: $buildDir. Check CMakePresets.json binaryDir."
}
Invoke-Step '3/4' 'python' @(
    $parserScript,
    '--build-dir', $buildDir,
    '--source-dir', $repoRoot,
    '--out-dir', $OutDir
)

# 4. Reconfigure with option OFF so future incremental builds stay lean.
# The .map files persist on disk; the parser can re-read them any time
# (use -ReportOnly).
Invoke-Step '4/4' 'cmake' @('--preset', $Preset, '-DAETHERCORE_DEAD_STRIP_REPORT=OFF')

Write-Host ""
Write-Host "Done." -ForegroundColor Green
Write-Host "  Reports: $OutDir" -ForegroundColor Green
Write-Host "  Open:    $OutDir\summary.md" -ForegroundColor Green
Write-Host ""
Write-Host "Re-run the report without rebuilding via:" -ForegroundColor Gray
Write-Host "  $PSCommandPath -Preset $Preset -ReportOnly" -ForegroundColor Gray
