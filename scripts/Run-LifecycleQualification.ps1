#requires -Version 7.4
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$ProductDir,
    [Parameter(Mandatory)][string]$StateDir,
    [Parameter(Mandatory)][string]$ReportPath,
    [ValidateNotNullOrEmpty()][ValidateSet('Full','UiShell','Runtime')][string[]]$Profiles = @('Full','UiShell','Runtime'),
    [string]$Project = '', [string]$ControlClient = '',
    [ValidateRange(1,10000)][int]$Cycles = 3,
    [ValidateRange(1,86400)][int]$RunSeconds = 12,
    [ValidateRange(1,600)][int]$ReadyTimeoutSeconds = 90,
    [ValidateRange(1,120)][int]$ShutdownTimeoutSeconds = 30
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/Qualification.Common.ps1"
$phases = [Collections.Generic.List[object]]::new()
$report = [ordered]@{ schemaVersion = 1; status = 'fail'; kind = 'local-lifecycle'; cleanMachine = 'blocked'; cycles = $Cycles; profiles = $Profiles; runSeconds = $RunSeconds; readyTimeoutSeconds = $ReadyTimeoutSeconds; shutdownTimeoutSeconds = $ShutdownTimeoutSeconds; phases = $phases }
try {
    if ($Profiles.Count -eq 0 -or @($Profiles | Select-Object -Unique).Count -ne $Profiles.Count) { throw 'At least one unique lifecycle profile is required; duplicate profiles are unsupported' }
    $product = (Resolve-Path -LiteralPath $ProductDir).Path
    if (-not ((Test-Path -LiteralPath (Join-Path $product 'BUILD_PROVENANCE.json') -PathType Leaf) -or (Test-Path -LiteralPath (Join-Path $product 'aether-product') -PathType Leaf))) { throw 'Lifecycle qualification requires a delivered product marker (BUILD_PROVENANCE.json or aether-product)' }
    $report.isolationMode = 'delivered-marker-default'
    Assert-QualifiedOutputOutsideBundle $product $StateDir
    Assert-QualifiedOutputOutsideBundle $product $ReportPath
    if (Test-Path -LiteralPath (Join-Path $product 'BUILD_PROVENANCE.json') -PathType Leaf) {
        & python "$PSScriptRoot/Write-ReleaseIdentity.py" --payload $product --verify-stage
        if ($LASTEXITCODE -ne 0) { throw 'Delivered lifecycle bundle does not match its retained staged provenance' }
    }
    $bundleBefore=Get-QualifiedBundleInventory $product
    $report.bundleBefore=$bundleBefore
    $state = [IO.Path]::GetFullPath($StateDir)
    if (Test-Path -LiteralPath $state) { throw 'StateDir must be unused to prevent stale readiness/shutdown evidence' }
    New-Item -ItemType Directory $state | Out-Null
    $suffix = if ($env:OS -eq 'Windows_NT') { '.exe' } else { '' }
    foreach ($cycle in 1..$Cycles) {
        foreach ($profile in $Profiles) {
            $name = switch ($profile) { 'Full' { 'Editor' } 'UiShell' { 'Launcher' } 'Runtime' { 'AetherGame' } }
            $runState = Join-Path $state "$cycle-$profile"
            New-Item -ItemType Directory $runState | Out-Null
            $start = [Diagnostics.ProcessStartInfo]::new()
            $start.FileName = Join-Path $product "$name$suffix"; $start.WorkingDirectory = $product
            $start.UseShellExecute = $false; $start.CreateNoWindow = $true
            foreach ($key in @($start.Environment.Keys | Where-Object { $_ -match '^(AETHER_|AETHERCORE_|DOTNET_)' })) { $start.Environment.Remove($key) | Out-Null }
            $start.Environment['AETHER_USER_DATA_DIR'] = $runState
            $start.Environment['AETHER_AGENT_SESSION'] = '1'
            $start.Environment['AETHER_AGENT_NAME'] = 'qualification'
            $start.Environment['AETHER_AGENT_BACKGROUND'] = '0'
            $start.Environment['DOTNET_ROOT'] = Join-Path $product 'dotnet'
            $start.Environment['DOTNET_MULTILEVEL_LOOKUP'] = '0'
            $start.Environment['PATH'] = if ($env:OS -eq 'Windows_NT') { "$product;$env:SystemRoot/System32;$env:SystemRoot" } else { "${product}:/usr/bin:/bin" }
            $start.Environment['AETHER_PROJECT_DIR'] = ''
            $start.Environment['AETHER_CONTROL_PORT'] = 'off'
            $portProbe = [Net.Sockets.UdpClient]::new(0)
            $controlPort = $portProbe.Client.LocalEndPoint.Port
            $portProbe.Dispose()
            if ($profile -eq 'Full') {
                if (-not $Project) { throw 'Full profile requires an explicit project' }
                $start.ArgumentList.Add('--project'); $start.ArgumentList.Add([IO.Path]::GetFullPath($Project))
                $start.ArgumentList.Add('--agent'); $start.ArgumentList.Add('--background')
                $start.ArgumentList.Add('--agent-name'); $start.ArgumentList.Add('lifecycle-qualification')
                $start.ArgumentList.Add('--control-port'); $start.ArgumentList.Add([string]$controlPort)
            }
            $process = $null
            $phase = [ordered]@{ name = "$cycle-$profile"; status = 'fail'; state = $runState; profile = $profile }
            try {
                $process = [Diagnostics.Process]::Start($start)
                $phase.pid = $process.Id
                $deadline = [datetime]::UtcNow.AddSeconds($ReadyTimeoutSeconds)
                $ready = $false
                do {
                    if ($process.HasExited) { throw 'Process exited before readiness' }
                    $logs = @(Get-ChildItem -LiteralPath $runState -Recurse -Filter '*.log' -ErrorAction SilentlyContinue)
                    $lines = @($logs | Get-Content -ErrorAction SilentlyContinue)
                    $marker = if ($profile -eq 'UiShell') { 'Launcher: MCP control-port forwarding disabled|Launcher MCP endpoint uses port' } else { 'Startup scene .* loaded' }
                    $ready = [bool]($lines -match $marker)
                    if (-not $ready) { Start-Sleep -Milliseconds 200 }
                } while (-not $ready -and [datetime]::UtcNow -lt $deadline)
                if (-not $ready) { throw 'Readiness deadline exceeded (or isolated-state override unavailable)' }
                $before = $process.WorkingSet64
                $deadline = [datetime]::UtcNow.AddSeconds($RunSeconds)
                while ([datetime]::UtcNow -lt $deadline) {
                    if ($process.HasExited) { throw 'Process exited during soak' }
                    Start-Sleep -Milliseconds 250
                }
                $process.Refresh()
                $phase.memory = @{ startWorkingSetBytes = $before; endWorkingSetBytes = $process.WorkingSet64; peakWorkingSetBytes = $process.PeakWorkingSet64 }
                $phase.shutdown = if ($profile -eq 'Full' -and $ControlClient) { Stop-QualifiedProcess $process $ControlClient $controlPort $ShutdownTimeoutSeconds } else { Stop-QualifiedProcess $process '' 0 $ShutdownTimeoutSeconds }
                if ($phase.shutdown.status -ne 'pass') { throw 'Graceful shutdown failed' }
                Assert-ShutdownLog @($logs | Get-Content)
                $phase.status = 'pass'
            } catch { $phase.detail = $_.Exception.Message }
            finally {
                if ($process -and -not $process.HasExited) {
                    $phase.cleanup = Stop-QualifiedProcess $process '' 0 $ShutdownTimeoutSeconds
                }
                if ($process) { $process.Dispose() }
                $phases.Add([pscustomobject]$phase)
            }
        }
    }
    if ($phases.Count -gt 0 -and @($phases | Where-Object status -ne 'pass').Count -eq 0 -and $phases.Count -eq $Cycles * $Profiles.Count) { $report.status = 'pass' }
} catch { $report.detail = $_.Exception.Message }
if ($bundleBefore) {
    $immutable=[ordered]@{status='fail';beforeSha256=$bundleBefore.inventorySha256}
    try {
        $after=Get-QualifiedBundleInventory $product
        $immutable.afterSha256=$after.inventorySha256
        Assert-QualifiedBundleUnchanged $bundleBefore $after
        $immutable.status='pass'
    } catch { $immutable.detail=$_.Exception.Message; $report.status='fail' }
    $report.bundleImmutability=$immutable
}
New-Item -ItemType Directory -Force (Split-Path ([IO.Path]::GetFullPath($ReportPath))) | Out-Null
$report | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $ReportPath -Encoding utf8
if ($report.status -ne 'pass') { exit 1 }
exit 0
