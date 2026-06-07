#!/usr/bin/env pwsh
<#
.SYNOPSIS
    Format all C++ source files with clang-format using the project's .clang-format.
.DESCRIPTION
    Finds .cpp/.hpp/.h files under src/ and tools/ (excluding build dirs) and
    runs clang-format on them. Pass --check to validate without modifying.
.PARAMETER Check
    If set, checks formatting without modifying files (non-zero exit on diffs).
.EXAMPLE
    ./scripts/Format.ps1
    ./scripts/Format.ps1 -Check
#>

param(
    [switch]$Check
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot

# --- Locate clang-format -------------------------------------------------
$ClangFormat = Get-Command "clang-format.exe" -ErrorAction SilentlyContinue
if (-not $ClangFormat) {
    $Candidates = @(
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Professional\VC\Tools\Llvm\bin\clang-format.exe"
        "${env:ProgramFiles}\LLVM\bin\clang-format.exe"
        "${env:LOCALAPPDATA}\Microsoft\WinGet\Packages\LLVM.ClangFormat_Microsoft.Winget.Source_8wekyb3d8bbwe\clang-format.exe"
    )
    foreach ($c in $Candidates) {
        if (Test-Path $c) { $ClangFormat = $c; break }
    }
}
if (-not $ClangFormat) {
    Write-Error "clang-format not found. Install via: winget install LLVM.ClangFormat"
    exit 1
}
$Exe = $ClangFormat.Source ?? $ClangFormat

# --- Gather source files -------------------------------------------------
$Dirs = @(
    "$RepoRoot\src"
    "$RepoRoot\tools"
)
$ExcludeDirs = @('build', 'build-*', 'out', '_deps')

$Files = Get-ChildItem -Recurse -File -Include '*.cpp', '*.hpp', '*.h' -Path $Dirs |
    Where-Object {
        $dir = $_.DirectoryName.Replace($RepoRoot, '').TrimStart('\')
        -not ($dir -match '^build' -or $dir -match '^out' -or $dir -match '_deps' -or $dir -match 'CMakeFiles')
    }

if ($Files.Count -eq 0) {
    Write-Host "No source files found." -ForegroundColor Yellow
    exit 0
}

Write-Host "Found $($Files.Count) source files" -ForegroundColor Cyan

# --- Run clang-format ----------------------------------------------------
$Flag = if ($Check) { '--dry-run --Werror' } else { '-i' }

$elapsed = Measure-Command {
    $Files | ForEach-Object -Parallel {
        $exe = $using:Exe
        $flag = $using:Flag
        $style = "$using:RepoRoot\.clang-format"
        $result = & $exe --style=file:$style $flag $_.FullName 2>&1
        if ($LASTEXITCODE -ne 0) {
            Write-Host "FAIL  $($_.FullName)" -ForegroundColor Red
            $result | ForEach-Object { Write-Host "  $_" -ForegroundColor DarkRed }
        } elseif (-not $flag.Contains('dry-run')) {
            Write-Host "OK    $($_.FullName)" -ForegroundColor Green
        }
    } -ThrottleLimit 16
}

if ($Check) {
    Write-Host "`nFormat check complete in $($elapsed.TotalSeconds.ToString('F1'))s" -ForegroundColor Cyan
} else {
    Write-Host "`nFormatted $($Files.Count) files in $($elapsed.TotalSeconds.ToString('F1'))s" -ForegroundColor Cyan
}
