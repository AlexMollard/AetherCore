# AetherCore MCP server

A dependency-free (Python stdlib only) [Model Context Protocol](https://modelcontextprotocol.io)
server that gives an AI agent two capabilities over AetherCore:

1. **Run the test gauntlet** — build + unit + (optionally) GPU validation smokes,
   returning the structured pass/fail report. No running engine needed.
2. **Drive the live editor** — query engine state, list/create/delete entities,
   edit transforms, and dump the render graph, by shelling out to the `aether-ctl`
   binary which speaks the editor's JSON-over-ENet control endpoint.

ENet is a C library with no maintained Python binding, so it lives entirely on
the C++ side (`src/app/editor/ControlServer`, `tools/control-client/aether-ctl`).
This server only spawns subprocesses — it imports nothing beyond the stdlib.

```
 MCP client (opencode / Claude)
        │  JSON-RPC over stdio
        ▼
 aethercore_mcp.py ──subprocess──► pwsh Run-DebugGauntlet.ps1   (run_gauntlet)
        │
        └────────────subprocess──► aether-ctl ──ENet──► editor  (scene / rendergraph)
```

## Tools

| Tool | Needs editor running? | Purpose |
|------|:---:|---------|
| `run_gauntlet` | no | Build App/GameRuntime/EngineTests, run the unit suite; `mode:"full"` also runs the GPU validation smokes. Returns `gauntlet-report.json`. |
| `engine_info` | yes | Scene name, entity count, frame index, fps. |
| `list_entities` | yes | Every entity: id, name, world position. |
| `get_entity` | yes | One entity's full detail: name, position, scale, component types. |
| `create_entity` | yes | Create an entity (`name`, `position`). Returns its id. |
| `rename_entity` | yes | Rename an entity by `id`. |
| `parent_entity` | yes | Set an entity's parent by `id` (`parent:0` unparents to root). |
| `set_transform` | yes | Set an entity's `position` / `rotationEuler` (deg) / `scale` by `id`. |
| `add_component` / `remove_component` | yes | Add/remove a core component (`Transform`, `Name`, `Hierarchy`) by `id`. |
| `delete_entity` | yes | Delete an entity by `id`. |
| `save_scene` | yes | Save the live scene to its committable `.scene.toml`. |
| `load_scene` | yes | Switch the editor to a scene by `name`. |
| `new_scene` | yes | Replace the live scene with a fresh empty one. |
| `play` / `stop` / `toggle_play` | yes | Enter/exit Play mode (mirrors the editor Play button; `play` rebuilds + starts the C# scripts). |
| `query_rendergraph` | yes | Every compiled pass: type, dependencies, produced/consumed frame products, CPU time. |

Rich components (mesh/material/physics) stay inspector-authored; they need
meshes/materials/defaults the Add-Component palette wires. A viewport screenshot
tool is the remaining gap (needs GPU readback).

The editor exposes a **Control Server** panel (Window menu) to start/stop the
endpoint, pick the port, toggle auto-start, and watch live request stats.

## Prerequisites

```powershell
# 1. Build the editor and the control client
cmake --build build-vs2022-msvc --config Debug --target App aether-ctl

# 2. For the live-editor tools, run the editor with the control endpoint on:
$env:AETHER_CONTROL_PORT = "8787"
.\build-vs2022-msvc\src\app\Debug\App.exe   # (run from the build-tree root)
```

`run_gauntlet` works without step 2.

## Install into Codex / OpenCode / Claude Code (one command)

```powershell
./scripts/Install-Mcp.ps1            # all three; -Targets codex,opencode to pick
./scripts/Install-Mcp.ps1 -DryRun    # preview
./scripts/Install-Mcp.ps1 -Remove    # uninstall
```

It writes the entry in each tool's own format (Codex `~/.codex/config.toml`,
OpenCode `~/.config/opencode/opencode.jsonc`, Claude Code via `claude mcp add`),
idempotently and with a `.bak` backup. Restart each tool afterwards. The manual
steps below are the equivalent if you'd rather do it by hand.

## Registration (opencode)

Add to `opencode.jsonc` under `"mcp"`:

```jsonc
"aethercore": {
  "type": "local",
  "command": ["python", "D:\\AetherCore\\tools\\mcp\\aethercore_mcp.py"],
  "enabled": true,
  "env": { "AETHER_REPO": "D:\\AetherCore", "AETHER_CONTROL_PORT": "8787" }
}
```

## Configuration (environment)

| Var | Default | Meaning |
|-----|---------|---------|
| `AETHER_REPO` | two levels above this file | Repo root. |
| `AETHER_BUILD_DIR` | first of `build-vs2022-msvc`, `build`, `build-ninja-clang` | CMake build dir. |
| `AETHER_CTL` | found under the build dir | Path to `aether-ctl(.exe)`. |
| `AETHER_CONTROL_PORT` | `8787` | Editor control port (must match the editor's `AETHER_CONTROL_PORT`). |
| `AETHER_PWSH` | `pwsh` | PowerShell used to run the gauntlet. |

## Quick manual check (no MCP client)

```bash
printf '%s\n%s\n' \
  '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}' \
  '{"jsonrpc":"2.0","id":2,"method":"tools/list"}' \
  | python tools/mcp/aethercore_mcp.py
```
