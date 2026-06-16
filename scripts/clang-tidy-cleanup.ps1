$ErrorActionPreference = 'Continue'
$repoRoot = 'D:\AetherCore'
$clangTidy = 'C:\Program Files\LLVM\bin\clang-tidy.exe'
$buildDir = "$repoRoot\build-ninja-clang"

# Organized output directories
$outDir = 'D:\AetherCore\audit'
$fixesDir = "$outDir\fixes"
$statusPath = "$outDir\status.txt"

New-Item -ItemType Directory -Path $outDir -Force | Out-Null
New-Item -ItemType Directory -Path $fixesDir -Force | Out-Null
Remove-Item "$outDir\*.txt" -ErrorAction SilentlyContinue
Remove-Item "$fixesDir\*.yaml" -ErrorAction SilentlyContinue
Set-Location $repoRoot

# 1. C++23 & Vulkan 1.4 Optimized Checks
$checks = @(
    # 🧹 Dead code & Unused 
    'misc-unused-*', 'bugprone-unused-*', 'readability-redundant-*',
    'clang-analyzer-deadcode.*', 'clang-analyzer-core.uninitialized.*',
    
    # 🛡️ Core Guidelines (Memory Safety & Architecture)
    'cppcoreguidelines-special-member-functions', # Rule of 5 enforcement
    'cppcoreguidelines-slicing',                  # Catches silent object slicing
    'cppcoreguidelines-pro-type-cstyle-cast',     # Bans dangerous C-style casts (Use // NOLINT for Vulkan pNext)
    
    # 🧵 Concurrency
    'concurrency-mt-unsafe',                      # Flags non-thread-safe C functions
    
    # 🚀 C++23 Modernization & Performance
    'modernize-use-std-print',                    # Replaces printf/cout with C++23 std::print (LLVM 18+)
    'modernize-use-std-numbers',                  # Replaces 3.14f with std::numbers::pi_v
    'bugprone-unchecked-optional-access',         # Prevents crashes on std::optional/expected
    'modernize-use-nodiscard',                    # Prevents ignoring error codes/VkResult
    'modernize-pass-by-value',                    # Optimizes unnecessary const-ref copies
    'performance-noexcept-move-constructor',      # Ensures fast vector reallocations
    'modernize-use-using',                        # typedef -> using
    'modernize-loop-convert',                     # C-for -> range-based for
    
    # 🧹 Deep Readability
    'readability-convert-member-functions-to-static', 
    'readability-container-size-empty',           
    
    # ❌ EXPLICITLY DISABLED FOR VULKAN / ENGINES
    '-readability-qualified-auto',                # Stops the auto VkDevice handle issue
    '-cppcoreguidelines-owning-memory',           # CRITICAL: Stops it from demanding unique_ptr for VkHandles
    '-modernize-use-auto',                        # Stops aggressive type stripping
    '-cppcoreguidelines-pro-bounds-pointer-arithmetic', # Allows custom allocator math
    '-readability-magic-numbers',                 # Allows math/rendering constants
    '-cppcoreguidelines-avoid-magic-numbers'      # Allows math/rendering constants
) -join ','

$checkArg = '-*,' + $checks

# 2. Inject Compiler Warnings for C++23
$extraArgs = @(
    # 🚨 CRITICAL: Force C++23 AST Parsing
    '--extra-arg=-std=c++23', # (Use -std=c++2b if on Clang 15/16)
    
    # Unused code injection
    '--extra-arg=-Wunused-function',
    '--extra-arg=-Wunused-member-function',
    '--extra-arg=-Wunused-variable',
    '--extra-arg=-Wunused-label',
    '--extra-arg=-Wunused-macros',
    
    # 🛡️ Future-Proofing & Tech Debt Args
    '--extra-arg=-Wold-style-cast',           # Force static_cast/reinterpret_cast
    '--extra-arg=-Wnon-virtual-dtor',         # Catch polymorphic memory leaks
    '--extra-arg=-Wshadow',                   # Catch variables hiding members
    '--extra-arg=-Wimplicit-fallthrough',     # Catch missing switch breaks
    '--extra-arg=-Wdeprecated-declarations',  # Hunt down dead APIs (e.g., deprecated Vulkan extensions)
    '--extra-arg=-Wheader-hygiene'            # Ban 'using namespace' in headers
)

# Faster file discovery using Where-Object instead of -Include
$cppFiles = Get-ChildItem "$repoRoot\src" -Recurse -File | Where-Object { $_.Extension -match '^\.(cpp|cc|cxx|c)$' } | Sort-Object FullName
$total = $cppFiles.Count
"Total: $total files" | Out-File -FilePath $statusPath -Encoding utf8

# 3. Dynamic concurrency based on CPU cores (leave 1 core for OS)
$maxJobs = [Math]::Max(1, [Environment]::ProcessorCount - 1)
Write-Host "Using $maxJobs parallel jobs (System has $([Environment]::ProcessorCount) logical processors)"

$runspacePool = [runspacefactory]::CreateRunspacePool(1, $maxJobs)
$runspacePool.Open()

$jobs = @()
foreach ($f in $cppFiles) {
    $rel = $f.FullName.Substring($repoRoot.Length + 1) -replace '\\','/'
    # Unique YAML file per source file for safe bulk applying later
    $fixFile = "$fixesDir\$($f.BaseName)_$($f.GetHashCode()).yaml"
    
    $ps = [powershell]::Create()
    $ps.RunspacePool = $runspacePool
    $null = $ps.AddScript({
        param($file, $rsTidy, $rsChecks, $rBuildDir, $rExtraArgs, $rFixFile)
        
        # --header-filter ensures we ALSO lint the .h files included by this .cpp
        $argsList = @("-p", $rBuildDir, "--checks=$rsChecks", "--quiet", "--header-filter=.*src.*")
        $argsList += $rExtraArgs
        $argsList += "--export-fixes=$rFixFile"
        $argsList += $file
        
        $output = & $rsTidy $argsList 2>&1
        
        $lines = @()
        foreach ($line in $output) {
            if ($line -match 'warning:|error:') {
                $lines += $line
            }
        }
        $lines
    }).AddArgument($f.FullName).AddArgument($clangTidy).AddArgument($checkArg).AddArgument($buildDir).AddArgument($extraArgs).AddArgument($fixFile)
    
    $h = $ps.BeginInvoke()
    $jobs += [PSCustomObject]@{ 
        PS = $ps
        Handle = $h 
        Rel = $rel 
        Processed = $false
    }
}

Write-Host "Started $($jobs.Count) jobs. Waiting for completion..."
$done = 0

# Stream writing to prevent Out-Of-Memory errors on massive projects
$summaryPath = "$outDir\summary.txt"
Remove-Item $summaryPath -ErrorAction SilentlyContinue
$summaryStream = [System.IO.StreamWriter]::new($summaryPath, $true, [System.Text.Encoding]::UTF8)
$summaryStream.AutoFlush = $false

# 4. Process jobs as they finish to avoid the "straggler" bottleneck
while ($done -lt $jobs.Count) {
    $completedJobs = $jobs | Where-Object { $_.Handle.IsCompleted }
    foreach ($j in $completedJobs) {
        if ($j.Processed) { continue }
        
        $output = $j.PS.EndInvoke($j.Handle)
        $j.PS.Dispose()
        $j.Processed = $true
        $done++
        
        if ($output) {
            foreach ($line in $output) {
                $summaryStream.WriteLine($line)
            }
        }
        
        if ($done % 50 -eq 0 -or $done -eq $jobs.Count) {
            $summaryStream.Flush()
            "$done/$total done - last: $($j.Rel)" | Out-File -FilePath $statusPath -Encoding utf8
            Write-Host "  [$done/$total] $($j.Rel)"
        }
    }
    Start-Sleep -Milliseconds 100 # Prevent CPU spinning
}

$summaryStream.Flush()
$summaryStream.Close()
$runspacePool.Close()
$runspacePool.Dispose()

Write-Host "All done. Output: $summaryPath"
Write-Host "Fixes exported to: $fixesDir"
Write-Host "To apply fixes automatically, run:"
Write-Host "clang-apply-replacements $fixesDir"