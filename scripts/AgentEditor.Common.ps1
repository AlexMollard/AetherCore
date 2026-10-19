# Shared ownership checks and bounded control calls for the agent editor helpers.
Set-StrictMode -Version Latest
$OutputEncoding = [Text.UTF8Encoding]::new($false)
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)

function Get-AgentEditorProcess($Receipt) {
    $process = Get-Process -Id ([int]$Receipt.pid) -ErrorAction SilentlyContinue
    if ($null -eq $process) { return $null }
    if ($process.StartTime.ToUniversalTime().Ticks -ne ([datetime]$Receipt.startTime).ToUniversalTime().Ticks) {
        throw 'Pog Champ, the PID has been reused; refusing to touch this process.'
    }
    if ([IO.Path]::GetFullPath($process.Path) -ine [IO.Path]::GetFullPath($Receipt.executable)) {
        throw 'Pog Champ, the executable no longer matches the ownership receipt.'
    }
    return $process
}

function Assert-AgentEditorEndpoint($Receipt) {
    $process = Get-AgentEditorProcess $Receipt
    if ($null -eq $process) { throw 'Pog Champ, the owned editor process has exited.' }
    $owners = @(Get-NetUDPEndpoint -LocalPort ([int]$Receipt.port) -ErrorAction SilentlyContinue)
    if ($owners.Count -eq 0 -or @($owners | Where-Object OwningProcess -ne $process.Id).Count -gt 0) {
        throw 'Pog Champ, the control port does not belong exclusively to the owned editor.'
    }
}

function Invoke-AgentEditorControl($Receipt, [string]$Method, $Parameters = @{}, [int]$TimeoutMs = 5000, [switch]$AllowStructuredFailure) {
    Assert-AgentEditorEndpoint $Receipt
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $Receipt.controlClient
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.StandardOutputEncoding = [Text.UTF8Encoding]::new($false)
    $start.StandardErrorEncoding = [Text.UTF8Encoding]::new($false)
    $start.RedirectStandardInput = $true
    $start.StandardInputEncoding = [Text.UTF8Encoding]::new($false)
    foreach ($argument in @('--port', [string]$Receipt.port, $Method, '-')) {
        $start.ArgumentList.Add($argument)
    }
    $client = [Diagnostics.Process]::Start($start)
    try {
        $client.StandardInput.Write(($Parameters | ConvertTo-Json -Compress -Depth 20))
        $client.StandardInput.Close()
        $stdout = $client.StandardOutput.ReadToEndAsync()
        $stderr = $client.StandardError.ReadToEndAsync()
        if (!$client.WaitForExit($TimeoutMs)) {
            $client.Kill()
            $client.WaitForExit()
            throw "Pog Champ, control call '$Method' timed out."
        }
        $outputText = $stdout.GetAwaiter().GetResult()
        if ($client.ExitCode -ne 0) {
            if ($AllowStructuredFailure) {
                try {
                    $payload = $outputText | ConvertFrom-Json
                    $expectedCode = switch ($payload.report.status) { 'failed' { 1 }; 'cancelled' { 130 }; 'skipped' { 3 }; default { -1 } }
                    if ($payload.ok -eq $false -and $payload.report.schemaVersion -eq 1 -and
                        $payload.report.exitCode -eq $expectedCode -and $client.ExitCode -eq $expectedCode) { return $payload }
                } catch { }
            }
            throw "Pog Champ, control call '$Method' failed: $($stderr.GetAwaiter().GetResult()) $outputText"
        }
        $result = $outputText | ConvertFrom-Json
        if ($result.PSObject.Properties['error']) { throw "Pog Champ, control call '$Method' failed: $($result.error)" }
        return $result
    }
    finally { $client.Dispose() }
}

function Assert-AgentEditorIdentity($Receipt, $Info) {
    Assert-AgentEditorEndpoint $Receipt
    if ($Receipt.PSObject.Properties['background'] -and
        ([bool]$Info.background -ne [bool]$Receipt.background -or ($Receipt.background -and $Info.windowVisible))) {
        throw 'Pog Champ, the endpoint background/visibility policy no longer matches the owned launch.'
    }
    if ([int]$Info.pid -ne [int]$Receipt.pid -or
        [IO.Path]::GetFullPath([string]$Info.projectRoot).TrimEnd('\', '/') -ine [IO.Path]::GetFullPath([string]$Receipt.project).TrimEnd('\', '/') -or
        [string]$Info.agentName -cne [string]$Receipt.name -or !$Info.agentSession -or $Info.autosaveEnabled) {
        throw 'Pog Champ, the endpoint identity or autosave policy does not match the owned launch.'
    }
}

function ConvertTo-AgentEditorArgument([string]$Value) {
    # Start-Process joins ArgumentList into one Windows command line. Quote using
    # CommandLineToArgvW rules, including quotes and a final backslash in paths.
    return '"' + [regex]::Replace($Value, '(\\*)("|$)', {
        param($match)
        $slashes = $match.Groups[1].Value
        if ($match.Groups[2].Value -eq '"') { return $slashes + $slashes + '\"' }
        return $slashes + $slashes
    }) + '"'
}