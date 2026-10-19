[CmdletBinding()]
param(
	[ValidateSet('Native', 'Wasm', 'All')][string]$Target = 'All',
	[string]$EmsdkPath = '',
	[string]$WasmToolchain = 'nightly-2026-10-18',
	[string]$OutputRoot = '',
	[string]$LogRoot = ''
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '../../../..')).Path
if (!$OutputRoot) { $OutputRoot = Join-Path $repoRoot '.temp/rust-webgpu/interop' }
if (!$LogRoot) { $LogRoot = Join-Path $repoRoot 'logs/rust-webgpu/interop' }
New-Item -ItemType Directory -Force -Path $OutputRoot, $LogRoot | Out-Null
$OutputRoot = (Resolve-Path $OutputRoot).Path

function Invoke-ProbeTool([string]$Executable, [string[]]$Arguments, [string]$LogName)
{
	& $Executable @Arguments 2>&1 | Tee-Object -FilePath (Join-Path $LogRoot $LogName) | Write-Host
	if ($LASTEXITCODE -ne 0) { throw "$Executable failed with exit code $LASTEXITCODE; see $LogRoot/$LogName" }
}

$manifest = Join-Path $PSScriptRoot 'Cargo.toml'
$harness = Join-Path $PSScriptRoot 'InteropProbe.cpp'
if ($Target -in @('Native', 'All'))
{
	if (!$IsWindows) { throw 'This native reproduction currently selects the Windows MSVC Rust ABI.' }
	$nativeOutput = Join-Path $OutputRoot 'native'
	New-Item -ItemType Directory -Force -Path $nativeOutput | Out-Null
	Invoke-ProbeTool 'cargo' @('build', '--manifest-path', $manifest, '--release', '--target', 'x86_64-pc-windows-msvc', '--target-dir', (Join-Path $nativeOutput 'rust'), '--locked') 'native-rust.log'
	$library = Join-Path $nativeOutput 'rust/x86_64-pc-windows-msvc/release/aether_interop_probe.lib'
	$executable = Join-Path $nativeOutput 'interop-probe.exe'
	Invoke-ProbeTool 'clang++' @($harness, $library, '-std=c++26', '-O2', '-o', $executable, '-luserenv', '-lws2_32', '-lntdll', '-ldbghelp') 'native-link.log'
	Invoke-ProbeTool $executable @() 'native-result.json'
}

if ($Target -in @('Wasm', 'All'))
{
	if (!$EmsdkPath) { throw 'Wasm requires -EmsdkPath pointing at the pinned Emscripten SDK.' }
	$environmentScript = Join-Path $EmsdkPath 'emsdk_env.ps1'
	if (!(Test-Path $environmentScript)) { throw "Missing SDK environment script: $environmentScript" }
	$previousQuiet = $env:EMSDK_QUIET
	try
	{
		$env:EMSDK_QUIET = '1'
		. $environmentScript | Out-Null
	}
	finally { $env:EMSDK_QUIET = $previousQuiet }
	$wasmOutput = Join-Path $OutputRoot 'wasm'
	New-Item -ItemType Directory -Force -Path $wasmOutput | Out-Null
	# Rebuild std against this SDK to avoid assuming prebuilt Rust/Emscripten ABI compatibility.
	Invoke-ProbeTool 'cargo' @("+$WasmToolchain", 'build', '-Zbuild-std=std,panic_abort', '--manifest-path', $manifest, '--release', '--target', 'wasm32-unknown-emscripten', '--target-dir', (Join-Path $wasmOutput 'rust'), '--locked') 'wasm-rust.log'
	$library = Join-Path $wasmOutput 'rust/wasm32-unknown-emscripten/release/libaether_interop_probe.a'
	Invoke-ProbeTool 'em++' @($harness, $library, '-std=c++26', '-O2', '-fno-exceptions', '-sALLOW_MEMORY_GROWTH=1', '-sASSERTIONS=2', '-sEXIT_RUNTIME=1', '-sENVIRONMENT=web,node', '--shell-file', (Join-Path $PSScriptRoot 'shell.html'), '-o', (Join-Path $wasmOutput 'index.html')) 'wasm-link.log'
	Write-Host "Serve $wasmOutput over HTTP; browser result: globalThis.feasibilityResult. Node invocation alone does not qualify the browser gate."
}
