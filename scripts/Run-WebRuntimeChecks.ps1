[CmdletBinding()]
param(
    [string]$EmsdkPath = '',
    [string]$AssetPackerPath = '',
    [string]$SlangCompiler = '',
    [switch]$SkipHostBuild,
    [switch]$BrowserChecks,
    [string]$PlaywrightModule = $env:AETHER_PLAYWRIGHT_MODULE,
    [string]$BrowserExecutable = '',
    [string]$ReportPath = 'logs/web-runtime/report.json'
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo
if (!$EmsdkPath) { $EmsdkPath = Join-Path $repo '.temp/toolchains/emsdk-main' }
$reportFile = [IO.Path]::GetFullPath($ReportPath)
$logRoot = Split-Path -Parent $reportFile
New-Item -ItemType Directory -Force -Path $logRoot | Out-Null
$report = @{ schemaVersion = 1; profile = 'browser-foundation-v1'; status = 'failed'; checks = @(); rendererReady = $false; audioReady = $false; storageReady = $false; inputReady = $false }

function Invoke-Checked([string]$Name, [string]$Executable, [string[]]$Arguments)
{
    $log = Join-Path $logRoot "$Name.log"
    & $Executable @Arguments > $log 2>&1
    if ($LASTEXITCODE -ne 0) { throw "$Name failed; see $log" }
    $report.checks += @{ name = $Name; status = 'passed'; log = $log }
}

try
{
    if (!$SkipHostBuild)
    {
        $hostPreset = if ($IsWindows) { 'default' } else { 'linux-clang' }
        Invoke-Checked 'host-configure' 'cmake' @('--preset', $hostPreset)
        Invoke-Checked 'host-tools-build' 'cmake' @('--build', '--preset', $hostPreset, '--target', 'AssetPacker')
    }
    if (!$AssetPackerPath)
    {
        $AssetPackerPath = if ($IsWindows) { Join-Path $repo 'build/default/tools/RelWithDebInfo/AssetPacker.exe' } else { Join-Path $repo 'build/linux-clang/tools/AssetPacker' }
    }
    if (!$SlangCompiler)
    {
        $candidate = if ($IsWindows -and $env:VULKAN_SDK) { Join-Path $env:VULKAN_SDK 'Bin/slangc.exe' } else { '' }
        $SlangCompiler = if ($IsWindows -and (Test-Path $candidate)) { $candidate } else { (Get-Command slangc -ErrorAction Stop).Source }
    }
    $env:EMSDK_QUIET = '1'
    . (Join-Path $EmsdkPath 'emsdk_env.ps1')
    $env:AETHER_WEB_ASSETPACKER = [IO.Path]::GetFullPath($AssetPackerPath)
    $env:AETHER_WEB_SLANGC = [IO.Path]::GetFullPath($SlangCompiler)
    Invoke-Checked 'web-configure' 'cmake' @('--preset', 'webgpu-browser', '-DCMAKE_DISABLE_FIND_PACKAGE_Vulkan=TRUE', '-DAETHERCORE_WEB_TEST_BRIDGE=ON')
    Invoke-Checked 'web-build' 'cmake' @('--build', '--preset', 'webgpu-browser', '-j', '6')
    $directory = Join-Path $repo 'build/webgpu-browser/web'
    Push-Location $directory
    try { Invoke-Checked 'wasm-execution' $env:EMSDK_NODE @('AetherGame.js') }
    finally { Pop-Location }
    $line = Get-Content (Join-Path $logRoot 'wasm-execution.log') | Where-Object { $_.StartsWith('{') } | Select-Object -Last 1
    $runtime = $line | ConvertFrom-Json
    if ($runtime.status -ne 'passed' -or $runtime.workers -ne 0) { throw 'Runtime foundation state failed' }
    $report.runtime = $runtime
    $report.importAudit = Get-Content 'build/webgpu-browser/web-imports.json' -Raw | ConvertFrom-Json
    $report.sourceAudit = Get-Content 'build/webgpu-browser/web-sources.json' -Raw | ConvertFrom-Json
    if ($BrowserChecks)
    {
        $browserReport = Join-Path $logRoot 'browser.json'
        $browserArguments = @('tools/web-runtime/browser-tests.mjs', "--report=$browserReport")
        if ($PlaywrightModule) { $browserArguments += "--playwright=$PlaywrightModule" }
        if ($BrowserExecutable) { $browserArguments += "--executable=$BrowserExecutable" }
        Invoke-Checked 'browser-checks' $env:EMSDK_NODE $browserArguments
        $report.browser = Get-Content -LiteralPath $browserReport -Raw | ConvertFrom-Json
        if ($report.browser.status -ne 'passed') { throw 'Browser runtime verification failed' }
        $required = @('sprite-input-resize-shutdown', 'unavailable', 'limits', 'device-loss', 'stop-during-init', 'stop-in-flight', 'layers', 'batch-capacity', 'responsive', 'drop-owner-init', 'stop-after-upload', 'assets-ready', 'assets-missing', 'assets-corrupt', 'assets-version', 'assets-target', 'assets-cancel', 'platform-save-reload', 'platform-storage-denied', 'platform-storage-quota', 'platform-audio-resume', 'reference-gameplay', 'browser-features', 'browser-gamepad', 'browser-focus-resume', 'invalid-font', 'invalid-scene-version', 'invalid-atlas-version', 'invalid-audio-component', 'browser-key-latch', 'browser-ui-order')
        foreach ($case in $required)
        {
            $matches = @($report.browser.cases | Where-Object { $_.name -eq $case -and $_.status -eq 'passed' })
            if ($matches.Count -ne 1) { throw "Required browser case missing, duplicated or failed: $case" }
        }
        $report.profile = 'browser-webgpu-2d-v1'
        $report.audioReady = $true
        $report.storageReady = $true
        $report.inputReady = $true
        $report.pointerReady = $true
        $report.gamepadApiReady = $true
        $report.physicalGamepadQualified = $false
        $report.fontReady = $true
        $report.rendererReady = $true
        $report.keyboardReady = $true
    }
    $report.status = 'passed'
}
catch { $report.error = $_.Exception.Message; throw }
finally { $report | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $reportFile -Encoding utf8 }
