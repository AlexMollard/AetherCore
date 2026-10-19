[CmdletBinding()]
param(
	[ValidateSet('Native', 'Wasm', 'All')][string]$Target = 'All',
	[string]$EmsdkPath = '',
	[string]$WasmToolchain = 'nightly-2026-10-18',
	[string]$OutputRoot = '',
	[string]$LogRoot = '',
	[string]$PlaywrightModule = '',
	[string]$BrowserExecutable = '',
	[ValidateRange(1, 7200)][int]$StageTimeoutSeconds = 600
)
$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
if (!$OutputRoot) { $OutputRoot = Join-Path $repoRoot 'artifacts/gameplay-api/fixture' }
if (!$LogRoot) { $LogRoot = Join-Path $repoRoot 'logs/gameplay-api/fixture' }
if (!$EmsdkPath) { $EmsdkPath = Join-Path $repoRoot '.temp/toolchains/emsdk-main' }
New-Item -ItemType Directory -Force -Path $OutputRoot, $LogRoot | Out-Null
$OutputRoot = (Resolve-Path $OutputRoot).Path
$LogRoot = (Resolve-Path $LogRoot).Path

function Invoke-Tool([string]$Executable, [string[]]$Arguments, [string]$LogName)
{
	$info = [Diagnostics.ProcessStartInfo]::new()
	$info.FileName = @(Get-Command $Executable -CommandType Application -ErrorAction Stop)[0].Source
	$info.WorkingDirectory = $repoRoot
	$info.UseShellExecute = $false
	$info.CreateNoWindow = $true
	$info.RedirectStandardOutput = $true
	$info.RedirectStandardError = $true
	foreach ($argument in $Arguments) { $info.ArgumentList.Add($argument) }
	$child = [Diagnostics.Process]::Start($info)
	$stdout = $child.StandardOutput.ReadToEndAsync()
	$stderr = $child.StandardError.ReadToEndAsync()
	$timedOut = !$child.WaitForExit($StageTimeoutSeconds * 1000)
	if ($timedOut) { $child.Kill($true); $child.WaitForExit() }
	$content = $stdout.GetAwaiter().GetResult() + $stderr.GetAwaiter().GetResult()
	[IO.File]::WriteAllText((Join-Path $LogRoot $LogName), $content, [Text.UTF8Encoding]::new($false))
	$code = $child.ExitCode
	$child.Dispose()
	if ($timedOut -or $code -ne 0)
	{
		Write-Host $content
		throw "$Executable failed (exit $code; timed out $timedOut): $LogRoot/$LogName"
	}
	Write-Host "$LogName passed"
}

$manifest = Join-Path $PSScriptRoot 'Cargo.toml'
$harness = Join-Path $PSScriptRoot 'GameplayAbiFixture.cpp'
$validation = Join-Path $repoRoot 'src/engine/scripting/GameplayAbiValidation.cpp'
$include = Join-Path $repoRoot 'src/engine'
if ($Target -in @('Native', 'All'))
{
	if (!$IsWindows) { throw 'Native fixture currently selects x86_64-pc-windows-msvc.' }
	$native = Join-Path $OutputRoot 'native'
	New-Item -ItemType Directory -Force $native | Out-Null
	Invoke-Tool 'cargo' @('build', '--manifest-path', $manifest, '--release', '--target', 'x86_64-pc-windows-msvc', '--target-dir', "$native/rust", '--locked') 'native-rust.log'
	Invoke-Tool 'clang++' @($harness, $validation, "$native/rust/x86_64-pc-windows-msvc/release/aether_gameplay_abi_fixture.lib", "-I$include", '-std=c++26', '-O2', '-o', "$native/gameplay-abi-fixture.exe", '-luserenv', '-lws2_32', '-lntdll', '-ldbghelp') 'native-link.log'
	Invoke-Tool "$native/gameplay-abi-fixture.exe" @() 'native-result.json'
}
if ($Target -in @('Wasm', 'All'))
{
	$quiet = $env:EMSDK_QUIET
	try { $env:EMSDK_QUIET = '1'; . (Join-Path $EmsdkPath 'emsdk_env.ps1') | Out-Null }
	finally { $env:EMSDK_QUIET = $quiet }
	$wasm = Join-Path $OutputRoot 'wasm'
	New-Item -ItemType Directory -Force $wasm | Out-Null
	Invoke-Tool 'cargo' @("+$WasmToolchain", 'build', '-Zbuild-std=std,panic_abort', '--manifest-path', $manifest, '--release', '--target', 'wasm32-unknown-emscripten', '--target-dir', "$wasm/rust", '--locked') 'wasm-rust.log'
	# Enable C++ JS exception catching to test production validator containment. Rust remains panic=abort.
	Invoke-Tool 'em++' @($harness, $validation, "$wasm/rust/wasm32-unknown-emscripten/release/libaether_gameplay_abi_fixture.a", "-I$include", '-std=c++26', '-O2', '-fexceptions', '-sALLOW_MEMORY_GROWTH=1', '-sASSERTIONS=2', '-sEXIT_RUNTIME=1', '-sENVIRONMENT=web', '--shell-file', "$PSScriptRoot/shell.html", '-o', "$wasm/index.html") 'wasm-link.log'
	$arguments = @("$PSScriptRoot/browser-runner.mjs", "--root=$wasm", "--report=$LogRoot/browser-result.json")
	if ($PlaywrightModule) { $arguments += "--playwright=$PlaywrightModule" }
	if ($BrowserExecutable) { $arguments += "--executable=$BrowserExecutable" }
	Invoke-Tool 'node' $arguments 'browser-runner.log'
}
