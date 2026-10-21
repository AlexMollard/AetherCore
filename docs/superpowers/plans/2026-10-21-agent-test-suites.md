# Build-once agent gameplay test suites

**Goal:** Remove repeated compilation from multi-scenario agent verification while preserving C# and Rust behavior.

**Scope:** The user authorized selecting and implementing the next workflow bottleneck without check-ins. Independent native tests currently rebuild once per scenario (measured warm compilation roughly 1.8s Rust/2.8s C#). Checkpoints share one process/input history. Add optional `cases` (1..32) to desktop `test` requests; keep existing single-case and Web paths intact.

**Design:** Each case has a unique nonblank name (at most 128 UTF-8 bytes, no NUL), optional scene/ticks/step/launchConfig overrides inherited from the top-level request (launchConfig replaces the whole object). Reject unknown case fields. Validate all cases, assertions and named scenes before compilation. Build once into the job's existing private outputs. Run each case sequentially in a fresh owned process using indexed evidence directories. Continue after ordinary case failures, stop on cancellation/shared deadline. Retain every case report, snapshot and runtime log plus a suite-summary.json. Aggregate through AgentReport v1 with unique `case:<name>` required checks; copy detailed failure diagnostics with case context. No new report schema, arbitrary-state migration, save-store isolation, default language change or browser test semantics.

- [ ] Add failing structural/normalization tests and record individual-job baseline.
- [ ] Implement bounded request normalization and shared build-once native execution, with all-case preflight.
- [ ] Expose cases through MCP and the existing PowerShell request entry point; commit verified implementation.
- [ ] Exercise pass/fail/pass, repair, invalid final case before compilation, cancellation/timeout, module/process isolation and single-case compatibility for both languages. Compare equivalent individual vs suite timing without skipping checks.
- [ ] Run native gauntlet and affected Python/MCP checks, fresh review, update docs and commit qualification.

**Constraints:** Keep build artifacts private, compiler caches locked, deadlines/cancellation authoritative, windows hidden/owned and autosave disabled. Saves remain project-scoped and can persist between cases. Preserve explicit language choice, C# defaults, manual-only CI, CRLF and existing local-tool folders. Reuse the established checkout.
