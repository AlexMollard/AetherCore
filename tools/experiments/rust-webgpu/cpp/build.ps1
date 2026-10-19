[CmdletBinding()]
param(
	[Parameter(Mandatory)][string]$EmsdkRoot,
	[Parameter(Mandatory)][string]$Box2dSource,
	[string]$OutputDirectory = ''
)
$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '../../../..')).Path
if (!$OutputDirectory) { $OutputDirectory = Join-Path $repoRoot 'artifacts/rust-webgpu/cpp' }
. (Join-Path $EmsdkRoot 'emsdk_env.ps1') | Out-Null
& emcmake cmake -S $PSScriptRoot -B $OutputDirectory -G Ninja "-DAETHER_BOX2D_SOURCE=$Box2dSource" -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) { throw 'Browser C++/Box2D probe configure failed' }
& cmake --build $OutputDirectory --parallel 4
if ($LASTEXITCODE -ne 0) { throw 'Browser C++/Box2D probe build failed' }
