[CmdletBinding()]
param(
	[string]$EmsdkRoot,
	[string]$OutputDirectory,
	[string]$SlangCompiler = 'slangc'
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '../../../..')).Path
if (!$EmsdkRoot) { $EmsdkRoot = Join-Path $repoRoot '.temp/toolchains/emsdk-main' }
if (!$OutputDirectory) { $OutputDirectory = Join-Path $repoRoot 'artifacts/rust-webgpu/webgpu' }
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
$emCompiler = Join-Path $EmsdkRoot 'upstream/emscripten/em++.exe'
if (!(Test-Path -LiteralPath $emCompiler)) { $emCompiler = Join-Path $EmsdkRoot 'upstream/emscripten/em++.bat' }
if (!(Test-Path -LiteralPath $emCompiler)) { throw "Emscripten compiler missing: $emCompiler" }
if (!(Get-Command $SlangCompiler -ErrorAction SilentlyContinue)) { throw "Slang compiler missing: $SlangCompiler" }
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null

# Import the selected SDK environment into this process. No tool installation is performed.
$sdkEnvironment = & $env:ComSpec /d /s /c "`"`"$EmsdkRoot\emsdk_env.bat`" >nul 2>nul && set`""
if ($LASTEXITCODE -ne 0) { throw 'Failed to initialize Emscripten environment' }
foreach ($line in $sdkEnvironment)
{
	if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process') }
}

$wgslPath = Join-Path $OutputDirectory 'sprite.wgsl'
& $SlangCompiler (Join-Path $PSScriptRoot 'sprite.slang') -target wgsl -entry vertexMain -entry fragmentMain -o $wgslPath
if ($LASTEXITCODE -ne 0) { throw 'Slang-to-WGSL compilation failed' }
$wgsl = [IO.File]::ReadAllText($wgslPath)
if ($wgsl.Contains(')AETHER_WGSL"')) { throw 'Unexpected raw-string delimiter in generated shader' }
[IO.File]::WriteAllText((Join-Path $OutputDirectory 'SpriteShader.inc'), "constexpr char kSpriteShader[] = R`"AETHER_WGSL($wgsl)AETHER_WGSL`";`n", [Text.UTF8Encoding]::new($false))

$compilerArguments = @(
	(Join-Path $PSScriptRoot 'SpriteProbe.cpp'), '-std=c++26', '-O2', '-g2',
	'--use-port=emdawnwebgpu', '-sASSERTIONS=2', '-sALLOW_MEMORY_GROWTH=1',
	'-sEXIT_RUNTIME=0', '-sENVIRONMENT=web', '-I', $OutputDirectory,
	'-o', (Join-Path $OutputDirectory 'probe.js')
)
& $emCompiler @compilerArguments
if ($LASTEXITCODE -ne 0) { throw 'C++ WebGPU probe compilation failed' }
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'index.html') -Destination (Join-Path $OutputDirectory 'index.html') -Force
$metadata = [ordered]@{
	status = 'built'
	entry = (Join-Path $OutputDirectory 'index.html')
	emcc = ((& $emCompiler --version) | Select-Object -First 1)
	slang = ((& $SlangCompiler -version 2>&1) -join "`n")
	shaderSha256 = (Get-FileHash -LiteralPath $wgslPath -Algorithm SHA256).Hash
	flags = $compilerArguments
	artifacts = @(Get-ChildItem -LiteralPath $OutputDirectory -File | Where-Object Name -ne 'build-result.json' | ForEach-Object { @{ name = $_.Name; bytes = $_.Length } })
}
$metadata | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'build-result.json') -Encoding utf8
$metadata | ConvertTo-Json -Depth 5
