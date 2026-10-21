# Agent gameplay failure feedback

**Goal:** Reduce the investigation needed after a failed gameplay assertion for both C# and Rust.

**Scope:** The user authorized choosing and implementing the next agent workflow bottleneck and committing as work progresses. Inspection found expected/actual evidence already computed but absent from terminal reports, and failed headless/checkpoint snapshots omitted from artifacts. This improves repair feedback; no runtime latency gain is claimed.

**Design:** Extend the shared state writer with an optional report destination. After a snapshot is written successfully, attach it and append one diagnostic per failed assertion before throwing. Keep AgentReport v1: stable reason codes, JSON context in the message, and source.file pointing to the snapshot. Report completed ticks, entity/type/property, expected/actual/tolerance, match count and whether a property was found. Preserve exact integer, tolerance, vector and missing-exists comparison semantics. Wire both native runners; retain checkpoint artifacts and accurate final artifact sizes. No schema, transport, language default, browser runtime or reload changes.

- [x] Reproduce generic assertion feedback with a failing unit test.
- [x] Implement and test shared diagnostics and artifact capture; wire headless/desktop callers; commit.
- [x] Exercise real C# and Rust final/checkpoint failures and repair, including missing/ambiguous targets and missing properties.
- [x] Run native gauntlet and affected project/MCP tests, obtain a fresh review, document evidence and commit.

**Constraints:** Keep C# and Rust builds, existing profiles and process cleanup working. Preserve manual-only CI, leave existing untracked local tool folders untouched, use CRLF. Reuse the established checkout.

**Execution notes:** Core committed as `e19ae63`. Review found that synthesized absence records could change another assertion's missing-entity reason; the mixed-assertion regression failed, then passed after recording real presence before synthesis. A second regression pins absent `$engine.exists=true` as a value mismatch with actual false. Missing entity/type classification applies only when no target record matches. The first gauntlet retained the deliberately red regression as a failure; the following full run passed 1,463 cases and both GPU smokes. Final checks below include the last classification correction.

**MCP parity:** Inspection also found the advertised launch schema omitted existing profile/checkpoint fields. The canonical reference fixture failed the schema regression before the fields were exposed; all 23 MCP tests then passed. This is discoverability of existing runner capabilities, with no new wire method. Committed as `ff200c0`. Real fresh stdio MCP start/status calls passed both-language headless failure/repair and desktop checkpoint feedback, recorded in `logs/agent-feedback-mcp-real.json`. The original installed Rust MCP connection lacks the CLI's `RUSTUP_HOME`; its toolchain-discovery failure is retained separately, not counted as successful qualification.

**Final review:** No remaining findings after the existence-classification correction (`efd06ea`). Focused tests pass 2 cases/40 assertions, including exact 64-bit values, UTF-8/escaped strings, actual null versus missing, duplicate names, mixed absence assertions and unavailable snapshot writes. Real integration repeated both language/profile failure and repair checks and detach failures successfully (`logs/agent-feedback-integration-final.log`). Project tests ran 40 cases with two opt-in GPU skips; MCP tests passed 23. Graphify was refreshed. This scope changes native verification/reporting and MCP discoverability; browser runtime/export code was not modified and no external-host qualification was attempted.

**Final acceptance:** `logs/agent-feedback-native-final.json` passed all phases in full mode: 1,463/1,463 cases, editor and runtime ready, zero GPU validation findings. `logs/agent-feedback-acceptance.json` records the aggregate and limitations. No owned Editor, game runtime, CLI or test process remained. The four known smoke-generated shader edits were verified against HEAD and removed; existing local-tool folders were preserved.
