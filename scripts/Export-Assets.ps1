#!/usr/bin/env pwsh
<#
.SYNOPSIS
    Rebuild and repack all game assets into assets.pak.
.DESCRIPTION
    Presents an interactive menu to select a build preset and configuration,
    then configures (if needed), builds the asset-packing target (Editor by
    default, EngineAssetsPak in runtime-only trees - see -Target), and syncs the
    LSP compilation database.
.PARAMETER Preset
    Skip the menu and use this preset directly.
.PARAMETER Config
    Build configuration (Debug, RelWithDebInfo, Release). Default: RelWithDebInfo.
.PARAMETER Target
    CMake target to build (default: Editor, which compiles AssetPacker and runs
    the packer via its dependency chain). In a build tree configured with
    AETHERCORE_BUILD_EDITOR=OFF the script falls back to the EngineAssetsPak
    custom target, which packs data/engine.pak without any editor present.
.PARAMETER NoLspSync
    Skip the final cmake --preset clangd step.
.EXAMPLE
    ./scripts/Export-Assets.ps1
    ./scripts/Export-Assets.ps1 -Preset vs2022-msvc -Config Debug
    ./scripts/Export-Assets.ps1 -Preset default -NoLspSync
#>

param(
    [string]$Preset = "",
    [string]$Config = "",
    [string]$Target = "",
    [switch]$NoLspSync
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot

# ── Available presets ────────────────────────────────────────────────────
$Presets = @(
    @{ Name = "vs2022-clang"; Desc = "VS 2022 ClangCL (C++26)" }
    @{ Name = "vs2022-msvc";  Desc = "VS 2022 MSVC (C++23)" }
    @{ Name = "default";      Desc = "VS 2022 MSVC (RelWithDebInfo default)" }
    @{ Name = "vs2026-msvc";  Desc = "VS 2026 MSVC" }
    @{ Name = "vs2026-clang"; Desc = "VS 2026 ClangCL (C++26)" }
)
$Configs = @("Debug", "RelWithDebInfo", "Release")

# ── Select preset ───────────────────────────────────────────────────────
if (-not $Preset) {
    Write-Host "`nSelect build preset:" -ForegroundColor Cyan
    for ($i = 0; $i -lt $Presets.Count; $i++) {
        Write-Host "  [$($i+1)] $($Presets[$i].Desc)"
    }
    do {
        $sel = Read-Host "Choice (1-$($Presets.Count))"
    } while ($sel -lt 1 -or $sel -gt $Presets.Count)
    $Preset = $Presets[$sel - 1].Name
}

# ── Select config ───────────────────────────────────────────────────────
if (-not $Config) {
    Write-Host "`nSelect configuration:" -ForegroundColor Cyan
    for ($i = 0; $i -lt $Configs.Count; $i++) {
        $tag = if ($Configs[$i] -eq "RelWithDebInfo") { " (recommended)" } else { "" }
        Write-Host "  [$($i+1)] $($Configs[$i])$tag"
    }
    do {
        $sel = Read-Host "Choice (1-$($Configs.Count))"
    } while ($sel -lt 1 -or $sel -gt $Configs.Count)
    $Config = $Configs[$sel - 1]
}

Write-Host "`n── Preset: $Preset  |  Config: $Config ──`n" -ForegroundColor Green

# All cmake commands must run from the repo root (CMakePresets.json lives there)
Push-Location $RepoRoot
try {
    # ── Configure if needed ─────────────────────────────────────────────
    $BinDir = "build/$Preset"
    if ($Preset -eq "default") { $BinDir = "build/default" }

    if (-not (Test-Path "$RepoRoot\$BinDir\CMakeCache.txt")) {
        Write-Host "Configuring $Preset... " -ForegroundColor Yellow
        cmake --preset $Preset *>&1 | Out-Host
        if ($LASTEXITCODE -ne 0) { Write-Error "cmake configure failed"; exit 1 }
    }

    # ── Resolve the target to build ────────────────────────────────────
    # Default is Editor (AssetPacker compiles as its dependency and the pack step
    # runs in the chain). A runtime-only tree (AETHERCORE_BUILD_EDITOR=OFF) has no
    # Editor target, so fall back to EngineAssetsPak - the unconditional custom
    # target that packs data/engine.pak - unless the caller named a target.
    if (-not $Target) {
        $Target = "Editor"
        $CacheFile = "$RepoRoot\$BinDir\CMakeCache.txt"
        if (Test-Path $CacheFile -PathType Leaf) {
            if (Select-String -LiteralPath $CacheFile -Pattern '^AETHERCORE_BUILD_EDITOR:BOOL=OFF' -Quiet) {
                $Target = "EngineAssetsPak"
                Write-Host "AETHERCORE_BUILD_EDITOR=OFF - building EngineAssetsPak instead of Editor." -ForegroundColor Yellow
            }
        }
    }

    # ── Force full repack ──────────────────────────────────────────────
    # Delete the incremental-build manifest so AssetPacker re-processes
    # every source file regardless of mtime (important after packer changes).
    $PakManifest = "$RepoRoot\$BinDir\data\assets.pak.manifest"
    if (Test-Path $PakManifest) {
        Write-Host "Removing incremental manifest (forcing full repack)..." -ForegroundColor Yellow
        Remove-Item -LiteralPath $PakManifest -Force
    }

    # ── Build the resolved target (Editor pulls in AssetPacker; EngineAssetsPak
    #    runs the packer directly in runtime-only trees) ────────────────
    Write-Host "`nBuilding $Target..." -ForegroundColor Yellow
    cmake --build --preset $Preset --config $Config --target $Target *>&1 | Out-Host
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Build failed - check errors above"
        exit 1
    }

    # ── Sync LSP compilation database ──────────────────────────────────
    # The MSVC build already generates compile_commands.json via
    # CMAKE_EXPORT_COMPILE_COMMANDS=ON. Copy it to the clangd build dir
    # so clangd picks it up (works because clangd translates MSVC flags).
    if (-not $NoLspSync) {
        $SrcDb = "$RepoRoot\$BinDir\compile_commands.json"
        $DstDir = "$RepoRoot\build\ninja-clang"
        $DstDb = "$DstDir\compile_commands.json"
        if (Test-Path $SrcDb) {
            New-Item -ItemType Directory -Path $DstDir -Force | Out-Null
            Copy-Item -LiteralPath $SrcDb -Destination $DstDb -Force
            Write-Host "LSP database synced from $BinDir to build/ninja-clang." -ForegroundColor Green
        } else {
            Write-Warning "compile_commands.json not found at $SrcDb - LSP may show stale diagnostics"
        }
    }

    # ── Summary ─────────────────────────────────────────────────────────
    $PakPath = "$RepoRoot\$BinDir\data\assets.pak"
    if (Test-Path $PakPath) {
        $size = (Get-Item $PakPath).Length
        $sizeStr = if ($size -gt 1GB) { "$([math]::Round($size/1GB, 2)) GB" }
                   elseif ($size -gt 1MB) { "$([math]::Round($size/1MB, 1)) MB" }
                   else { "$([math]::Round($size/1KB, 0)) KB" }
        Write-Host "`n✔ Assets exported to $PakPath ($sizeStr)" -ForegroundColor Green
    } else {
        Write-Host "`n⚠ assets.pak not found at expected path" -ForegroundColor Yellow
        Write-Host "  (looked: $PakPath)"
    }
} finally {
    Pop-Location
}
