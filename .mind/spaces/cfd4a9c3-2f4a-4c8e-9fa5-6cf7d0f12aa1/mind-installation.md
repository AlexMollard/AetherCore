---
id: 6fb57609-24ab-4b23-9b4b-35bb7f88e076
space: projects/aethercore
name: mind-installation
tier: 2
pinned: false
tags:
  - cat:config
  - status:final
links_to: []
created_at: 2026-06-10 03:52:48
changed_at: 2026-06-10 03:52:48
---
**What**: Mind memory layer installed for opencode and AetherCore project
**Where**: 
- Source: `~/.local/share/mind/` (cloned from GabrielMartinMoran/mind)
- Launcher: `~/.bun/bin/mind.cmd`
- Config: `~/.config/opencode/opencode.jsonc` (MCP + instructions plugin)
- Plugin: `~/.config/opencode/plugins/mind-automation.js`
- Instructions: `~/.config/opencode/instructions/mind-memory-protocol.md`
- Project space: `projects/aethercore`
- Autosync: `.mind/` directory at project root
**Version**: v1.5.0 (latest)
**Notes**: 
- Windows: mind.cmd fix needed because setup writes bash script path by default
- MCP command uses `mind.cmd` instead of bash `mind` script
- Plugin MIND_BIN uses `mind.cmd` instead of bash `mind` script
- Uses Bun 1.3.14 runtime