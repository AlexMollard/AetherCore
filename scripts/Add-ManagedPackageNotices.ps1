#requires -Version 7.4
[CmdletBinding()]
param([Parameter(Mandatory)][string]$PayloadDir, [string]$NugetPackages = (Join-Path ([Environment]::GetFolderPath('UserProfile')) '.nuget/packages'))
$ErrorActionPreference='Stop'
$root=(Resolve-Path -LiteralPath $PayloadDir).Path
$aggregate=Join-Path $root 'THIRD_PARTY_NOTICES.txt'
if (-not (Test-Path -LiteralPath $aggregate -PathType Leaf)) { throw 'Native notice inventory must be staged first' }
$deps=Get-Content -LiteralPath (Join-Path $root 'data/scripts/managed/AetherCore.Interop.deps.json') -Raw | ConvertFrom-Json -AsHashtable
$records=[Collections.Generic.List[object]]::new()
$text=[Text.StringBuilder]::new()
$null=$text.AppendLine("`n=== MANAGED PACKAGE NOTICE AUGMENTATION ===")
$null=$text.AppendLine('This inventory preserves shipped component notices; it is not a legal-clearance certification.')
foreach ($notice in @('LICENSE.txt','ThirdPartyNotices.txt')) {
    $source=Join-Path $root "dotnet/$notice"
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "Bundled runtime notice missing: $source" }
    $relative="notices/managed/dotnet/$notice"
    $destination=Join-Path $root $relative
    New-Item -ItemType Directory -Force (Split-Path $destination) | Out-Null
    Copy-Item -LiteralPath $source -Destination $destination
    $records.Add(@{component='bundled-dotnet';file=$relative;sha256=(Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash})
    $null=$text.AppendLine("`n--- Bundled .NET $notice ---")
    $null=$text.AppendLine((Get-Content -LiteralPath $source -Raw))
}
foreach ($id in @('Microsoft.CodeAnalysis.Common','Microsoft.CodeAnalysis.CSharp')) {
    $keys=@($deps.libraries.Keys | Where-Object { $_ -like "$id/*" })
    if ($keys.Count -ne 1) { throw "Expected one exact restored package identity for $id" }
    $version=$keys[0].Split('/')[1]
    $package=Join-Path $NugetPackages "$($id.ToLower())/$version"
    $metadata=Join-Path $package "$($id.ToLower()).nuspec"
    [xml]$nuspec=Get-Content -LiteralPath $metadata -Raw
    $license=$nuspec.package.metadata.license.InnerText
    if (-not $license) { throw "License metadata absent for $id/$version" }
    $dll=if ($id -eq 'Microsoft.CodeAnalysis.Common') { 'Microsoft.CodeAnalysis.dll' } else { 'Microsoft.CodeAnalysis.CSharp.dll' }
    $shippedHash=(Get-FileHash -LiteralPath (Join-Path $root "data/scripts/managed/$dll") -Algorithm SHA256).Hash
    $matching=@(Get-ChildItem -LiteralPath (Join-Path $package 'lib') -Recurse -Filter $dll -File | Where-Object { (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash -eq $shippedHash })
    if (-not $matching.Count) { throw "Shipped $dll does not match restored package $id/$version" }
    $relativeDir="notices/managed/$id/$version"
    New-Item -ItemType Directory -Force (Join-Path $root $relativeDir) | Out-Null
    foreach ($file in @($metadata, (Join-Path $package 'ThirdPartyNotices.rtf'))) {
        if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "Package notice missing: $file" }
        $relative="$relativeDir/$([IO.Path]::GetFileName($file))"
        Copy-Item -LiteralPath $file -Destination (Join-Path $root $relative)
        $records.Add(@{component="$id/$version";license=$license;file=$relative;sha256=(Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash;assemblySha256=$shippedHash})
    }
    $null=$text.AppendLine("`n$id/$version; license expression: $license; exact metadata and ThirdPartyNotices.rtf: $relativeDir")
}
$original=Get-Content -LiteralPath $aggregate -Raw
$original=($original -split '(?m)^=== MANAGED PACKAGE NOTICE AUGMENTATION ===')[0].TrimEnd()
Set-Content -LiteralPath $aggregate -Value ($original+"`n"+$text.ToString()) -Encoding utf8
@{schemaVersion=1;status='inventory-generated-not-legal-clearance';files=$records} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $root 'notices/managed-inventory.json') -Encoding utf8
Write-Host "Preserved $($records.Count) exact runtime/Roslyn notice sources."
