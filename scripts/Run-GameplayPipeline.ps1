#requires -Version 7.4
[CmdletBinding()]
param(
    [string]$Project,
    [ValidateSet('desktop', 'web')][string]$Target = 'desktop',
    [ValidateSet('create', 'inspect', 'build', 'run', 'relaunch', 'test', 'publish', 'status', 'cancel')][string]$Operation = 'inspect',
    [string]$Scene,
    [string]$LaunchConfig,
    [string]$ReportPath,
    [switch]$Async,
    [string]$JobId,
    [string]$Name,
    [string]$Template,
    [string]$Language,
    [ValidateRange(1, 1000000)][int]$Ticks = 60,
    [ValidateScript({ $_ -gt 0 -and $_ -le 1 -and [double]::IsFinite($_) })][double]$Step = (1.0 / 60.0),
    [ValidateRange(100, 600000)][int]$TimeoutMs = 180000
)

$ErrorActionPreference = 'Stop'
if ($Target -eq 'web' -and !$PSBoundParameters.ContainsKey('TimeoutMs')) { $TimeoutMs = 600000 }
$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$python = if ($env:AETHER_PYTHON) { $env:AETHER_PYTHON } else { 'python' }
$jobScript = Join-Path $repoRoot 'tools/project/gameplay_jobs.py'
$arguments = [System.Collections.Generic.List[string]]::new()
$arguments.Add($jobScript)
$arguments.Add('--repo')
$arguments.Add($repoRoot)

if ($JobId -and !$PSBoundParameters.ContainsKey('Operation')) { $Operation = 'status' }
if ($Operation -in @('status', 'cancel')) {
    if ([string]::IsNullOrWhiteSpace($JobId)) { throw 'Pog Champ, status and cancel require -JobId.' }
    $arguments.Add($Operation)
    $arguments.Add($JobId)
}
else {
    if ([string]::IsNullOrWhiteSpace($Project)) { throw 'Pog Champ, this operation requires -Project.' }
    if ($JobId) { throw 'Pog Champ, -JobId is only valid for status or cancel.' }
    $request = [ordered]@{
        project = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Project)
        operation = $Operation
        target = $Target
        ticks = $Ticks
        step = $Step
        timeoutMs = $TimeoutMs
    }
    foreach ($entry in @(@('scene', $Scene), @('name', $Name), @('template', $Template), @('language', $Language))) {
        if (![string]::IsNullOrEmpty($entry[1])) { $request[$entry[0]] = $entry[1] }
    }
    if ($LaunchConfig) {
        $config = Get-Content -LiteralPath $LaunchConfig -Raw | ConvertFrom-Json -AsHashtable
        if ($config -isnot [System.Collections.IDictionary]) { throw 'Pog Champ, -LaunchConfig must contain a JSON object.' }
        $request.launchConfig = $config
    }
    $requestDirectory = Join-Path $repoRoot 'logs/gameplay-requests'
    New-Item -ItemType Directory -Force -Path $requestDirectory | Out-Null
    $requestPath = Join-Path $requestDirectory "$([guid]::NewGuid().ToString('D')).json"
    $request | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $requestPath -Encoding utf8NoBOM
    $arguments.Add('start')
    $arguments.Add('--request')
    $arguments.Add($requestPath)
    if (!$Async) { $arguments.Add('--wait') }
}
if ($ReportPath) {
    $arguments.Add('--report')
    $arguments.Add($ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($ReportPath))
}

# ArgumentList passes every path literally; the detached Python supervisor owns cleanup.
$startInfo = [System.Diagnostics.ProcessStartInfo]::new()
$startInfo.FileName = $python
$startInfo.UseShellExecute = $false
$startInfo.CreateNoWindow = $true
$startInfo.RedirectStandardOutput = $true
$startInfo.RedirectStandardError = $true
$startInfo.StandardOutputEncoding = [System.Text.Encoding]::UTF8
$startInfo.StandardErrorEncoding = [System.Text.Encoding]::UTF8
$startInfo.WorkingDirectory = $repoRoot
foreach ($argument in $arguments) { $startInfo.ArgumentList.Add($argument) }
$process = [System.Diagnostics.Process]::Start($startInfo)
try {
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    $process.WaitForExit()
    $text = $stdout.GetAwaiter().GetResult()
    $errorText = $stderr.GetAwaiter().GetResult()
    if ($text) { Write-Output $text.TrimEnd() }
    if ($errorText) { [Console]::Error.Write($errorText) }
    exit $process.ExitCode
}
finally { $process.Dispose() }
