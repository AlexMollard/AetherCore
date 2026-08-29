#!/usr/bin/env pwsh
# GPU abstraction guard
#
# Cheap local pre-submit check that mirrors
# .github/workflows/gpu-abstraction-guard.yml. Use this before pushing
# to catch regressions locally without waiting for CI.
#
# Usage (from repo root):
#   pwsh scripts/check-gpu-abstraction.ps1
#
# Exit 0 = clean. Non-zero = one or more leaks detected.

param(
    # Rewrite the baseline from what the tree currently contains. For when a tolerated line
    # is legitimately reworded - the entry then matches nothing and its violation reads as
    # new - not for waving through violations you just introduced. Review the diff.
    [switch]$UpdateBaseline
)

$ErrorActionPreference = 'Stop'

# Engine dirs that must remain vulkan-free. These match the audit
# doc's "non-gpu, non-vulkan" category.
$engineDirs = @(
    'src/engine/rendering',
    'src/engine/passes',
    'src/engine/ui',
    'src/engine/physics',
    'src/engine/material',
    'src/engine/mesh',
    'src/engine/assets',
    'src/engine/animation',
    'src/engine/scene',
    'src/engine/utils',
    'src/engine/camera',
    'src/engine/platform',
    'src/engine/io',
    # Added when the guard was found scanning a 'text' directory that no longer exists
    # (rg errored per-check rather than failing, so those checks quietly did nothing) while
    # missing five live subsystems. A list of directories drifts silently; the entries below
    # are the ones that existed and were never covered.
    'src/engine/memory',
    'src/engine/net',
    'src/engine/particles',
    'src/engine/physics2d',
    'src/engine/scripting',
    'src/engine/AetherCore.cpp',
    'src/engine/AetherCore.hpp'
)

# gpu/*.hpp headers are the public surface of the GPU facade. They must
# also be vulkan-free so including them in engine TUs does not leak
# Vulkan tokens across the abstraction layer.
$gpuHeaders = Get-ChildItem -Path 'src/engine/gpu' -Filter '*.hpp' -Recurse

$failed = $false

# Violations that predate the guard being enforced, one normalised match per line.
# The point is a ratchet, not amnesty: anything in here is tolerated, anything NEW
# fails. Clearing an entry means deleting its line - the file only shrinks.
$baselineFile = Join-Path $PSScriptRoot 'gpu-abstraction-baseline.txt'
$baseline = @{}
if (Test-Path $baselineFile) {
    Get-Content $baselineFile | ForEach-Object {
        $line = $_.Trim()
        if ($line -and -not $line.StartsWith('#')) { $baseline[$line] = $false }
    }
}

# Paths arrive with mixed separators and variable indentation depending on which rg
# invocation produced them; without normalising, the same violation would not match
# its own baseline entry.
function Get-NormalisedMatch([string]$text) {
    $normalised = ($text -replace '\\', '/' -replace '\s+', ' ').Trim()

    # Drop the line number, keeping "<path>: <code>".
    #
    # The baseline is a ratchet: a tolerated violation stays tolerated until it is fixed.
    # Keying on the line number broke that, because editing anything ABOVE a tolerated
    # line renumbers it, so the same violation stops matching its own entry and is
    # reported as new. That is what had this guard failing on every commit for days while
    # reporting "no new ones" in the same breath.
    #
    # The staleness check below already says line numbers move and that treating that as
    # an error would make the guard flaky. This is the same argument applied to matching.
    return ($normalised -replace '^(.+?\.(?:cpp|hpp|h|inl)):\d+:', '$1:')
}

$script:seenViolations = [System.Collections.Generic.List[string]]::new()

function Test-Empty([string]$label, [string[]]$matches) {
    $new = @()
    foreach ($m in $matches) {
        $key = Get-NormalisedMatch $m
        $script:seenViolations.Add($key)
        if ($baseline.ContainsKey($key)) { $baseline[$key] = $true } else { $new += $m }
    }

    if ($new) {
        Write-Host "[FAIL] $label" -ForegroundColor Red
        $new | ForEach-Object { Write-Host "  $_" }
        $script:failed = $true
    } elseif ($matches) {
        Write-Host "[BASE] $label - $($matches.Count) known violation(s), no new ones" -ForegroundColor DarkYellow
    } else {
        Write-Host "[ OK ] $label" -ForegroundColor Green
    }
}

# 1. No non-gpu, non-vulkan engine TU includes <vulkan/*>, volk.hpp,
#    or vk_mem_alloc.h.
$includes = rg -l 'volk\.hpp|<vulkan/|vk_mem_alloc\.h' @engineDirs
Test-Empty 'No <vulkan/*>, volk.hpp, or vk_mem_alloc.h in non-gpu/non-vulkan engine code' $includes

# 1b. No gpu/*.hpp header (the public surface of the GPU facade)
#     includes any Vulkan header. The boundary layer's .cpp files
#     are allowed to include Vulkan (they implement the cast at the
#     seam), but the headers must remain vulkan-free so that no
#     engine TU that consumes them sees a Vulkan token.
$gpuHeaderIncludes = foreach ($h in $gpuHeaders) {
    $content = Get-Content $h.FullName -Raw
    $lines = $content -split "`n"
    for ($i = 0; $i -lt $lines.Length; $i++) {
        $line = $lines[$i]
        if ($line -match '^\s*#\s*include\s*[<"]?(volk\.hpp|<vulkan/|vk_mem_alloc\.h)') {
            "$($h.FullName):$($i+1): $line"
        }
    }
}
Test-Empty 'No <vulkan/*>, volk.hpp, or vk_mem_alloc.h in gpu/*.hpp headers (public surface)' $gpuHeaderIncludes

# 2. No architectural static_cast<Vk*> in non-gpu/non-vulkan code.
$casts = rg 'static_cast<Vk(Buffer|Image|ImageView|CommandBuffer|Pipeline|PipelineLayout|Device|DescriptorSet|DescriptorSetLayout|Queue|CommandPool|Sampler)' @engineDirs
Test-Empty 'No static_cast<Vk*> in non-gpu/non-vulkan engine code' $casts

# 2b. No static_cast<Vk*> or reinterpret_cast<Vk*> in gpu/ at all.
#     The gpu/ dir is the engine-side facade; casts to Vk* belong in vulkan/.
#     Filter comment-only lines (//) so the header documentation
#     referencing these casts in passing does not trigger the check.
#     Use --field-match-separator=' ' so the output is `<lineno> <content>`
#     (Windows paths have colons that confuse a naive split).
# 2b. No static_cast<Vk*> or reinterpret_cast<Vk*> in gpu/ at all.
#     The gpu/ dir is the engine-side facade; casts to Vk* belong in vulkan/.
#     Filter comment-only lines (//) so the header documentation
#     referencing these casts in passing does not trigger the check.
#     Use rg with a perl regex: the format is `<path>:<line>:<content>`.
#     We extract the content (everything after the second `:`) and
#     check whether the rest of the line is a comment.
$gpuCastsRaw = rg -n '(static_cast|reinterpret_cast)<Vk(Buffer|Image|ImageView|CommandBuffer|Pipeline|PipelineLayout|Device|DescriptorSet|DescriptorSetLayout|Queue|CommandPool|Sampler)' src/engine/gpu
$gpuCasts = @()
foreach ($line in $gpuCastsRaw) {
    # rg output on Windows is "<path>:<line>:<content>". Find the second
    # colon and take everything after it. The path itself may contain
    # colons (rare on Windows) but paths to gpu/ are simple
    # "src/engine/gpu/*.hpp" so the second colon is unambiguous.
    $firstColon = $line.IndexOf(':')
    if ($firstColon -lt 0) { continue }
    $rest = $line.Substring($firstColon + 1)
    $secondColon = $rest.IndexOf(':')
    if ($secondColon -lt 0) { continue }
    $content = $rest.Substring($secondColon + 1)
    if (-not $content.TrimStart().StartsWith('//')) {
        $gpuCasts += $line
    }
}
Test-Empty 'No static_cast<Vk*> or reinterpret_cast<Vk*> in gpu/ (must stay in vulkan/)' $gpuCasts

# 2c. No `vk` or `Vk` named identifiers (functions, variables, parameters)
#     anywhere outside vulkan/ and gpu/. The gpu/ dir is allowed to have
#     comments that reference vk* / Vk* types as documentation. Other
#     engine dirs are completely forbidden. Filter comments.
function Get-NonCommentLines([string[]]$raw) {
    $result = @()
    foreach ($line in $raw) {
        $firstColon = $line.IndexOf(':')
        if ($firstColon -lt 0) { continue }
        $rest = $line.Substring($firstColon + 1)
        $secondColon = $rest.IndexOf(':')
        if ($secondColon -lt 0) { continue }
        $content = $rest.Substring($secondColon + 1)
        if (-not $content.TrimStart().StartsWith('//')) {
            $result += $line
        }
    }
    return ,$result
}

$vkFuncCalls = Get-NonCommentLines (rg -n '\bvk[A-Z][a-zA-Z]+\(' src/engine --glob '!src/engine/vulkan/**' --glob '!src/engine/gpu/**')
Test-Empty 'No vk* function calls outside vulkan/ and gpu/' $vkFuncCalls

$vkIdents = Get-NonCommentLines (rg -n '\bvk[A-Z][a-zA-Z]+' src/engine --glob '!src/engine/vulkan/**' --glob '!src/engine/gpu/**')
Test-Empty 'No vk* identifier names outside vulkan/ and gpu/' $vkIdents

$VkIdents = Get-NonCommentLines (rg -n '\bVk[A-Z]\w*' src/engine --glob '!src/engine/vulkan/**' --glob '!src/engine/gpu/**')
Test-Empty 'No Vk* identifier names outside vulkan/ and gpu/' $VkIdents

# 3. No direct vk* function call in non-gpu/non-vulkan engine code.
$calls = rg '\bvk[A-Z][a-zA-Z]+\(' @engineDirs
Test-Empty 'No vk* function calls in non-gpu/non-vulkan engine code' $calls

# 4. The engine-side gpu::GpuProfiler header (gpu/GpuProfiler.hpp) must
#    not contain any `Vk` token in code (a comment-only mention is
#    allowed). Tracy types count as leaks too. Use rg with field-match
#    so each output line is `<lineno> <content>` (Windows paths have
#    colons that confuse a naive split).
$headerLeaks = rg -n --field-match-separator=' ' '\bVk[A-Z]\w*\b' 'src/engine/gpu/GpuProfiler.hpp' |
    ForEach-Object {
        # Strip the leading line number, then check the rest is a comment.
        $trimmed = $_ -replace '^\s*\d+\s+', ''
        if ($trimmed.TrimStart().StartsWith('//')) { return }
        $_
    }
Test-Empty 'No Vk* tokens in gpu/GpuProfiler.hpp (excluding comments)' $headerLeaks

# A baseline entry that no longer matches anything means the violation was fixed. Say so
# rather than failing: the line numbers in here move whenever the file above them changes,
# so treating staleness as an error would make the guard flaky instead of useful.
if ($UpdateBaseline) {
    $header = @(
        '# GPU abstraction baseline - violations that predate the guard being enforced.',
        '#',
        '# A ratchet, not amnesty: everything here is tolerated so CI can be green, and any NEW',
        '# violation fails the build. Shrink this list; never grow it to silence a fresh leak.',
        '#',
        '# Entries carry no line number on purpose - editing anything above a tolerated line',
        '# would otherwise renumber it and make the guard report it as new.',
        '#',
        '# Regenerate with: pwsh scripts/check-gpu-abstraction.ps1 -UpdateBaseline'
    )
    $unique = $script:seenViolations | Sort-Object -Unique
    Set-Content -LiteralPath $baselineFile -Value ($header + $unique)
    Write-Host ""
    Write-Host "Baseline rewritten: $($unique.Count) entr(y/ies)." -ForegroundColor Yellow
    exit 0
}

$stale = $baseline.Keys | Where-Object { -not $baseline[$_] }
if ($stale) {
    Write-Host ""
    Write-Host "$($stale.Count) baseline entr(y/ies) no longer match - delete them from scripts/gpu-abstraction-baseline.txt:" -ForegroundColor Cyan
    $stale | Select-Object -First 10 | ForEach-Object { Write-Host "  $_" -ForegroundColor DarkGray }
}

if ($failed) {
    Write-Host ""
    Write-Host "GPU abstraction guard: FAILED" -ForegroundColor Red
    Write-Host "Rule: engine code outside src/engine/vulkan/ must contain no raw vk*/Vk* tokens - use gpu:: types and typed handles."
    Write-Host "If a violation is genuinely unavoidable for now, add its line to scripts/gpu-abstraction-baseline.txt."
    exit 1
}

Write-Host ""
Write-Host "GPU abstraction guard: CLEAN" -ForegroundColor Green
exit 0
