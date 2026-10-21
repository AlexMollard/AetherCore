# Browser automation and verification reports

Task 11 provides a development-only browser transport and a bounded local qualification command. Native editor control remains ENet; browser automation exposes no process, host filesystem or arbitrary evaluator operations. Selected Rust and C# exports use the separate [target-aware publisher](../web-export.md).

## Run it

```powershell
./scripts/Run-BrowserGauntlet.ps1 -Project projects/AgentPipelineRust `
    -PlaywrightModule <absolute-path-to-pinned-playwright> `
    -ReportPath logs/browser-report.json
```

The script builds the native asset tool, automation Wasm and a separate production artifact, then runs headless hardware Chromium. `-SkipBuild` reuses existing artifacts; their bridge boundary is still checked. `-SlangCompiler` and `-EmsdkPath` select the pinned installations. `-BrowserExecutable` selects Chromium; otherwise installed Chrome is used. `-TimeoutSeconds` is an overall deadline covering builds and browser execution. Every spawned command has an owned process object; timeout/cancellation terminates its process tree. The Node runner also owns its local server, browser contexts and legacy regression worker, closes them on all handled outcomes and reports cleanup results. No test window is opened or focused.

MCP `run_browser_gauntlet` calls this same script and returns its structured report, with `isError` for every outcome except passed. It requires no running editor and never uses the default control port. Restart the MCP client after installing changed server code to discover the new static tool. Arguments include project, reportPath, skipBuild, playwrightModule, browserExecutable and timeoutSeconds.

The current build harness embeds AgentPipelineRust. Requests for another project, including C#, return blocked explicitly rather than silently testing that reference fixture. Generic project export selection is available through [target-aware publishing](../web-export.md); the qualification gauntlet intentionally retains its fixed reference scenario.

## Native/browser lighting comparisons

```powershell
python tools/web-runtime/lighting-parity-checks.py --build build/webgpu-browser --playwright <absolute-path-to-pinned-playwright> --report logs/lighting-parity.json
```

This development-only comparison creates a private project, copies the maintained
fixtures and runs the canonical native `AssetPacker bake-all` path before testing.
It records generated artifact hashes and verifies that authored sources were not
changed. Hidden native gameplay tests capture tick 10; an owned hardware browser
captures the same scene at tick 10 and the measured native output extent. Exposure
is fixed at 1 with ACES, with unrelated post effects disabled for this qualification.

The report compares RGB samples across the whole image without alignment or edge
masking: mean absolute error must be at most 8 and the 95th percentile at most 18.
Feature-specific probe channel errors must be at most 12. Independent checks on
both renderers verify radiance, backgrounds, material maps, lighting and shadow
On/Off differences; matching black images cannot pass. An asymmetric cutout must
cast from its opaque half and transmit through its transparent half. Model-load
warnings invalidate the comparison instead of being misreported as visual parity.
Shadow receiver probes avoid caster silhouettes; whole-image sampling still
includes their edges. The report records the probe coordinates and checks the
effective machine-local render scale without modifying user settings.

The shared deadline bounds baking, native tests and browser captures. Browser,
server and native job cleanup are owned by the helper. This does not publish a
game or unlock the production 3D export profile.

## Bridge contract version 1

`AETHERCORE_WEB_TEST_BRIDGE` defaults OFF. The gauntlet opts the development build in; production is configured explicitly OFF in `build/webgpu-production`. Only opted-in builds copy `test-bridge.js`, include its HTML script reference, link Rust's automation panic fixture and expose device-loss injection. Production verification scans HTML/JS/Wasm for bridge/fault export markers and loads the production game in a real browser with fault URL parameters to verify those parameters cannot activate automation.

The development global `aetherTest` exposes:

| Method | Contract |
| --- | --- |
| `version` | Protocol version 1. |
| `readiness()` | Readiness flags, terminal flag and an isolated state snapshot. Audio still requires the real Start gesture. |
| `snapshot()` / `logs()` | Copied runtime state / diagnostics. Mutating a result cannot mutate ECS state. |
| `input({key, down})` | Queue a shared key code, 0..511. Reject unknown fields and queues larger than 128 commands. |
| `step(ticks, timeoutMs)` | Advance 1..600 fixed simulation ticks, with a bounded completion promise. Only one step request may be pending. |
| `capture(timeoutMs)` | Request engine GPU readback and return copied pixels after completion. |
| `loseDevice()` / `panic()` | Test-build fault injection; panic runs an actual Rust panic on the next queued tick. |
| `stop()` | Stop the runtime and reject outstanding work. |

`?reference=1&automation=1` suspends automatic gameplay advancement from startup. Input commands are consumed by the engine frame as fixed updates are requested; they feed shared Input and the actual Rust/GameplayHost path. Tests do not edit transforms, scripts or ECS components from Playwright. A multi-tick command advances at most one fixed tick per frame and waits across asynchronous save barriers. Its completion snapshot is published after the engine frame. An expired command clears injected keys and queued input. Focus/visibility release injected keys just as physical input does.

The bridge is intentionally a narrow test protocol. Deterministic game-state assertions use fixed steps and snapshots. Rendering assertions separately allow raster variation and require actual WebGPU readback. Web Audio starts from a trusted browser click; the test checks nonzero analyser signal rather than claiming physical speaker output.

## Report and failure semantics

The versioned JSON report identifies operation, target/profile, project/language, phase, status, diagnostics, timings, pinned toolchain, OS/architecture, browser/Playwright version, GPU identity, adapter classification and bridge version. Each new case retains state, console errors, page stack traces, duration and a screenshot where capture remains possible. The artifact directory also retains the 31-case platform/renderer regression report and its log.

Zero exit requires all named cases exactly once and passed: the 31 established renderer/platform cases plus ten bridge/production cases. Missing, skipped, unavailable or software-only hardware checks cannot aggregate to passed. No browser installation or hardware adapter yields blocked, a real regression yields failed, and timeout/cancellation yields cancelled. A partial `run.mjs --only=...` development run explicitly leaves all unexecuted required cases blocked and exits nonzero. Native gauntlet evidence stays in a separate report; browser results never imply C# or native validation.

Coverage includes exact 90-tick reference gameplay (score 60), collision/trigger behavior, actual sound, a completed two-record save, scene transition and fresh-page reload; UI/animation/tile pixels; input focus and controlled visibility; package/version/target/font failures; denied/quota storage; unsupported WebGPU; device loss; and a real Rust panic/abort. A 300 MiB declared response streamed with only an initial prefix exercises the size limit and synchronous fetch-cancellation callback. That regression caught and fixed recursive close/error-handler reentry without increasing the Wasm stack.

Timeout tests launch real stalled child processes and confirm descendants exit. The PowerShell supervisor has a corresponding stalled-compiler test. The production bridge absence check executes the game as well as examining artifact bytes.

## Qualification limits

Qualification is Windows hardware Chrome with the pinned toolchain. Injected Gamepad API coverage does not qualify a physical controller. Controlled visibility events do not qualify operating-system tab suspension. Other browsers/OSes, hosted embeds and generic published projects are not covered. Reports retain failed attempts as evidence; they are not replaced by inferred success.

Task 11 qualification passed all 41 required cases on Chrome 155.0.8059.85 / NVIDIA Blackwell hardware, including production bridge absence. Evidence: `logs/task11/verified/report.json` and its ten screenshots plus legacy report. Native `logs/task11/native-gauntlet.json` passed all 1,460 tests, both Editor/GameRuntime builds and both hidden validation smokes; C# and Rust desktop reference scenarios passed. Four Node contract/cleanup tests, one real PowerShell deadline test, 22 MCP tests and 13 cooking/audit tests passed. The missing-browser and timed-out-run reports remained blocked/cancelled, returned nonzero and confirmed owned-resource cleanup. clangd and graphify were refreshed. No CI triggers or git hooks were introduced.
