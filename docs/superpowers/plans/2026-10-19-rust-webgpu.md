# Rust Gameplay and WebGPU Browser Exports Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox syntax for tracking. This is a proposed plan, not permission to begin product implementation.

**Goal:** Add Rust alongside desktop C# and deliver agent-verifiable WebGPU browser exports, first for Rust 2D games and later for C# games.

**Architecture:** Retain the C++ engine, EnTT world and native Vulkan renderer. Introduce a narrow shared gameplay host contract with C# and Rust adapters, plus a capability-limited WebGPU/browser runtime consuming extracted scene packets. Route CLI, MCP and editor operations through shared build/publish services.

**Tech Stack:** Existing CMake/C++23 or C++26, EnTT, Box2D, .NET; new Rust/Cargo, Emscripten, Emdawnwebgpu, WGSL, and Playwright browser verification. Exact toolchain pins are selected by Task 1 evidence.

**Spec:** ../specs/2026-10-19-rust-webgpu-design.md

## Global Constraints

- User-selected sequence: Rust browser first; C# browser follows. Desktop C# remains supported throughout.
- One gameplay language per project initially. Missing language metadata means csharp. Unknown explicit values fail validation.
- CI remains workflow_dispatch-only. No git hooks, scheduled runs or paid hosted browser testing are introduced.
- No git mutation or remote operation unless Pog Champ asks in that message. No task below implicitly authorizes a commit, branch, fetch or push.
- Keep Windows/Linux desktop and VS/Rider/CLion + CMake working. Native backend remains Vulkan. Engine lifecycle changes require dependency analysis for Full and UiShell.
- Preserve render-thread ECS isolation, descriptor-set conventions on Vulkan, GPU/resource lifetime rules and runtime-safe interop boundaries.
- Do not modify generated build output or third-party sources. Runtime logs/reports go under logs/ or other ignored output. Regenerate clangd after source/CMake changes during implementation.
- Browser MVP is single-threaded, single-player webgpu-2d-v1. No WebGL fallback, browser multiplayer, mixed C#/Rust scripts, or desktop-renderer parity is implied.
- Every new command/path/interface below is proposed unless identified as existing. Validate the task's exact interfaces against preceding landed tasks before coding.

## Review Focus

1. Legacy project/scene files lack backend metadata or retain missing script types: preserve defaults, names, values and references (Tasks 3-4).
2. A failed rebuild, missing DLL or ABI mismatch must leave a diagnosable state and must not execute stale function pointers (Tasks 2, 5-6).
3. Wasm32 layout, UTF-8 strings, panic outcomes and stale handles must not become silent corruption (Tasks 1-2, 5).
4. No GPU, denied adapter/device, context loss, tab suspension or failed asset fetch must not hang or falsely pass verification (Tasks 8-11, 13).
5. Relocated exports, iframe permissions and storage denial must preserve clear startup/save semantics (Tasks 10-13).

## Delivery order

M0: Tasks 1-2 feasibility and baseline.
M1: Tasks 3-4 shared host and compatible project selection.
M2: Tasks 5-7 desktop Rust and agent pipeline.
M3: Tasks 8-9 browser build and first engine-rendered scene.
M4: Tasks 10-12 playable Rust browser package.
M5: Task 13 unattended end-to-end acceptance and distribution qualification.
M6: Task 14 C# browser follow-on.
M7: Task 15 separately gated 3D/Rust-network expansion.

Do not expand a milestone while its exit checks fail. A failing feasibility gate changes the design explicitly rather than silently choosing another graphics API or deleting C# support.

## File and responsibility map

Existing files below were inspected during planning; new filenames are intended ownership boundaries, not claims that they exist.

| Responsibility | Existing integration points | Proposed additions |
| --- | --- | --- |
| Gameplay host and C ABI | src/engine/scripting/ManagedInterop.hpp; src/app/scripting/CSharpScriptingSubsystem.*; src/app/systems/ScriptComponentSystem.* | src/engine/scripting/GameplayAbi.h; src/app/scripting/GameplayHost.hpp; CSharpGameplayHost.*; RustGameplayHost.* |
| Rust declarations and safe SDK | Existing C#/native exports and component reflection | rust/Cargo.toml; rust/aethercore-sys/; rust/aethercore/; rust/aethercore-macros/; tools/gameplay-api/ |
| Project selection and scene data | src/app/project/ProjectCommon.*; src/editor/editor/EditorProjectContext.hpp; src/app/RuntimeProjectSettings.*; src/app/scene/serde/ScriptSerde.cpp | Shared project-language/capability records under src/app/scripting/; Rust templates under resources/ |
| Build and backend selection | CMake/Dependencies.cmake; CMake/SlangShaders.cmake; src/engine/CMakeLists.txt; src/app/CMakeLists.txt; CMakePresets.json | CMake/RustGameplay.cmake; CMake/WebRuntime.cmake; explicit browser source/dependency groups |
| Browser GPU and scheduling | src/engine/gpu/; src/engine/vulkan/; src/engine/rendering/RenderFramePacket.hpp; src/engine/AetherCore.*; src/app/Application.* | src/engine/webgpu/; src/engine/platform/web/; portable backend selection/capabilities under gpu/ |
| Web shaders and assets | src/shaders/sprite2d.slang; src/shaders/include/DevicePointer.slangh; tools/assetpack/; src/engine/io/ | Backend-specific shader bindings; browser-compatible texture cooking and VFS/storage adapters |
| Publishing and agent execution | src/editor/editor/publish/; tools/mcp/aethercore_mcp.py; scripts/Run-DebugGauntlet.ps1 | tools/project/; scripts/Run-GameplayPipeline.ps1; scripts/Run-BrowserGauntlet.ps1; tools/browser-tests/; resources/web/ |
| Reference games and checks | tests/app/; tests/scene/; tests/scripting/; managed/AetherCore.Tests/ | projects/AgentPipelineRust/; projects/AgentPipelineCSharp/; focused ABI/host/export/browser checks |

Read directory-local AGENTS.md files before creating/editing these paths. Adapt the map if actual ownership differs; do not duplicate services just to match a filename.

## Task 1: Prove the combined browser toolchain before architectural extraction

**Files:** Create tools/experiments/rust-webgpu/ and docs/architecture/rust-webgpu-toolchain.md. Product engine files remain unchanged for this isolated experiment.

- [x] Record the existing revision, build presets, C# reference scenes and available GPU/browser/toolchain versions. Run the existing local gauntlet and managed checks as documented by the repo; record baseline failures separately from new work. Do not claim a pre-existing failure was fixed.
- [x] Build the smallest native C++/Rust C-ABI caller/callee, including an integer, a UTF-8 buffer, an owned string result copied through a caller-provided buffer, a callback, and a layout record checked on both sides.
- [x] Build the same calls with Rust wasm32-unknown-emscripten and compatible Emscripten C++. Verify callbacks and memory ownership in a served browser page. Investigate exception flags, Rust std compatibility, LLVM output and archive/linker compatibility against the actual pinned versions.
- [x] Render a textured sprite through Emdawnwebgpu and a browser canvas. Compile a minimal Slang-to-WGSL sprite variant with explicit storage/uniform layouts; validate shader creation and pixels on a real GPU. Exercise denied/unavailable adapter as a failure test.
- [x] Probe representative first-profile C++ standard-library/dependency usage with the same toolchain. Record required standard/feature exclusions and ensure these do not lower the native build's language requirements.
- [x] Save exact versions, compiler flags, native and Wasm ABI layout results, browser/GPU identity, artifact bytes, startup time and reproducible commands. Keep outputs under ignored paths; commit-worthy source contains no binaries or generated shader output.

**Gate:** Both real interop and real WebGPU rendering succeed. If either fails, update the design with the specific blocker and smallest viable alternative before Task 3. A standalone Rust wasm-bindgen demo is not evidence of C++/Rust Emscripten interoperability.

## Task 2: Establish the runtime and agent contracts

**Files:** Create GameplayAbi.h, tools/gameplay-api/ manifest/generator and focused tests/scripting ABI tests. Extend shared diagnostic/report services rather than creating incompatible parallel report formats.

**Interfaces decided here:** ABI major 1, explicit structure size negotiation; engine and module function tables; type/property descriptors; caller-owned UTF-8 buffer results; opaque module instance handles. Proposed module entry is int32_t aether_gameplay_query(uint32_t requestedMajor, uint32_t outSize, AetherGameplayModuleApi* outApi). Define the complete v1 record layouts in this task and use static assertions plus Rust layout tests. C++ callers initialize the table before invocation; a failure does not expose callable entries.

- [x] Write failing ABI tests for major/size mismatch, missing mandatory entry, non-ASCII names, short buffer handling, lifecycle callback order and error propagation. Include 32-bit Wasm execution, not only native sizeof assertions.
- [x] Define the narrow shared lifecycle/metadata API around the existing managed API's actual behavior. Keep GC controls, Roslyn compilation and C# editor-window registry in the C# adapter. Define capability discovery for unsupported language/profile operations.
- [x] Define the engine-call manifest for the initial shared surface and generate thin C/Rust declarations. Keep ergonomic C# wrappers and existing export names compatible; add a manifest parity check rather than rewriting the entire SDK.
- [x] Specify handle validation, world identity, call-thread constraints, buffer ownership and panic strategy. Add tests for destroyed/recycled entities, wrong-world handles and reentrant structural mutation; do not return ECS borrows across the boundary.
- [x] Define JSON report schema version 1, including job/status, phase, language/target/profile, diagnostic code/source span, toolchain identity, artifacts and timing. Extend existing publisher report semantics through adapters. Specify terminal success/failure/cancelled/skipped states and exit codes.

**Gate:** Native and Wasm contract fixtures pass; failure reports are machine-readable; no pointer-bearing layout becomes persistent content.

## Task 3: Adapt existing C# execution without changing gameplay semantics

**Files:** GameplayHost.hpp, CSharpGameplayHost.*, CSharpScriptingSubsystem.*, ScriptComponentSystem.*, ScriptSerde.cpp, src/app/net/CSharpScriptFieldBridge.*, existing script inspector/control consumers and managed tests.

**Proposed internal interface:** GameplayHost exposes language/capabilities, script type/property discovery, create/destroy, attach/update/detach, event dispatch, property get/set/defaults and diagnostic reporting using existing Expected/error conventions. It adapts the v1 function table without exposing a .NET pointer to generic consumers. Backend-private operations are not forced into universal no-op methods.

- [x] Add characterization checks for attach/update/detach, multiple scripts, missing scripts, property defaults, scene/prefab entity-reference remapping, physics events, C# RPC/replication and script rebuild failure. Review the unchanged backend-private C# editor-command path.
- [x] Route the runner, persistence, inspector/MCP property access and networking bridge through the shared host where necessary. Retain C#-specific compilation/reload behavior behind its adapter and preserve cache-generation invalidation.
- [x] Verify unknown script types retain their serialized fields and report a diagnostic. Inactive/missing backends must not erase scenes or convert type identities.
- [x] Run existing C++/managed checks and the native gauntlet. Open/run/publish the baseline C# reference project through current control operations.

**Gate:** C# behavior and package operation remain valid. No Rust execution is necessary to prove this task.

## Task 4: Add compatible per-project language selection

**Files:** ProjectCommon.*, EditorProjectContext.hpp, RuntimeProjectSettings.*, project descriptor serializer, shared creation/control services, Rust template resources; tests/app/Project* and runtime package resolution checks.

**Metadata:** [scripting].language accepts csharp or rust; absent means csharp. Keep existing script type strings intact. Publish target is separate from project gameplay language. The first profile permits exactly one host per project.

- [x] Test missing, valid and unknown language values; Rust selection with missing toolchain; existing C# scene load; relocated runtime project/module resolution; and rejection of mixed-language declarations.
- [x] Generate Rust Cargo manifests and template gameplay using shared project creation. Resolve SDK paths from declared engine/toolchain locations, not a hardcoded developer checkout. Preserve current C# generation for old projects.
- [x] Expose language and profile through CLI/MCP capabilities and project creation. Reports identify missing prerequisites before spawning builds. Avoid modal picker dependencies in agent paths.

**Gate:** An agent can create and inspect either language project through the same service. Missing Rust tooling does not break C# configure/run.

**Task 4 verification (Windows, 2026-10-19):** The full native gauntlet passed with 1,401 native cases and both GPU smokes reporting zero validation findings, including strict metadata, settings serialization, recent projects, missing prerequisites, custom scripts paths, stale selection and moved runtime/module paths. Live agent controls created Rust and default C# projects with Cargo/rustc overrides deliberately unavailable. Rust opened/saved/reopened without CoreCLR, its Cargo scaffold checked offline, and Play/publish returned schema-v1 failures through CLI and MCP. C# Play/Stop and publishing succeeded; the published C# package booted after relocation. Standalone managed results remain 131 passed / 29 failed, with the exact Task 3 failure set. Generator checks, five generator tests, four report-schema tests, eight transport tests and the raw ABI crate test passed. Evidence: `logs/task4/`.

At the Task 4 checkpoint Rust gameplay was intentionally unavailable; Task 5 below adds execution.

## Task 5: Implement desktop Rust gameplay and the safe SDK

**Files:** rust/ workspace and crates, RustGameplayHost.*, CMake/RustGameplay.cmake, relevant Engine/RuntimeFramework source selections, tests/scripting/ and projects/AgentPipelineRust/.

- [x] Add ABI/layout tests and Cargo unit/compile-fail tests proving invalid safe-SDK handle use returns errors and safe callers cannot obtain long-lived mutable ECS references.
- [x] Generate registration/property metadata from Rust script declarations. Reuse stable names and existing property representations; include property defaults, strings, enums, entity/component references and physics callbacks required by the reference scenario.
- [x] Implement native shared-library registration/loading with target-correct C ABI, version checks and backend-owned allocation/destruction. Load the new module only after build success. Safely stop calls, destroy instances and release callbacks before unloading; first version may relaunch the child runtime instead of live unload in editor.
- [x] Test missing library, wrong ABI, recoverable script error, caught unwind panic, process-aborting panic/fault and failing rebuild. Report each accurately; never allow failed code to execute via an old pointer.
- [x] Add safe SDK operations for transforms, input, sprites, logging, scene/prefab operations and supported physics/UI/audio/save functionality as their fixture needs them. Unsupported Rust APIs return explicit capabilities/errors rather than misleading stubs.
- [x] Build/run the Rust fixture on native Windows and Linux where available; report unavailable platform checks explicitly. Package the module and Rust runtime dependencies needed by the selected toolchain.

**Gate:** Rust executes within the existing engine; property edits and scene round-trips work; desktop C# remains green. Rust does not claim network/editor-tool parity yet.

**Task 5 verification (Windows, 2026-10-19):** The native gauntlet passed 1,425 cases and both validation-enabled Editor/GameRuntime smokes with zero findings. The final review rebuild passed all 1,426 native cases (217,093 assertions), including real CMake/Cargo builds, failed rebuild preservation, loader/ABI failures, strict agent property typing and cancelled-build/staging diagnostics. Cargo passed nine safe-SDK tests, three raw ABI/layout tests and three compile-fail checks. Five bounded provider child checks covered normal callbacks, errors, unwind, abort and panic-abort. Live Rust exercised movement, input, sprite, Box2D events, UI, saves, UTF-8/NUL properties, references, Stop restoration, failed-build recovery, contained panic/sibling continuation and terminal cancellation. Publishing passed and the relocated Rust package executed 60 updates. Existing C# lifecycle, ownership, RPC, physics, hot reload, Stop restoration and publishing passed. Standalone managed tests remain 131 passed / the exact same 29 failures as Task 4. Generator/schema/transport checks passed. Linux could not be qualified: no Linux Rust/C++ development toolchain is available. Evidence: `logs/task5/`. Browser export, network/editor-tool parity and unified headless jobs remain later tasks.

## Task 6: Make rebuild/run/relaunch an agent operation

**Files:** Shared gameplay build service under src/app/scripting/, tools/project/ native command target, scripts/Run-GameplayPipeline.ps1, tools/mcp/aethercore_mcp.py and existing control dispatch; tests/app/ integration checks.

- [x] Add command-level tests for argument validation, structured Cargo diagnostics, compiler failure, missing SDK, timeout, cancellation and paths with spaces/non-ASCII characters. Invoke tools with argument arrays and controlled working directories.
- [x] Implement asynchronous create/build/run/test/publish jobs with a job ID and status polling. CMake orchestrates Cargo and existing dotnet/native tools; CMake remains the canonical engine build, with lockfiles/toolchain pins recorded.
- [x] Define initial Rust iteration as rebuild then child-runtime relaunch from a named scene and explicit launch configuration, starting at tick zero. Preserve the editor-authored scene snapshot/property references through existing serialization; headless runs use saved content. Persistent saves load only when requested. Test authored-field/reference restoration, tick-zero restart, explicit save loading and absence of automatic coroutine/physics/live-heap restoration. A failed build preserves source/content and a usable previous artifact without calling it the new result.
- [x] Add CLI/MCP access with the same services and report schema. No live editor is required for build/publish/test. Preserve existing editor control methods as adapters.

- [x] Verify editor modals release viewport capture and own keyboard input, including Escape, without stopping Play or restoring/resetting the scene. Preserve the authored setup and normal game Escape behavior when no editor modal is active.

**Acceptance command:** ./scripts/Run-GameplayPipeline.ps1 -Project projects/AgentPipelineRust -Target desktop -Operation test -Scene default -Ticks 60 -LaunchConfig projects/AgentPipelineRust/gameplay-test.json -ReportPath logs/rust-desktop-report.json

**Gate:** Zero exit code plus terminal passed report, script state assertions and captured logs. Failure/cancellation tests must terminate within their declared timeout.

**Task 6 verification (Windows, 2026-10-19):** Guard/build and 1,451 native cases passed. Hidden Editor/runtime GPU smokes passed with zero validation findings. Rust and C# CLI/MCP jobs, C# editor Play/hot reload/publish, failure preservation, crash/cancel/timeout and save opt-in checks passed. Native Unicode launch/preview/save and real recovery Escape/capture checks passed with owned-process cleanup. MCP stdio discovery and persisted status survived process restart; Python jobs passed on Windows and Linux WSL. Standalone managed baseline remains 131 passed / the same 29 failures. Linux native qualification is pending; the initial headless profile explicitly excludes GPU/3D and interactive UI. Evidence: `logs/task6/` and `logs/modal-input-*`.

## Task 7: Establish an equivalent C#/Rust reference scenario

**Files:** projects/AgentPipelineRust/, projects/AgentPipelineCSharp/, fixture input/state expectations, local comparison/report tooling.

- [x] Implement equivalent small scenarios: moving sprite/player, collision/trigger, spawned/despawned prefab, UI counter, sound, transition to a second scene and a saved score. Reuse assets and test inputs rather than fork two unrelated games.
- [x] Use seeded inputs and fixed simulation steps for state assertions, with tolerances for physics. Compare observable gameplay outcomes, not internal memory layouts or byte-identical physics.
- [x] Record cold/warm build, edit-to-ready, compiler feedback/correction cycles under a documented comparable agent task, startup, memory and frame timing. Do not rank languages from a single run or claim agent productivity from compiler timing alone.

**Gate:** Both desktop scenarios pass without editor interaction. This fixture becomes the browser acceptance game; M2 is complete.

**Task 7 verification (Windows, 2026-10-19):** Both hidden desktop scenarios passed three paired samples, including native checkpoints, rendered captures, saved score/Unicode readback and source-located compiler feedback/correction. First-tick readiness is separate from completion timing. Final guard/build and 1,454 native cases passed with the real Rust SDK module enabled; both hidden Editor/runtime GPU smokes had zero validation findings. Managed SDK: 137 passed / exact same 29 baseline failures, including six new passing save cases. Python project suite: 12 passed / one explicit GPU test skipped by default; the opt-in two-language detach-failure regression passed. MCP 20, safe Rust SDK 9 and compile-fail 3 passed. See [reference comparison](../../gameplay-reference-comparison.md). Browser and native Linux qualification remain later work.

## Task 8: Separate browser runtime sources, dependencies and host tools

**Files:** CMake/Dependencies.cmake, CMake/DotNet.cmake, CMake/SlangShaders.cmake, src/engine/CMakeLists.txt, src/app/CMakeLists.txt, CMakePresets.json, CMake/WebRuntime.cmake and source-group definitions.

- [x] Add configure/build checks for native default, runtime-only native, and proposed webgpu-browser preset. The browser configure succeeds without desktop Vulkan/.NET discovery and excludes editor, Vulkan/VMA/Aftermath, ENet/plum, Jolt and unused thread-dependent native systems.
- [x] Replace broad inclusion only where target selection requires it; keep shared portable sources shared. Explicitly select Box2D; keep browser audio/storage/input unavailable until Task 10, as documented in the foundation capability report. Audit IOThread, asset worker queues and physics workers, not just render-thread startup.
- [x] Build AssetPacker and shader compiler for the host and feed their verified executable paths into cross-build packaging. Add a test that rejects a Wasm binary offered as a host tool.
- [x] Run build dependency and artifact-import audits: browser output contains neither hostfxr nor Vulkan/native socket imports. Native C# and Launcher UiShell continue configuring and running.
- [x] Regenerate clangd using the existing preset and run graphify update after structural changes; never edit generated databases.

**Gate:** A runtime-only Wasm target links using the pinned tools, native tools produce its assets, and no unsupported native dependency sneaks into the browser graph.

**Qualified:** Windows default/runtime-only native and pinned Wasm builds; 9 audit tests; detailed native 1,454/1,454 after one non-reproducing failure in an earlier gauntlet run; both hidden GPU smokes with zero validation findings; C#/Rust desktop fixtures and Launcher UiShell. See [build boundary and evidence](../../architecture/browser-runtime-build.md). Browser graphics remain Task 9.

## Task 9: Add the minimum WebGPU backend and browser frame lifecycle

**Files:** gpu/ backend/capability/resource interfaces, webgpu/ implementation, platform/web/, AetherCore.*, Application.*, RenderFramePacket.hpp consumers, sprite rendering and shader build rules.

- [x] Document Full/UiShell/native/browser initialization and teardown dependencies before editing them. Add an asynchronous browser initializing/ready/failed/device-lost lifecycle; no main-thread synchronous wait for adapter/device/queue completion.
- [x] Separate portable resource/pass intent from Vulkan barriers, descriptor addresses and timeline semaphores. Introduce only operations needed by the 2D path, retaining Vulkan's existing implementations and invariants.
- [x] Implement WebGPU buffers, textures, samplers, pipeline/layout creation, upload, render pass, canvas resize and asynchronous retirement/completion. Validate resource lifetimes under delayed completion and shutdown/device loss.
- [x] Compile the Task 1 validated sprite WGSL path with explicit frame/instance buffers and bounded texture batches. Add layout, clipping, transparency/sort and texture-batch-overflow tests. Never silently truncate draws to a binding limit.
- [x] Drive a browser frame callback through input, game update, extraction and rendering sequentially. Consume actual engine frame packets; do not substitute the standalone spike renderer.
- [x] Verify textured sprite pixels, resize, unsupported adapter/limits, device loss and no-hang shutdown. Test on a real GPU and record validation errors. Software-only rendering cannot satisfy the real-GPU gate.

**Gate:** A Rust script changes a real scene entity that is rendered through WebGPU with clean validation. M3 is complete.

**Qualified:** Engine World/packet + safe Rust SDK/provider gameplay rendered on hardware Chromium, including input, all 4,096 texture pixels, clipping, transparency, resize, overflow draws, terminal errors and asynchronous shutdown. C#/Rust desktop fixtures, native build/unit/GPU checks preserved. See [lifecycle and qualification](../../architecture/webgpu-lifecycle.md). Complete assets/audio/save/export packaging remain Tasks 10-11.

## Task 10: Port browser assets, audio, input and saves

**Files:** io/ VFS/storage adapters, tools/assetpack/ texture and package cooking, audio/ backend selection, platform/web/ input, resources/web/ loader, 2D/UI/animation rendering paths.

- [x] Add tests for asynchronous asset preload, missing/corrupt package, unknown asset version, UTF-8 paths and target format mismatch. Reuse VFS/PAK contracts after validating endian/alignment/compression behavior.
- [x] Add target-qualified cooked artifacts. Use RGBA8 as the initial portable texture baseline; compression is capability-gated. Rebuild desktop BC-format artifacts separately; never reuse a desktop-only DDS cache by accident.
- [x] Support the reference game's sprites/animation, tilemap batches, Box2D and basic UI text/images/widgets. Validate font/assets and large texture/batch limits; unsupported features remain explicit.
- [x] Implement browser audio initialization after a gesture and test sound scheduling/resume. Route supported miniaudio browser backend or Web Audio integration based on the pinned toolchain evidence; do not claim audio success from a silent/headless placeholder.
- [x] Map canvas keyboard/pointer/gamepad input, resize/focus and visibility transitions. Reset timing on resume to avoid a giant catch-up delta. Preserve fixed-step simulation semantics.
- [x] Implement browser save storage with explicit asynchronous flush/error completion. Test a fresh page reading the saved value at the same origin and meaningful failure on denied/quota-exhausted storage; do not report a save before flush succeeds.

**Gate:** Reference gameplay, sound, transition and save/reload all work in-browser. No threads/SharedArrayBuffer are required by the first profile.

**Qualified:** 31/31 hardware Chromium cases, including real Rust reference gameplay/audio/transition/durable save/reload and short-key/UI-order/audio-component regressions; 13 host/audit Python checks; native 1,460/1,460 plus hidden GPU smokes and C#/Rust desktop fixtures. See [assets and platform contracts](../../architecture/browser-assets-and-platform.md). Physical controllers and other browser/OS combinations remain unqualified.

## Task 11: Add browser automation and honest verification reports

**Files:** tools/browser-tests/, scripts/Run-BrowserGauntlet.ps1, dev-only runtime/browser test bridge, shared fixture/report definitions and tools/mcp/aethercore_mcp.py.

- [x] Implement a versioned test-build bridge exposing readiness, queued fixed-step/input commands, state snapshots, logs and capture. Dispatch gameplay commands on the engine tick; do not bypass game behavior by editing arbitrary state from Playwright.
- [x] Exclude the bridge from release packages and verify its absence. Native editor ENet control remains native; do not expose subprocess/file operations in the browser bridge.
- [x] Launch a bounded local server and real browser process, exercise the reference scenario, capture console/page/WebGPU errors, screenshots, state evidence and timing. Clean up child processes on timeout/cancellation/failure.
- [x] Add tests for no WebGPU, failed asset fetch, runtime trap/panic, lost device, visibility resume, input focus and save denial. Required unavailable hardware/browser checks produce skipped/blocked results, never aggregate passed.
- [x] Add tolerant rendering assertions independently from deterministic state checks. Record browser/OS/GPU, adapter kind and test-bridge version with every report.

**Proposed command:** ./scripts/Run-BrowserGauntlet.ps1 -Project projects/AgentPipelineRust -ReportPath logs/browser-report.json

**Gate:** Zero exit code requires all required cases executed and passed. Native gauntlet results and browser results remain distinguishable.

**Qualified:** 41/41 Windows hardware Chromium cases, including engine-owned fixed-step input, actual Rust panic, production bridge absence and oversize-fetch cancellation; real command/descendant timeout tests; native 1,460/1,460, hidden GPU smokes and C#/Rust desktop reference fixtures. Reports and limits are documented in [browser automation](../../architecture/browser-automation.md). Generic project publishing remains Task 12.

## Task 12: Add target-aware Web publishing with preflight

**Files:** src/editor/editor/publish/PublishPlan.*, PublishSteps.*, PublishReport.*, PublishVerify.*, shared publish service/CLI, resources/web/; tests/app/Publish*; project additional-assets handling.

- [x] Add explicit desktop/web target and webgpu-2d-v1 profile selection without confusing host OS or editor build config with runtime target. Preserve old target defaults for existing C# desktop publishing.
- [x] Preflight language, toolchains, module capabilities, reachable scenes/prefabs/assets and explicit additional assets. Test unknown dynamic scene paths, Rust networking, unsupported shader/material/3D content and C# Web requested before M6; each gets a specific diagnostic and nonzero result.
- [x] Stage index.html, loader, Wasm, target assets and a versioned manifest with relative paths, sizes/hashes, engine revision and toolchain pins. A failed build does not overwrite an existing valid export. Make the HTML start/loading/error flow usable inside an iframe.
- [x] Package a ZIP rooted at index.html. Verify unpacking into a different directory and serving at a nested URL with correct MIME types and secure-context requirements. Do not depend on source checkout paths or native executable assets.
- [x] Wire CLI/MCP publish/status and the editor Publish panel to this shared service. Include package verification evidence in the terminal report.

**Proposed command:** ./scripts/Run-GameplayPipeline.ps1 -Project projects/AgentPipelineRust -Target web -Operation publish -ReportPath logs/web-publish-report.json

**Gate:** Relocated browser ZIP runs locally and in a local iframe; missing WebGPU shows an actionable message. M4 is complete.

**Execution (2026-10-20):** Complete. Selected-project Rust static linking and target cooking, shared editor/agent publisher, declared-content preflight, manifest/ZIP staging and relocated hardware-browser/iframe verification are implemented. Failed compilation preserves the old verified ZIP byte-for-byte. A customized module with different score behavior and another retaining ReferenceStart without the fixture transition both publish correctly. Actual hidden editor publish/status exposes its verification report and closes cleanly. C# Web fails explicitly; C# desktop publishing and C#/Rust desktop references pass.

Verification: native full gauntlet 1,462/1,462 with Editor/GameRuntime builds and hidden GPU smokes; final 41-case hardware Chrome browser suite; 47 focused native publishing/pipeline cases; 29 Python project tests (28 passed, one opt-in integration skipped) and 22 MCP tests. Actual Web export checks run separately and pass. Review findings fixed with regressions: bounded length-prefixed tilemap dependency parsing, canonical scene/camera defaults with headerless prefab support, generic verification without fixture-specific gameplay assertions. A real startup failure also exposed the missing Web diagnostic sink; logging now preserves length-aware UTF-8/NUL and bounded structured evidence. Follow-up review clear. See [Web exports](../../web-export.md) and local reports under `logs/task12/`. External hosting qualification and unattended creation-to-export remain Task 13.

## Task 13: Verify autonomous creation-to-export and distribution

**Files:** scripts/ gauntlet entry points, tools/browser-tests/, tools/mcp/README.md, docs/getting-started.md and new docs/rust-gameplay.md / docs/web-export.md.

- [x] Start from a fresh agent-created Rust project, generate gameplay/content through supported services, deliberately introduce and correct a compiler error, then build/test/publish. Assert no modal or human editor action is required at any step.
- [x] Run native C# regression, desktop Rust and browser suites. Extend the existing gauntlet report or aggregate it without changing the meaning of full native verification. Do not automatically install toolchains/browsers or trigger hosted CI.
- [x] Measure ZIP download bytes, uncached first interactive frame, warm startup, memory, p95 frame time and edit-to-ready. Report measured values with machine/test details; set numerical budgets from this baseline before expanding content scope.
- [x] Qualify real-GPU Chromium/Windows first; record named browser/OS versions for Firefox/macOS/Safari checks and explicit unsupported combinations. Test iframe WebGPU permission policy, audio gesture, keyboard capture and reload behavior.
- [ ] Prepare an itch.io-compatible package and instructions. Exercise an actual draft/test upload only when Pog Champ authorizes the upload or provides an existing authorized destination. Local iframe success does not substitute for host qualification. Missing authorization blocks only this external qualification step, not local verification.
- [x] Document supported capabilities, deferred features, troubleshooting codes, example commands, dependency pins and how agents reproduce failures. Correct affected stale feature claims rather than adding contradictory docs.

**Gate:** M5 passes unattended local acceptance. Mark host qualification separately pending/passed with a URL and evidence. No first-release claim of broad browser support without recorded qualification.

Local gate passed (2026-10-20): `logs/task13/distribution-ui-final.json` records fresh
create/error/repair/build/native/publish/edit/re-publish, rejected-content preservation,
measured hardware direct/iframe behavior, full native 1462/1462 plus GPU smokes and
41/41 browser cases. Windows Chrome 155.0.8059.85 / NVIDIA hardware observed; other
browser/OS combinations remain explicitly unqualified. Chrome did not recognize a
`webgpu` permissions-policy token, recorded as unknown rather than claimed enforced.
External-host qualification remains pending by Pog Champ's explicit choice.

User-facing follow-up: the player uses the actual editor PNG and Night Amber colors,
explains file:// local serving, and focuses the canvas on Play. Empty development URLs
select the playable Ember Run sample. `logs/task13/ember-browser.json` verifies movement,
score, collection audio, retry, completed round, durable best score/reload and direct/iframe
Play focus; `logs/task13/ember-native.json` verifies the desktop sample. Review clear.

## Task 14: Deliver C# browser support as the explicit follow-on

**Files:** Isolated .NET Wasm experiment first; then browser-managed build configuration, managed/AetherCore.Interop/, existing Native imports/ABI adapter, browser runtime linkage, publisher capability checks and AgentPipelineCSharp fixture.

- [x] Prove a pinned .NET Wasm runtime can link/call the selected C++/WebGPU engine subset: scalar/string/struct calls, unmanaged callbacks and per-frame script update. Test trimming/AOT/reflection metadata and toolchain version compatibility. Do not reuse desktop hostfxr or assembly-unload assumptions.
- [x] Choose and document interpreter/AOT publishing and build host ownership from measured evidence. Keep one linked memory/ABI strategy explicit. Validate current reflection-discovered scripts/properties under the actual publish configuration and generate preservation metadata where needed.
- [x] Keep desktop compilation/reload/editor tooling intact; browser runs prebuilt gameplay. Report unsupported CLR/NuGet/native APIs during export or with a clear startup diagnostic. Browser hot compilation/reload is not an acceptance requirement.
- [x] Run the same reference scenario, state/storage/iframe checks and JSON reports with C#. Compare payload/startup/iteration against Rust without changing the existing default language automatically.
- [x] Update capability manifests so C# Web is enabled only for the proven profile, with tests showing older unsupported-target errors are replaced by actual functioning exports.

**Gate:** M6 requires a real C# game in the browser and desktop regression evidence. If interop fails, retain the working Rust browser release and publish the specific blocker; do not label C# browser complete.

**Task 14 verification (Windows, 2026-10-20):** Pinned stock and engine-toolchain .NET Wasm ABI probes passed. CMake now links the existing C# gameplay adapter and WebGPU engine into Mono's single Wasm heap; the supported export mode is trimmed interpretation. AOT was tested separately and rejected by the pinned compiler with `Target-incompatible DataLayout`, so it remains unsupported. Fresh C# projects passed normal CLI Web publishing, serialized float/vector/string/enum/entity/component-reference overrides after trimming, a semantic edit and republish, and the same direct/cross-origin iframe physics, audio, scene and durable UTF-8/NUL save/reload scenario as Rust. Native reference scenarios for both languages, C# desktop publishing, all 1,462 native cases, both GPU validation smokes and all 41 Rust browser cases passed; the serial aggregate's local acceptance passed. Standalone managed tests returned 137 passed and the exact same 29 baseline failures; no new failure names. The paired C# ZIP measured 2,151,472 bytes, 381.0/437.6 ms cold/warm interactive navigation, 32 MiB linear memory and 19.2 seconds edit-to-verified-export. SDK prerequisites and unsupported APIs fail explicitly; an actual unimplemented native import left the previous verified ZIP unchanged. Evidence: `logs/task14/qualified.json`, `logs/task14/managed-baseline-comparison.json`, `logs/task14/unsupported-managed/qualification.json` and [C# browser support](../../csharp-web.md). External hosting remains pending. Task 15 is not started.

## Task 15: Gate expansion on measured game needs

This is a roadmap boundary, not permission to expand the initial release.

- [ ] Define a separate WebGPU 3D profile with mesh/material/depth/PBR, skeletal animation and supported lighting requirements before adding passes. Test resource limits and desktop/browser differences explicitly.
- [ ] Define Rust networking parity independently: shared replication/property semantics and native transport first; browser WebSocket/WebRTC transport and host interoperability require a separate design. Existing ENet behavior is not browser transport support.
- [x] Measure whether Rust rebuild/relaunch materially limits agent throughput. Design safe state migration/native hot reload only if those measurements justify its additional lifetime complexity.
- [ ] Revisit default gameplay language only after equivalent agent tasks and shipped scenarios establish a clear benefit. Keep C# project compatibility independent of that choice.

**Agent workflow follow-on (2026-10-21):** The user selected throughput work. Repeated native jobs discarded their compiler intermediates. Project-scoped Cargo/MSBuild caches now retain compilation work while each runnable module stays job-private, with process locking through staging and current-invocation artifact validation. Three-sample median build time improved from 3.48 to 1.83 seconds for Rust and 5.33 to 2.81 seconds for C#; test/relaunch improved 29–33%. Semantic edits, intentional compile failure and repair passed for both. These results support incremental compiler reuse; they do not justify arbitrary-state migration or native hot reload. No default language, WebGPU 3D or networking decision is implied. See [agent iteration](../../agent-iteration.md) and the [scoped follow-on plan](2026-10-21-agent-iteration.md).

## Verification and execution notes

Existing native verification command: ./scripts/Run-DebugGauntlet.ps1 (or the existing MCP run_gauntlet tool). Preserve required full mode semantics; skipped smokes do not count as a full pass.

All newly named scripts/targets are deliverables, not commands available today. Each must have help output, exact documented parameters, bounded execution, nonzero failure codes and JSON examples when introduced. Validate actual upstream compiler flags at Task 1 and record them instead of guessing commands in advance.

After each task: run its targeted checks, then appropriate affected regression checks; attach report/artifact paths. After CMake/source additions regenerate clangd. No implementation, test run, toolchain installation or external upload was performed as part of writing this plan.

## Planning self-review

- Shared host, Rust desktop, WebGPU sources/shaders, browser platform, target-aware cooking/publish, unattended testing and follow-on C# browser each map to explicit tasks and gates.
- Legacy defaults, missing scripts, ABI mismatch, invalid handles, panic outcomes, missing GPU, device loss, interrupted jobs, storage denial and moved packages have named verification ownership.
- New ABI layouts and toolchain pins intentionally depend on executable feasibility evidence rather than unsupported fixed values. No downstream step assumes a prototype proves full engine portability.
- Initial capability scope is stated; first-release success does not silently imply Rust feature parity, C# Web, multiplayer or full 3D.
- Fresh design-builder review completed; its restart-state question was resolved in the spec and Task 6 using explicit scene/configuration restoration, rather than arbitrary runtime-state migration.

## Task 1 execution record (2026-10-19)

Completed the isolated Rust/C++ native and browser ABI checks, real-GPU WebGPU sprite/negative cases, and representative C++26/Box2D browser probe. Sources and bounded reproduction runner are under tools/experiments/rust-webgpu/. See docs/architecture/rust-webgpu-toolchain.md for tested pins, evidence and existing baseline failures. Native gauntlet passes; standalone managed baseline has 29 failures. Task 2 has not begun; M0 is not yet complete. No engine architecture was changed. Fresh review's timeout finding was fixed, failure-path verified, and scoped re-review found no significant residual issues.

## Task 2 execution record (2026-10-19)

Implemented the additive C-compatible v1 ABI, native validation and world/thread/lifetime access guards; generated C/Rust declarations with five-export C#/native parity checks; owning diagnostics and schema v1 with a pure legacy publisher adapter. The existing C# loader, managed ABI, compiler, hot reload and gameplay runner remain on their prior path. Production host routing is Task 3, not part of this task.

Verification: native and Chrome Wasm32 foreign-consumer fixtures each pass139 checks including85 layout comparisons. Native gauntlet passes1367 cases plus editor/runtime GPU smokes. Generator/parity checks and four JSON Schema tests pass. Standalone managed results remain131 passed/29 failed with the exact same failing-test set as the Task1 baseline. Fresh review found no actionable P1/P2 issues. See docs/architecture/gameplay-abi.md and logs/gameplay-api/. M0 Tasks1-2 are complete; Task3 has not begun.

## Task 3 execution record (2026-10-19)

Added the reusable `GameplayHost` consumer and `CSharpGameplayHost` v1 adapter. The runner, inspector, MCP type discovery, live scene capture and replicated-property bridge now use the shared path. CoreCLR compilation/reload, RPC metadata/invocation, GC and editor commands/windows retain their backend-specific implementation. Managed exports, scene formats and publisher payloads remain compatible. Legacy C# uses world identity zero; checked Rust world access remains Task 5.

Verification: nine focused adapter/runner tests pass145 assertions; the full native gauntlet passes1376 cases plus editor/runtime GPU validation smokes with zero findings. API parity/generator CTests pass. Managed results remain131 passed/29 failed with exactly the same failing names as Task2. A fresh independent source review found no actionable P1/P2 defects. The first gauntlet had a transient native failure and a control-port clash from a concurrent fixture; a detailed native rerun and the final isolated full gauntlet pass without product changes.

Created a blank C# 2D reference project through the Launcher control endpoint under `.temp/task3-csharp-reference`. Live checks verify sibling scripts, movement, explicit UTF-8/NUL strings, ownership, local RPC, physics callbacks, Stop restoration, added-field/default/behavior reload and failing-build recovery. Publishing succeeds, and its relocated standalone package executes the expected script/RPC/ownership/collision markers and exits cleanly. Editor command and replication metadata implementations remain unchanged and were reviewed alongside existing tests; no new generic editor-command operation is introduced. Evidence is under `logs/task3/`. Task3 is complete; project language selection is next (Task4).
