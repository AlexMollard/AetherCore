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
| `run_gauntlet` | no | Build Editor/GameRuntime/EngineTests, run the unit suite; `mode:"full"` also runs the GPU validation smokes. Returns `gauntlet-report.json`. |
| `launcher_info` / `list_projects` | Launcher | Inspect hub and recent-project state before handoff. |
| `open_project` / `create_project` | Launcher | Open an existing project or create a `blank_2d` / `blank_3d` project, then hand the same control port to the Editor. |
| `engine_info` | yes | Scene name and 2D/3D kind, entity count, frame index, fps. |
| `get_console_log` | yes | Bounded, filterable live console tail with levels, categories, source locations, and monotonic sequence ids for polling. |
| `list_entities` | yes | Every entity: id, name, world position. |
| `get_entity` | yes | One entity's full detail: name, position, scale, component types. |
| `select_entity` / `get_selection` | yes | Set / read the editor's entity selection (the Inspector shows the primary). `select_entity` first, then `set_window` the Inspector open, then `screenshot` to *see* an entity's components. |
| `list_windows` / `set_window` | yes | List every editor panel + visibility; open/close one by name (case-insensitive). Lets the agent stage the editor before a screenshot. |
| `inspect_component` | yes | Open the Inspector and scroll a named component's drawer into view (force-opening it), e.g. `Rigid Body`. Select an entity first, then screenshot that component. |
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
| `render_stats` | yes | Render-graph frame profile: pass/barrier counts, transient cache hit/miss, transient GPU heap used vs. capacity, fps. |
| `render_benchmark` | yes | Per-pass CPU-time benchmark of the last frame, slowest-first, plus hottest pass and total graph CPU ms. Poll to sample min/avg/max. |
| `list_textures` | yes | Every registered texture / render target: name, format, aspect, extent, mips, layers, bindless slot. |
| `capture_texture` | yes | Capture *any* registered texture (name from `list_textures`) to a compressed `.png` — 8-bit color, depth (normalized grayscale), or HDR (tonemapped). Lets the agent *see* shadow maps, GBuffer, scene color, asset textures — not just the viewport. |
| `scene_stats` | yes | Per-component-type entity-count histogram over the live ECS. |
| `list_lights` | yes | Every light: id, name, type (point/spot), position, color, intensity, radius, shadow flag (spots add cone angles + aim). |
| `camera_info` | yes | Active camera: projection, world position, forward direction, vertical FOV, and orthographic height. |
| `get_settings` | yes | Current engine settings: resolution, vsync, target fps. |
| `list_component_types` | yes | The 28 components the ComponentCatalog can add (name + category). |
| `screenshot` | yes | Capture the current editor frame to a compressed `.png` and return its path — lets the agent *see* what's rendered. |

Rich components (mesh/material/physics) stay inspector-authored; they need
meshes/materials/defaults the Add-Component palette wires. The agent can now
*see* any GPU texture (`capture_texture`), not just the viewport, and query
render-graph, scene, light, camera and settings state across subsystems.

The editor exposes a **Control Server** panel (Window menu) to start/stop the
endpoint, pick the port, toggle auto-start, and watch live request stats.

## Prerequisites

```powershell
# 1. Build the editor and the control client
cmake --build build/vs2022-msvc --config Debug --target Editor aether-ctl

# 2. Launch the hub. Its localhost MCP endpoint starts on 8787 by default, so
#    launcher_info, list_projects, open_project, create_project, and screenshot
#    are available immediately. The same port is handed to the spawned Editor:
.\build\vs2022-msvc\src\app\Debug\Launcher.exe   # run from the build-tree root; open a project in the hub
```

`run_gauntlet` works without step 2.

### Launcher → Editor MCP handoff

The Launcher runs a small localhost control endpoint on its UiShell runtime. It exposes
only hub-safe methods: `launcher_info`, `list_projects`, `open_project`, `create_project`, and `screenshot`. When the user
opens or creates a project, the Launcher stops that endpoint immediately before spawning
the Editor; the Editor inherits the same `AETHER_CONTROL_PORT`. The MCP client therefore
stays on one port across the handoff. The cached MCP manifest retains the union of Launcher
and Editor tools, so fixed tool-list clients can drive both sides of the transition.
Resolution (read once at launcher startup from
`AETHER_CONTROL_PORT`):

| `AETHER_CONTROL_PORT` on the Launcher | Behaviour |
|---------------------------------------|-----------|
| unset (default)                       | **8787** - Launcher endpoint, then the spawned Editor |
| a port, e.g. `9000`                   | Launcher endpoint, then spawned Editor, on that port |
| `off` / `none` / `0`                  | disabled - no Launcher or spawned-Editor endpoint |

You can still bypass the Launcher and run the Editor directly:
`$env:AETHER_CONTROL_PORT="8787"; .\build\vs2022-msvc\src\app\Debug\Editor.exe --project <path>`.

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
| `AETHER_BUILD_DIR` | first of `build/vs2022-msvc`, `build/default`, `build/ninja-clang` | CMake build dir. |
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
