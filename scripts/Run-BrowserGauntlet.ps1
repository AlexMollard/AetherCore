[CmdletBinding()]
param(
    [string]$Project = 'projects/AgentPipelineRust',
    [string]$ReportPath = 'logs/browser-report.json',
    [switch]$SkipBuild,
    [string]$PlaywrightModule = $env:AETHER_PLAYWRIGHT_MODULE,
    [string]$BrowserExecutable = '',
    [int]$TimeoutSeconds = 240,
    [string]$SlangCompiler = '',
    [string]$EmsdkPath = ''
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo
$reportFile = [IO.Path]::GetFullPath($ReportPath)
$logRoot = Split-Path -Parent $reportFile
New-Item -ItemType Directory -Force -Path $logRoot | Out-Null
$timer = [Diagnostics.Stopwatch]::StartNew()
$phase = 'preflight'
. "$PSScriptRoot/Invoke-BrowserCommand.ps1"
function Invoke-Phase([string]$Executable, [string[]]$CommandArguments, [string]$LogName)
{
    $remaining = [int]($TimeoutSeconds * 1000 - $timer.ElapsedMilliseconds)
    return Invoke-BrowserCommand $Executable $CommandArguments (Join-Path $logRoot $LogName) $remaining
}
try
{
    if ($TimeoutSeconds -lt 1 -or $TimeoutSeconds -gt 1200) { throw 'TimeoutSeconds must be 1..1200' }
    $projectPath = [IO.Path]::GetFullPath($Project)
    if ($projectPath -ne (Join-Path $repo 'projects/AgentPipelineRust'))
    {
        throw 'This regression gauntlet qualifies AgentPipelineRust only; use Web publishing for generic Rust exports. C# browser execution is not implemented'
    }
    if (!$SkipBuild)
    {
        $phase = 'test-build'
        $foundationReport = Join-Path $logRoot 'browser-build.json'
        if (!$EmsdkPath) { $EmsdkPath = Join-Path $repo '.temp/toolchains/emsdk-main' }
        $env:EMSDK_QUIET = '1'
        . (Join-Path $EmsdkPath 'emsdk_env.ps1')
        $env:AETHER_WEB_ASSETPACKER = if ($IsWindows) { Join-Path $repo 'build/default/tools/RelWithDebInfo/AssetPacker.exe' } else { Join-Path $repo 'build/linux-clang/tools/AssetPacker' }
        if (!$SlangCompiler) { $SlangCompiler = if ($IsWindows -and $env:VULKAN_SDK) { Join-Path $env:VULKAN_SDK 'Bin/slangc.exe' } else { (Get-Command slangc -ErrorAction Stop).Source } }
        $env:AETHER_WEB_SLANGC = $SlangCompiler
        $shell = (Get-Process -Id $PID).Path
        $code = Invoke-Phase $shell @('-NoProfile', '-File', "$PSScriptRoot/Run-WebRuntimeChecks.ps1", '-SlangCompiler', $SlangCompiler, '-EmsdkPath', $EmsdkPath, '-ReportPath', $foundationReport) 'browser-build.log'
        if ($code -ne 0) { throw 'Browser automation build failed' }
        $phase = 'production-build'
        $cmake = (Get-Command cmake -ErrorAction Stop).Source
        $code = Invoke-Phase $cmake @('--preset', 'webgpu-browser', '-B', 'build/webgpu-production', '-DAETHERCORE_WEB_TEST_BRIDGE=OFF') 'production-configure.log'
        if ($code -ne 0) { throw 'Production configure failed' }
        $code = Invoke-Phase $cmake @('--build', 'build/webgpu-production', '-j6') 'production-build.log'
        if ($code -ne 0) { throw 'Production build failed' }
    }
    $phase = 'browser-verification'
    $arguments = @('tools/browser-tests/run.mjs', "--project=$projectPath", "--report=$reportFile", "--timeout=$($TimeoutSeconds * 1000)")
    if ($PlaywrightModule) { $arguments += "--playwright=$PlaywrightModule" }
    if ($BrowserExecutable) { $arguments += "--executable=$BrowserExecutable" }
    $result = Invoke-Phase (Get-Command node -ErrorAction Stop).Source $arguments 'browser-run.log'
    if (!(Test-Path -LiteralPath $reportFile)) { throw 'Browser runner produced no report' }
    Get-Content -LiteralPath $reportFile -Raw
    exit $result
}
catch
{
    $terminalStatus = if ($_.Exception -is [TimeoutException]) { 'cancelled' } else { 'blocked' }
    $report = @{
        schemaVersion = 1; operation = 'browser-test'; target = 'web'; profile = 'webgpu-2d-v1'
        project = $Project; status = $terminalStatus; phase = $phase; durationMs = $timer.ElapsedMilliseconds
        diagnostics = @(@{ code = 'BROWSER_PREFLIGHT'; message = $_.Exception.Message; phase = $phase })
        cases = @(@{ name = 'browser-qualification'; status = 'blocked'; reason = 'Required checks did not finish' }); artifacts = @()
    }
    $report | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $reportFile -Encoding utf8
    $report | ConvertTo-Json -Depth 10
    exit 1
}
