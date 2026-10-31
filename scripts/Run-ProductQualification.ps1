#requires -Version 7.4
<# Local relocated-byte qualification. Run on a clean standard-user VM separately:
   this script records local proof and never certifies the VM/installer gate. #>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$PayloadDir,
    [Parameter(Mandatory)][string]$StageRoot,
    [Parameter(Mandatory)][string]$ControlClient,
    [Parameter(Mandatory)][string]$ModelFixture,
    [Parameter(Mandatory)][string]$ReportPath,
    [ValidateRange(1024,65535)][int]$Port = 18987,
    [ValidateRange(1,1800)][int]$TimeoutSeconds = 120,
    [ValidateRange(1,10000)][int]$Transitions = 3,
    [switch]$RequireSigned
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/Qualification.Common.ps1"
$phases = [Collections.Generic.List[object]]::new()
$report = [ordered]@{ schemaVersion=1; status='fail'; kind='local-relocated-product'; phases=$phases; cleanVm='blocked-requires-external-environment'; installerUpgradeUninstall='blocked-requires-VM'; signing='unsigned-not-verified'; transitions=$Transitions }
$owned = [Collections.Generic.List[object]]::new()
function Phase([string]$Name, [scriptblock]$Action) {
    $entry = [ordered]@{ name=$Name; status='fail' }
    try { $entry.evidence = & $Action; $entry.status='pass' } catch {
        $entry.detail=$_.Exception.Message
        if ($_.Exception.Data.Contains('QualificationEvidence')) { $entry.evidence=$_.Exception.Data['QualificationEvidence'] }
        if ($_.Exception.Data.Contains('ExitCode')) {
            $entry.pid=$_.Exception.Data['ProcessId']; $entry.exitCode=$_.Exception.Data['ExitCode']; $entry.exitCodeHex=$_.Exception.Data['ExitCodeHex']
        }
        throw
    }
    finally { $phases.Add([pscustomobject]$entry) }
}
function Call([string]$Method, $Params=@{}) {
    if ($env:OS -eq 'Windows_NT') {
        $owners=@(Get-NetUDPEndpoint -LocalPort $Port -ErrorAction SilentlyContinue | Select-Object -ExpandProperty OwningProcess -Unique)
    } else {
        if (-not (Get-Command ss -ErrorAction SilentlyContinue)) { throw 'Endpoint ownership validation requires ss on Linux' }
        $socketText=(& ss -H -lunp "sport = :$Port") -join "`n"
        $owners=@([regex]::Matches($socketText, 'pid=(\d+)') | ForEach-Object { [int]$_.Groups[1].Value } | Select-Object -Unique)
    }
    if ($owners.Count -ne 1) { throw 'Control port must belong exclusively to one owned process' }
    $owner=Get-Process -Id ([int]$owners[0])
    $known=@($owned | Where-Object { -not $_.HasExited -and $_.Id -eq $owner.Id -and $_.StartTime -eq $owner.StartTime })
    if (-not $known.Count) {
        # The only unregistered process accepted is the editor directly spawned by
        # our Launcher. A copied executable path alone does not establish ownership.
        $parentId=if ($env:OS -eq 'Windows_NT') { (Get-CimInstance Win32_Process -Filter "ProcessId=$($owner.Id)").ParentProcessId } else { [int]((Get-Content "/proc/$($owner.Id)/status" | Where-Object { $_ -match '^PPid:' }) -replace '^PPid:\s*','') }
        if (-not $launcher -or $parentId -ne $launcher.Id -or [IO.Path]::GetFullPath($owner.Path) -ne (Join-Path $product "Editor$suffix") -or $owner.StartTime -lt $launcher.StartTime) { throw 'Control endpoint is not an owned Launcher/editor child' }
        Retain-QualifiedProcessHandle $owner
        $owned.Add($owner)
    } else {
        $owner.Dispose()
    }
    Invoke-QualifiedControl $ControlClient $Port $Method $Params 10000
}
function Save-CurrentScene {
    $saved=Call 'scene.save'
    if (-not $saved.saved) { throw 'Scene save was not acknowledged' }
    $undo=Call 'editor.undo_status'
    if ($null -eq $undo.unsavedChanges -or $undo.unsavedChanges -ne $false) { throw 'Successful current-scene save did not clear the unsaved edit baseline' }
    @{ save=$saved; undo=$undo }
}
function Wait-State([string]$Method, [string]$Field, [string]$Expected) {
    $deadline = [datetime]::UtcNow.AddSeconds($TimeoutSeconds)
    $last = $null
    $expectedProcess = if ($Method.StartsWith('launcher.')) { $launcher } else { $editor }
    do {
        Assert-QualifiedProcessAlive $expectedProcess "waiting for $Method.$Field=$Expected"
        try {
            $last = Call $Method
        } catch { $lastError=$_.Exception.Message }
        Assert-QualifiedProcessAlive $expectedProcess "waiting for $Method.$Field=$Expected"
        if ([string]$last.$Field -eq $Expected) { return $last }
        if ($last.$Field -in @('failed','error')) { throw "Operation failed: $($last | ConvertTo-Json -Compress -Depth 10)" }
        Start-Sleep -Milliseconds 200
    } while ([datetime]::UtcNow -lt $deadline)
    throw "Deadline waiting for $Method.$Field=$Expected; last=$($last | ConvertTo-Json -Compress -Depth 10); $lastError"
}
function Start-Owned([string]$Exe, [string[]]$Arguments=@(), [string]$WorkingDir=$product) {
    $start=[Diagnostics.ProcessStartInfo]::new()
    $start.FileName=$Exe; $start.WorkingDirectory=$WorkingDir; $start.UseShellExecute=$false; $start.CreateNoWindow=$true
    foreach ($arg in $Arguments) { $start.ArgumentList.Add($arg) }
    foreach ($key in @($start.Environment.Keys | Where-Object { $_ -match '^(AETHER_|AETHERCORE_|DOTNET_)' })) { $start.Environment.Remove($key) | Out-Null }
    $start.Environment['AETHER_USER_DATA_DIR']=$state
    $start.Environment['AETHER_AGENT_SESSION']='1'
    $start.Environment['AETHER_AGENT_NAME']='qualification'
    $start.Environment['AETHER_AGENT_BACKGROUND']='0'
    # BUILD_PROVENANCE.json selects installed defaults in the actual application.
    # Do not force QA-only isolation: this gate must prove ordinary delivered use.
    $start.Environment['AETHER_CONTROL_PORT']=[string]$Port
    $start.Environment['DOTNET_ROOT']=Join-Path $product 'dotnet'
    $start.Environment['DOTNET_MULTILEVEL_LOOKUP']='0'
    # Only OS tools and delivered bytes are visible to the launched product.
    $start.Environment['PATH']=if ($env:OS -eq 'Windows_NT') { "$product;$env:SystemRoot/System32;$env:SystemRoot" } else { "${product}:/usr/bin:/bin" }
    $process=[Diagnostics.Process]::Start($start)
    $owned.Add($process)
    return $process
}
try {
    $repo=[IO.Path]::GetFullPath((Split-Path $PSScriptRoot))
    $stage=[IO.Path]::GetFullPath($StageRoot)
    if ($stage.TrimEnd('\','/') -eq $repo.TrimEnd('\','/') -or $stage.StartsWith($repo.TrimEnd('\','/')+[IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw 'StageRoot must be outside the source/build tree' }
    if (Test-Path -LiteralPath $stage) { throw 'StageRoot must be unused (no stale evidence)' }
    $payload=(Resolve-Path -LiteralPath $PayloadDir).Path
    Assert-QualifiedOutputOutsideBundle $payload $ReportPath
    Assert-QualifiedOutputOutsideBundle $payload $stage
    $sourceBefore=Get-QualifiedBundleInventory $payload
    $report.sourceBefore=$sourceBefore
    $ControlClient=(Resolve-Path -LiteralPath $ControlClient).Path
    $model=(Resolve-Path -LiteralPath $ModelFixture).Path
    if (@(Get-ChildItem -LiteralPath $payload -Recurse -Force | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }).Count) { throw 'Payload symlinks/reparse points are unsupported in isolated qualification' }
    # Bind before launch to reject another endpoint; subsequent calls also verify identity.
    $probe=[Net.Sockets.UdpClient]::new($Port); $probe.Dispose()
    $product=Join-Path $stage 'Aether product Ω'
    $state=Join-Path $stage 'Isolated user Ω'
    $project=Join-Path $stage 'Reference project Ω'
    New-Item -ItemType Directory -Force $product,$state | Out-Null
    Get-ChildItem -LiteralPath $payload -Force | Copy-Item -Destination $product -Recurse
    $suffix=if ($env:OS -eq 'Windows_NT') { '.exe' } else { '' }
    $report.product=$product; $report.state=$state
    Assert-QualifiedOutputOutsideBundle $product $ReportPath
    Phase 'payload' {
        & pwsh -NoProfile -File "$PSScriptRoot/Verify-Package.ps1" -PayloadDir $product -RequireBundledRuntime -RequireBuildProvenance
        if ($LASTEXITCODE -ne 0) { throw 'Staged package verification failed' }
        @(Get-ChildItem -LiteralPath $product -Recurse -File | ForEach-Object { @{ path=$_.FullName.Substring($product.Length+1); sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash } })
    }
    Phase 'signing' {
        if ($RequireSigned) {
            if ($env:OS -ne 'Windows_NT') { throw 'Signature verification requires a platform-specific signing verifier' }
            $signatures=@(Get-ChildItem -LiteralPath $product -File -Filter '*.exe' | Get-AuthenticodeSignature)
            if (-not $signatures.Count -or @($signatures | Where-Object Status -ne 'Valid').Count) { throw 'Required executable signature missing or invalid' }
            $report.signing='verified'; $signatures | Select-Object Path,Status,SignerCertificate
        } else { 'Private build: signing not required; unsigned/unverified' }
    }
    $productBefore=Get-QualifiedBundleInventory $product
    Assert-QualifiedBundleUnchanged $sourceBefore $productBefore
    $report.productBefore=$productBefore
    $launcher=Start-Owned (Join-Path $product "Launcher$suffix")
    Phase 'UiShell-ready' { Wait-State 'launcher.info' 'launchingEditor' 'False' }
    Phase 'create' { Call 'launcher.create_project' @{root=$project;name='Reference Ω';template='blank_3d';language='csharp'} }
    Phase 'Full-ready' {
        $deadline=[datetime]::UtcNow.AddSeconds($TimeoutSeconds)
        do {
            try {
                $info=Call 'info'
                if ($info.pid -and [IO.Path]::GetFullPath($info.projectRoot) -eq $project) { break }
            } catch { }
            Start-Sleep -Milliseconds 200
        } while ([datetime]::UtcNow -lt $deadline)
        if (-not $info.pid -or [IO.Path]::GetFullPath($info.projectRoot) -ne $project) { throw 'Spawned editor readiness/identity deadline' }
        $script:editor=Get-Process -Id ([int]$info.pid)
        if ([IO.Path]::GetFullPath($editor.Path) -ne (Join-Path $product "Editor$suffix") -or $editor.StartTime -lt $launcher.StartTime) { throw 'Editor executable/creation time differs from owned handoff' }
        Retain-QualifiedProcessHandle $editor
        $owned.Add($editor)
        $info
    }
    $assetDir=Join-Path $project 'assets/models'
    New-Item -ItemType Directory -Force $assetDir | Out-Null
    $copiedModel=Join-Path $assetDir ([IO.Path]::GetFileName($model))
    Copy-Item -LiteralPath $model -Destination $copiedModel
    Phase 'import' { Call 'scene.add_model' @{path=$copiedModel;name='QualificationModel'} }
    $sceneName=(Call 'info').scene
    Phase 'save' { Save-CurrentScene }
    foreach ($transition in 1..$Transitions) {
        Phase "play-$transition" { $null=Call 'engine.play'; Wait-State 'info' 'playState' 'playing' }
        Phase "stop-$transition" { $null=Call 'engine.stop'; Wait-State 'info' 'playState' 'editing' }
        Phase "reopen-$transition" {
            $null=Call 'scene.load' @{name=$sceneName}
            $entities=Call 'scene.entities'
            if (@($entities.entities | Where-Object name -eq 'QualificationModel').Count -ne 1) { throw 'Imported model was lost after scene reload' }
            $entities
        }
    }
    # Scene reload is undoable and therefore dirties edit history. Save explicitly;
    # never answer a close confirmation by discarding qualification work.
    Phase 'save-before-restart' { Save-CurrentScene }
    Phase 'project-close' {
        $result=Stop-QualifiedProcess $editor $ControlClient $Port 30
        if ($result.status -ne 'pass') {
            $failure=[InvalidOperationException]::new("Editor graceful shutdown before project reopen failed: $($result | ConvertTo-Json -Compress -Depth 8)")
            $failure.Data['QualificationEvidence']=$result
            throw $failure
        }
        $logs=@(Get-ChildItem -LiteralPath $state -Recurse -Filter 'Editor.log')
        Assert-ShutdownLog @($logs | Get-Content)
        foreach ($log in $logs) {
            Copy-Item -LiteralPath $log.FullName -Destination (Join-Path $stage 'Editor-before-reopen.log')
            # Logger may append. Preserve settings while forcing fresh per-process evidence.
            Remove-Item -LiteralPath $log.FullName
        }
        $result
    }
    Phase 'project-reopen' {
        $script:editor=Start-Owned (Join-Path $product "Editor$suffix") @('--project',$project,'--agent','--background','--agent-name','product-qualification','--control-port',[string]$Port)
        $info=Wait-State 'info' 'playState' 'editing'
        if ([int]$info.pid -ne $editor.Id -or [IO.Path]::GetFullPath($info.projectRoot) -ne $project) { throw 'Reopened project identity differs' }
        $null=Call 'scene.load' @{name=$sceneName}
        $entities=Call 'scene.entities'
        if (@($entities.entities | Where-Object name -eq 'QualificationModel').Count -ne 1) { throw 'Saved import missing after process restart' }
        $entities
    }
    Phase 'publish' { $null=Call 'project.publish'; Wait-State 'project.publish_status' 'state' 'succeeded' }
    Phase 'save-before-final-close' { Save-CurrentScene }
    Phase 'Full-close' {
        $result=Stop-QualifiedProcess $editor $ControlClient $Port 30
        if ($result.status -ne 'pass') {
            $failure=[InvalidOperationException]::new("Editor graceful shutdown failed: $($result | ConvertTo-Json -Compress -Depth 8)")
            $failure.Data['QualificationEvidence']=$result
            throw $failure
        }
        Assert-ShutdownLog @(Get-ChildItem -LiteralPath $state -Recurse -Filter 'Editor.log' | Get-Content)
        $result
    }
    Phase 'UiShell-close' {
        if (-not $launcher.WaitForExit(30000) -or $launcher.ExitCode -ne 0) { throw 'Launcher did not exit cleanly after handoff' }
        Assert-ShutdownLog @(Get-ChildItem -LiteralPath $state -Recurse -Filter 'Launcher.log' | Get-Content)
    }
    Phase 'delivered-runtime' {
        $games=@(Get-ChildItem -LiteralPath (Join-Path $project 'Builds') -Recurse -File | Where-Object { $_.Name -eq "AetherGame$suffix" })
        if ($games.Count -ne 1) { throw 'Expected one published native runtime' }
        $runtimeProduct=Join-Path $stage 'Copied game Ω'
        Copy-Item -LiteralPath $games[0].Directory.FullName -Destination $runtimeProduct -Recurse
        & pwsh -NoProfile -File "$PSScriptRoot/Run-LifecycleQualification.ps1" -ProductDir $runtimeProduct -StateDir (Join-Path $stage 'Runtime state Ω') -ReportPath (Join-Path $stage 'runtime-report.json') -Profiles Runtime -Cycles 1
        if ($LASTEXITCODE -ne 0) { throw 'Copied published runtime lifecycle failed' }
        @{
            lifecycle = Get-Content (Join-Path $stage 'runtime-report.json') -Raw | ConvertFrom-Json
            payload = @(Get-ChildItem -LiteralPath $runtimeProduct -Recurse -File | ForEach-Object { @{path=$_.FullName.Substring($runtimeProduct.Length+1);sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash;bytes=$_.Length} })
        }
    }
    $report.status='pass'
} catch { $report.detail=$_.Exception.Message }
finally {
    foreach ($process in $owned) {
        if (-not $process.HasExited) {
            $cleanup=Stop-QualifiedProcess $process '' 0 15
            $phases.Add([pscustomobject]@{name="cleanup-$($process.Id)";status=$cleanup.status;evidence=$cleanup})
            if ($cleanup.status -ne 'pass') { $report.status='fail' }
        }
        $process.Dispose()
    }
    foreach ($binding in @(@{name='source-immutability';before=$sourceBefore;root=$payload},@{name='product-immutability';before=$productBefore;root=$product})) {
        if (-not $binding.before) { continue }
        $immutable=[ordered]@{name=$binding.name;status='fail';beforeSha256=$binding.before.inventorySha256}
        try {
            $after=Get-QualifiedBundleInventory $binding.root
            $immutable.afterSha256=$after.inventorySha256
            Assert-QualifiedBundleUnchanged $binding.before $after
            $immutable.status='pass'
        } catch { $immutable.detail=$_.Exception.Message; $report.status='fail' }
        $phases.Add([pscustomobject]$immutable)
    }
    New-Item -ItemType Directory -Force (Split-Path ([IO.Path]::GetFullPath($ReportPath))) | Out-Null
    $report | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $ReportPath -Encoding utf8
}
if ($report.status -ne 'pass') { exit 1 }
exit 0
