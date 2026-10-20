#requires -Version 7.4
<#
.SYNOPSIS
Qualify fresh Rust and C# games from CLI creation through measured Web export and native regressions.
.EXAMPLE
./scripts/Run-DistributionGauntlet.ps1 -PlaywrightModule <pinned-module-path> -SlangCompiler <slangc-path>
#>
[CmdletBinding()]
param(
 [string]$ReportPath = 'logs/distribution-report.json',
 [ValidateRange(1,3600)][int]$TimeoutSeconds = 1200,
 [string]$PlaywrightModule = $env:AETHER_PLAYWRIGHT_MODULE,
 [string]$SlangCompiler = $env:AETHER_WEB_SLANGC,
 [string]$Budgets = (Join-Path $PSScriptRoot '../tools/browser-tests/distribution-budgets.json')
)
$ErrorActionPreference = 'Stop'
$python = if ($env:AETHER_PYTHON) { $env:AETHER_PYTHON } else { 'python' }
$arguments = @((Join-Path $PSScriptRoot '../tools/project/distribution.py'), '--report', [IO.Path]::GetFullPath($ReportPath), '--timeout-seconds', [string]$TimeoutSeconds, '--budgets', [IO.Path]::GetFullPath($Budgets))
if ($PlaywrightModule) { $arguments += @('--playwright-module', $PlaywrightModule) }
if ($SlangCompiler) { $arguments += @('--slang-compiler', $SlangCompiler) }
& $python @arguments
exit $LASTEXITCODE
