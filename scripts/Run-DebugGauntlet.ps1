#!/usr/bin/env pwsh
<#
.SYNOPSIS
    Full autonomous build/test/validation gauntlet for AetherCore.
.DESCRIPTION
    One entry point that runs the whole debug loop without a human in it:

      0. Guard   - the GPU abstraction guard (scripts/check-gpu-abstraction.ps1).
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
    Targets to build (default: Editor GameRuntime EngineTests). In a build tree
    configured with AETHERCORE_BUILD_EDITOR=OFF, Editor is dropped automatically
    and the editor smoke is skipped (runtime + tests still run).
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
.PARAMETER SkipGuard / -SkipBuild / -SkipUnit / -SkipEditor / -SkipRuntime
    Skip individual phases.
.PARAMETER CI
    Hosted-runner mode: skips the Editor and Runtime smokes (a hosted runner has no
    GPU or display). Explicit only - the manual CI workflow passes it. It is NOT
    inferred from $env:CI: agent and tool shells set that variable, which silently
    downgraded the local full run to build+unit while still printing GAUNTLET PASSED.
.EXAMPLE
    ./scripts/Run-DebugGauntlet.ps1
    ./scripts/Run-DebugGauntlet.ps1 -Config Release -Repack
    ./scripts/Run-DebugGauntlet.ps1 -CI            # build + unit only
    ./scripts/Run-DebugGauntlet.ps1 -SkipRuntime -RunSeconds 25
#>

param(
    # Reuse the dev tree instead of the separate Debug one. Same validation coverage (the
    # smokes force --validation, and the layer is compiled into both dev configs), and it
    # skips a second full build of the whole engine - which is what makes the default mode
    # expensive after any widely-included header changes. The tradeoff is real and worth
    # stating: RelWithDebInfo drops AE_ASSERT, so this trades assert coverage for minutes.
    # Use it as the routine gate; run the default before merging.
    [switch]$Fast,
    [string]$BuildDir = "",
    [ValidateSet("Debug", "RelWithDebInfo", "Release")]
    [string]$Config = "",
    [string[]]$Targets = @("Editor", "GameRuntime", "EngineTests"),
    [int]$RunSeconds = 12,
    [int]$ReadyTimeoutSeconds = 60,
    [switch]$Repack,
    [string]$ReportPath = "",
    [switch]$SkipBuild,
    [switch]$SkipUnit,
    [switch]$SkipEditor,
    [switch]$SkipRuntime,
    [switch]$SkipGuard,
    [switch]$CI
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot

# -Fast picks the dev tree; an explicit -BuildDir/-Config still wins over both.
if (-not $BuildDir) { $BuildDir = if ($Fast) { "build/default" } else { "build/vs2022-msvc" } }
if (-not $Config)   { $Config   = if ($Fast) { "RelWithDebInfo" } else { "Debug" } }

$BuildRoot = if ([System.IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $RepoRoot $BuildDir }

# Where CMake actually put the binaries.
#
# Multi-config generators (Visual Studio) nest them under <config>/; single-config ones
# (Ninja) put them straight in the target directory. Hard-coding the config subdirectory
# meant every phase reported "not found" against a Ninja tree in which everything was
# built and passing - the same assumption that once staged the app bundle one level below
# its own executable.
#
# CMAKE_CONFIGURATION_TYPES is the authority: CMake writes it into the cache only for a
# multi-config generator, which is exactly the distinction being made here.
$IsMultiConfig = $false
$CacheFile = Join-Path $BuildRoot "CMakeCache.txt"
if (Test-Path $CacheFile) {
    $IsMultiConfig = [bool](Select-String -LiteralPath $CacheFile -Pattern '^CMAKE_CONFIGURATION_TYPES:' -Quiet)
}

# Join a build-tree path whose <config> segment is written as {config}, resolving that
# segment to the configuration directory or to nothing.
function Join-BuildPath([string]$relative) {
    $resolved = if ($IsMultiConfig) { $relative -replace '\{config\}', $Config } else { $relative -replace '\{config\}/', '' -replace '/\{config\}', '' }
    return Join-Path $BuildRoot $resolved
}
if (-not $ReportPath) { $ReportPath = Join-Path $BuildRoot "gauntlet-report.json" }

# Runtime-only tolerance. A tree configured with AETHERCORE_BUILD_EDITOR=OFF has no
# Editor target, so asking cmake to build it (or smoking its exe) would fail for a
# reason that is not a regression. The build directory's cache is the authority;
# a not-yet-configured tree keeps the default all-targets behaviour.
$EditorDisabled = $false
if (Test-Path $CacheFile) {
    $EditorDisabled = [bool](Select-String -LiteralPath $CacheFile -Pattern '^AETHERCORE_BUILD_EDITOR:BOOL=OFF' -Quiet)
}
if ($EditorDisabled -and $Targets -contains "Editor") {
    $Targets = @($Targets | Where-Object { $_ -ne "Editor" })
    Write-Host "  AETHERCORE_BUILD_EDITOR=OFF in this build tree - dropped Editor from the target list." -ForegroundColor DarkYellow
}

# -CI is explicit on purpose; see the .PARAMETER note. Do not re-add $env:CI sniffing.
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
    # Vulkan loader diagnostics about machine-installed implicit overlays do not
    # describe application validation failures. Keep the patterns narrow so
    # synchronization, best-practice, and resource warnings still fail the smoke.
    $loaderNoise = "Validation: General: (Removing layer .* because it is a duplicate|Layer .* uses API version .* older than the application specified API version|Layer .* forced disabled because name matches filter of env var)"
    $benignValidation = @($logLines | Where-Object { $_ -match "\bWARN\b.*Validation:" -and $_ -match $loaderNoise })
    $errors     = @($logLines | Where-Object { $_ -match "\bERRO\b" })
    $valWarn    = @($logLines | Where-Object { $_ -match "\bWARN\b.*Validation:" -and $_ -notmatch $loaderNoise })
    $valInfo    = @($logLines | Where-Object { $_ -match "\bINFO\b.*Validation:" })
    $otherWarn  = @($logLines | Where-Object { $_ -match "\bWARN\b" -and $_ -notmatch "Validation:" }) + $benignValidation
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

# --- Phase 0: Static guards ----------------------------------------------
# The architectural check that used to run on a hosted runner. It costs seconds on the
# machine the code was written on, so there is no reason for it to live anywhere else.
function Invoke-Guard {
    Write-Head "Phase 0: GPU abstraction guard"
    $guardScript = Join-Path $PSScriptRoot "check-gpu-abstraction.ps1"
    if (-not (Test-Path $guardScript)) {
        Write-Bad "check-gpu-abstraction.ps1 not found at $guardScript"
        Add-Phase @{ name = "guard"; status = "fail"; detail = "guard script missing" }
        return $false
    }
    # The guard shells out to ripgrep for every check. Without it each rg call errors and
    # the checks quietly pass on nothing, so refuse rather than report a false clean.
    if (-not (Get-Command rg -ErrorAction SilentlyContinue)) {
        Write-Bad "ripgrep (rg) is not on PATH - the guard cannot run"
        Add-Phase @{ name = "guard"; status = "fail"; detail = "ripgrep not installed" }
        return $false
    }
    Push-Location $RepoRoot
    try { & $guardScript; $exit = $LASTEXITCODE }
    finally { Pop-Location }
    $ok = ($exit -eq 0)
    if ($ok) { Write-Ok "No new GPU abstraction leaks" }
    else { Write-Bad "GPU abstraction guard failed (exit $exit)" }
    Add-Phase @{ name = "guard"; status = if ($ok) { "pass" } else { "fail" } }
    return $ok
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
    # CMake prints status (e.g. "volk: using Vulkan_INCLUDE_DIRS...") on stderr; under
    # $ErrorActionPreference="Stop" the 2>&1 merge turns those into terminating
    # NativeCommandErrors. Success is judged by $LASTEXITCODE + error regex below.
    $ErrorActionPreference = "Continue"
    $output = & cmake @args 2>&1
    $ErrorActionPreference = "Stop"
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
    $exe = Join-BuildPath "tests/{config}/EngineTests.exe"
    if (-not (Test-Path $exe)) {
        Write-Bad "EngineTests.exe not found at $exe"
        Add-Phase @{ name = "unit"; status = "fail"; detail = "EngineTests.exe missing" }
        return $false
    }
    # Same NativeCommandError trap as Invoke-Build: the exe logs to stderr and
    # $ErrorActionPreference="Stop" would terminate on the merge.
    $ErrorActionPreference = "Continue"
    $output = & $exe 2>&1
    $ErrorActionPreference = "Stop"
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
    $exe = Join-BuildPath $ExePath
    if (-not (Test-Path $exe)) {
        Write-Bad "$Name executable not found at $exe"
        Add-Phase @{ name = $Name; status = "fail"; detail = "executable missing: $exe" }
        return $false
    }
    $log = Join-Path $LogDir $LogName
    if (Test-Path $log) { Remove-Item $log -Force }

    # Launch from the build root: shaders:// and engine.pak resolve relative to CWD.
    # Ask for validation explicitly rather than inheriting the config's default. The layer
    # is compiled into BOTH dev configs (VULKAN_CPU_DEBUG is gated on AE_DEV_TOOLING) but
    # only defaults on in Debug - so without this the same harness pointed at a
    # RelWithDebInfo tree runs with no validation at all and still reports a clean pass.
    $ExeArgs = ("$ExeArgs --validation").Trim()
    $proc = Start-Process -FilePath $exe -ArgumentList $ExeArgs -WorkingDirectory $BuildRoot -PassThru

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

    # A smoke that cannot confirm the layer was loaded proves nothing: zero findings and
    # zero coverage look identical from here. Treat an undetected tier as a failure rather
    # than reporting the most reassuring possible result for the least informative run.
    $tierUnknown = $f.Tier -eq "unknown"
    if ($tierUnknown) {
        Write-Bad "$Name ran without the validation layer (tier: unknown) - findings from this run mean nothing."
    }

    $ok = (-not $liveCrash) -and ($f.Failing -eq 0) -and (-not $tierUnknown)
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
Write-Host "  build: $BuildRoot ($Config)  |  mode: $(if ($CI) { 'CI (build+unit)' } elseif ($Fast) { 'fast (dev tree, no asserts)' } else { 'full' })" -ForegroundColor DarkGray

$results = @{}
$overall = $true

# Independent of the build, so it runs first and never short-circuits it.
if ($SkipGuard) { Write-Head "Phase 0: GPU abstraction guard"; Write-Skip "skipped"; Add-Phase @{ name = "guard"; status = "skip" } }
else { $results.guard = Invoke-Guard; if (-not $results.guard) { $overall = $false } }

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
    elseif ($EditorDisabled) {
        Write-Head "Phase: editor smoke"
        Write-Skip "skipped (AETHERCORE_BUILD_EDITOR=OFF - runtime-only tree, no Editor to smoke)"
        Add-Phase @{ name = "editor"; status = "skip"; detail = "editor not built (AETHERCORE_BUILD_EDITOR=OFF)" }
    }
    # The editor is always project-scoped now: pass --project (as F5 does), else it
    # exits requesting one. (Launching the Launcher would spawn a separate process the
    # smoke can't track, so drive the Editor directly.)
    # Editor.exe's OUTPUT dir follows the CMakeLists location (src/app), not the
    # source tree move - only the sources live in src/editor.
    else { $results.editor = Invoke-Smoke -Name "editor" -ExePath "src/app/{config}/Editor.exe" -LogName "Editor.log" -ExeArgs "--project `"$RepoRoot\projects\TestingProject`""; if (-not $results.editor) { $overall = $false } }

    if ($SkipRuntime) { Write-Head "Phase: runtime smoke"; Write-Skip $(if ($CI) { "skipped (CI: no GPU/display)" } else { "skipped" }); Add-Phase @{ name = "runtime"; status = "skip" } }
    # Point the runtime at the same project the editor smoke uses. Without this it resolves
    # its project from the build tree, and a build tree that has ever been packed into has a
    # data/project.pak - which the runtime correctly reads as "published package, use the
    # baked settings" - so it boots an EMPTY world, never logs the ready marker, and the
    # smoke reports a hang for a process that is perfectly healthy. AETHER_PROJECT_DIR is the
    # documented override and beats that check.
    else {
        $env:AETHER_PROJECT_DIR = "$RepoRoot\projects\TestingProject"
        try { $results.runtime = Invoke-Smoke -Name "runtime" -ExePath "src/app/{config}/AetherGame.exe" -LogName "AetherGame.log" }
        finally { Remove-Item Env:\AETHER_PROJECT_DIR -ErrorAction SilentlyContinue }
        if (-not $results.runtime) { $overall = $false }
    }
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
