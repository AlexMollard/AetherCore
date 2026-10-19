#requires -Version 7.4
# Run directly with pwsh -NoProfile -File tests/automation/AgentEditor.Tests.ps1.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../../scripts/AgentEditor.Common.ps1')
function Assert-Throws([scriptblock]$Action, [string]$Pattern) {
    try { & $Action } catch {
        if ($_.Exception.Message -notmatch $Pattern) { throw }
        return
    }
    throw "Expected rejection matching $Pattern"
}
$owner = Get-Process -Id $PID
$socket = [Net.Sockets.UdpClient]::new(0)
try {
    $receipt = [pscustomobject]@{
        pid = $PID; startTime = $owner.StartTime.ToUniversalTime().ToString('o')
        executable = $owner.Path; project = $PSScriptRoot; name = 'Named owner'; port = $socket.Client.LocalEndPoint.Port
    }
    $info = [pscustomobject]@{
        pid = $PID; projectRoot = $PSScriptRoot; agentName = 'Named owner'; agentSession = $true; autosaveEnabled = $false
    }
    Assert-AgentEditorIdentity $receipt $info
    $receipt.startTime = $owner.StartTime.AddSeconds(-1).ToUniversalTime().ToString('o')
    Assert-Throws { Get-AgentEditorProcess $receipt } 'PID has been reused'
    $receipt.startTime = $owner.StartTime.ToUniversalTime().ToString('o')
    $info.pid = $PID + 1
    Assert-Throws { Assert-AgentEditorIdentity $receipt $info } 'endpoint identity'
    $info.pid = $PID
    $info.projectRoot = [IO.Path]::GetTempPath()
    Assert-Throws { Assert-AgentEditorIdentity $receipt $info } 'endpoint identity'
    $info.projectRoot = $PSScriptRoot
    $info.agentName = 'Another owner'
    Assert-Throws { Assert-AgentEditorIdentity $receipt $info } 'endpoint identity'
    $info.agentName = 'Named owner'
    $info.autosaveEnabled = $true
    Assert-Throws { Assert-AgentEditorIdentity $receipt $info } 'autosave policy'
    $info.autosaveEnabled = $false
    $info.agentSession = $false
    Assert-Throws { Assert-AgentEditorIdentity $receipt $info } 'endpoint identity'
    # A valid owner PID is insufficient when its control port belongs elsewhere.
    $receipt.port = 0
    Assert-Throws { Assert-AgentEditorEndpoint $receipt } 'control port'
    foreach ($script in @('Start-AgentEditor.ps1', 'Stop-AgentEditor.ps1', 'AgentEditor.Common.ps1')) {
        $parseErrors = $null
        $null = [Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot "../../scripts/$script"), [ref]$null, [ref]$parseErrors)
        if ($parseErrors.Count -gt 0) { throw ($parseErrors | Out-String) }
    }
    $scratch = Join-Path $PSScriptRoot '../../.temp/task6'
    New-Item -ItemType Directory -Path $scratch -Force | Out-Null
    $fixture = Join-Path $scratch 'echo-arguments.ps1'
    '[Console]::WriteLine((ConvertTo-Json -InputObject @($args) -Compress))' | Set-Content -LiteralPath $fixture
    $values = @('plain', 'with spaces', 'quote"value', 'trailing\', 'dollar$HOME; & literal')
    $childArguments = @('-NoProfile', '-File', (Resolve-Path -LiteralPath $fixture).ProviderPath) + $values
    $output = Join-Path $scratch 'arguments.json'
    $child = Start-Process -FilePath $owner.Path -WindowStyle Hidden -PassThru -Wait -RedirectStandardOutput $output `
        -ArgumentList (($childArguments | ForEach-Object { ConvertTo-AgentEditorArgument $_ }) -join ' ')
    $actual = @(Get-Content -LiteralPath $output -Raw | ConvertFrom-Json)
    if ($child.ExitCode -ne 0 -or $actual.Count -ne $values.Count) { throw 'Literal argument child failed.' }
    for ($i = 0; $i -lt $values.Count; ++$i) {
        if ($actual[$i] -cne $values[$i]) { throw "Argument $i was changed by launch quoting." }
    }
    Write-Output 'Pog Champ, agent ownership tests passed (PID reuse, wrong PID/project/name/mode/autosave/port, script syntax).'
}
finally { $socket.Dispose() }