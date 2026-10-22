# Desktop gameplay reference comparison

Task 7 uses `projects/AgentPipelineRust` and `projects/AgentPipelineCSharp` as the
same small acceptance game in two languages. The Rust project's existing `Player`
and `Probe` fixtures remain available for the scripting host tests.

Both games use identical scenes, prefab, sprite texture, short PCM sound and input
sequence. `tools/project/reference_fixture.py` is their shared data source; run it
without arguments to check parity, or with `--write` to regenerate those authored
files. The original Rust default scene is independent of the reference scenes.

The reference starts at `ReferenceStart`, simulates 90 updates at 1/60 second and:

- Holds Space for 20 updates to increment the score.
- Moves a dynamic sprite through a trigger and into a static wall.
- Instantiates a prefab at update 5 and destroys it at update 15.
- Updates the native UI text and starts the shared sound clip.
- Writes the score, collision/trigger outcomes and an embedded-NUL Unicode label.
- Switches to `ReferenceResult` at update 60 and reads those saved values.

Checkpoints at updates 10, 20 and 59 inspect the native world, not just script
claims. Final assertions check the destination scene, restored score, UI text,
removed entities and audio voice-start count. Physics positions have explicit
tolerances. The comparison does not require byte-identical physics state.

## Run it

Build through CMake:

```powershell
cmake --build --preset default --target aether-project EngineTests
python tools/project/reference_fixture.py
python tools/project/compare_reference.py --samples 3
```

The comparison writes a new directory under `logs/reference-comparison/`, including
`comparison.json`, individual native pipeline reports, compiler logs, copied state
and PNG captures. `--output` selects a new output directory; existing directories
are refused. It copies sources into owned scratch projects and never edits the
checked-in game scripts during the compiler exercise.

Each sample alternates language order and records:

- A build with an empty project artifact directory, followed by a warm build.
- Build-to-ready time for a complete desktop test.
- Compiler feedback for an intentionally undefined movement-speed symbol.
- One scripted correction to 2.1 units/second, followed by a passing desktop test.
- Runtime startup, resident/peak memory, CPU update and wall frame intervals.

Build-to-ready ends at a receipt written after the first gameplay update. Complete
run durations are recorded separately, including the remaining ticks, assertions,
captures and teardown. Readiness receipts use Unix nanoseconds; a wall-clock jump
outside the measured operation fails measurement instead of producing a time.

"Cold" does not clear OS, NuGet, Cargo download or GPU driver caches. The repair
exercise has a documented task and a fixed correction count; it does not measure
an autonomous agent's reasoning or productivity. Native renderer and driver costs
are included in process memory and frame intervals. GPU execution timing is not
measured. A few local samples are a baseline, not a language ranking.

Audio uses the normal decoder/voice path but agent sessions are muted. A passing
voice-start check does not certify audible speaker output. PNG captures provide a
rendering artifact; the tooling does not claim pixel-exact equivalence.

## Bounded desktop pipeline profile

The existing CLI/MCP gameplay jobs accept the same request as before, with
`launchConfig.profile = "native-desktop-2d-v1"`. Use `scene: "ReferenceStart"`,
`ticks: 90`, and the project's `reference-test.json` as `launchConfig`.

The desktop profile boots the ordinary runtime in a hidden, named agent window,
injects input before polling, advances gameplay with the requested fixed step,
captures the render at update 10 when the run is long enough, and closes its owned
window at the tick bound. The existing supervisor owns cancellation and timeout
of the process tree. Settings persistence is disabled for these test runs.

`launchConfig.checkpoints` contains `{tick, assertions}` objects, using completed
update counts starting at 1. Input event ticks start at 0. Duplicate checkpoint
ticks fail validation. Script properties use their script type name. Native
assertions use `type: "$engine"` and expose `exists`, `position`, `localToWorld`
(16 column-major floats) and `uiText`. The reserved `$runtime` entity exposes
`scene`, `audioStarted` and `simulatedTicks` (completed simulation steps).

`tools/web-runtime/gameplay-parity-checks.py` diffs these snapshots against the browser
test bridge's `aetherTest.entities()` (same schema, dev/test builds only) at checkpoint
ticks: every named entity's world position/rotation/scale plus script properties, with a
recorded absolute tolerance (default 1e-5) and a built-in perturbation control.

The maintained 3D fixtures are `BrowserBodies3D`, `BrowserRaycast3D`, `BrowserContacts3D`,
`BrowserCharacter3D`, `BrowserRagdoll3D`, `BrowserSkinClips3D`, `BrowserPrefab3D` and
`BrowserTransition3D` (+ `BrowserTransition3DResult`), authored in
`resources/web/reference/scenes` and driven by the same probe script types in Rust
(`projects/AgentPipelineRust/fixtures3d/gameplay3d.rs`) and C#
(`projects/AgentPipelineCSharp/fixtures3d/Gameplay3D.cs`). The probes live outside each
project's `scripts/` directory because the 2D production publisher scans everything under
it; the harness injects them into its private staged project (`--stage-project` stages
one without running). Run them with `--scenes <names> --checkpoints 1,10,30,60,120,150`;
C# needs `--web-root` pointing at a managed export (`tools/web-runtime/managed-parity-build.py`).
The Human model they use is copied from `projects/HumanDemo` by `prepare_reference.mjs`.
Collider `half_extents` are world-space sizes, never multiplied by the entity scale.
Ragdoll bones are named by role so their world transforms are compared like any entity.

The default profile remains `native-headless-2d-v1`, which creates no GPU/window or
audio device. It rejects desktop checkpoints and unsupported scene content rather
than reporting those checks as passed. Both profiles share script-state copying
and assertion matching, including ambiguous-name rejection and numeric tolerances.

## Shared saves

C# can now use `SaveData.Write(key, value)` and `SaveData.TryRead(key, out value)`;
Rust retains `Context::write_save` and `Context::read_save`. Both adapters use the
same native store, UTF-8 byte lengths and key restrictions: 1–128 ASCII letters,
digits, underscores or hyphens. Values may contain embedded NUL characters.

The historical `rust-saves/project-<hex UTF-8 project directory name>/` location is
retained so existing Rust saves remain readable. Project directories with the same
basename share that historical namespace. The reference overwrites its own two
keys before reading them, making its outcomes independent of prior fixture runs.
Headless/desktop automation disables prior-save reads unless `readSaves` is true;
the reference explicitly enables it for the second-scene readback.

The explicit GPU regression gate can be run with:

```powershell
$env:AETHER_DESKTOP_REFERENCE_TESTS = "1"
python -m unittest discover -s tools/project -p test_desktop_reference.py
Remove-Item Env:AETHER_DESKTOP_REFERENCE_TESTS
```

It injects a final-scene detach failure in both languages and requires failed job
reports. Successful desktop runs explicitly detach gameplay before reporting passed.
Without that environment opt-in, ordinary Python unit discovery skips GPU work.

Browser export remains a later milestone. This fixture is the desktop baseline
against which the eventual Rust/WebGPU browser profile must be checked.
