# Shared, fail-closed qualification checks. Dot-source; no application is launched here.
function Assert-QualifiedOutputOutsideBundle([string]$Bundle, [string]$OutputPath) {
    $root=[IO.Path]::GetFullPath($Bundle).TrimEnd('\','/')
    $output=[IO.Path]::GetFullPath($OutputPath)
    $comparison=if ($env:OS -eq 'Windows_NT') { [StringComparison]::OrdinalIgnoreCase } else { [StringComparison]::Ordinal }
    if ($output.Equals($root,$comparison) -or $output.StartsWith($root+[IO.Path]::DirectorySeparatorChar,$comparison)) { throw 'Qualification state and reports must remain outside the immutable bundle' }
}

function Get-QualifiedBundleInventory([string]$Bundle) {
    $root=(Resolve-Path -LiteralPath $Bundle).Path
    $entries=@(Get-ChildItem -LiteralPath $root -Recurse -Force -ErrorAction Stop)
    if (@($entries | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }).Count) { throw 'Immutable bundle inventory rejects symlinks/reparse points' }
    $files=@($entries | Where-Object { -not $_.PSIsContainer } | ForEach-Object {
        [pscustomobject][ordered]@{path=$_.FullName.Substring($root.Length+1).Replace('\','/');bytes=$_.Length;sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256 -ErrorAction Stop).Hash}
    } | Sort-Object -Property path -CaseSensitive)
    if (-not $files.Count) { throw 'Immutable bundle inventory is empty' }
    $bytes=[Text.Encoding]::UTF8.GetBytes((ConvertTo-Json -InputObject $files -Depth 5 -Compress))
    return @{root=$root;files=$files;inventorySha256=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($bytes))}
}

function Assert-QualifiedBundleUnchanged($Before, $After) {
    $previous=[Collections.Generic.Dictionary[string,object]]::new([StringComparer]::Ordinal)
    foreach ($file in $Before.files) { $previous.Add($file.path,$file) }
    foreach ($file in $After.files) {
        if (-not $previous.ContainsKey($file.path)) { throw "Immutable bundle added file: $($file.path)" }
        $old=$previous[$file.path]
        if ($old.bytes -ne $file.bytes -or $old.sha256 -cne $file.sha256) { throw "Immutable bundle changed file: $($file.path)" }
        $previous.Remove($file.path) | Out-Null
    }
    if ($previous.Count) { throw "Immutable bundle deleted file: $(@($previous.Keys)[0])" }
}

function Read-NativeTestReport([string]$Path, [datetime]$StartedUtc, [string]$ExpectedExecutable) {
    $file=Get-Item -LiteralPath $Path -ErrorAction Stop
    if ($file.PSIsContainer -or $file.LastWriteTimeUtc -lt $StartedUtc.AddSeconds(-2)) { throw 'Native doctest report is stale or not a file' }
    $settings=[Xml.XmlReaderSettings]::new(); $settings.DtdProcessing=[Xml.DtdProcessing]::Prohibit; $settings.XmlResolver=$null
    $reader=[Xml.XmlReader]::Create($file.FullName,$settings)
    try { $xml=[Xml.XmlDocument]::new(); $xml.XmlResolver=$null; $xml.Load($reader) } finally { $reader.Dispose() }
    if ($xml.DocumentElement.Name -ne 'doctest') { throw 'Native report is not a doctest report' }
    $reportedExecutable=$xml.DocumentElement.GetAttribute('binary')
    if (-not [IO.Path]::IsPathFullyQualified($reportedExecutable) -or -not [IO.Path]::IsPathFullyQualified($ExpectedExecutable)) { throw 'Native report and CTest executable identities must be absolute paths' }
    $reportedExecutable=[IO.Path]::GetFullPath($reportedExecutable).Replace('\','/')
    $expected=[IO.Path]::GetFullPath($ExpectedExecutable).Replace('\','/')
    $comparison=[StringComparison]::Ordinal
    if ($env:OS -eq 'Windows_NT') {
        $comparison=[StringComparison]::OrdinalIgnoreCase
        if ($expected.EndsWith('.exe',$comparison)) { $expected=$expected.Substring(0,$expected.Length-4) }
        if ($reportedExecutable.EndsWith('.exe',$comparison)) { $reportedExecutable=$reportedExecutable.Substring(0,$reportedExecutable.Length-4) }
    }
    if (-not $reportedExecutable.Equals($expected,$comparison)) { throw 'Native report executable differs from the EngineTests CTest command' }
    $counts=@{}
    foreach ($group in @(@{tag='OverallResultsTestCases';prefix=''},@{tag='OverallResultsAsserts';prefix='assertion'})) {
        $nodes=$xml.SelectNodes("/doctest/$($group.tag)")
        if ($nodes.Count -ne 1) { throw "Native doctest report lacks a unique final $($group.tag)" }
        foreach ($attribute in @('successes','failures')) {
            $value=$nodes[0].GetAttribute($attribute)
            if ($value -notmatch '^\d+$') { throw "Invalid native $($group.tag) $attribute count" }
            $counts["$($group.prefix)$attribute"]=[int]$value
        }
    }
    return @{testCases=$counts.successes+$counts.failures;passed=$counts.successes;failed=$counts.failures;assertions=$counts.assertionsuccesses+$counts.assertionfailures;assertionsFailed=$counts.assertionfailures;path=$file.FullName;sha256=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash}
}

function Get-NativeTestSummary([string[]]$Lines) {
    $text=($Lines -join "`n") -replace '\x1B\[[0-?]*[ -/]*[@-~]', ''
    # CTest prefixes every physical line; stdout/stderr interleaving can put
    # the numeric summary on the next line after the doctest header.
    $text=$text -replace '(?m)^[ \t]*\d+:[ \t]*', ''
    $match=[regex]::Match($text, '\[doctest\][ \t]*test cases:\s*(\d+)\s*\|\s*(\d+)\s*passed\s*\|\s*(\d+)\s*failed')
    if (-not $match.Success) { return $null }
    return @{testCases=[int]$match.Groups[1].Value;passed=[int]$match.Groups[2].Value;failed=[int]$match.Groups[3].Value}
}

function Assert-TestInventory($Inventory, [string[]]$Expected = @('EngineTests', 'GameplayApiParity', 'GameplayApiGeneratorTests', 'ManagedTests')) {
    $names = @($Inventory.tests | ForEach-Object { $_.name })
    $missing = @($Expected | Where-Object { $_ -notin $names })
    if ($missing.Count) { throw "Required CTest suites missing: $($missing -join ', '). Configure with Python and the pinned .NET SDK available." }
    if ($names.Count -ne @($names | Select-Object -Unique).Count) { throw 'Duplicate CTest suite names' }
}

function Get-QualificationStatus($Phases, [string[]]$Required) {
    foreach ($name in $Required) {
        $phase = @($Phases | Where-Object { $_.name -eq $name })
        if ($phase.Count -ne 1 -or $phase[0].status -ne 'pass') { return 'INCOMPLETE' }
    }
    return 'PASS'
}

function Invoke-QualifiedControl([string]$Client, [int]$Port, [string]$Method, $Parameters = @{}, [int]$TimeoutMs = 5000) {
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $Client; $start.UseShellExecute = $false; $start.CreateNoWindow = $true
    $start.RedirectStandardInput = $true; $start.RedirectStandardOutput = $true; $start.RedirectStandardError = $true
    $start.StandardInputEncoding = [Text.UTF8Encoding]::new($false)
    $start.StandardOutputEncoding = [Text.UTF8Encoding]::new($false)
    foreach ($arg in @('--port', [string]$Port, $Method, '-')) { $start.ArgumentList.Add($arg) }
    $clientProcess = [Diagnostics.Process]::Start($start)
    try {
        $clientProcess.StandardInput.Write(($Parameters | ConvertTo-Json -Depth 20 -Compress)); $clientProcess.StandardInput.Close()
        $stdout = $clientProcess.StandardOutput.ReadToEndAsync(); $stderr = $clientProcess.StandardError.ReadToEndAsync()
        if (-not $clientProcess.WaitForExit($TimeoutMs)) { $clientProcess.Kill(); $clientProcess.WaitForExit(); throw "Control timeout: $Method" }
        if ($clientProcess.ExitCode -ne 0) { throw "Control failed: $Method $($stderr.GetAwaiter().GetResult())" }
        $result = $stdout.GetAwaiter().GetResult() | ConvertFrom-Json
        if ($result.error) { throw "Control rejected: $Method $($result.error)" }
        return $result
    } finally { $clientProcess.Dispose() }
}

function Assert-QualifiedProcessAlive($Process, [string]$Context) {
    if (-not $Process) { throw "No owned process is available while $Context" }
    # HasExited queries the owned process handle while the process is live; no
    # process-name lookup or replacement PID can satisfy this check.
    if ($Process.HasExited) {
        $code = [int]$Process.ExitCode
        $hex = '0x{0:X8}' -f ($code -band 0xffffffffL)
        $failure = [InvalidOperationException]::new("Owned process $($Process.Id) exited while $Context; exitCode=$code ($hex)")
        $failure.Data['ProcessId'] = $Process.Id
        $failure.Data['ExitCode'] = $code
        $failure.Data['ExitCodeHex'] = $hex
        throw $failure
    }
}

function Retain-QualifiedProcessHandle([Diagnostics.Process]$Process) {
    Assert-QualifiedProcessAlive $Process 'retaining the discovered process handle'
    # Get-Process objects otherwise query through temporary handles; after the
    # process exits Windows can no longer supply its exit status. Retain now.
    if ($Process.Handle -eq [IntPtr]::Zero) { throw 'Owned process handle could not be retained' }
}

function Stop-QualifiedProcess($Process, [string]$ControlClient = '', [int]$Port = 0, [int]$TimeoutSeconds = 30) {
    if ($Process.HasExited) { return @{ status = 'fail'; detail = 'exited before requested shutdown'; forced = $false } }
    $requested = $false
    $quitResponse = $null
    $controlError = $null
    $deadline = [datetime]::UtcNow.AddSeconds($TimeoutSeconds)
    if ($ControlClient -and $Port -gt 0) {
        try {
            $info = Invoke-QualifiedControl $ControlClient $Port 'info'
            if ([int]$info.pid -ne $Process.Id) { throw 'Control endpoint PID differs from owned process' }
            $requested = $true
            # A clean app can exit before answering app.quit. Its exit/log are authoritative.
            $quitResponse = Invoke-QualifiedControl $ControlClient $Port 'app.quit' @{} ([math]::Min(5000, $TimeoutSeconds * 1000))
        } catch { $controlError = $_.Exception.Message }
    }
    elseif ([System.Runtime.InteropServices.RuntimeInformation]::IsOSPlatform([System.Runtime.InteropServices.OSPlatform]::Windows)) {
        $requested = $Process.CloseMainWindow()
    }
    elseif (Get-Command wmctrl -ErrorAction SilentlyContinue) {
        # wmctrl -c sends WM_DELETE_WINDOW; SIGTERM/XDestroyWindow do not run GLFW teardown.
        foreach ($window in @(& wmctrl -lp)) {
            if ($window -match '^([0-9a-fx]+)\s+\S+\s+(\d+)\s' -and [int]$Matches[2] -eq $Process.Id) {
                & wmctrl -ic $Matches[1]; $requested = $LASTEXITCODE -eq 0
            }
        }
    }
    $remaining = [math]::Max(0, [int]($deadline - [datetime]::UtcNow).TotalMilliseconds)
    if ($requested -and $Process.WaitForExit($remaining)) {
        return @{ status = $(if ($Process.ExitCode -eq 0) { 'pass' } else { 'fail' }); exitCode = $Process.ExitCode; forced = $false; quitResponse = $quitResponse; controlError = $controlError }
    }
    # Kill only the process handle created by this harness, never a process-name match.
    if (-not $Process.HasExited) { $Process.Kill(); $Process.WaitForExit() }
    return @{ status = 'fail'; detail = 'graceful shutdown unavailable or deadline exceeded'; controlError = $controlError; quitResponse = $quitResponse; forced = $true }
}

function Measure-ValidationFindings([string[]]$LogLines) {
    $clean = @($LogLines | ForEach-Object { $_ -replace "$([char]27)\[[0-9;]*m", '' })
    # Exact loader-policy exceptions shared by startup, sustained run and teardown.
    # A VUID always remains an application finding, even if its message contains
    # wording resembling an installed implicit-layer diagnostic.
    $loaderNoise = 'Validation: General: (Removing layer .* because it is a duplicate|Layer .* uses API version .* older than the application specified API version|Layer .* forced disabled because name matches filter of env var)'
    $benignValidation = @($clean | Where-Object { $_ -match '\bWARN\b.*Validation:' -and $_ -match $loaderNoise -and $_ -notmatch '\bVUID-|Validation (Error|Warning)' })
    $errors = @($clean | Where-Object { $_ -match '\b(ERRO|ERROR|FATAL)\b' })
    $valWarn = @($clean | Where-Object { $_ -match '\bWARN\b.*Validation:' -and $_ -notin $benignValidation })
    $valInfo = @($clean | Where-Object { $_ -match '\bINFO\b.*Validation:' })
    $otherWarn = @($clean | Where-Object { $_ -match '\bWARN\b' -and $_ -notmatch 'Validation:' }) + $benignValidation
    $tierLine = @($clean | Where-Object { $_ -match 'validation layer enabled' }) | Select-Object -First 1
    return [pscustomobject]@{
        Errors=$errors; ValWarn=$valWarn; ValInfo=$valInfo; OtherWarn=$otherWarn
        Tier=if ($tierLine) { ($tierLine -replace '^.*validation layer enabled', 'validation layer enabled').Trim() } else { 'unknown' }
        Failing=$errors.Count+$valWarn.Count+$valInfo.Count
    }
}

function Assert-ShutdownLog([string[]]$Lines) {
    if (-not ($Lines -match 'Application shutdown complete\.')) { throw 'Final application shutdown marker missing' }
    $findings=Measure-ValidationFindings $Lines
    if ($findings.Failing) { throw "Error or validation diagnostic in lifecycle log: $(($findings.Errors+$findings.ValWarn+$findings.ValInfo | Select-Object -First 1))" }
}
