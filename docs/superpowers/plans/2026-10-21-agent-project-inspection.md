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

## Qualification completed

- Native full gauntlet in RelWithDebInfo (`-Fast`): 1,470 cases passed; editor
  and runtime GPU smokes passed with zero validation findings.
- Six inspection unit cases cover language-specific inputs, source-linked repair
  evidence, traversal rejection, cancellation, sorting/truncation, malformed and
  oversized scenes, UTF-8 startup names and directory rejection.
- Fresh MCP stdio checks passed for Rust and legacy-default C#, with invalid
  compiler overrides, custom paths and Unicode roots. Sources stayed byte-identical;
  inspection produced no build/runtime logs or readiness receipt. Latest native
  durations were 444 ms (Rust) and 443 ms (C#), excluding MCP process startup/polling.
- Project Python suite: 54 cases, 49 passed and five GPU opt-in skips; MCP suite:
  26 passed. Real C#/Rust desktop failure/repair and compiler-error checks passed.
- Broader CTest found existing standalone SDK tests bypassing IEngineBackend for
  spawn rotation. Routing those same native operations through the existing
  backend restored the gate; all 168 managed SDK cases pass, including two new
  host/solo yaw checks. Native behavior and public SDK/native ABI remain unchanged.
- Read-only review caught and resolved startup UTF-8/regular-file checks and
  cancelled/timeout inline evidence. Final review found no further actionable issues.

Machine-local evidence is in `logs/project-inspection-final-native.json`,
`logs/project-inspection-acceptance.json`, `logs/project-inspection-python.log`,
`logs/project-inspection-mcp.log`, `logs/inspection-final-desktop.log` and
`logs/inspection-final-managed-ctest.log`. These logs are intentionally untracked.
