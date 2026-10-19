# Shared gameplay ABI and agent diagnostics v1

Implemented in Tasks 2 through 5 of the Rust/WebGPU plan. C# lifecycle, discovery and properties now route through the shared host and v1 module table. The existing CoreCLR loader, ManagedInterop.hpp, managed ABI, compiler and project format remain compatible. Projects select C# or desktop Rust; Rust uses the checked host and safe SDK described below.

## Ownership and negotiation

src/engine/scripting/GameplayAbi.h is the canonical C-compatible definition. The module entry is aether_gameplay_query(requestedMajor, outSize, outApi), returning an explicit int32 status. The current ABI major is 1. Both tables include structSize, abiMajor, capabilities and an opaque context token.

V1 requires exact known sizes. Major mismatch, undersized or oversized output, invalid mandatory callbacks and unknown module capability bits are rejected. QueryGameplayModule clears the caller's known output region before invoking an untrusted module and validates a temporary result before exposing callable entries. PublishGameplayModule is the corresponding provider helper. Neither writes outside min(outSize, sizeof(table)); rejected tables cannot retain stale function pointers. An appended layout requires explicit renegotiation rather than an assumed compatible copy.

Callbacks and pointers are process-local, including Wasm linear-memory pointers. Pointer-bearing records must never be persisted or sent over a network. The same declarations are compiled separately for native x64 and Wasm32. Lengths in the new ABI are uint32, not size_t. Fixed-width does not imply the same pointer-containing layout across targets.

## Data and lifecycle

- Instance handles are opaque uint64 values owned by the provider.
- Entity values contain a uint64 world/session token, the complete uint32 EnTT ID including generation, and a reserved zero field.
- Property values contain a size, tag, numeric lanes and entity value; string data uses a separate caller-owned buffer. Property kinds preserve the existing None/Float/Int/Bool/Vector3/String/Enum/Entity/Component values 0 through 8. Component metadata includes its catalogue type name; enum metadata stays separate.
- UTF-8 input views contain a pointer and explicit byte length. Null is allowed only for an empty view. Text need not be terminated and may contain embedded NUL.
- UTF-8 output buffers carry capacity and a required-size result. Required size includes a trailing NUL. Too-small buffers receive BUFFER_TOO_SMALL, clear the first byte when storage is available, and copy no partial UTF-8 text. Callers must check the status and required size. Text validation rejects overlong encodings, surrogate code points and incomplete sequences.
- Type/property descriptor views are immutable provider-owned data valid until shutdown; a consumer copies anything it needs beyond that lifetime.
- Lifecycle is create, attach, zero or more updates, detach, destroy. An instance whose attach failed is still destroyed. Providers must validate their instance state and return explicit errors.

Optional events use capability bits Physics=1, Ownership=2 and Network=4. A provider advertising an event capability must supply dispatchEvent; absent support produces UNSUPPORTED. DispatchGameplayEvent checks the capability and contains C++ callback exceptions. GC policy, Roslyn compilation and C# editor-window registration are deliberately absent from this shared module table.

Providers contain their own exceptions/panics. Query and event helpers additionally translate C++ exceptions into SCRIPT_ERROR. The test fixture enables Emscripten C++ exception catching to exercise this behavior. The browser ABI fixture uses Rust panic=abort; the desktop SDK contains unwind panics when the project uses panic=unwind. This does not make Rust panic-abort, native access violations or arbitrary foreign unwinding recoverable.

## C# host routing

`src/app/scripting/GameplayHost` owns copies of validated module descriptors and marshals lifecycle, ownership events and property values through the v1 function table. `CSharpGameplayHost` adapts the existing managed table, retaining its opaque managed handles so RPC dispatch still resolves the same instances. The registered host object's address stays stable across reloads.

`CSharpScriptingSubsystem` retains CoreCLR hosting, source compilation, build/reload state, error queues, RPC reflection/invocation, GC and managed editor windows. Its compatibility discovery/property methods forward to `Gameplay()`. The runner, inspector, MCP discovery and replication property bridge use the shared host; scene capture reaches it through the runner's existing live-property synchronization. Serialization stays name-based and preserves unknown types and fields.

Every load attempt refreshes the shared metadata generation and invalidates the adapter's active-handle set. Descriptor discovery does not instantiate scripts or read default getters. Legacy enum/component metadata strings, long UTF-8 strings and embedded NULs cross caller-owned buffers without retaining managed scratch pointers. Managed void callback failures are observed through the existing error callback; native C++ exceptions are contained, while native faults and runtime fail-fast remain unrecoverable.

Bootstrapping and shutdown remain backend-owned. The current uint32 entity creation path explicitly represents legacy C# world identity zero. It does not install `GameplayAccess` guards around existing C# exports. Rust supplies scoped entity identities and checked engine access through its native host. Physics events still execute inside the managed update callback; the adapter advertises only its shared ownership-event capability, rather than claiming a new physics or RPC event implementation.

Nine focused native adapter/runner tests cover lifecycle, missing types, stale handles, reported errors, property kinds, callback-driven storage growth and live scene-reference round-trips. A real C# fixture additionally exercises in-place reload, failed rebuild/Play recovery, sibling scripts, UTF-8/NUL strings, ownership, local RPC, 2D collision callbacks, movement and Stop restoration. Its published package also runs after relocation. Evidence is under `logs/task3/`.

## Project language selection

`ProjectSettings.toml` accepts one `[scripting]` `language`: `"csharp"` or `"rust"`. A missing declaration preserves C# for existing projects. Unknown values, non-string declarations and mixed Cargo/C# manifests in the configured scripts directory are rejected. Script type names in scenes remain unchanged. Changing language requires reopening the project; Play and publishing revalidate the descriptor before building or cleaning output.

The shared project creator, Launcher picker and `create_project` agent tool accept a language. Omit it for C#. Rust supports `blank_2d` and `blank_3d`; multiplayer templates remain C# only. Rust projects contain `scripts/Cargo.toml` and `scripts/src/lib.rs`, with no C# project/solution generation. The safe SDK generates their provider entry point and typed registration. Rust source projects compile through CMake/Cargo, then load a native module without starting CoreCLR. Play builds asynchronously; failed builds leave the editor editable and never execute the previous artifact as the new result.

`gameplay_capabilities` is available in both Launcher and Editor. It returns language, desktop profile, supported operations and a schema-v1 prerequisite report. In the Launcher, pass `root` to inspect a project, or `language` to inspect prerequisites for creation. The Editor defaults to its open project. Rust prerequisite checks locate Cargo, rustc, CMake and the safe SDK without starting builds. `AETHERCORE_CARGO`, `AETHERCORE_RUSTC` and `AETHERCORE_CMAKE` override executable discovery; an invalid override reports missing tooling. C# does not require Rust tools. Backend availability is reported separately from installed prerequisites.

The dependency-free `rust/aethercore-abi` crate ships under `data/sdk/rust/aethercore-abi`; its ABI declarations are generated from the canonical C header. `AETHERCORE_RUST_SDK` can select a Cargo.toml explicitly; otherwise resolution checks the executable bundle and development hint. Generated project dependencies use relative paths where possible. This raw crate underlies `rust/aethercore`, the safe SDK staged alongside it. New projects depend on `aethercore`; `AETHERCORE_RUST_GAMEPLAY_SDK` overrides its Cargo.toml. The build driver is staged under `data/sdk/rust/build` and can be overridden by `AETHERCORE_RUST_BUILD`.

Runtime settings and module resolution use the executable directory for moved packages. Native tests cover relocated packages, source overrides, invalid/mixed manifests, missing language defaults, settings round-trips and stale selection. CLI rejected gameplay operations return the terminal report exit code while retaining JSON; MCP marks them `isError` and preserves that report. A capability query remains successful even when its prerequisite report identifies unavailable operations.

## Engine access guard

GameplayAccess provides a minimal checked engine table for entity validation, copied position get/set and diagnostic delivery. Each access session receives a unique token and records the owner thread. It validates world/session, full generation-bearing ID and registry liveness before touching components. Stale tables fail after Invalidate or destruction. Wrong-thread calls fail without accessing the World.

The World must outlive GameplayAccess, or Invalidate must run before World teardown. The Rust host invalidates access and destroys instances before World teardown and before unloading code. Callback and structural-mutation scopes are exclusive and reject nested entry. They are not yet installed into the existing C# runner, and do not intercept direct calls to World. Diagnostic sinks execute synchronously outside the context registry lock and cannot recursively report on the same session.

The checked position callback edits scene transforms; it is not a replacement for existing app-level physics-aware C# exports. The C# adapter retains those exports and their physics/controller semantics. No borrowed ECS reference is exposed.

## Desktop Rust SDK and execution

`gameplay_script!` declares public fields, defaults and property metadata. Implement `Script` for lifecycle/events, then export the types:

```rust
use aethercore::*;
gameplay_script! { pub struct Player { pub speed: f32 = 3.0 } }
impl Script for Player {
    fn update(&mut self, ctx: &mut Context<'_>, delta: f32) -> Result {
        let mut p = ctx.position(ctx.entity())?;
        p.x += self.speed * delta;
        ctx.set_position(ctx.entity(), p)
    }
}
export_gameplay!(Player);
```

The gameplay library must be named `aether_gameplay` with crate-type `cdylib`, as generated by project creation. Macro fields support float, integer, bool, Vec3, UTF-8 strings (including NUL), enum, entity and typed component references. `gameplay_enum!` and `gameplay_component_ref!` provide their metadata. Agent `scene.add_script` uses Rust declarations to validate JSON before mutating the scene; entity fields accept numeric IDs and typed components accept `{entity, component}` objects. Serialization remains the existing name-based scene format.

Safe code receives copied values and opaque generation-bearing handles, not ECS pointers or mutable references. A callback-scoped Context cannot escape into Script state or another thread. The host rejects expired sessions, wrong worlds/threads, stale IDs and reentrant access. These checks protect safe SDK use; arbitrary unsafe native modules are not sandboxed.

The base v1 ABI layout remains unchanged. Optional `GameplayServices.h` is negotiated separately by `aether_gameplay_bind_services`. It supplies input, sprite color, Box2D velocity, find/create/deferred-destroy entity, deferred scene requests, prefab instantiation, audio playback, UI text and saves. Missing services return `UNSUPPORTED`. Save keys are restricted filenames under the user configuration directory's `rust-saves/project-<hex UTF-8 namespace>` folder; the namespace uses the source project's folder name or the packaged executable folder name. Renaming that folder selects a different save namespace. Network/editor-tool API parity is not advertised.

`RustGameplayHost` checks the module table and owns all native instances. Failed callbacks disable that instance while siblings continue. Unwind panics are caught inside Rust; abort panics/faults terminate the process and are verified in bounded child probes. Destruction and callbacks complete before the library unloads. Application teardown unloads Rust before removing scene/physics systems, including partial startup failures.

`CMake/RustGameplay.cmake` is opt-in: C# configure/run requires no Rust toolchain. Source builds use a standalone CMake driver and literal process arguments, offline Cargo builds, Cargo JSON diagnostics and one shared timeout. External crate dependencies must already be cached. Reports record actual rustc/Cargo versions and the host triple. A successful artifact is copied into a unique load directory so Windows can rebuild while the old module is loaded. Play invalidates old instances before loading the new successful artifact; Stop restores the authored scene. Stopping a compilation immediately reports cancelled and cannot later load its result; the bounded compiler worker may finish before another build starts. General headless jobs and process cancellation remain Task 6.

`project.gameplay_status` / `gameplay_status` reports module availability and the schema-v1 build/load/runtime diagnostics. Missing tools, compilation errors, failed staging and invalid ABI are explicit failures. Different Rust source projects open in separate Editor processes through the Launcher. Publishing validates registration and packages only the native module plus normal engine assets/runtime dependencies; Cargo, SDK source and managed assemblies are excluded from Rust game content. A relocated package loads from its executable directory without Cargo/rustc or CoreCLR initialization.

Task 5 evidence is under `logs/task5/`: Windows live movement/input/sprite/Box2D/UI/save/property restoration, failing-build recovery, native provider errors/panics, Rust publishing and relocated execution; C# lifecycle/RPC/ownership/hot reload/publishing were repeated. Linux is unqualified on this host because no Linux Rust/C++ development toolchain is installed. This task does not implement browser export.

## Declaration generation and compatibility checks

tools/gameplay-api/engine-calls.json records the initial five existing C exports: create entity, destroy entity, entity validity, get position and set position. generate.py checks native signatures against the existing C# imports, emits thin C/Rust declarations, and derives Rust ABI records/function tables from GameplayAbi.h. It supports the deliberately restricted declaration grammar used by v1; it is not a general C parser.

The generated raw exports remain unsafe and context-dependent. Safe Rust gameplay must use the checked host API; these declarations alone are not a Rust SDK. No existing C# export name or managed declaration was replaced.

```powershell
python tools/gameplay-api/generate.py
python tools/gameplay-api/generate.py --check
python tools/gameplay-api/test_generate.py
```

GameplayApiParity and GameplayApiGeneratorTests are registered with CTest when Python is available. Generated output is checked in and generated with CRLF. Do not hand-edit generated files. The real Rust consumer checks reserved keyword escaping as well as native/Wasm layouts.

## Agent report contract

AgentDiagnostics.hpp/.cpp owns the app-layer report model. SerializeAgentReport validates before returning JSON; it does not expose nlohmann/json in the engine's C ABI. FromGameplayDiagnostic copies borrowed ABI spans into an owning AgentDiagnostic, checking the exact diagnostic record size, severity, UTF-8 and source coordinates.

Schema: docs/schemas/agent-report-v1.schema.json. Examples in docs/schemas/examples/ are compared with actual serializer output by native tests.

| Status | exitCode | Meaning |
| --- | --- | --- |
| queued / running | null | Snapshot, not terminal success |
| passed | 0 | Completed without error diagnostics; all required checks passed |
| failed | 1 | Requires at least one error diagnostic |
| cancelled | 130 | Terminal cancellation |
| skipped | 3 | Terminal non-execution, not success |

Every report carries schemaVersion=1, jobId, phase, target, language, profile, durationMs, diagnostics, toolchains, artifacts, requiredChecks and exitCode. Source locations have a file and optional one-based line/column; a column requires a line. Toolchains identify name/version/target. Artifacts have a path and optional bytes/hash. Empty identifiers, invalid UTF-8, negative duration and contradictory pass results are rejected. Required-check names must be unique in the producer; JSON Schema alone cannot enforce uniqueness by a selected object field.

BuildPublishAgentReport is a pure adapter around the existing PublishReport. Existing ok/summary/text fields are preserved. A pending publish stays queued/running; a completed legacy result determines passed/failed and a failure diagnostic. The live editor retains its existing publish control payload; the independent gameplay pipeline below reuses the shared publish steps.

For JSON Schema verification install the pinned test-only dependency outside the source tree:

```powershell
python -m pip install --target .temp/gameplay-api-python -r tools/gameplay-api/requirements-test.txt
$env:PYTHONPATH = (Resolve-Path .temp/gameplay-api-python).Path
python tools/gameplay-api/test_report_schema.py
```

## Executable verification

tools/gameplay-api/fixture/Run-Fixture.ps1 compiles a Rust test provider against the generated declarations and a C++ caller against the actual production validation helpers. Native and real Chrome/Wasm32 execute the same contract assertions. The fixture covers all table/record sizes and field offsets, lifecycle order, failed-attach cleanup, mandatory callback rejection, callback errors, capability rejection, UTF-8 descriptors, short-buffer canaries and engine-table calls.

```powershell
./tools/gameplay-api/fixture/Run-Fixture.ps1 -Target All -PlaywrightModule D:/path/to/playwright
```

The fixture reuses the qualified Task 1 Emscripten 6.0.12 and Rust nightly-2026-10-18 combination, rebuilding Rust std. It installs no toolchains; stages have a configurable timeout and terminate their process tree on expiry. Native reproduction currently selects Windows/MSVC; Linux is not claimed qualified. Reports are under logs/gameplay-api/fixture/; generated binaries stay in artifacts/gameplay-api/fixture/.

Native EngineTests additionally cover actual World generation reuse, wrong-world/thread access, stale contexts, reentrant scopes and diagnostic sink-to-owning-report forwarding. Existing native gauntlet and separate managed tests remain the C# compatibility baseline. The pre-existing standalone managed suite had 29 failures out of 160 before this task; do not report that suite green unless those independent issues are fixed.


## Agent gameplay pipeline

Build `Editor` (which also builds `aether-project`) through CMake, then run a saved scene without opening a window:

```powershell
./scripts/Run-GameplayPipeline.ps1 -Project projects/AgentPipelineRust -Operation test -Scene default -Ticks 60 -LaunchConfig projects/AgentPipelineRust/gameplay-test.json -ReportPath logs/gameplay-test.json
```

The same pipeline supports `create`, `inspect`, `build`, `run`, `relaunch`, `test`, and `publish` for desktop Rust and C#. `create` accepts `-Name`, `-Template` and `-Language`; C# remains the default. `-Async` returns a job ID immediately. Use `-Operation status -JobId <id>` or `-Operation cancel -JobId <id>` from another process. Jobs and logs persist under `logs/gameplay-jobs/<id>/`. MCP exposes `gameplay_job_start`, `gameplay_job_status` and `gameplay_job_cancel` with the same request fields. Restart an already-running MCP client once to discover these new static tools.

CMake drives Cargo or dotnet into isolated job directories. A failed rebuild preserves prior job artifacts and never runs stale code. Each run/relaunch/test starts a fresh bounded child runtime at frame zero from the saved scene, applying authored fields and remapped entity references. It does not restore live physics, coroutines, or heap state. Default limits are 60 ticks, step 1/60 second, and 180 seconds overall; requests cap ticks at one million and timeout at ten minutes. Cancellation and timeout terminate only the owned process tree.

The `native-headless-2d-v1` profile has no window, renderer, audio output or interactive UI layout. It supports script properties, transforms, synthetic keyboard input and Box2D gameplay. Unsupported 3D, linked scenes/prefabs, tilemap and interactive UI content fails explicitly. This verifies gameplay state, not rendered pixels. Existing editor Play, C# hot reload and visual publishing remain available.

Launch configuration is JSON with optional `input` entries (`tick`, `key`, `down`), `assertions` (`entity`, script `type`, `property`, `equals`, optional numeric `tolerance`), and `readSaves`. Tests require at least one assertion. Save reads are disabled unless `readSaves: true`; writes remain real project save writes. Snapshots include initial frame, tick count, policy and script properties. Compiler diagnostics include source locations where available; terminal reports follow `agent-report-v1.schema.json` and include logs/artifacts.

## Owned background editor and game windows

```powershell
$owned = ./scripts/Start-AgentEditor.ps1 -Project projects/AgentPipelineRust -Name 'Rust gameplay inspection' -Port 8891 | ConvertFrom-Json
'{}' | Set-Content logs/info-params.json
./scripts/Invoke-AgentEditor.ps1 -Receipt $owned.receiptPath -Method engine_info -ParamsPath logs/info-params.json
./scripts/Stop-AgentEditor.ps1 -Receipt $owned.receiptPath
```

Editor launch defaults to background mode: the native window stays hidden and does not request focus. Add `-Visible` for inspection. Agent windows always have a name, autosave is disabled for that session, and the title and receipt remind the agent to close them when finished. Cleanup saves deliberate dirty scene edits by default; `-DiscardChanges` explicitly discards them. Receipts validate PID, start time, executable, project and endpoint ownership before calls or closure. Keep the receipt even if a later operation fails.

MCP equivalents are `agent_editor_launch` (`project`, `name`, `port`, optional `background`, default true), `agent_editor_call` (`receiptPath`, `method`, optional `arguments`) and `agent_editor_close` (`receiptPath`, optional `discardChanges`). Receipt calls accept either wire method names or MCP tool aliases and target the owned window. General engine tools still use their configured default port.

A packaged game accepts `AetherGame --agent --agent-name "Gameplay inspection" --background` from its package directory. It uses baked project settings; a development runtime may also supply `--project <path>`. Native background mode keeps the window hidden through later show/mode changes. The launching agent owns game cleanup and must close its matching process/window when finished. For unattended gameplay verification, prefer the windowless jobs above.

## Editor modal input ownership

The editor prepares pending recovery/unsaved prompts and active modal ownership through `EngineClient::OnBeforeInput`, before input polling, camera updates and gameplay scripts. Other engine clients keep the default no-op hook; initialization and shutdown ordering are unchanged.

An editor modal releases FPS capture, forces a visible pointer and blocks gameplay keyboard, mouse and controller input. Global editor shortcuts are also blocked behind it. Keys and buttons handled while the modal is open remain quarantined through their release, so closing it cannot replay the same press into gameplay. Pointer capture requires an explicit viewport click afterward. Normal game Escape behavior remains available when no editor modal is open.

Escape on recovery chooses **Decide later**: it leaves the current scene, running Play session and recovery copies intact. Escape on the unsaved prompt cancels navigation. Neither path restores, discards or stops Play. `info` exposes `cursorLocked`, `cursorLockRequested`, `osCursorVisible`, `gameOwnsInput` and `editorModalInputBlocked` for agent inspection.

Task 6 qualification on Windows: build/abstraction guard and 1,451 native cases passed; both hidden Editor/GameRuntime validation smokes had zero findings. Actual Rust/C# jobs covered tests, fresh relaunch, publishing and failure handling. A recovery dialog on an isolated desktop preserved a running C# session, authored setup and both scene/recovery files after Escape; a fresh game Escape still reached the script. Owned editors and packaged games remained hidden, named and cleanly closed; Chinese/emoji project launch, preview and scene save passed. Python jobs passed eight tests on Windows and Linux WSL, MCP passed twenty tests, SDK/raw ABI passed nine/three tests plus three compile-fail checks, generator/schema passed five/four tests, and both ownership PowerShell suites passed. Standalone managed tests remain 131 passed and the exact same 29 baseline failures. Linux native qualification remains pending because its native toolchain is unavailable. Evidence: `logs/task6/`, plus `logs/modal-input-*` for focused RED/GREEN regressions.
