# Build-once agent gameplay test suites

**Goal:** Remove repeated compilation from multi-scenario agent verification while preserving C# and Rust behavior.

**Scope:** The user authorized selecting and implementing the next workflow bottleneck without check-ins. Independent native tests currently rebuild once per scenario (measured warm compilation roughly 1.8s Rust/2.8s C#). Checkpoints share one process/input history. Add optional `cases` (1..32) to desktop `test` requests; keep existing single-case and Web paths intact.

**Design:** Each case has a unique nonblank name (at most 128 UTF-8 bytes, no NUL), optional scene/ticks/step/launchConfig overrides inherited from the top-level request (launchConfig replaces the whole object). Reject unknown case fields. Validate all cases, assertions and named scenes before compilation. Build once into the job's existing private outputs. Run each case sequentially in a fresh owned process using indexed evidence directories. Continue after ordinary case failures, stop on cancellation/shared deadline. Retain every case report, snapshot and runtime log plus a suite-summary.json. Aggregate through AgentReport v1 with unique `case:<name>` required checks; copy detailed failure diagnostics with case context. No new report schema, arbitrary-state migration, save-store isolation, default language change or browser test semantics.

- [x] Add failing structural/normalization tests and record individual-job baseline.
- [x] Implement bounded request normalization and shared build-once native execution, with all-case preflight.
- [x] Expose cases through MCP and the existing PowerShell request entry point; commit verified implementation.
- [x] Exercise pass/fail/pass, repair, invalid final case before compilation, cancellation/timeout, module/process isolation and single-case compatibility for both languages. Compare equivalent individual vs suite timing without skipping checks.
- [x] Run native gauntlet and affected Python/MCP checks, fresh review, update docs and commit qualification.

**Constraints:** Keep build artifacts private, compiler caches locked, deadlines/cancellation authoritative, windows hidden/owned and autosave disabled. Saves remain project-scoped and can persist between cases. Preserve explicit language choice, C# defaults, manual-only CI, CRLF and existing local-tool folders. Reuse the established checkout.

## Execution evidence

Core committed `78f7255`; public terminal evidence preservation committed `4f0101a`; UTF-8 stdio and live cancellation coverage committed `cb512dd`. The structural suite test first failed because cases were unsupported, then passed with existing request tests (4 cases/38 assertions). All-case preflight reuses the existing runtime config and project scene validators and now occurs before native compilation for single run/test/relaunch too. Build-only and publishing paths retain their existing semantics.

Three sequential samples per language compare three independent 90-tick input branches against one suite of the same branches. Median Rust totals improve 13,292.5 to 10,031.7 ms (24.5%); C# improves 15,404.3 to 11,752.7 ms (23.7%). Both use warm compiler caches and real fresh desktop runtimes with identical assertions; no general/cold-cache performance claim. Baseline and clean suite reports: `logs/agent-test-suite/baseline.json`, `optimized-final.json`. The earlier `optimized.json` overlapped integration activity and is excluded.

Live C#/Rust qualification passed pass/fail/pass and repair, separate state/checkpoints, one shared private module path, invalid final config before any build, compile errors without case launch, direct native deadline and public job cancellation with later cases skipped. Evidence: `logs/agent-suite-qualification.log`, `logs/agent-suite-cancellation.log`. The supervisor's previous replacement of valid terminal reports dropped evidence: two real-worker tests failed with zero artifacts, then passed after preserving only validated reports while forcing cancellation/timeout status (12 job tests pass).

Actual existing MCP connections passed two-case suites for both languages. PowerShell passed a single-case array with a Unicode name for both languages. Actual MCP stdio revealed legacy Windows input decoding corrupted raw UTF-8; a subprocess regression reproduced it under `PYTHONIOENCODING=cp1252`, then passed with explicit UTF-8 streams. Fresh stdio suites for both languages preserved `café & 世界` names under that legacy inherited encoding with Rust home variables absent, also exercising automatic Windows Rust-home recovery (`logs/agent-test-suite/stdio.json`).

Native full gauntlet passed 1,464/1,464 cases and both GPU smokes with zero validation findings (`logs/agent-suite-native.json`). Python project tests ran 51 cases with five opt-in GPU skips; all 26 MCP tests passed. Fresh review cleared suite execution, terminal evidence preservation and UTF-8 boundary fixes. Browser runtime/export sources were unchanged; no external hosting was attempted.

Existing single-test failure/repair and detach-error compatibility passed again for both languages (`logs/agent-suite-single-compatibility.log`, two integration tests). Final aggregate acceptance is `logs/agent-test-suite/acceptance.json`. No owned editor/runtime/CLI/test process remained. The four known smoke-generated shader edits were compared against HEAD and removed; baseline local-tool folders were preserved. Graphify was refreshed after implementation.
