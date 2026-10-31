[CmdletBinding()]
param([Parameter(Mandatory)][string]$BuildDir, [string]$Config = 'Debug', [Parameter(Mandatory)][string]$ReportPath)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/Qualification.Common.ps1"
$report = [ordered]@{ schemaVersion = 1; status = 'fail'; expected = @('EngineTests','GameplayApiParity','GameplayApiGeneratorTests','ManagedTests'); suites = @(); testCases = 0; passed = 0; failed = 0 }
try {
    foreach ($tool in @('ctest','dotnet')) { if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) { throw "Missing prerequisite: $tool" } }
    $inventoryText = & ctest --test-dir $BuildDir -C $Config --show-only=json-v1 2>&1
    if ($LASTEXITCODE -ne 0) { throw "CTest inventory failed: $inventoryText" }
    $inventory = ($inventoryText -join "`n") | ConvertFrom-Json
    Assert-TestInventory $inventory
    $report.suites = @($inventory.tests | ForEach-Object { $_.name })
    $nativeCommand=@($inventory.tests | Where-Object name -eq 'EngineTests')[0].command
    $nativeOutput=@($nativeCommand | Where-Object { $_ -like '--out=*' })
    if (@($nativeCommand | Where-Object { $_ -eq '--reporters=xml' }).Count -ne 1 -or $nativeOutput.Count -ne 1) { throw 'EngineTests must declare an isolated XML reporter output; regenerate this build configuration' }
    $nativePath=[IO.Path]::GetFullPath($nativeOutput[0].Substring(6))
    $buildRoot=[IO.Path]::GetFullPath($BuildDir).TrimEnd('\','/')+[IO.Path]::DirectorySeparatorChar
    if (-not $nativePath.StartsWith($buildRoot,[StringComparison]::OrdinalIgnoreCase)) { throw 'Native report output must stay inside the selected build directory' }
    $junit = [IO.Path]::ChangeExtension([IO.Path]::GetFullPath($ReportPath), '.xml')
    New-Item -ItemType Directory -Force (Split-Path $junit) | Out-Null
    if ($nativePath -eq $junit) { throw 'Native and CTest reports must use distinct files' }
    foreach ($previous in @($nativePath,$junit)) { if (Test-Path -LiteralPath $previous) { Remove-Item -LiteralPath $previous -ErrorAction Stop } }
    $started=[datetime]::UtcNow
    $output = & ctest --test-dir $BuildDir -C $Config --verbose --timeout 900 --no-tests=error --output-junit $junit 2>&1
    $testExit = $LASTEXITCODE
    $report.ctestExitCode = $testExit
    $output | Set-Content "$ReportPath.log" -Encoding utf8
    # Capture the native result before any failing/skipped suite can throw. A
    # failing qualification must still retain how many native cases ran/failed.
    $summary = Read-NativeTestReport $nativePath $started $nativeCommand[0]
    $report.testCases = $summary.testCases; $report.passed = $summary.passed; $report.failed = $summary.failed
    $report.assertions=$summary.assertions; $report.assertionsFailed=$summary.assertionsFailed
    $report.nativeReport=@{path=$summary.path;sha256=$summary.sha256}
    [xml]$results = Get-Content -LiteralPath $junit -Raw
    $cases = @($results.testsuite.testcase)
    foreach ($name in $report.suites) {
        $suite = @($cases | Where-Object { $_.name -eq $name })
        if ($suite.Count -ne 1 -or $suite[0].status -ne 'run' -or $suite[0].failure -or $suite[0].skipped) { throw "Suite failed, omitted or skipped: $name" }
    }
    if ($report.passed + $report.failed -ne $report.testCases) { throw 'Native doctest case summary counts are inconsistent' }
    if ($testExit -ne 0 -or $report.testCases -eq 0 -or $report.failed -ne 0 -or $report.assertionsFailed -ne 0) { throw "CTest failed (exit $testExit)" }
    $report.status = 'pass'
} catch { $report.detail = $_.Exception.Message }
New-Item -ItemType Directory -Force (Split-Path ([IO.Path]::GetFullPath($ReportPath))) | Out-Null
$report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $ReportPath -Encoding utf8
if ($report.status -ne 'pass') { Write-Error $report.detail -ErrorAction Continue; exit 1 }
Write-Host "All $($report.suites.Count) CTest suites passed; $($report.passed) native cases."
exit 0
