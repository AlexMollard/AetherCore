#requires -Version 7.4
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Receipt,
    [Parameter(Mandatory)][string]$Method,
    [Parameter(Mandatory)][string]$ParamsPath,
    [ValidateRange(1, 120000)][int]$TimeoutMs = 45000
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'AgentEditor.Common.ps1')
$owned = $null
try {
    $owned = Get-Content -LiteralPath $Receipt -Raw | ConvertFrom-Json
    if ($null -eq (Get-AgentEditorProcess $owned)) { throw 'Pog Champ, the owned editor has exited.' }
    $info = Invoke-AgentEditorControl $owned 'info'
    Assert-AgentEditorIdentity $owned $info
    $manifest = Invoke-AgentEditorControl $owned 'describe'
    $matchingMethods = @($manifest.methods | Where-Object { $_.name -ceq $Method -or $_.tool -ceq $Method })
    if ($matchingMethods.Count -ne 1) { throw "Pog Champ, unknown or ambiguous method/tool on this owned editor: $Method" }
    $parameters = Get-Content -LiteralPath $ParamsPath -Raw | ConvertFrom-Json -AsHashtable
    if ($parameters -isnot [System.Collections.IDictionary]) { throw 'Pog Champ, method arguments must be a JSON object.' }
    # Identity and endpoint ownership are checked before dispatch; the shared
    # helper revalidates process/start time/port immediately before each send.
    $payload = Invoke-AgentEditorControl $owned $matchingMethods[0].name $parameters $TimeoutMs -AllowStructuredFailure
    $ok = !($payload.PSObject.Properties['ok'] -and $payload.ok -is [bool] -and !$payload.ok)
    $result = [ordered]@{
        ok = $ok
        receiptPath = $Receipt
        result = $payload
        cleanupRequired = $true
        cleanupInstructions = $owned.cleanupInstructions
    }
    if (!$ok) { $result.message = 'Pog Champ, the owned editor rejected the operation; inspect result.report.' }
    $result | ConvertTo-Json -Depth 100
    if (!$ok) { exit 1 }
}
catch {
    [ordered]@{
        ok = $false
        message = $_.Exception.Message
        receiptPath = $Receipt
        cleanupRequired = $true
        cleanupInstructions = "Pog Champ, close the verified owned editor with agent_editor_close using receiptPath '$Receipt' when finished."
    } | ConvertTo-Json -Depth 100
    exit 1
}
