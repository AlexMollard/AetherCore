# MCP setup

Install steps for the MCP servers this repo's agents expect. If an agent reports a missing server, install it from here.

> **Paths use `<user>` as a placeholder for your Windows username.** Substitute your own — e.g. `alexm` on the home PC, `alex.mollard` on the work PC. In PowerShell you can also use `$env:USERPROFILE` (or `%USERPROFILE%` inside `.cmd` files) to avoid hand-editing per machine.

| Server | Install command |
|--------|----------------|
| **Mind** | `git clone https://github.com/GabrielMartinMoran/mind.git C:\Users\<user>\mind && cd C:\Users\<user>\mind && pip install -e . && mind setup codex && mind setup opencode` |
| **AetherCore MCP** | Built into this repository. Run `./scripts/Install-Mcp.ps1` to register it with Codex, OpenCode, and Claude Code. See [AetherCore MCP](#aethercore-mcp) below. |
| **clangd-mcp** | `git clone https://github.com/felipeerias/clangd-mcp-server.git C:\Users\<user>\clangd-mcp-server && cd C:\Users\<user>\clangd-mcp-server && npm install && npx tsc` |
| **clangd-mcp launcher** | Create `C:\Users\<user>\.bun\bin\clangd-mcp.cmd`: `node "C:\Users\<user>\clangd-mcp-server\build\index.js"` |
| **Token Optimizer** | `npm install -g @cocaxcode/token-optimizer-mcp` |
| **Context7** | Used via `npx @upstash/context7-mcp` |
| **GitHub MCP** | Download `github-mcp-server_Windows_x86_64.zip` from releases, extract to `C:\Users\<user>\.bun\bin\github-mcp-server.exe`. Set `GITHUB_PERSONAL_ACCESS_TOKEN` env var. |
| **codebase-memory** | Download `codebase-memory-mcp-windows-amd64.zip` from releases, extract to `$env:LOCALAPPDATA\Programs\codebase-memory-mcp\codebase-memory-mcp.exe`. Run `codebase-memory-mcp install -y`. If Opencode wasn't auto-detected, add manually to `~\.config\opencode\opencode.jsonc`. |
| **graphify** | CLI that builds the `graphify-out/` knowledge graph. Surfaced to OpenCode via `.opencode/plugins/graphify.js` and to Claude Code as the `/graphify` skill. Rebuild with `graphify update .`; query with `graphify query "<question>"`. |
| **vs-mcp** | See [VisualStudio-MCP.md](VisualStudio-MCP.md) for setup. |

## AetherCore MCP

This repository ships its own stdlib-only MCP server at
`tools/mcp/aethercore_mcp.py`. It can run the build-and-test gauntlet without a
running editor, and can control a live editor through the local `aether-ctl`
bridge: scene and entity operations, editor panels, render-graph inspection,
GPU texture captures, screenshots, and play mode.

Build the editor and bridge, then register the server with the coding tools you
use:

```powershell
cmake --build --preset default --target Editor aether-ctl
./scripts/Install-Mcp.ps1
# Or limit registration, for example: ./scripts/Install-Mcp.ps1 -Targets codex
```

For live tools, start the Launcher: its small hub endpoint binds to `127.0.0.1`
on `8787` by default and exposes hub status, recent projects, and screenshots.
When you open a project, the Launcher hands that same port to the full Editor
endpoint automatically. `GameRuntime` never includes either endpoint.

```powershell
.\build\default\src\app\RelWithDebInfo\Launcher.exe
```

Alternatively, start a project-scoped Editor directly with a matching port:
`$env:AETHER_CONTROL_PORT = "8787"; .\build\default\src\app\RelWithDebInfo\Editor.exe --project <path>`.
You can also use the editor's **Control Server** panel to start or stop its
endpoint and choose its port. `run_gauntlet` needs no live app. The complete
tool catalog, manual registration examples, and environment variables are in
[tools/mcp/README.md](../tools/mcp/README.md).
