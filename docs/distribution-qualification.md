# Autonomous distribution qualification

Task 13 provides a local acceptance command that starts from a new Rust project and retains reports for every required stage. It never uploads, installs dependencies, changes CI triggers or opens a human editor.

```powershell
./scripts/Run-DistributionGauntlet.ps1 `
    -PlaywrightModule <absolute-path-to-pinned-playwright> `
    -SlangCompiler <absolute-path-to-pinned-slangc> `
    -TimeoutSeconds 1200 -ReportPath logs/distribution-report.json
```

Python entry point: `python tools/project/distribution.py --help`. The PowerShell command uses the same options as `-ReportPath`, `-TimeoutSeconds`, `-PlaywrightModule`, `-SlangCompiler` and `-Budgets`. Prerequisites and tool pins are in [Web exports](web-export.md) and [toolchain.json](../tools/experiments/rust-webgpu/toolchain.json). Missing tools fail the run; there is no automatic installation.

## Acceptance and artifacts

The command uses the existing `aether-project` CLI to create a new blank Rust 2D project under `.temp/distribution/<identity>`, authors a deterministic gameplay scenario and assets, introduces an actual compiler error, verifies its structured diagnostic, repairs it, and builds/tests the game. It publishes a Web ZIP, edits a gameplay log message, publishes again and confirms the edited selected module actually runs. A separate extraction of that ZIP is tested at a nested URL and inside a sandboxed iframe on a second loopback origin. The generated sources and package remain available for inspection.

The browser verifies keyboard scoring, gesture-enabled audio with a nonzero analyser peak, completed saves, fresh-page save loading, and save loading after iframe navigation. It records hardware/browser/OS identity and observed iframe policy support. A recognized policy must permit the feature; an unrecognized WebGPU policy token is recorded as unknown, not as an enforced allow/deny test. Local cross-origin success does not prove external hosting works.

The gate also runs the existing full native gauntlet (including Editor and GameRuntime GPU smokes), C# and Rust native reference scenarios, C# desktop publishing, and the full 41-case browser regression suite. Missing or skipped required phases cannot pass. Every command shares one deadline and owns its child process tree. Logs and individual reports live beside the aggregate report in a unique `distribution-<identity>` directory.

`localAcceptance` reports the local gate. `hostQualification` is independent and remains `pending` without an authorized external URL. A passing local report is not a claim that the full first-browser-release host gate has passed. The report includes an explicit browser matrix: actual Chrome on Windows evidence, untested Firefox/other OS combinations, and unsupported missing-WebGPU/C#-Web/3D/networking cases.

## Measurements and budgets

The measurement report records ZIP bytes, one browser-cache-disabled navigation and one cache-enabled reload after priming, Wasm linear memory and a p95 from 120 observed engine frame intervals. Startup means navigation to the first observed focused gameplay tick, including the automated trusted gesture. Cold does not mean empty OS, shader or compiler caches. Frame intervals are observed engine submissions sampled at animation callbacks, not GPU execution time. Linear memory excludes GPU allocations and browser overhead; the optional Chromium JS heap estimate is recorded separately.

Edit-to-ready measures a semantic Rust source edit through the second publish's completed browser verification and final promotion. Cold/warm Web publish durations and native repair-through-test duration are also recorded. Package transfer byte counts are separate from resource transfer sizes; the latter show whether the warm HTTP cache was used.

Fixture-specific numerical ceilings are checked from [distribution-budgets.json](../tools/browser-tests/distribution-budgets.json): ZIP 2 MiB, cold/warm startup 1,000 ms each, Wasm linear memory 32 MiB, p95 observed frame interval 34 ms, and warm edit-to-verified-publish 60 seconds. Initial reference measurements were 1,067,180 ZIP bytes, 261.3/242 ms startup, 17,563,648 linear-memory bytes and 16.8 ms p95. These are local guardrails for this small fixture, not broad engine performance claims. New content scope or another machine needs recorded measurements and an explicit budget decision.

## Preparing an itch.io draft

The checked ZIP has `index.html` at its root and includes every runtime file with relative names. The local package check enforces the published file-count, path-length and extracted-size limits. On an authorized draft, select **HTML Game**, upload the ZIP, enable browser play, choose an embed size or fullscreen launch, and retain click-to-play for audio. These instructions follow [itch.io's HTML5 documentation](https://itch.io/docs/creators/html5).

After upload, test the actual draft URL in hardware Chrome: click Start, focus the canvas, hold Space, observe score/audio/save completion, reload the result scene, and test the embedded/fullscreen viewport. Record the URL, browser/OS/GPU, console and permissions policy, screenshots and save behavior. Leave qualification pending if WebGPU is blocked by the host or if the check has not run. Do not enable mobile-friendly or claim Firefox/Safari support without recorded evidence on those combinations. No external host upload has been performed by this task.

## Recorded local qualification

The final Task 13 run on 2026-10-20 passed at `logs/task13/distribution-ui-final.json`:
Windows 11 build 26200, Intel64 Family 6 Model 198, 24 logical CPUs, hardware NVIDIA
adapter and Chrome 155.0.8059.85. The fresh qualification game ZIP measured 1,106,113
bytes, cold/warm interactive startup 399.9/272.8 ms, Wasm linear memory 17,563,648 bytes,
p95 observed frame interval 18.2 ms, and edit-to-verified-publish 17,708 ms. All fixture
budgets passed. Native verification passed 1,462 tests plus both GPU smoke checks;
browser verification passed all 41 cases and confirmed cleanup.

The playable public sample is [Ember Run](../projects/EmberRun/README.md), separate from
the deterministic distribution fixture. Its standalone export and sample scenario
passed at `logs/task13/ember-publish.json` and `logs/task13/ember-browser.json`; desktop
120-tick score/state assertions passed at `logs/task13/ember-native.json`.
External hosting remains pending by Pog Champ's explicit choice; no upload was made.
