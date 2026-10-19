#requires -Version 7.4
$ErrorActionPreference = 'Stop'
$fixture = Join-Path $PSScriptRoot '../../.temp/task6/compiling-cleanup-test'
New-Item -ItemType Directory -Path $fixture -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $PSScriptRoot '../../scripts/Stop-AgentEditor.ps1') -Destination $fixture -Force
# Exercise the real close script with a controlled compiler/control transport.
$transport = @'
$script:editorState = 'compiling'
$script:stopQueued = $false
$script:stopPolls = 0
function Get-AgentEditorProcess($Receipt) {
    if ($script:editorState -eq 'exited') { return $null }
    return [pscustomobject]@{ Id = $Receipt.pid }
}
function Assert-AgentEditorIdentity($Receipt, $Info) { }
function Invoke-AgentEditorControl($Receipt, [string]$Method, $Parameters = @{}, [int]$TimeoutMs = 5000) {
    Add-Content -LiteralPath $Receipt.transitionLog -Value $Method
    switch ($Method) {
        'info' {
            if ($script:stopQueued -and ++$script:stopPolls -ge 2) { $script:editorState = 'editing' }
            return [pscustomobject]@{ playState = $script:editorState }
        }
        'engine.stop' { $script:stopQueued = $true; return [pscustomobject]@{ ok = $true } }
        'app.quit' {
            if ($script:editorState -ne 'editing') { throw 'Quit before compilation stopped.' }
            $script:editorState = 'exited'; return [pscustomobject]@{ requested = $true } }
        default { throw "Unexpected cleanup call: $Method" }
    }
}
'@
$transport | Set-Content -LiteralPath (Join-Path $fixture 'AgentEditor.Common.ps1')
$transitions = Join-Path $fixture 'transitions.txt'
if (Test-Path -LiteralPath $transitions) { Remove-Item -LiteralPath $transitions }
$fixtureReceipt = Join-Path $fixture 'receipt.json'
@{ pid = 42; cleanupRequired = $true; ready = $true; receiptPath = $fixtureReceipt; transitionLog = $transitions } |
    ConvertTo-Json | Set-Content -LiteralPath $fixtureReceipt
& (Join-Path $fixture 'Stop-AgentEditor.ps1') -Receipt $fixtureReceipt -TimeoutSeconds 2 | Out-Null
$order = @(Get-Content -LiteralPath $transitions)
if (($order -join ',') -ne 'info,engine.stop,info,info,app.quit') { throw "Unexpected compilation cleanup order: $order" }
if ((Get-Content -LiteralPath $fixtureReceipt -Raw | ConvertFrom-Json).cleanupRequired) { throw 'Compilation cleanup left its receipt open.' }
Write-Output 'Pog Champ, compiling cleanup stops compilation, waits for editing, and quits.'
