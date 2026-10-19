[CmdletBinding()]
param(
	[string]$EmsdkRoot = '',
	[string]$Box2dSource = '',
	[string]$PlaywrightModule = $env:AETHER_PLAYWRIGHT_MODULE,
	[string]$BrowserExecutable = '',
	[string]$OutputRoot = '',
	[string]$ReportRoot = '',
	[ValidateRange(1, 7200)][int]$StageTimeoutSeconds = 600
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
if (!$EmsdkRoot) { $EmsdkRoot = Join-Path $repoRoot '.temp/toolchains/emsdk-main' }
if (!$Box2dSource) { $Box2dSource = Join-Path $repoRoot 'build/default/_deps/box2d-src' }
if (!$OutputRoot) { $OutputRoot = Join-Path $repoRoot 'artifacts/rust-webgpu' }
if (!$ReportRoot) { $ReportRoot = Join-Path $repoRoot 'logs/rust-webgpu' }
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
$ReportRoot = [IO.Path]::GetFullPath($ReportRoot)
New-Item -ItemType Directory -Force -Path $OutputRoot, $ReportRoot | Out-Null
$pins = Get-Content (Join-Path $PSScriptRoot 'toolchain.json') -Raw | ConvertFrom-Json
$report = [ordered]@{ schemaVersion = 1; status = 'failed'; phase = 'feasibility'; startedAt = [DateTimeOffset]::UtcNow.ToString('o'); pins = $pins; stages = @(); artifacts = @() }

function Invoke-Stage([string]$Name, [string]$Executable, [string[]]$Arguments)
{
	$timer = [Diagnostics.Stopwatch]::StartNew()
	$logPath = Join-Path $ReportRoot "$Name.log"
	$stage = [ordered]@{ name = $Name; status = 'failed'; log = $logPath }
	$report.stages += $stage
	$startInfo = [Diagnostics.ProcessStartInfo]::new()
	$startInfo.FileName = @(Get-Command $Executable -CommandType Application -ErrorAction Stop)[0].Source
	$startInfo.WorkingDirectory = $repoRoot
	$startInfo.UseShellExecute = $false
	$startInfo.CreateNoWindow = $true
	$startInfo.RedirectStandardOutput = $true
	$startInfo.RedirectStandardError = $true
	foreach ($argument in $Arguments) { $startInfo.ArgumentList.Add($argument) }
	$child = [Diagnostics.Process]::Start($startInfo)
	$stage.processId = $child.Id
	$stdout = $child.StandardOutput.ReadToEndAsync()
	$stderr = $child.StandardError.ReadToEndAsync()
	if (!$child.WaitForExit($StageTimeoutSeconds * 1000))
	{
		$stage.timedOut = $true
		$child.Kill($true)
		$child.WaitForExit()
	}
	[IO.File]::WriteAllText($logPath, $stdout.GetAwaiter().GetResult() + $stderr.GetAwaiter().GetResult(), [Text.UTF8Encoding]::new($false))
	$stage.exitCode = $child.ExitCode
	$stage.durationMs = $timer.ElapsedMilliseconds
	$child.Dispose()
	if ($stage.timedOut -or $stage.exitCode -ne 0)
	{
		Get-Content $logPath -Tail 15 | Write-Host
		if ($stage.timedOut) { throw "$Name timed out after $StageTimeoutSeconds seconds; child process tree terminated; see $logPath" }
		throw "$Name failed; see $logPath"
	}
	$stage.status = 'passed'
	Write-Host "$Name passed ($($stage.durationMs) ms)"
}

try
{
	$emCompiler = Join-Path $EmsdkRoot 'upstream/emscripten/em++.exe'
	if (!(Test-Path $emCompiler)) { $emCompiler = Join-Path $EmsdkRoot 'upstream/emscripten/em++.bat' }
	if (!(Test-Path $emCompiler)) { throw 'Install the pinned Emscripten SDK first; this script does not install toolchains.' }
	if (!(Test-Path (Join-Path $Box2dSource 'CMakeLists.txt'))) { throw 'Box2D source missing; specify -Box2dSource.' }
	$report.actualTools = [ordered]@{
		rustNative = (& rustc --version) -join ''
		cargo = (& cargo --version) -join ''
		clangNative = ((& clang++ --version) | Select-Object -First 1)
		cmake = ((& cmake --version) | Select-Object -First 1)
		rustWasm = (& rustc "+$($pins.rustWasm)" --version) -join ''
		emcc = ((& $emCompiler --version) | Select-Object -First 1)
		slang = (& slangc -version 2>&1) -join ''
	}
	if ($report.actualTools.rustNative -notmatch ('^rustc ' + [regex]::Escape($pins.rustNative) + ' ') -or
	    $report.actualTools.emcc -notmatch [regex]::Escape($pins.emscripten) -or
	    $report.actualTools.slang -notmatch [regex]::Escape($pins.slang) -or
	    $report.actualTools.rustWasm -notmatch [regex]::Escape($pins.rustWasmVersion))
	{
		throw 'Toolchain differs from the tested pins; inspect toolchain.json and record a new qualified combination explicitly.'
	}
	Invoke-Stage 'runner-checks' 'node' @('--test', (Join-Path $PSScriptRoot 'browser-checks.test.mjs'))
	Invoke-Stage 'interop' 'pwsh' @('-NoProfile', '-File', (Join-Path $PSScriptRoot 'interop/Run-InteropProbe.ps1'), '-Target', 'All', '-EmsdkPath', $EmsdkRoot, '-WasmToolchain', $pins.rustWasm, '-OutputRoot', (Join-Path $OutputRoot 'interop'), '-LogRoot', (Join-Path $ReportRoot 'interop'))
	Invoke-Stage 'cpp-box2d' 'pwsh' @('-NoProfile', '-File', (Join-Path $PSScriptRoot 'cpp/build.ps1'), '-EmsdkRoot', $EmsdkRoot, '-Box2dSource', $Box2dSource, '-OutputDirectory', (Join-Path $OutputRoot 'cpp'))
	Invoke-Stage 'webgpu-build' 'pwsh' @('-NoProfile', '-File', (Join-Path $PSScriptRoot 'webgpu/build.ps1'), '-EmsdkRoot', $EmsdkRoot, '-OutputDirectory', (Join-Path $OutputRoot 'webgpu'))
	$browserReport = Join-Path $ReportRoot 'browser-report.json'
	$browserArguments = @((Join-Path $PSScriptRoot 'browser-runner.mjs'), "--root=$OutputRoot", "--report=$browserReport")
	if ($PlaywrightModule) { $browserArguments += "--playwright=$PlaywrightModule" }
	if ($BrowserExecutable) { $browserArguments += "--executable=$BrowserExecutable" }
	Invoke-Stage 'browser-tests' 'node' $browserArguments
	$report.browser = Get-Content $browserReport -Raw | ConvertFrom-Json
	if ($report.browser.status -ne 'passed' -or $report.browser.cases.Count -ne 5) { throw 'The complete five-case browser suite must pass.' }
	if ($report.browser.playwrightVersion -ne $pins.playwright) { throw 'Playwright version differs from the tested pin.' }
	$report.native = Get-Content (Join-Path $ReportRoot 'interop/native-result.json') -Raw | ConvertFrom-Json
	$report.webgpuBuild = Get-Content (Join-Path $OutputRoot 'webgpu/build-result.json') -Raw | ConvertFrom-Json
	$report.artifacts = @(Get-ChildItem $OutputRoot -File -Recurse | Where-Object { $_.Extension -in @('.wasm', '.js', '.html', '.wgsl') -and $_.FullName -notmatch '[\\/]rust[\\/]|[\\/]CMakeFiles[\\/]' } | ForEach-Object { [ordered]@{ path = [IO.Path]::GetRelativePath($OutputRoot, $_.FullName); bytes = $_.Length; sha256 = (Get-FileHash $_.FullName -Algorithm SHA256).Hash } })
	$report.status = 'passed'
}
catch
{
	$report.error = $_.Exception.Message
	Write-Host $report.error
}
finally
{
	$report.finishedAt = [DateTimeOffset]::UtcNow.ToString('o')
	$reportPath = Join-Path $ReportRoot 'feasibility-report.json'
	$report | ConvertTo-Json -Depth 18 | Set-Content -LiteralPath $reportPath -Encoding utf8
	Write-Host "Feasibility $($report.status): $reportPath"
}
if ($report.status -ne 'passed') { exit 1 }
