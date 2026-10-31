#requires -Version 7.4
[CmdletBinding()]
param([Parameter(Mandatory)][string]$SourceRoot, [Parameter(Mandatory)][ValidatePattern('^\d+\.\d+\.\d+([-.][a-zA-Z0-9.]+)?$')][string]$Version, [Parameter(Mandatory)][string]$PayloadDir)
$ErrorActionPreference='Stop'
$source=(Resolve-Path -LiteralPath $SourceRoot).Path
$payload=(Resolve-Path -LiteralPath $PayloadDir).Path
$destination=Join-Path $payload 'dotnet'
if (Test-Path -LiteralPath $destination) { throw 'dotnet destination must not exist; refusing to mix runtime versions or SDK files' }
$hostName=if ($env:OS -eq 'Windows_NT') { 'dotnet.exe' } else { 'dotnet' }
$inputs=@($hostName,'LICENSE.txt','ThirdPartyNotices.txt',"host/fxr/$Version","shared/Microsoft.NETCore.App/$Version")
foreach ($relative in $inputs) { if (-not (Test-Path -LiteralPath (Join-Path $source $relative))) { throw "Runtime source incomplete: $relative" } }
New-Item -ItemType Directory -Force $destination | Out-Null
foreach ($relative in $inputs) {
    $target=Join-Path $destination $relative
    New-Item -ItemType Directory -Force (Split-Path $target) | Out-Null
    Copy-Item -LiteralPath (Join-Path $source $relative) -Destination $target -Recurse
}
@{schemaVersion=1;source=$source;runtimeVersion=$Version;kind='copied-runtime-only-no-sdk';files=@(Get-ChildItem -LiteralPath $destination -Recurse -File | ForEach-Object { @{path=$_.FullName.Substring($destination.Length+1);sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash} })} |
    ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $payload 'dotnet-runtime-source.json') -Encoding utf8
Write-Host "Staged runtime-only .NET $Version from $source."
