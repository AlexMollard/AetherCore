#requires -Version 7.4
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Project,
    [Parameter(Mandatory)][ValidateScript({ ![string]::IsNullOrWhiteSpace($_) -and $_ -notmatch '[\x00-\x1f]' })][string]$Name,
    [Parameter(Mandatory)][ValidateRange(1, 65535)][int]$Port,
    [string]$BuildDir = (Join-Path $PSScriptRoot '../build/default'),
    [ValidateRange(1, 600)][int]$TimeoutSeconds = 90,
    [switch]$Visible
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'AgentEditor.Common.ps1')
$projectRoot = (Resolve-Path -LiteralPath $Project).ProviderPath
if (!(Test-Path -LiteralPath (Join-Path $projectRoot 'ProjectSettings.toml') -PathType Leaf)) {
    throw 'Pog Champ, Project must be an existing project root containing ProjectSettings.toml.'
}
$buildRoot = (Resolve-Path -LiteralPath $BuildDir).ProviderPath
$editor = Join-Path $buildRoot 'src/app/RelWithDebInfo/Editor.exe'
$client = Join-Path $buildRoot 'tools/control-client/RelWithDebInfo/aether-ctl.exe'
if (!(Test-Path -LiteralPath $editor) -or !(Test-Path -LiteralPath $client)) {
    throw 'Pog Champ, build Editor and aether-ctl (RelWithDebInfo) before launching.'
}
if (@(Get-NetUDPEndpoint -LocalPort $Port -ErrorAction SilentlyContinue).Count -gt 0) {
    throw "Pog Champ, UDP port $Port is already owned; choose a free port."
}
$launchId = [guid]::NewGuid().ToString('N')
$logRoot = Join-Path $PSScriptRoot "../logs/agent-editor/$launchId"
New-Item -ItemType Directory -Path $logRoot -Force | Out-Null
$logRoot = (Resolve-Path -LiteralPath $logRoot).ProviderPath
$receiptPath = Join-Path $logRoot 'launch.json'
$stdout = Join-Path $logRoot 'stdout.log'
$stderr = Join-Path $logRoot 'stderr.log'
$arguments = @('--agent', '--agent-name', $Name, '--project', $projectRoot, '--control-port', [string]$Port)
if (!$Visible) { $arguments += '--background' }
$commandLine = ($arguments | ForEach-Object { ConvertTo-AgentEditorArgument $_ }) -join ' '
$process = Start-Process -FilePath $editor -ArgumentList $commandLine -WorkingDirectory $buildRoot -WindowStyle Hidden -PassThru `
    -RedirectStandardOutput $stdout -RedirectStandardError $stderr -Environment @{ AETHER_EDITOR_READY_EVENT = '' }
$receipt = [ordered]@{
    schemaVersion = 1; launchId = $launchId; pid = $process.Id
    startTime = $process.StartTime.ToUniversalTime().ToString('o')
    executable = $editor; controlClient = $client; buildDir = $buildRoot
    project = $projectRoot; name = $Name; port = $Port
    title = $null; background = !$Visible; visible = $false; autosaveEnabled = $false; ready = $false
    logs = @{ stdout = $stdout; stderr = $stderr }; receiptPath = $receiptPath
    cleanupRequired = $true
    cleanupInstructions = "Pog Champ, close this owned editor when work finishes: & '$(Join-Path $PSScriptRoot 'Stop-AgentEditor.ps1')' -Receipt '$receiptPath'"
}
$receipt | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $receiptPath -Encoding utf8
$deadline = [datetime]::UtcNow.AddSeconds($TimeoutSeconds)
$lastError = 'Editor has not exposed its endpoint yet.'
while ([datetime]::UtcNow -lt $deadline) {
    if ($null -eq (Get-AgentEditorProcess ([pscustomobject]$receipt))) { break }
    try {
        $remaining = [int]($deadline - [datetime]::UtcNow).TotalMilliseconds
        $info = Invoke-AgentEditorControl ([pscustomobject]$receipt) 'info' @{} ([Math]::Max(1, [Math]::Min(5000, $remaining)))
        Assert-AgentEditorIdentity ([pscustomobject]$receipt) $info
        $process.Refresh()
        $receipt.title = $info.windowTitle
        $receipt.visible = [bool]$info.windowVisible
        if ($receipt.background -ne [bool]$info.background -or ($receipt.background -and $receipt.visible)) {
            throw 'Pog Champ, the editor background/visibility policy does not match the launch.'
        }
        if ([string]::IsNullOrWhiteSpace($receipt.title) -or !$receipt.title.Contains("[agent: $Name]")) {
            throw 'Pog Champ, the owned editor window is not yet named.'
        }
        $receipt.ready = $true
        $receipt | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $receiptPath -Encoding utf8
        $receipt | ConvertTo-Json -Depth 10
        return
    }
    catch { $lastError = $_.Exception.Message }
    Start-Sleep -Milliseconds 150
}
# A failed launch still owns its process. Attempt only the verified graceful path;
# identity failures leave an actionable receipt rather than killing a stranger.
try {
    $null = & (Join-Path $PSScriptRoot 'Stop-AgentEditor.ps1') -Receipt $receiptPath -TimeoutSeconds 15
}
catch { $lastError += " Cleanup failed: $($_.Exception.Message)" }
throw "Pog Champ, agent editor did not become ready: $lastError. Ownership receipt: $receiptPath. $($receipt.cleanupInstructions)"