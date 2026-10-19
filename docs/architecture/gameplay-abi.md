# Shared gameplay ABI and agent diagnostics v1

Implemented in Tasks 2 and 3 of the Rust/WebGPU plan. C# lifecycle, discovery and properties now route through the shared host and v1 module table. The existing CoreCLR loader, ManagedInterop.hpp, managed ABI, compiler and project format remain compatible. Project language selection is Task 4; Rust execution is Task 5.

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

Providers contain their own exceptions/panics. Query and event helpers additionally translate C++ exceptions into SCRIPT_ERROR. The test fixture enables Emscripten C++ exception catching to exercise this behavior; Rust remains panic=abort. This does not make Rust panic-abort, native access violations or arbitrary foreign unwinding recoverable.

## C# host routing

`src/app/scripting/GameplayHost` owns copies of validated module descriptors and marshals lifecycle, ownership events and property values through the v1 function table. `CSharpGameplayHost` adapts the existing managed table, retaining its opaque managed handles so RPC dispatch still resolves the same instances. The registered host object's address stays stable across reloads.

`CSharpScriptingSubsystem` retains CoreCLR hosting, source compilation, build/reload state, error queues, RPC reflection/invocation, GC and managed editor windows. Its compatibility discovery/property methods forward to `Gameplay()`. The runner, inspector, MCP discovery and replication property bridge use the shared host; scene capture reaches it through the runner's existing live-property synchronization. Serialization stays name-based and preserves unknown types and fields.

Every load attempt refreshes the shared metadata generation and invalidates the adapter's active-handle set. Descriptor discovery does not instantiate scripts or read default getters. Legacy enum/component metadata strings, long UTF-8 strings and embedded NULs cross caller-owned buffers without retaining managed scratch pointers. Managed void callback failures are observed through the existing error callback; native C++ exceptions are contained, while native faults and runtime fail-fast remain unrecoverable.

Bootstrapping and shutdown remain backend-owned. The current uint32 entity creation path explicitly represents legacy C# world identity zero. It does not install `GameplayAccess` guards around existing C# exports. Rust must supply scoped entity identities and checked engine access when its backend is implemented. Physics events still execute inside the managed update callback; the adapter advertises only its shared ownership-event capability, rather than claiming a new physics or RPC event implementation.

Nine focused native adapter/runner tests cover lifecycle, missing types, stale handles, reported errors, property kinds, callback-driven storage growth and live scene-reference round-trips. A real C# fixture additionally exercises in-place reload, failed rebuild/Play recovery, sibling scripts, UTF-8/NUL strings, ownership, local RPC, 2D collision callbacks, movement and Stop restoration. Its published package also runs after relocation. Evidence is under `logs/task3/`.

## Engine access guard

GameplayAccess provides a minimal checked engine table for entity validation, copied position get/set and diagnostic delivery. Each access session receives a unique token and records the owner thread. It validates world/session, full generation-bearing ID and registry liveness before touching components. Stale tables fail after Invalidate or destruction. Wrong-thread calls fail without accessing the World.

The World must outlive GameplayAccess, or Invalidate must run before World teardown. A future host is responsible for that ownership order. Callback and structural-mutation scopes are exclusive and reject nested entry. They are not yet installed into the existing C# runner, and do not intercept direct calls to World. Diagnostic sinks execute synchronously outside the context registry lock and cannot recursively report on the same session.

The new position callback edits scene transforms; it is not a replacement for existing app-level physics-aware C# exports. The C# adapter retains those exports and their physics/controller semantics. No borrowed ECS reference is exposed.

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

BuildPublishAgentReport is a pure adapter around the existing PublishReport. Existing ok/summary/text fields are preserved. A pending publish stays queued/running; a completed legacy result determines passed/failed and a failure diagnostic. This task does not change the current publish control payload or switch existing C# publishing onto a new transport.

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
