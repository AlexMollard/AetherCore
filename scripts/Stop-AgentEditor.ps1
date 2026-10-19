#requires -Version 7.4
[CmdletBinding()]
param(
    [Parameter(Mandatory)][object]$Receipt,
    [ValidateRange(1, 120)][int]$TimeoutSeconds = 30,
    [switch]$DiscardChanges
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'AgentEditor.Common.ps1')
if ($Receipt -is [string]) { $Receipt = Get-Content -LiteralPath $Receipt -Raw | ConvertFrom-Json }
$process = Get-AgentEditorProcess $Receipt
if ($null -eq $process) {
    $Receipt.cleanupRequired = $false
    $Receipt.ready = $false
    $Receipt | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $Receipt.receiptPath -Encoding utf8
    @{ closed = $true; alreadyExited = $true; pid = $Receipt.pid; cleanupRequired = $false } | ConvertTo-Json
    return
}
$deadline = [datetime]::UtcNow.AddSeconds($TimeoutSeconds)
function Invoke-OwnedControl([string]$Method, $Parameters = @{}) {
    $remaining = [int]($deadline - [datetime]::UtcNow).TotalMilliseconds
    if ($remaining -le 0) { throw 'Pog Champ, graceful close exceeded its timeout; the editor remains owned and requires cleanup.' }
    Invoke-AgentEditorControl $Receipt $Method $Parameters ([Math]::Min(5000, $remaining))
}
$info = Invoke-OwnedControl 'info'
Assert-AgentEditorIdentity $Receipt $info
if ($info.playState -in @('playing', 'paused', 'compiling')) {
    $result = Invoke-OwnedControl 'engine.stop'
    do {
        $info = Invoke-OwnedControl 'info'
        Assert-AgentEditorIdentity $Receipt $info
        if ($info.playState -eq 'editing') { break }
        Start-Sleep -Milliseconds 100
    } while ([datetime]::UtcNow -lt $deadline)
    if ($info.playState -ne 'editing') { throw 'Pog Champ, Play or compilation did not stop; cleanup remains required.' }
}
try { $null = Invoke-OwnedControl 'app.quit' }
catch {
    if ($null -ne (Get-AgentEditorProcess $Receipt)) { throw }
}
$answered = $false
while ([datetime]::UtcNow -lt $deadline) {
    if ($null -eq (Get-AgentEditorProcess $Receipt)) {
        $Receipt.cleanupRequired = $false
        $Receipt.ready = $false
        $Receipt | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $Receipt.receiptPath -Encoding utf8
        @{ closed = $true; pid = $Receipt.pid; cleanupRequired = $false; discarded = [bool]$DiscardChanges } | ConvertTo-Json
        return
    }
    if (!$answered) {
        try {
            $info = Invoke-OwnedControl 'info'
            Assert-AgentEditorIdentity $Receipt $info
            $widgets = Invoke-OwnedControl 'ui.query' @{ window = '###unsavedChanges' }
            $choice = if ($DiscardChanges) { 'Discard' } else { 'Save' }
            $buttons = @($widgets.items | Where-Object { $_.window -like '*###unsavedChanges' -and $_.label -match "(^|\s)$choice$" })
            if ($buttons.Count -eq 1) {
                $null = Invoke-OwnedControl 'ui.click' @{ window = $buttons[0].window; label = $buttons[0].label }
                $answered = $true
            }
        }
        catch {
            if ($null -ne (Get-AgentEditorProcess $Receipt)) { throw }
        }
    }
    Start-Sleep -Milliseconds 100
}
throw "Pog Champ, the editor did not close. Saving may need a scene name or have failed; inspect the owned window and logs. Cleanup remains required for PID $($Receipt.pid)."