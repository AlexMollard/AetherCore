#!/usr/bin/env pwsh
<#
.SYNOPSIS
    Register (or remove) the AetherCore MCP server in Codex, OpenCode, and Claude
    Code.
.DESCRIPTION
    Writes the server entry into each tool's own config format, idempotently and
    with a .bak backup for the files it edits:

      * Codex       ~/.codex/config.toml            [mcp_servers.aethercore] table
      * OpenCode    ~/.config/opencode/opencode.jsonc   "mcp": { "aethercore": ... }
      * Claude Code  via `claude mcp add` (user scope); the repo's committed
                     .mcp.json already covers this project.

    The MCP itself is scripts/../tools/mcp/aethercore_mcp.py and needs only Python
    on PATH. Re-running is safe: an existing entry is left alone unless -Force.
.PARAMETER Targets
    Which tools to install into: any of codex, opencode, claude, or all (default).
.PARAMETER Port
    Editor control port baked into the server's env (default 8787).
.PARAMETER Name
    Server name to register (default "aethercore").
.PARAMETER Remove
    Uninstall the entry instead of installing it.
.PARAMETER Force
    Overwrite an existing entry instead of leaving it in place.
.PARAMETER DryRun
    Print what would change without writing anything.
.EXAMPLE
    ./scripts/Install-Mcp.ps1
    ./scripts/Install-Mcp.ps1 -Targets codex,opencode
    ./scripts/Install-Mcp.ps1 -Force
    ./scripts/Install-Mcp.ps1 -Remove
#>

param(
    [ValidateSet("all", "codex", "opencode", "claude")]
    [string[]]$Targets = @("all"),
    [int]$Port = 8787,
    [string]$Name = "aethercore",
    [switch]$Remove,
    [switch]$Force,
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
$RepoFwd = $RepoRoot -replace '\\', '/'
$McpScript = "$RepoFwd/tools/mcp/aethercore_mcp.py"

if ($Targets -contains "all") { $Targets = @("codex", "opencode", "claude") }

# --- Console helpers -----------------------------------------------------
function Write-Head($t) { Write-Host "`n=== $t ===" -ForegroundColor Cyan }
function Write-Ok($t)   { Write-Host "  [OK]   $t" -ForegroundColor Green }
function Write-Skip($t) { Write-Host "  [SKIP] $t" -ForegroundColor DarkGray }
function Write-Warn2($t) { Write-Host "  [WARN] $t" -ForegroundColor Yellow }
function Write-Info2($t) { Write-Host "  $t" -ForegroundColor DarkYellow }

if (-not (Test-Path $McpScript)) {
    Write-Error "MCP script not found at $McpScript"
    exit 1
}
if (-not (Get-Command python -ErrorAction SilentlyContinue)) {
    Write-Warn2 "'python' is not on PATH. The entries still install, but the MCP won't start until Python is available."
}

function Backup-File($path) {
    if ((Test-Path $path) -and -not $DryRun) {
        Copy-Item $path "$path.bak" -Force
    }
}

# --- Codex (~/.codex/config.toml, TOML) ----------------------------------
function Install-Codex {
    Write-Head "Codex"
    $path = Join-Path $HOME ".codex/config.toml"
    $header = "[mcp_servers.$Name]"
    $lines = (Test-Path $path) ? (Get-Content $path) : @()
    $exists = ($lines -join "`n") -match ("(?m)^\s*" + [regex]::Escape($header))

    if (-not $Remove -and $exists -and -not $Force) {
        Write-Skip "already registered in $path (use -Force to overwrite)"
        return
    }

    # Drop any existing [mcp_servers.<name>] table and its .env sub-table.
    $kept = [System.Collections.Generic.List[string]]::new()
    $dropping = $false
    foreach ($line in $lines) {
        if ($line -match '^\s*\[') {
            $dropping = ($line -match ("^\s*\[mcp_servers\." + [regex]::Escape($Name) + "(\.|\])"))
        }
        if (-not $dropping) { $kept.Add($line) }
    }

    if ($Remove) {
        if (-not $exists) { Write-Skip "not present in $path"; return }
        if ($DryRun) { Write-Info2 "(dry-run) would remove $header from $path"; return }
        Backup-File $path
        Set-Content -Path $path -Value $kept -Encoding UTF8
        Write-Ok "removed from $path"
        return
    }

    $block = @(
        ""
        $header
        'command = "python"'
        "args = [`"$McpScript`"]"
        ""
        "[mcp_servers.$Name.env]"
        "AETHER_CONTROL_PORT = `"$Port`""
        "AETHER_REPO = `"$RepoFwd`""
    )
    if ($DryRun) { Write-Info2 "(dry-run) would write $header to $path"; return }
    if (-not (Test-Path (Split-Path $path))) { New-Item -ItemType Directory -Force -Path (Split-Path $path) | Out-Null }
    Backup-File $path
    Set-Content -Path $path -Value ($kept + $block) -Encoding UTF8
    Write-Ok ($exists ? "updated in $path" : "installed to $path")
}

# --- OpenCode (~/.config/opencode/opencode.jsonc, JSON) ------------------
function Install-OpenCode {
    Write-Head "OpenCode"
    $path = Join-Path $HOME ".config/opencode/opencode.jsonc"
    if (-not (Test-Path $path)) { Write-Warn2 "no opencode config at $path; skipping"; return }

    try {
        $obj = Get-Content $path -Raw | ConvertFrom-Json -AsHashtable
    }
    catch {
        Write-Warn2 "could not parse $path (comments/JSONC?). Add this under `"mcp`" manually:"
        Write-Info2 "`"$Name`": { `"type`": `"local`", `"command`": [`"python`", `"$McpScript`"], `"enabled`": true, `"env`": { `"AETHER_CONTROL_PORT`": `"$Port`", `"AETHER_REPO`": `"$RepoFwd`" } }"
        return
    }
    if (-not $obj.ContainsKey("mcp")) { $obj["mcp"] = @{} }
    $exists = $obj["mcp"].ContainsKey($Name)

    if ($Remove) {
        if (-not $exists) { Write-Skip "not present in $path"; return }
        if ($DryRun) { Write-Info2 "(dry-run) would remove mcp.$Name from $path"; return }
        $obj["mcp"].Remove($Name)
        Backup-File $path
        $obj | ConvertTo-Json -Depth 30 | Set-Content -Path $path -Encoding UTF8
        Write-Ok "removed from $path"
        return
    }
    if ($exists -and -not $Force) { Write-Skip "already registered in $path (use -Force to overwrite)"; return }

    $obj["mcp"][$Name] = [ordered]@{
        type    = "local"
        command = @("python", $McpScript)
        enabled = $true
        env     = [ordered]@{ AETHER_CONTROL_PORT = "$Port"; AETHER_REPO = $RepoFwd }
    }
    if ($DryRun) { Write-Info2 "(dry-run) would write mcp.$Name to $path"; return }
    Backup-File $path
    $obj | ConvertTo-Json -Depth 30 | Set-Content -Path $path -Encoding UTF8
    Write-Ok ($exists ? "updated in $path" : "installed to $path")
}

# --- Claude Code (`claude mcp add`, user scope) --------------------------
function Install-Claude {
    Write-Head "Claude Code"
    $claude = Get-Command claude -ErrorAction SilentlyContinue
    if (-not $claude) {
        Write-Warn2 "'claude' CLI not on PATH. The repo's committed .mcp.json already registers the server for THIS project; for a global install run:"
        Write-Info2 "claude mcp add $Name -s user -e AETHER_CONTROL_PORT=$Port -e AETHER_REPO=$RepoFwd -- python `"$McpScript`""
        return
    }

    if ($Remove) {
        if ($DryRun) { Write-Info2 "(dry-run) would run: claude mcp remove $Name -s user"; return }
        & $claude.Source mcp remove $Name -s user 2>&1 | Out-Null
        Write-Ok "removed (user scope); project .mcp.json is unaffected"
        return
    }

    # Idempotent: remove any existing user-scope entry, then add fresh.
    if ($DryRun) { Write-Info2 "(dry-run) would run: claude mcp add $Name -s user -e AETHER_CONTROL_PORT=$Port -e AETHER_REPO=$RepoFwd -- python `"$McpScript`""; return }
    & $claude.Source mcp remove $Name -s user 2>&1 | Out-Null
    & $claude.Source mcp add $Name -s user -e "AETHER_CONTROL_PORT=$Port" -e "AETHER_REPO=$RepoFwd" -- python "$McpScript" 2>&1 | Out-Null
    if ($LASTEXITCODE -eq 0) { Write-Ok "installed (user scope). The project .mcp.json also covers this repo." }
    else { Write-Warn2 "`claude mcp add` returned $LASTEXITCODE; check `claude mcp list`." }
}

# --- Run -----------------------------------------------------------------
Write-Host "AetherCore MCP installer" -ForegroundColor White
Write-Host "  server: $Name  |  script: $McpScript  |  port: $Port  |  action: $(if ($Remove){'remove'}else{'install'})$(if($DryRun){' (dry-run)'})" -ForegroundColor DarkGray

foreach ($t in $Targets) {
    switch ($t) {
        "codex"    { Install-Codex }
        "opencode" { Install-OpenCode }
        "claude"   { Install-Claude }
    }
}

Write-Host "`nDone. Restart each tool (or reload its MCP servers) to pick up the change." -ForegroundColor Cyan
Write-Host "The scene/rendergraph tools need the editor running with the control endpoint on;" -ForegroundColor DarkGray
Write-Host "run_gauntlet works without it." -ForegroundColor DarkGray
