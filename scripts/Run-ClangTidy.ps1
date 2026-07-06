#!/usr/bin/env pwsh
<#
.SYNOPSIS
    Run clang-tidy static analysis on the C++ codebase.
.DESCRIPTION
    Uses compile_commands.json from the clangd CMake preset. Regenerates it
    automatically if missing. Select specific files or paths to limit scope.
.PARAMETER Path
    Source file(s) or directories to analyse (default: src/ tools/).
.PARAMETER Fix
    Apply suggested fixes automatically (clang-tidy -fix).
.PARAMETER Checks
    Comma-separated checks to enable (default: project-wide sensible set).
.PARAMETER Refresh
    Regenerate compile_commands.json before analysing (use after adding/removing
    source files or changing build flags, so clang-tidy sees current commands).
.EXAMPLE
    ./scripts/Run-ClangTidy.ps1
    ./scripts/Run-ClangTidy.ps1 -Path src/engine/gpu -Fix
    ./scripts/Run-ClangTidy.ps1 -Path src/app/main.cpp
#>

param(
    [string]$Path = "",
    [switch]$Fix,
    [string]$Checks = "",
    [switch]$Refresh
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot

# --- Locate clang-tidy ---------------------------------------------------
$ClangTidy = Get-Command "clang-tidy.exe" -ErrorAction SilentlyContinue
if (-not $ClangTidy) {
    $Candidates = @(
        "${env:ProgramFiles}\LLVM\bin\clang-tidy.exe"
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Professional\VC\Tools\Llvm\bin\clang-tidy.exe"
    )
    foreach ($c in $Candidates) {
        if (Test-Path $c) { $ClangTidy = $c; break }
    }
}
if (-not $ClangTidy) {
    Write-Error "clang-tidy not found. Install via: winget install LLVM"
    exit 1
}
$Exe = $ClangTidy.Source ?? $ClangTidy

# --- Ensure compile_commands.json ----------------------------------------
$BuildDir = "$RepoRoot\build-ninja-clang"
$CompileDb = "$BuildDir\compile_commands.json"
if ($Refresh -or -not (Test-Path $CompileDb)) {
    $why = if ($Refresh) { "Refreshing" } else { "Generating" }
    Write-Host "$why compile_commands.json (clangd preset)..." -ForegroundColor Yellow
    Push-Location $RepoRoot
    try {
        & cmake --preset clangd 2>&1 | Out-Host
    } finally {
        Pop-Location
    }
    if (-not (Test-Path $CompileDb)) {
        Write-Error "compile_commands.json still missing after cmake --preset clangd"
        exit 1
    }
}

# --- Gather source files -------------------------------------------------
if ($Path -and (Test-Path $Path -PathType Leaf)) {
    $Files = @(Get-Item $Path)
} elseif ($Path -and (Test-Path $Path -PathType Container)) {
    $resolved = Join-Path $RepoRoot $Path
    if (-not (Test-Path $resolved)) { $resolved = $Path }
    $Files = Get-ChildItem -Recurse -File -Include '*.cpp', '*.hpp', '*.h' -Path $resolved |
        Where-Object { $_.DirectoryName -notmatch '\\build' -and $_.DirectoryName -notmatch '_deps' }
} else {
    $Dirs = @(
        "$RepoRoot\src"
        "$RepoRoot\tools"
    )
    $Files = Get-ChildItem -Recurse -File -Include '*.cpp', '*.hpp', '*.h' -Path $Dirs |
        Where-Object {
            $dir = $_.DirectoryName.Replace($RepoRoot, '').TrimStart('\')
            -not ($dir -match '^build' -or $dir -match '^out' -or $dir -match '_deps' -or $dir -match 'CMakeFiles')
        }
}

if ($Files.Count -eq 0) {
    Write-Host "No source files to analyse." -ForegroundColor Yellow
    exit 0
}

Write-Host "Analysing $($Files.Count) file(s) with clang-tidy..." -ForegroundColor Cyan

# --- Default checks if not specified -------------------------------------
if (-not $Checks) {
    $Checks = @(
        'clang-analyzer-*',
        'bugprone-*',
        'performance-*',
        'modernize-*',
        '-modernize-use-trailing-return-type',
        '-modernize-pass-by-value',
        'readability-*',
        '-readability-identifier-length',
        '-readability-magic-numbers',
        '-readability-convert-member-functions-to-static'
    ) -join ','
}

# --- Run clang-tidy ------------------------------------------------------
$DoFix = [bool]$Fix
$issueFiles = [System.Collections.Concurrent.ConcurrentBag[string]]::new()

$elapsed = Measure-Command {
    $Files | ForEach-Object -Parallel {
        $exe = $using:Exe
        $db = $using:CompileDb
        $checks = $using:Checks
        $doFix = $using:DoFix
        $issues = $using:issueFiles
        $file = $_.FullName

        # clang-tidy analyses .cpp files; headers get checked as they're included.
        # Running on headers produces no useful output and duplicates work.
        if ($file -notmatch '\.cpp$') { return }

        # Build the argument list explicitly. Passing an empty '' string (the old
        # behaviour when -Fix was off) made clang-tidy treat it as an extra source
        # path — the current directory — and emit spurious "expected exactly one
        # compiler job" errors, so only append --fix when actually fixing.
        $tidyArgs = @("--checks=$checks", "-p=$db")
        if ($doFix) { $tidyArgs += '--fix' }
        $tidyArgs += $file

        $output = & $exe @tidyArgs 2>&1
        $hasIssues = ($LASTEXITCODE -ne 0) -or ($output -match 'warning:' -or $output -match 'error:')

        if ($hasIssues) {
            $issues.Add($file)
            Write-Host "[!] $file" -ForegroundColor Yellow
            $output | ForEach-Object {
                if ($_ -match 'warning:') {
                    Write-Host "    $_" -ForegroundColor DarkYellow
                } elseif ($_ -match 'error:') {
                    Write-Host "    $_" -ForegroundColor Red
                }
            }
        } else {
            Write-Host "[ ] $file" -ForegroundColor DarkGreen
        }
    } -ThrottleLimit 8
}

$count = $issueFiles.Count
Write-Host "`nclang-tidy finished in $($elapsed.TotalSeconds.ToString('F1'))s - $count file(s) with issues" -ForegroundColor Cyan
if ($count -gt 0) { exit 1 }
