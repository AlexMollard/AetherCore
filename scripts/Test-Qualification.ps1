#requires -Version 7.4
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/Qualification.Common.ps1"
$script:checks = 0
function Check([bool]$Condition, [string]$Message) { if (-not $Condition) { throw $Message }; $script:checks++ }
function Must-Throw([scriptblock]$Action, [string]$Message) {
    $thrown = $false
    try { & $Action } catch { $thrown = $true }
    Check $thrown $Message
}
$names = @('EngineTests','GameplayApiParity','GameplayApiGeneratorTests','ManagedTests')
$splitSummary=Get-NativeTestSummary @("1: $([char]27)[32m[doctest] test cases:$([char]27)[0m",'1: 1700 | 1700 passed | 0 failed | 8 skipped')
Check ($splitSummary.testCases -eq 1700 -and $splitSummary.passed -eq 1700 -and $splitSummary.failed -eq 0) 'Split ANSI/CTest native summary lost counts'
Check ($null -eq (Get-NativeTestSummary @('1: [doctest] test cases:','1: 1700 | 1700 passed |'))) 'Truncated native summary accepted'
Check ($null -eq (Get-NativeTestSummary @('all tests passed'))) 'Missing native summary accepted'
Assert-TestInventory @{ tests = @($names | ForEach-Object { @{ name = $_ } }) }
foreach ($missing in $names) {
    Must-Throw { Assert-TestInventory @{ tests = @($names | Where-Object { $_ -ne $missing } | ForEach-Object { @{ name = $_ } }) } } "Missing $missing accepted"
}
Check ((Get-QualificationStatus @(@{name='unit';status='skip'}) @('unit')) -eq 'INCOMPLETE') 'Skipped phase accepted'
Check ((Get-QualificationStatus @() @('unit')) -eq 'INCOMPLETE') 'Omitted phase accepted'
Check ((Get-QualificationStatus @(@{name='unit';status='pass'}) @('unit')) -eq 'PASS') 'Completed phase rejected'
Must-Throw { Assert-ShutdownLog @('startup complete') } 'Absent shutdown marker accepted'
Must-Throw { Assert-ShutdownLog @('Application shutdown complete.', 'ERRO: GPU teardown failure') } 'Teardown error accepted'
Assert-ShutdownLog @('Application shutdown complete.')
$loaderWarnings=@(
    'WARN Validation: General: Removing layer VK_LAYER_AMD_switchable_graphics because it is a duplicate of VK_LAYER_AMD_switchable_graphics',
    'WARN Validation: General: Layer VK_LAYER_OBS_HOOK uses API version 1.3 which is older than the application specified API version of 1.4.',
    'WARN Validation: General: Layer VK_LAYER_TEST forced disabled because name matches filter of env var VK_LOADER_LAYERS_DISABLE'
)
Assert-ShutdownLog @($loaderWarnings + 'Application shutdown complete.')
Check ((Measure-ValidationFindings $loaderWarnings).Failing -eq 0) 'Known loader warnings classified as application validation findings'
foreach ($diagnostic in @(
    'WARN Validation: VUID-vkQueueSubmit-pCommandBuffers-00072: invalid synchronization',
    'ERRO Validation: VUID-vkDestroyDevice-device-05137: live child resource',
    'INFO Validation: Best practices: redundant synchronization',
    'WARN Validation: General: Layer VK_LAYER_TEST uses API version 1.0 older than the application specified API version; VUID-vkQueueSubmit-test-12345',
    'ERRO App: teardown failed', 'FATAL App: allocator corruption during shutdown'
)) {
    Must-Throw { Assert-ShutdownLog @('Application shutdown complete.', $diagnostic) } "Teardown diagnostic accepted: $diagnostic"
}

# Process doubles exercise timeout/forced-cleanup semantics without launching GPU apps.
function Fake-Process([bool]$Exited, [bool]$CloseAccepted, [bool]$ExitsOnWait, [int]$ExitCode = 0) {
    $fake = [pscustomobject]@{ HasExited=$Exited; Id=123; ExitCode=$ExitCode; closeAccepted=$CloseAccepted; exitsOnWait=$ExitsOnWait; killed=$false }
    $fake | Add-Member ScriptMethod CloseMainWindow { return $this.closeAccepted }
    $fake | Add-Member ScriptMethod WaitForExit { param($Timeout); if ($this.exitsOnWait) { $this.HasExited=$true }; return $this.HasExited }
    $fake | Add-Member ScriptMethod Kill { $this.killed=$true; $this.HasExited=$true }
    return $fake
}
$fake = Fake-Process $true $false $false
Check ((Stop-QualifiedProcess $fake).status -eq 'fail') 'Early exit accepted'
Must-Throw { Assert-QualifiedProcessAlive $null 'readiness fixture' } 'Missing owned readiness process accepted'
Assert-QualifiedProcessAlive (Fake-Process $false $false $false) 'readiness fixture'
$startupFailure = Fake-Process $true $false $false -1073741515
try { Assert-QualifiedProcessAlive $startupFailure 'readiness fixture'; throw 'Exited readiness process accepted' }
catch {
    Check ($_.Exception.Data['ExitCode'] -eq -1073741515) 'Readiness exit code lost'
    Check ($_.Exception.Data['ExitCodeHex'] -eq '0xC0000135') 'Readiness NTSTATUS hex lost'
    Check ($_.Exception.Message -match 'readiness fixture') 'Readiness failure context lost'
}
$fake = Fake-Process $false $true $false
$result = Stop-QualifiedProcess $fake '' 0 1
Check ($result.status -eq 'fail' -and $result.forced -and $fake.killed) 'Forced termination accepted'
if ($env:OS -eq 'Windows_NT') {
    $fake = Fake-Process $false $true $true
    Check ((Stop-QualifiedProcess $fake).status -eq 'pass') 'Graceful zero exit rejected'
    $fake = Fake-Process $false $true $true 5
    Check ((Stop-QualifiedProcess $fake).status -eq 'fail') 'Nonzero shutdown accepted'
}
Must-Throw { Invoke-QualifiedControl 'does-not-exist-qualification-client' 12345 info } 'Missing control tool accepted'

# Exercise the real shutdown helper against a refused close: its evidence must
# retain the application's unsaved prompt rather than imply an engine deadlock.
$controlImplementation=${function:Invoke-QualifiedControl}
try {
    function Invoke-QualifiedControl($Client,$Port,$Method,$Parameters,$TimeoutMs) {
        if ($Method -eq 'info') { return @{pid=123} }
        return @{requested=$true;unsaved=$true}
    }
    $fake=Fake-Process $false $true $false
    $refused=Stop-QualifiedProcess $fake 'fixture-client' 12345 0
    Check ($refused.status -eq 'fail' -and $refused.forced -and $refused.quitResponse.unsaved) 'Unsaved-close response lost or refused close accepted'
} finally { ${function:Invoke-QualifiedControl}=$controlImplementation }

# Load only the product save function's AST, never execute its application flow.
$productAst=[Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot 'Run-ProductQualification.ps1'),[ref]$null,[ref]$null)
$saveAst=$productAst.Find({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Save-CurrentScene'},$true)
. ([scriptblock]::Create($saveAst.Extent.Text))
try {
    function Call($Method) { if ($Method -eq 'scene.save') { return @{saved=$script:saveAcknowledged} }; return $script:saveUndo }
    $script:saveAcknowledged=$true; $script:saveUndo=@{unsavedChanges=$false}
    $saveEvidence=Save-CurrentScene
    Check ($saveEvidence.save.saved -and $saveEvidence.undo.unsavedChanges -eq $false) 'Clean acknowledged save rejected'
    $script:saveUndo=@{unsavedChanges=$true}
    Must-Throw { Save-CurrentScene } 'Dirty edit baseline after acknowledged save accepted'
    $script:saveUndo=@{}
    Must-Throw { Save-CurrentScene } 'Missing edit baseline after save accepted'
    $script:saveAcknowledged=$false
    Must-Throw { Save-CurrentScene } 'Unacknowledged save accepted'
} finally { Remove-Item function:Call; Remove-Item function:Save-CurrentScene }

# Real, non-GPU children verify discovered handles retain both zero and nonzero
# exit statuses. A null status must never become a successful zero exit.
foreach ($expectedExit in @(0,7)) {
    $start=[Diagnostics.ProcessStartInfo]::new()
    $start.FileName=(Get-Command pwsh).Source
    $start.UseShellExecute=$false; $start.CreateNoWindow=$true
    foreach ($arg in @('-NoProfile','-Command',"Start-Sleep -Seconds 2; exit $expectedExit")) { $start.ArgumentList.Add($arg) }
    $child=[Diagnostics.Process]::Start($start)
    $discovered=Get-Process -Id $child.Id
    try {
        Retain-QualifiedProcessHandle $discovered
        Check ($discovered.WaitForExit(10000)) 'Tiny discovered child exceeded exit deadline'
        Check ($null -ne $discovered.ExitCode -and $discovered.ExitCode -eq $expectedExit) "Discovered process lost actual exit code $expectedExit"
    } finally {
        if (-not $child.HasExited) { $child.Kill(); $child.WaitForExit() }
        $discovered.Dispose(); $child.Dispose()
    }
}
$fake=Fake-Process $false $true $true
$fake.ExitCode=$null
Check ((Stop-QualifiedProcess $fake).status -ne 'pass') 'Unknown exit code accepted as zero'

# CTest integration fixtures are synthetic harness tests, never production-suite evidence.
$scratch = Join-Path $PSScriptRoot "../.temp/qualification-tests-$([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Force $scratch | Out-Null
$immutableFixture=Join-Path $scratch 'immutable-bundle'
New-Item -ItemType Directory $immutableFixture | Out-Null
Set-Content -LiteralPath "$immutableFixture/original.bin" -Value 'original'
$before=Get-QualifiedBundleInventory $immutableFixture
Assert-QualifiedBundleUnchanged $before (Get-QualifiedBundleInventory $immutableFixture)
Check ($before.files.Count -eq 1 -and $before.inventorySha256.Length -eq 64) 'Immutable baseline lacks retained file hashes'
Set-Content -LiteralPath "$immutableFixture/original.bin" -Value 'modified'
Must-Throw { Assert-QualifiedBundleUnchanged $before (Get-QualifiedBundleInventory $immutableFixture) } 'Same-size runtime bundle mutation accepted'
Set-Content -LiteralPath "$immutableFixture/original.bin" -Value 'original'
Set-Content -LiteralPath "$immutableFixture/.added.ini" -Value 'runtime write'
Must-Throw { Assert-QualifiedBundleUnchanged $before (Get-QualifiedBundleInventory $immutableFixture) } 'Added runtime bundle file accepted'
Remove-Item -LiteralPath "$immutableFixture/.added.ini"
Set-Content -LiteralPath "$immutableFixture/remaining.bin" -Value 'keep inventory nonempty'
$beforeDeletion=Get-QualifiedBundleInventory $immutableFixture
Remove-Item -LiteralPath "$immutableFixture/original.bin"
Must-Throw { Assert-QualifiedBundleUnchanged $beforeDeletion (Get-QualifiedBundleInventory $immutableFixture) } 'Deleted runtime bundle file accepted'
Must-Throw { Assert-QualifiedOutputOutsideBundle $immutableFixture "$immutableFixture/state" } 'State inside immutable bundle accepted'
Assert-QualifiedOutputOutsideBundle $immutableFixture "$immutableFixture-sibling/state"
$emptyProfileCheck=Join-Path $scratch 'empty-profiles.ps1'
$lifecycleScript=(Join-Path $PSScriptRoot 'Run-LifecycleQualification.ps1').Replace("'","''")
$scratchLiteral=$scratch.Replace("'","''")
$unmarkedReport=Join-Path $scratch 'unmarked.json'
& pwsh -NoProfile -File $lifecycleScript -ProductDir $scratch -StateDir "$scratch/unmarked-state" -ReportPath $unmarkedReport -Profiles Runtime -Cycles 1
Check ($LASTEXITCODE -ne 0) 'Unmarked development payload accepted for delivered lifecycle'
$unmarked=Get-Content -LiteralPath $unmarkedReport -Raw | ConvertFrom-Json
Check ($unmarked.detail -match 'delivered product marker' -and $unmarked.phases.Count -eq 0 -and -not (Test-Path "$scratch/unmarked-state")) 'Unmarked lifecycle attempted a process or created misleading run evidence'
Set-Content -LiteralPath $emptyProfileCheck -Value "`$ErrorActionPreference='Stop'; & '$lifecycleScript' -ProductDir '$scratchLiteral' -StateDir '$scratchLiteral/unused-state' -ReportPath '$scratchLiteral/empty-profiles.json' -Profiles @(); exit `$LASTEXITCODE"
& pwsh -NoProfile -File $emptyProfileCheck
Check ($LASTEXITCODE -ne 0) 'Empty lifecycle profile selection accepted'
foreach ($case in @(@{name='duplicate';arguments="-Profiles @('Full','Full')"},@{name='zero-cycles';arguments='-Profiles Full -Cycles 0'})) {
    $wrapper=Join-Path $scratch "$($case.name).ps1"
    $rejectedReport=Join-Path $scratch "$($case.name).json"
    Set-Content -LiteralPath $wrapper -Value "`$ErrorActionPreference='Stop'; & '$lifecycleScript' -ProductDir '$scratchLiteral' -StateDir '$scratchLiteral/$($case.name)-state' -ReportPath '$scratchLiteral/$($case.name).json' $($case.arguments); exit `$LASTEXITCODE"
    & pwsh -NoProfile -File $wrapper
    Check ($LASTEXITCODE -ne 0) "Invalid lifecycle budget accepted: $($case.name)"
    if (Test-Path -LiteralPath $rejectedReport) {
        Check ((Get-Content -LiteralPath $rejectedReport -Raw | ConvertFrom-Json).status -ne 'pass') 'Zero-execution lifecycle report passed'
    }
}
$pwsh = (Get-Command pwsh).Source.Replace('\','/')
$fixture = Join-Path $scratch 'fixture.ps1'
$nativeFixturePath=(Join-Path $scratch 'EngineTests-Debug.doctest.xml').Replace('\','/')
function Set-NativeFixture([int]$Passed=3,[int]$Failed=0,[string]$Mode='valid') {
    $binary=$pwsh
    if ($env:OS -eq 'Windows_NT' -and $binary.EndsWith('.exe',[StringComparison]::OrdinalIgnoreCase)) { $binary=$binary.Substring(0,$binary.Length-4) }
    if ($Mode -eq 'wrong-binary') { $binary=$binary.Replace('/pwsh','/different-directory/pwsh') }
    if ($Mode -eq 'basename-only') { $binary=[IO.Path]::GetFileName($binary) }
    $binary=[Security.SecurityElement]::Escape($binary)
    $xml="<doctest binary='$binary'><OverallResultsTestCases successes='$Passed' failures='$Failed'/><OverallResultsAsserts successes='9' failures='$Failed'/></doctest>"
    if ($Mode -eq 'malformed') { $xml='<doctest>' }
    $body=@'
$outArg=@($args | Where-Object { $_ -like '--out=*' })
Write-Output '[doctest] test cases: 999 | )999 passed | 0 failed'
if ($outArg.Count) {
    $path='REPORTPATH'
    if ('MODE' -ne 'omitted') {
        Set-Content -LiteralPath $path -Value "XML"
        if ('MODE' -eq 'stale') { (Get-Item -LiteralPath $path).LastWriteTimeUtc=[datetime]::UtcNow.AddDays(-1) }
    }
}
exit EXIT
'@
    $body=$body.Replace('XML',$xml).Replace('MODE',$Mode).Replace('REPORTPATH',$nativeFixturePath).Replace('EXIT',$(if ($Failed) {'1'} else {'0'}))
    Set-Content -LiteralPath $fixture $body
}
Set-NativeFixture
$fixtureCmake = $fixture.Replace('\','/')
$testFile = @($names | ForEach-Object {
    $reportArgs=if ($_ -eq 'EngineTests') { " --reporters=xml `"--out=$nativeFixturePath`"" } else { '' }
    "add_test($_ `"$pwsh`" -NoProfile -File `"$fixtureCmake`"$reportArgs)"
})
Set-Content (Join-Path $scratch 'CTestTestfile.cmake') $testFile
& pwsh -NoProfile -File "$PSScriptRoot/Invoke-CTestQualification.ps1" -BuildDir $scratch -ReportPath "$scratch/report.json"
Check ($LASTEXITCODE -eq 0) 'Isolated native XML report rejected despite corrupted console'
$fixtureReport = Get-Content "$scratch/report.json" -Raw | ConvertFrom-Json
Check ($fixtureReport.testCases -eq 3 -and $fixtureReport.assertions -eq 9 -and $fixtureReport.suites.Count -eq 4 -and $fixtureReport.nativeReport.sha256) 'Native report case/assertion counts or identity lost'
foreach ($mode in @('omitted','malformed','stale','wrong-binary','basename-only')) {
    Set-Content -LiteralPath $nativeFixturePath "<doctest binary='EngineTests'><OverallResultsTestCases successes='3' failures='0'/><OverallResultsAsserts successes='9' failures='0'/></doctest>"
    Set-NativeFixture -Mode $mode
    & pwsh -NoProfile -File "$PSScriptRoot/Invoke-CTestQualification.ps1" -BuildDir $scratch -ReportPath "$scratch/$mode-native.json"
    Check ($LASTEXITCODE -ne 0) "Invalid native XML report accepted: $mode"
}
Set-NativeFixture -Passed 0
& pwsh -NoProfile -File "$PSScriptRoot/Invoke-CTestQualification.ps1" -BuildDir $scratch -ReportPath "$scratch/zero-native.json"
Check ($LASTEXITCODE -ne 0) 'Zero native report cases accepted'
Set-NativeFixture
Add-Content (Join-Path $scratch 'CTestTestfile.cmake') 'set_tests_properties(ManagedTests PROPERTIES DISABLED TRUE)'
& pwsh -NoProfile -File "$PSScriptRoot/Invoke-CTestQualification.ps1" -BuildDir $scratch -ReportPath "$scratch/skipped.json"
Check ($LASTEXITCODE -ne 0) 'Disabled registered suite accepted'
$disabledResult=Get-Content "$scratch/skipped.json" -Raw | ConvertFrom-Json
Check ($disabledResult.testCases -eq 3 -and $disabledResult.passed -eq 3) 'Native counts lost when another suite was disabled'
Set-NativeFixture -Passed 2 -Failed 1
Set-Content (Join-Path $scratch 'CTestTestfile.cmake') $testFile
& pwsh -NoProfile -File "$PSScriptRoot/Invoke-CTestQualification.ps1" -BuildDir $scratch -ReportPath "$scratch/failed-native.json"
Check ($LASTEXITCODE -ne 0) 'Failed native suite accepted'
$failedNative=Get-Content "$scratch/failed-native.json" -Raw | ConvertFrom-Json
Check ($failedNative.testCases -eq 3 -and $failedNative.passed -eq 2 -and $failedNative.failed -eq 1) 'Failed native counts lost'
Set-Content (Join-Path $scratch 'CTestTestfile.cmake') ($testFile | Select-Object -First 3)
& pwsh -NoProfile -File "$PSScriptRoot/Invoke-CTestQualification.ps1" -BuildDir $scratch -ReportPath "$scratch/omitted.json"
Check ($LASTEXITCODE -ne 0) 'Omitted suite accepted'
$payload=Join-Path $scratch 'payload'
$packages=Join-Path $scratch 'packages'
New-Item -ItemType Directory -Force "$payload/dotnet", "$payload/data/scripts/managed" | Out-Null
Set-Content "$payload/THIRD_PARTY_NOTICES.txt" 'Synthetic native notice fixture'
Set-Content "$payload/dotnet/LICENSE.txt" 'Synthetic runtime license fixture'
Set-Content "$payload/dotnet/ThirdPartyNotices.txt" 'Synthetic runtime notice fixture'
Set-Content "$payload/data/scripts/managed/AetherCore.Interop.deps.json" '{"libraries":{"Microsoft.CodeAnalysis.Common/1.0":{"type":"package"},"Microsoft.CodeAnalysis.CSharp/1.0":{"type":"package"}}}'
foreach ($id in @('Microsoft.CodeAnalysis.Common','Microsoft.CodeAnalysis.CSharp')) {
    $package=Join-Path $packages "$($id.ToLower())/1.0"
    New-Item -ItemType Directory -Force "$package/lib/net10.0" | Out-Null
    Set-Content "$package/$($id.ToLower()).nuspec" '<package><metadata><license type="expression">MIT</license></metadata></package>'
    Set-Content "$package/ThirdPartyNotices.rtf" 'Synthetic notice fixture'
    $dll=if ($id -like '*.Common') { 'Microsoft.CodeAnalysis.dll' } else { 'Microsoft.CodeAnalysis.CSharp.dll' }
    Set-Content "$package/lib/net10.0/$dll" 'Synthetic DLL identity fixture'
    Copy-Item "$package/lib/net10.0/$dll" "$payload/data/scripts/managed/$dll"
}
& pwsh -NoProfile -File "$PSScriptRoot/Add-ManagedPackageNotices.ps1" -PayloadDir $payload -NugetPackages $packages
Check ($LASTEXITCODE -eq 0) 'Exact managed notice fixture rejected'
Set-Content "$payload/data/scripts/managed/Microsoft.CodeAnalysis.dll" 'Mismatched DLL identity fixture'
& pwsh -NoProfile -File "$PSScriptRoot/Add-ManagedPackageNotices.ps1" -PayloadDir $payload -NugetPackages $packages
Check ($LASTEXITCODE -ne 0) 'Mismatched assembly notice identity accepted'
# Synthetic package-presence/provenance integration: these are placeholder bytes,
# not a runnable engine and never production qualification evidence.
$suffix=if ($env:OS -eq 'Windows_NT') { '.exe' } else { '' }
$nativeHost=if ($env:OS -eq 'Windows_NT') { 'nethost.dll' } else { 'libnethost.so' }
$required=@("Launcher$suffix","Editor$suffix","AetherGame$suffix",$nativeHost,'DISTRIBUTION.md','data/config/EngineSettings.toml','data/scripts/managed/AetherCore.dll','data/scripts/managed/AetherCore.Interop.dll','data/scripts/managed/AetherCore.Interop.runtimeconfig.json','data/templates/scenes/default.scene.toml','data/templates/scenes/default2d.scene.toml','data/sdk/managed/AetherCore/AetherCore.csproj','data/sdk/managed/Directory.Build.props')
foreach ($relative in $required) {
    $path=Join-Path $payload $relative
    New-Item -ItemType Directory -Force (Split-Path $path) | Out-Null
    Set-Content -LiteralPath $path 'Synthetic package fixture'
}
foreach ($relative in @('data/engine.pak','data/scripts/managed/Microsoft.CodeAnalysis.dll','data/scripts/managed/Microsoft.CodeAnalysis.CSharp.dll')) {
    $stream=[IO.File]::OpenWrite((Join-Path $payload $relative)); $stream.SetLength(1000000); $stream.Dispose()
}
& python "$PSScriptRoot/create_provenance_test_fixture.py" $payload $scratch
Check ($LASTEXITCODE -eq 0) 'Strict synthetic staged provenance fixture failed to generate'
& pwsh -NoProfile -File "$PSScriptRoot/Verify-Package.ps1" -PayloadDir $payload -RequireBuildProvenance
Check ($LASTEXITCODE -eq 0) 'Synthetic provenance package rejected'
Add-Content -LiteralPath "$payload/data/config/EngineSettings.toml" 'mutated after manifest'
& pwsh -NoProfile -File "$PSScriptRoot/Verify-Package.ps1" -PayloadDir $payload -RequireBuildProvenance
Check ($LASTEXITCODE -ne 0) 'Mutated staged bytes accepted by package verifier'
Write-Host "Qualification harness checks passed: $script:checks. Synthetic fixtures retained at $scratch"
exit 0
