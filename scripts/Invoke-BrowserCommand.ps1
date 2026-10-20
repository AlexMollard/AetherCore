function Invoke-BrowserCommand([string]$Executable, [string[]]$CommandArguments, [string]$Log, [int]$TimeoutMs)
{
    if ($TimeoutMs -le 0) { throw [TimeoutException]::new('Browser operation deadline expired') }
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $Executable
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    foreach ($argument in $CommandArguments) { $start.ArgumentList.Add($argument) }
    $owned = [Diagnostics.Process]::new()
    $owned.StartInfo = $start
    $started = $false
    try
    {
        [void]$owned.Start()
        $started = $true
        $output = $owned.StandardOutput.ReadToEndAsync()
        $errors = $owned.StandardError.ReadToEndAsync()
        if (!$owned.WaitForExit($TimeoutMs))
        {
            $owned.Kill($true)
            [void]$owned.WaitForExit(10000)
            throw [TimeoutException]::new('Browser operation deadline expired; owned process tree terminated')
        }
        return $owned.ExitCode
    }
    finally
    {
        if ($started -and !$owned.HasExited)
        {
            $owned.Kill($true)
            [void]$owned.WaitForExit(10000)
        }
        if ($output -and $errors)
        {
            $text = if ($output.Wait(2000)) { $output.Result } else { 'Output pipe did not close' }
            $text += if ($errors.Wait(2000)) { $errors.Result } else { 'Error pipe did not close' }
            [IO.File]::WriteAllText($Log, $text)
        }
        $owned.Dispose()
    }
}
