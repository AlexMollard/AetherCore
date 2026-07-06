# MCP setup

Install steps for the MCP servers this repo's agents expect. If an agent reports a missing server, install it from here.

> **Paths use `<user>` as a placeholder for your Windows username.** Substitute your own — e.g. `alexm` on the home PC, `alex.mollard` on the work PC. In PowerShell you can also use `$env:USERPROFILE` (or `%USERPROFILE%` inside `.cmd` files) to avoid hand-editing per machine.

| Server | Install command |
|--------|----------------|
| **Mind** | `git clone https://github.com/GabrielMartinMoran/mind.git C:\Users\<user>\mind && cd C:\Users\<user>\mind && pip install -e . && mind setup codex && mind setup opencode` |
| **clangd-mcp** | `git clone https://github.com/felipeerias/clangd-mcp-server.git C:\Users\<user>\clangd-mcp-server && cd C:\Users\<user>\clangd-mcp-server && npm install && npx tsc` |
| **clangd-mcp launcher** | Create `C:\Users\<user>\.bun\bin\clangd-mcp.cmd`: `node "C:\Users\<user>\clangd-mcp-server\build\index.js"` |
| **Token Optimizer** | `npm install -g @cocaxcode/token-optimizer-mcp` |
| **Context7** | Used via `npx @upstash/context7-mcp` |
| **GitHub MCP** | Download `github-mcp-server_Windows_x86_64.zip` from releases, extract to `C:\Users\<user>\.bun\bin\github-mcp-server.exe`. Set `GITHUB_PERSONAL_ACCESS_TOKEN` env var. |
| **codebase-memory** | Download `codebase-memory-mcp-windows-amd64.zip` from releases, extract to `$env:LOCALAPPDATA\Programs\codebase-memory-mcp\codebase-memory-mcp.exe`. Run `codebase-memory-mcp install -y`. If Opencode wasn't auto-detected, add manually to `~\.config\opencode\opencode.jsonc`. |
| **graphify** | CLI that builds the `graphify-out/` knowledge graph. Surfaced to OpenCode via `.opencode/plugins/graphify.js` and to Claude Code as the `/graphify` skill. Rebuild with `graphify update .`; query with `graphify query "<question>"`. |
| **vs-mcp** | See [VisualStudio-MCP.md](VisualStudio-MCP.md) for setup. |
