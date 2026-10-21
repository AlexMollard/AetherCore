# Agent project inspection

Improve the existing read-only `inspect` gameplay job so an agent can discover
resolved project paths, startup scene, authored scenes and gameplay build inputs
in one operation. Preserve C# defaults and Rust selection. An optional `scene`
returns bounded authored entity/script details; it does not instantiate prefabs,
load included scenes, compile scripts, or claim runtime verification.

1. Add shared inspection with deterministic bounded output and actionable
   source-linked missing-input diagnostics. Test both languages, invalid scene
   names, truncation, cancellation and authored scene parsing.
2. Write a versioned job-private inventory artifact from the CLI, expose it as
   `inspection` alongside the unchanged v1 report in CLI/MCP job snapshots, and
   test malformed/oversized artifact handling.
3. Qualify both fixture languages through real jobs without compiler/editor
   launches; run Python/MCP regression tests and the native gauntlet. Review,
   document the result, clean generated verification files and commit.
