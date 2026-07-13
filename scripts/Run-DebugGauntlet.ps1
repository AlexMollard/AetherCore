#!/usr/bin/env pwsh
<#
.SYNOPSIS
    Full autonomous build/test/validation gauntlet for AetherCore.
.DESCRIPTION
    One entry point that runs the whole debug loop without a human in it:

      1. Build   - compile Editor, GameRuntime and EngineTests (MSVC multi-config).
      2. Unit    - run EngineTests.exe and parse the doctest summary.
      3. Editor  - launch Editor.exe under whatever Vulkan validation tier the build
                   was compiled with, wait (by polling the log, not a fixed sleep)
                   until the startup scene loads, render a few frames, then stop.
                   Every Validation-category message is classified and any
                   error / warning / best-practice advisory fails the phase.
      4. Runtime - the same smoke for the shipped game runtime (AetherGame.exe).

    If a smoke process crashes (e.g. 0xC0000005) or never reaches the ready
    marker, the harness automatically re-runs it under cdb with the Vulkan layer
    symbols loaded and captures the faulting stack into the report.

    Emits a human-readable summary and a machine-readable report.json, and exits
    non-zero if any phase fails, so CI can gate on it.

    The Vulkan validation TIER is a compile-time choice in src/engine/Defines.hpp
    (VULKAN_CPU_DEBUG / VULKAN_GPU_DEBUG / VULKAN_BEST_PRACTICES); this harness
    runs whatever the current build compiled and reports which tier it detected.
.PARAMETER BuildDir
    CMake build directory (default: build/vs2022-msvc).
.PARAMETER Config
    Multi-config build type: Debug or Release (default: Debug).
.PARAMETER Targets
    Targets to build (default: Editor GameRuntime EngineTests).
.PARAMETER RunSeconds
    Frames-worth of extra runtime AFTER the startup scene loads, per smoke
    (default: 12). GPU-AV instrumentation is slow; bump this for that tier.
.PARAMETER ReadyTimeoutSeconds
    Max wait for the "startup scene loaded" marker before declaring a hang
    (default: 60; GPU-AV startup can take ~25s).
.PARAMETER Repack
    Delete the built engine.pak/project.pak before building so shaders repack
    from source (use after editing shaders or the SPIR-V processor).
.PARAMETER ReportPath
    Where to write the JSON report (default: <BuildDir>/gauntlet-report.json).
.PARAMETER SkipBuild / -SkipUnit / -SkipEditor / -SkipRuntime
    Skip individual phases.
.PARAMETER CI
    Continuous-integration mode: skips the Editor and Runtime smokes (GitHub
    runners have no GPU or display). Auto-enabled when $env:CI is set. Build and
    unit phases still run, which is what catches compile/link/test regressions.
.EXAMPLE
    ./scripts/Run-DebugGauntlet.ps1
    ./scripts/Run-DebugGauntlet.ps1 -Config Release -Repack
    ./scripts/Run-DebugGauntlet.ps1 -CI            # build + unit only
    ./scripts/Run-DebugGauntlet.ps1 -SkipRuntime -RunSeconds 25
#>

param(
    [string]$BuildDir = "build/vs2022-msvc",
    [ValidateSet("Debug", "Release")]
    [string]$Config = "Debug",
    [string[]]$Targets = @("Editor", "GameRuntime", "EngineTests"),
    [int]$RunSeconds = 12,
    [int]$ReadyTimeoutSeconds = 60,
    [switch]$Repack,
    [string]$ReportPath = "",
    [switch]$SkipBuild,
    [switch]$SkipUnit,
    [switch]$SkipEditor,
    [switch]$SkipRuntime,
    [switch]$CI
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
$BuildRoot = if ([System.IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $RepoRoot $BuildDir }
if (-not $ReportPath) { $ReportPath = Join-Path $BuildRoot "gauntlet-report.json" }

# GitHub Actions and most CI systems set $env:CI. No GPU/display there -> the
# validation smokes cannot run, so fold into build+unit automatically.
if ($env:CI) { $CI = $true }
if ($CI) { $SkipEditor = $true; $SkipRuntime = $true }

$LogDir = Join-Path $env:LOCALAPPDATA "AetherCore/logs"

# --- Console helpers -----------------------------------------------------
function Write-Head($text) { Write-Host "`n=== $text ===" -ForegroundColor Cyan }
function Write-Ok($text)   { Write-Host "  [PASS] $text" -ForegroundColor Green }
function Write-Bad($text)  { Write-Host "  [FAIL] $text" -ForegroundColor Red }
function Write-Skip($text) { Write-Host "  [SKIP] $text" -ForegroundColor DarkGray }
function Write-Note($text) { Write-Host "  $text" -ForegroundColor DarkYellow }

$phases = [System.Collections.Generic.List[object]]::new()
function Add-Phase($obj) { $phases.Add([pscustomobject]$obj) }

# Strip ANSI colour codes the engine logger emits, so pattern matching is clean.
function Remove-Ansi([string[]]$lines) {
    return $lines | ForEach-Object { $_ -replace "$([char]27)\[[0-9;]*m", "" }
}

# --- cdb (crash triage) --------------------------------------------------
function Get-Cdb {
    $candidates = @(
        "${env:ProgramFiles(x86)}\Windows Kits\10\Debuggers\x64\cdb.exe"
        "${env:ProgramFiles}\Windows Kits\10\Debuggers\x64\cdb.exe"
        "${env:ProgramFiles(x86)}\Windows Kits\11\Debuggers\x64\cdb.exe"
    )
    foreach ($c in $candidates) { if (Test-Path $c) { return $c } }
    return $null
}

function Get-SymbolPath {
    $sym = "srv*$env:LOCALAPPDATA\dbg\sym*https://msdl.microsoft.com/download/symbols"
    $vkBin = if ($env:VULKAN_SDK) { Join-Path $env:VULKAN_SDK "Bin" } else { $null }
    if (-not ($vkBin -and (Test-Path $vkBin))) {
        $vk = Get-ChildItem "C:\VulkanSDK" -Directory -ErrorAction SilentlyContinue | Sort-Object Name -Descending | Select-Object -First 1
        if ($vk) { $vkBin = Join-Path $vk.FullName "Bin" }
    }
    if ($vkBin -and (Test-Path $vkBin)) { $sym += ";$vkBin" }
    return $sym
}

# Attach cdb NON-INVASIVELY to a still-running (hung) process, dump every
# thread's stack, and return the frames in our own code (the interesting ones for
# a deadlock/livelock). Attaching + dumping returns immediately - unlike the
# crash path below, it must NEVER run 'g', which would block forever on a process
# that isn't going to fault.
function Get-HangStacks([int]$ProcId) {
    $cdb = Get-Cdb
    if (-not $cdb) { return @("<cdb not installed - install the Windows SDK Debuggers for auto hang triage>") }
    $out = Join-Path ([System.IO.Path]::GetTempPath()) ("gauntlet_hang_" + [System.IO.Path]::GetRandomFileName() + ".txt")
    $sym = Get-SymbolPath
    & $cdb -p $ProcId -y $sym -c "~*k 24; qd" > $out 2>&1
    $lines = Get-Content $out -ErrorAction SilentlyContinue
    Remove-Item $out -Force -ErrorAction SilentlyContinue
    if (-not $lines) { return @("<cdb produced no output attaching to pid $ProcId>") }
    $frames = @()
    foreach ($line in $lines) {
        # Keep frames in engine/app code or the thread/sync primitives that reveal
        # who is waiting on whom; skip pure CRT/OS noise and module-load lines.
        if ($line -match "!aether::|!main\b|ThreadLoop|WaitUntil|RenderGraph|ExecuteRenderFrame|RunFrameLoop|condition_variable::wait|Semaphore::Acquire") {
            $site = ($line -split "\s{2,}")[-1].Trim()
            if ($site -and $site -match "!" -and $frames -notcontains $site) { $frames += $site }
        }
    }
    if ($frames.Count -eq 0) { return @("<attached but no engine frames - see all-thread dump>") }
    return ($frames | Select-Object -First 24)
}

# Re-run a crashed exe under cdb and return the faulting stack (module!symbol
# frames). Returns $null if cdb is unavailable or no access violation reproduced.
function Get-CrashStack([string]$Exe, [string]$WorkDir) {
    $cdb = Get-Cdb
    if (-not $cdb) { return @("<cdb not installed - install the Windows SDK Debuggers for auto crash triage>") }
    $out = Join-Path ([System.IO.Path]::GetTempPath()) ("gauntlet_cdb_" + [System.IO.Path]::GetRandomFileName() + ".txt")
    $sym = Get-SymbolPath
    $cmds = "sxe av; g; .echo ===FAULT===; kb 25; q"
    Push-Location $WorkDir
    try {
        & $cdb -y $sym -c $cmds $Exe > $out 2>&1
    } finally {
        Pop-Location
    }
    $lines = Get-Content $out -ErrorAction SilentlyContinue
    Remove-Item $out -Force -ErrorAction SilentlyContinue
    if (-not $lines) { return @("<cdb produced no output>") }
    # Take the frames after the last ===FAULT=== marker.
    $faultIdx = -1
    for ($i = 0; $i -lt $lines.Count; $i++) { if ($lines[$i] -match "===FAULT===") { $faultIdx = $i } }
    if ($faultIdx -lt 0) { return @("<no access violation reproduced under cdb>") }
    $frames = @()
    for ($i = $faultIdx + 1; $i -lt $lines.Count -and $frames.Count -lt 20; $i++) {
        $line = $lines[$i]
        if ($line -match "!" -and $line -notmatch "^ModLoad|^\s*\*\*\*") {
            # Keep just the "module!symbol+offset" call site (last column).
            $site = ($line -split "\s{2,}")[-1].Trim()
            if ($site -and $site -match "!") { $frames += $site }
        }
    }
    if ($frames.Count -eq 0) { return @("<fault captured but no symbolic frames - see cdb>") }
    return $frames
}

# --- Validation-finding classification -----------------------------------
# A "finding" that fails a smoke: any ERRO (engine or validation), any
# Validation-category WARN, or any Validation-category INFO advisory (perf /
# best-practice). Benign non-validation warnings (e.g. the scene-format
# migration note) are reported but do not fail.
function Measure-Findings([string[]]$logLines) {
    $errors     = @($logLines | Where-Object { $_ -match "\bERRO\b" })
    $valWarn    = @($logLines | Where-Object { $_ -match "\bWARN\b.*Validation:" })
    $valInfo    = @($logLines | Where-Object { $_ -match "\bINFO\b.*Validation:" })
    $otherWarn  = @($logLines | Where-Object { $_ -match "\bWARN\b" -and $_ -notmatch "Validation:" })
    $tierLine   = @($logLines | Where-Object { $_ -match "validation layer enabled" }) | Select-Object -First 1
    return [pscustomobject]@{
        Errors     = $errors
        ValWarn    = $valWarn
        ValInfo    = $valInfo
        OtherWarn  = $otherWarn
        Tier       = if ($tierLine) { ($tierLine -replace "^.*validation layer enabled", "validation layer enabled").Trim() } else { "unknown" }
        Failing    = $errors.Count + $valWarn.Count + $valInfo.Count
    }
}

# --- Phase 1: Build ------------------------------------------------------
function Invoke-Build {
    Write-Head "Phase 1: Build ($Config)"
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    if ($Repack) {
        Get-ChildItem (Join-Path $BuildRoot "data") -Filter "*.pak*" -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
        Write-Note "Removed built paks - shaders will repack."
    }
    $args = @("--build", $BuildRoot, "--config", $Config, "--target") + $Targets
    $output = & cmake @args 2>&1
    $exit = $LASTEXITCODE
    $sw.Stop()
    $compileErrors = @($output | Where-Object { $_ -match "error C[0-9]|error LNK|error MSB|: error " } | Select-Object -First 25)
    $ok = ($exit -eq 0) -and ($compileErrors.Count -eq 0)
    if ($ok) { Write-Ok "Built $($Targets -join ', ') in $($sw.Elapsed.TotalSeconds.ToString('F1'))s" }
    else {
        Write-Bad "Build failed (exit $exit)"
        $compileErrors | ForEach-Object { Write-Note $_ }
    }
    Add-Phase @{ name = "build"; status = if ($ok) { "pass" } else { "fail" }; durationSec = [math]::Round($sw.Elapsed.TotalSeconds, 1); targets = $Targets; errors = $compileErrors }
    return $ok
}

# --- Phase 2: Unit tests -------------------------------------------------
function Invoke-UnitTests {
    Write-Head "Phase 2: Unit tests"
    $exe = Join-Path $BuildRoot "tests/$Config/EngineTests.exe"
    if (-not (Test-Path $exe)) {
        Write-Bad "EngineTests.exe not found at $exe"
        Add-Phase @{ name = "unit"; status = "fail"; detail = "EngineTests.exe missing" }
        return $false
    }
    $output = & $exe 2>&1
    $exit = $LASTEXITCODE
    $summary = ($output | Where-Object { $_ -match "test cases:" }) | Select-Object -First 1
    $cases = 0; $passed = 0; $failed = 0
    if ($summary -match "test cases:\s*(\d+)\s*\|\s*(\d+)\s*passed\s*\|\s*(\d+)\s*failed") {
        $cases = [int]$Matches[1]; $passed = [int]$Matches[2]; $failed = [int]$Matches[3]
    }
    $ok = ($exit -eq 0) -and ($failed -eq 0) -and ($cases -gt 0)
    if ($ok) { Write-Ok "$passed/$cases test cases passed" }
    else {
        Write-Bad "Tests failed ($failed failed / $cases cases, exit $exit)"
        $output | Where-Object { $_ -match "ERROR|FAILED|\.cpp:\d" } | Select-Object -First 15 | ForEach-Object { Write-Note $_ }
    }
    Add-Phase @{ name = "unit"; status = if ($ok) { "pass" } else { "fail" }; testCases = $cases; passed = $passed; failed = $failed }
    return $ok
}

# --- Smoke (shared by editor + runtime) ----------------------------------
function Invoke-Smoke([string]$Name, [string]$ExePath, [string]$LogName, [string]$ExeArgs = "") {
    Write-Head "Phase: $Name smoke"
    $exe = Join-Path $BuildRoot $ExePath
    if (-not (Test-Path $exe)) {
        Write-Bad "$Name executable not found at $exe"
        Add-Phase @{ name = $Name; status = "fail"; detail = "executable missing: $exe" }
        return $false
    }
    $log = Join-Path $LogDir $LogName
    if (Test-Path $log) { Remove-Item $log -Force }

    # Launch from the build root: shaders:// and engine.pak resolve relative to CWD.
    $proc = if ($ExeArgs) {
        Start-Process -FilePath $exe -ArgumentList $ExeArgs -WorkingDirectory $BuildRoot -PassThru
    } else {
        Start-Process -FilePath $exe -WorkingDirectory $BuildRoot -PassThru
    }

    # Condition-based readiness: poll the log for the startup-scene marker rather
    # than sleeping a fixed amount. Fail fast if the process dies first.
    $ready = $false; $crashedEarly = $false
    $deadline = (Get-Date).AddSeconds($ReadyTimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        if ($proc.HasExited) { $crashedEarly = $true; break }
        if (Test-Path $log) {
            $content = Remove-Ansi (Get-Content $log -ErrorAction SilentlyContinue)
            if ($content | Where-Object { $_ -match "Startup scene .* loaded" }) { $ready = $true; break }
        }
        Start-Sleep -Milliseconds 500
    }

    $crash = $null
    if ($crashedEarly -or -not $ready) {
        if ($proc.HasExited) {
            # Crashed before reaching the ready marker: re-run under cdb to catch
            # the access violation deterministically.
            $exitCode = "0x{0:X8}" -f ($proc.ExitCode -band 0xFFFFFFFF)
            Write-Bad "$Name crashed before ready (exit $exitCode) - triaging under cdb..."
            $stack = Get-CrashStack -Exe $exe -WorkDir $BuildRoot
        }
        else {
            # Still running but never signalled ready: a hang/deadlock. Attach to
            # the LIVE process and dump thread stacks (do not relaunch-and-'g').
            $exitCode = "hang (no ready marker in ${ReadyTimeoutSeconds}s)"
            Write-Bad "$Name hung - attaching to pid $($proc.Id) to dump live thread stacks..."
            $stack = Get-HangStacks -ProcId $proc.Id
            Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
        }
        $stack | Select-Object -First 10 | ForEach-Object { Write-Note $_ }
        $crash = [pscustomobject]@{ exitCode = $exitCode; stack = $stack }
        $logLines = if (Test-Path $log) { Remove-Ansi (Get-Content $log -ErrorAction SilentlyContinue) } else { @() }
        $f = Measure-Findings $logLines
        Add-Phase @{ name = $Name; status = "fail"; reachedReady = $false; crash = $crash; validationTier = $f.Tier; findings = @{ errors = $f.Errors.Count; warnings = $f.ValWarn.Count; advisories = $f.ValInfo.Count } }
        return $false
    }

    # Rendered to the startup scene; let it run a few more frames to exercise the
    # render graph and surface per-frame validation, then stop cleanly.
    Start-Sleep -Seconds $RunSeconds
    $liveCrash = $proc.HasExited
    $liveExit = if ($liveCrash) { "0x{0:X8}" -f ($proc.ExitCode -band 0xFFFFFFFF) } else { $null }
    if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
    Start-Sleep -Milliseconds 500

    $logLines = Remove-Ansi (Get-Content $log -ErrorAction SilentlyContinue)
    $f = Measure-Findings $logLines

    if ($liveCrash) {
        Write-Bad "$Name crashed mid-run (exit $liveExit) - triaging..."
        $stack = Get-CrashStack -Exe $exe -WorkDir $BuildRoot
        $stack | Select-Object -First 8 | ForEach-Object { Write-Note $_ }
        $crash = [pscustomobject]@{ exitCode = $liveExit; stack = $stack }
    }

    $ok = (-not $liveCrash) -and ($f.Failing -eq 0)
    if ($ok) {
        Write-Ok "$Name clean - tier: $($f.Tier); 0 validation findings"
        if ($f.OtherWarn.Count -gt 0) { Write-Note "($($f.OtherWarn.Count) benign non-validation warning(s), e.g. scene-format migration)" }
    }
    else {
        if ($f.Errors.Count)  { Write-Bad "$($f.Errors.Count) error line(s):";      $f.Errors  | Select-Object -First 6 | ForEach-Object { Write-Note $_ } }
        if ($f.ValWarn.Count) { Write-Bad "$($f.ValWarn.Count) validation warning(s):"; $f.ValWarn | Select-Object -First 6 | ForEach-Object { Write-Note $_ } }
        if ($f.ValInfo.Count) { Write-Bad "$($f.ValInfo.Count) best-practice/perf advisory(ies):"; $f.ValInfo | Select-Object -First 6 | ForEach-Object { Write-Note $_ } }
    }
    Add-Phase @{
        name = $Name; status = if ($ok) { "pass" } else { "fail" }; reachedReady = $true
        validationTier = $f.Tier
        findings = @{ errors = $f.Errors.Count; warnings = $f.ValWarn.Count; advisories = $f.ValInfo.Count; benignWarnings = $f.OtherWarn.Count }
        messages = @($f.Errors + $f.ValWarn + $f.ValInfo | Select-Object -First 30)
        crash = $crash
    }
    return $ok
}

# --- Run -----------------------------------------------------------------
Write-Host "AetherCore Debug Gauntlet" -ForegroundColor White
Write-Host "  build: $BuildRoot ($Config)  |  mode: $(if ($CI) { 'CI (build+unit)' } else { 'full' })" -ForegroundColor DarkGray

$results = @{}
$overall = $true

if ($SkipBuild) { Write-Head "Phase 1: Build"; Write-Skip "skipped"; Add-Phase @{ name = "build"; status = "skip" } }
else { $results.build = Invoke-Build; if (-not $results.build) { $overall = $false } }

# A broken build makes every later phase meaningless - short-circuit.
$buildOk = $SkipBuild -or $results.build
if (-not $buildOk) {
    Write-Head "Aborting: build failed"
}
else {
    if ($SkipUnit) { Write-Head "Phase 2: Unit tests"; Write-Skip "skipped"; Add-Phase @{ name = "unit"; status = "skip" } }
    else { $results.unit = Invoke-UnitTests; if (-not $results.unit) { $overall = $false } }

    if ($SkipEditor) { Write-Head "Phase: editor smoke"; Write-Skip $(if ($CI) { "skipped (CI: no GPU/display)" } else { "skipped" }); Add-Phase @{ name = "editor"; status = "skip" } }
    # The editor is always project-scoped now: pass --project (as F5 does), else it
    # exits requesting one. (Launching the Launcher would spawn a separate process the
    # smoke can't track, so drive the Editor directly.)
    else { $results.editor = Invoke-Smoke -Name "editor" -ExePath "src/app/$Config/Editor.exe" -LogName "Editor.log" -ExeArgs "--project `"$RepoRoot\projects\TestingProject`""; if (-not $results.editor) { $overall = $false } }

    if ($SkipRuntime) { Write-Head "Phase: runtime smoke"; Write-Skip $(if ($CI) { "skipped (CI: no GPU/display)" } else { "skipped" }); Add-Phase @{ name = "runtime"; status = "skip" } }
    else { $results.runtime = Invoke-Smoke -Name "runtime" -ExePath "src/app/$Config/AetherGame.exe" -LogName "AetherGame.log"; if (-not $results.runtime) { $overall = $false } }
}

# --- Report --------------------------------------------------------------
$report = [pscustomobject]@{
    timestamp = (Get-Date).ToString("o")
    repoRoot  = $RepoRoot
    buildDir  = $BuildRoot
    config    = $Config
    mode      = if ($CI) { "ci" } else { "full" }
    overall   = if ($overall) { "PASS" } else { "FAIL" }
    phases    = $phases
}
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $ReportPath) | Out-Null
$report | ConvertTo-Json -Depth 8 | Set-Content -Path $ReportPath -Encoding UTF8

Write-Head "Summary"
foreach ($p in $phases) {
    $colour = switch ($p.status) { "pass" { "Green" } "fail" { "Red" } default { "DarkGray" } }
    Write-Host ("  {0,-9} {1}" -f $p.name, $p.status.ToUpper()) -ForegroundColor $colour
}
Write-Host "`n  report: $ReportPath" -ForegroundColor DarkGray
if ($overall) { Write-Host "`nGAUNTLET PASSED`n" -ForegroundColor Green; exit 0 }
else { Write-Host "`nGAUNTLET FAILED`n" -ForegroundColor Red; exit 1 }
