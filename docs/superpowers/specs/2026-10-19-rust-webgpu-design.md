# Rust gameplay and WebGPU browser exports

Date: 2026-10-19 (Australia/Sydney)
Status: proposed design and implementation scope; no product implementation authorized by this document alone.

## Intent and agreed scope

Pog Champ wants Rust alongside C# and WebGPU browser exports, with agent-driven development as the primary workflow. Some games must be developable, verified and packaged without a human operating the editor.

The user selected Rust browser exports first, preserving desktop C#, with C# browser exports following as a separate milestone. This is not a C# removal or engine rewrite project. WebGPU is the selected browser graphics API.

Three representative outcomes define success:

1. An agent creates a Rust 2D project, edits gameplay, builds it, runs scripted input/assertions, and produces a playable browser ZIP without clicking editor panels.
2. An existing C# project opens, builds, serializes, runs and publishes on desktop with unchanged script identity and field values.
3. A project requesting an unsupported browser feature fails preflight with a feature-specific diagnostic and a nonzero exit code; it never silently drops gameplay or publishes a misleading success.

## Engineering decisions proposed by the planner

- One gameplay language per project initially: csharp or rust. Missing language metadata means csharp for backward compatibility. Explicit unknown values are errors. Rust is an explicit creation option; existing defaults stay compatible until measurements justify changing them.
- Keep C++ as the engine and EnTT as the scene world. Rust is a gameplay module and SDK, not a second ECS or replacement renderer.
- Reuse the current script lifecycle and scene-property model through a narrowly scoped language-neutral host contract. Keep existing C# exports working through an adapter.
- Rust browser games statically link gameplay with the runtime through the Emscripten-compatible target. Desktop Rust uses a project gameplay shared library for editor iteration and packaged desktop runs. Ship a tested library beside the runtime, resolving relative to the package rather than the working directory.
- The first Rust/browser profile is single-player 2D: scene/prefab operations, transforms, input, sprites/animation, Box2D, basic authored UI and audio, scene changes, and persistent saves. Tilemaps are included in the completed 2D profile, after the first playable sprite milestone.
- Start browser execution on one thread. Preserve extract/consume ownership while scheduling phases sequentially. Do not erase native parallel rendering or physics paths.
- Browser rendering uses explicit WebGPU buffers and bind groups, bounded texture batches, and WGSL. No assumption that Vulkan buffer addresses, descriptor heaps, shader objects or SPIR-V contracts translate directly.
- Prefer Slang-to-WGSL for shared compatible shader logic, subject to a real compiler/validation spike. Keep small backend-specific binding declarations. If the spike fails, explicitly record the smallest necessary WGSL shader subset; do not make all renderer code depend on a speculative translator.
- Rust development initially uses rebuild/relaunch from a named scene and an explicit launch configuration. Preserve the editor's authored scene snapshot, script properties and remapped entity references through existing serialization; a headless run uses its saved scene. The new run starts at simulation tick zero. Load persistent gameplay saves only when the launch configuration requests them. Do not automatically restore stacks, pending callbacks, coroutines, physics internals or arbitrary live heap state. C# retains its current reload workflow. Later Rust hot reload is conditional on measured iteration cost.
- C# browser is a required follow-on workstream with its own native/Wasm interop gate; desktop hostfxr and collectible assembly semantics are not assumed portable.

## Current boundaries and consequences

The graph was refreshed after the latest pull. These source files, not old README feature lists, establish the starting state:

| Existing boundary | Consequence |
| --- | --- |
| src/engine/scripting/ManagedInterop.hpp and managed/AetherCore.Interop/Abi.cs | A script function table already exists, but contains managed policy and editor functions. Adapt it; do not rename it into a universal ABI wholesale. |
| src/app/scripting/CSharpScriptingSubsystem.*, src/app/systems/ScriptComponentSystem.*, src/app/net/CSharpScriptFieldBridge.* | Script execution, property reads and networking have explicit C# coupling. Desktop C# regression checks are required during extraction. |
| src/app/scene/serde/ScriptSerde.cpp and src/engine/scene/Components.hpp | Script type names, property maps and scene/prefab entity references already persist. Preserve those semantics and keep missing scripts recoverable. |
| src/app/project/ProjectCommon.*, src/editor/editor/EditorProjectContext.hpp | Project generation currently creates C# projects. Put language selection in project metadata and route all creation surfaces through the same implementation. |
| src/engine/gpu/GpuTypes.hpp and GpuDevice.hpp | Many opaque types and methods still express Vulkan concepts, including addresses, queues and a VulkanContext accessor. Header opacity does not establish a portable backend. |
| src/engine/rendering/RenderFramePacket.hpp | Reuse extracted scene data and its lifetime contract for browser rendering. |
| src/shaders/*.slang and CMake/SlangShaders.cmake | Current output is SPIR-V with Vulkan layout assumptions. Build a separate validated WGSL output path. |
| CMake/Dependencies.cmake, src/engine/CMakeLists.txt, src/app/CMakeLists.txt | Native GPU, .NET and network dependencies are currently broadly linked. Select source/dependency sets before a browser configure can succeed. |
| src/app/RuntimeProjectSettings.*, src/editor/editor/publish/* | Existing publishing is host-oriented. Add an explicit target and runtime artifact provenance rather than copying the editor-adjacent executable for Web. |
| tools/mcp/README.md, scripts/Run-DebugGauntlet.ps1 | Extend existing control and report conventions; browser automation needs a browser transport, not the editor's ENet endpoint. |

## Gameplay contract

Define a versioned C ABI for Rust module registration and engine calls. Use fixed-width scalar types, explicit buffer lengths, ABI size/version checks and module-owned opaque instance handles. Carry strings as UTF-8 buffers with stated ownership. Never pass std::string, Rust String/Vec, references, trait objects, or exceptions across the boundary. Native pointers and function pointers are process-local; Wasm builds compile their own target-correct layout and must not serialize pointer-bearing ABI records.

Existing entity IDs are not automatically safe long-lived Rust references. The safe SDK uses value handles validated against world identity/liveness at each operation, honors EnTT generation semantics, and returns an error for stale or wrong-world handles. It does not hand Rust mutable references into the ECS across callbacks or structural changes.

Share script type enumeration, property metadata/defaults, attach/update/detach and physics/ownership events through the host contract. Use stable type and property names for persistence; never persist property ordinals or function pointers. Describe supported capabilities per backend/profile. Existing C# networking stays supported; Rust networking is explicitly unavailable until its later parity task.

Use macro-generated Rust registration/property tables, derived from one SDK declaration per script. Native component reflection remains its existing single source of truth; Rust does not introduce another hand-maintained component catalogue. A checked API manifest drives thin FFI declarations and verifies C#/Rust shared API parity, while language-specific ergonomic wrappers remain separate.

Report ABI mismatches, invalid handles, missing scripts, build errors and recoverable gameplay errors through structured diagnostics. Native Rust panic boundaries must use a documented strategy: catch unwind-enabled panics inside each exported callback and disable the failed instance; abort/trap outcomes fail the run and are detected by the harness. Never claim arbitrary native faults or panic-abort can be caught safely.

## Agent contract

Expose create, inspect capabilities, build, run, test, publish and poll-status operations without modal UI. CLI and MCP call the same project/build/publish services. New operations return a versioned JSON report containing status, phase, target, language, diagnostic code, source location where available, artifact paths, toolchain identities and timing. Operations are bounded, cancellable, and return nonzero on failure. A required unexecuted GPU/browser check is skipped or blocked, never passed.

Retain engine/editor tooling in native processes. Browser test builds expose a versioned test bridge through browser JavaScript, wrapping engine-owned test commands and state snapshots. It supports readiness, fixed simulation steps, input injection, state assertions, logs and capture. The harness drives it with Playwright. It is absent from normal release exports. No desktop subprocess commands, filesystem access, ENet listener or arbitrary code evaluator is exported into the browser bridge.

Fixed-step tests assert game state; rendering tests use tolerant image assertions and WebGPU validation. Neither claims bit-identical physics or pixels across platforms. Save tests use the same browser origin and a new page session to verify persistence. Audio start, focus, pointer lock and fullscreen use actual browser gestures where required; no automated test instructs users to disable security settings.

## Browser profile and publishing

Provide an explicit webgpu-2d-v1 capability profile. Supported feature requirements come from project settings, compiled module metadata, reachable scene/prefab content and asset types. Preserve dynamic scene loads by using existing dependency discovery where possible and an explicit additional-assets list for paths the packer cannot discover. Preflight reports unsupported or undeclared requirements and runtime capability errors cover genuinely dynamic use.

Exclude desktop-only systems from browser targets: hostfxr, Vulkan/VMA/Aftermath, process launching, native dialogs, port mapping, native ENet networking and unused editor tooling. Compile Jolt out of the 2D browser profile; include Box2D. Compiler and standard-library support for current C++23/26 usage is a measured gate, not an assumed Emscripten capability.

Build asset/shader tools for the host separately from target runtime code. A browser configure must not try to execute Wasm AssetPacker or control-client binaries. Produce target-specific shaders, GPU texture formats and package manifests. Reuse portable scene/PAK formats only after validating layout and supported compression. Initially preload the reachable package through the VFS; implement streaming only if measured size/startup results require it.

Web export produces index.html, loader JavaScript, runtime Wasm, target assets, manifest and ZIP. Include a start gesture, loading/progress/error states, canvas resize/focus, gamepad/keyboard/pointer input, visibility handling, Web Audio integration and browser-backed save storage. Publish into a temporary staging directory; atomically expose a complete result only after verification. Paths and manifests are relocatable and contain no developer-machine prerequisites.

Pin an Emscripten/Rust/Emdawnwebgpu/Slang combination only after the interop spike succeeds; record exact versions and relevant flags in the artifact manifest. Require HTTPS or localhost. Feature-detect WebGPU and requested limits at startup and show a useful unsupported-device message. Initial automated qualification is real-GPU Chromium on Windows plus a recorded Firefox/macOS/Safari qualification matrix before claiming broad browser support. Software-adapter runs are separately labeled. Test iframe embedding with the necessary permissions policy. Do not claim itch.io qualification until an actual uploaded draft/test embed has been exercised; uploading requires user authorization at execution time.

## Milestones and exit criteria

| Milestone | Required evidence |
| --- | --- |
| M0: baseline and feasibility | Desktop C# baseline; native and Wasm C++/Rust calls, strings and callbacks; real WebGPU sprite; pinned toolchain proposal and measured output. |
| M1: shared host contract | Existing C# lifecycle, serialization, editor tooling and networking remain valid; backend selection rejects invalid configuration. |
| M2: desktop Rust | Agent creates/builds/runs/packages Rust reference game; inspector/MCP properties and entity refs round-trip; failures produce useful diagnostics. |
| M3: browser runtime | Engine-owned scene packet renders a sprite under WebGPU; input reaches Rust gameplay; no native-only dependencies are linked. |
| M4: Rust browser export | Reference game includes sprites, tilemap, collision, UI, audio, scene changes and persistent saves; publish ZIP passes local iframe and browser tests. |
| M5: agent hardening | A fresh project completes scripted create-edit-build-test-publish without editor interaction; reports include unsupported-feature and failure cases; qualified host embed evidence recorded. |
| M6: C# browser | .NET Wasm interop gate passes; shared reference scenario runs with C#; browser capability differences are explicit; desktop behavior retained. |
| M7: targeted expansion | Basic Rust 3D/PBR/animation WebGPU profile and Rust net parity are independent follow-ons, each with explicit feature and transport gates. |

First browser release means M0-M5. Desktop Rust does not claim full C# feature parity at M2. M6 is in the roadmap, not implicitly part of first-release acceptance.

## Constraints and non-goals

- No git mutation or remote operation unless Pog Champ asks in that message. Planning adds only these documents; no commits are part of this request.
- CI remains workflow_dispatch-only. No git hooks, scheduled runs or paid hosted browser testing are introduced. Verification is local/manual-invoked automation.
- Keep Windows/Linux desktop and VS/Rider/CLion + CMake working. Native backend remains Vulkan. Engine lifecycle changes require dependency analysis for Full and UiShell.
- Preserve render-thread ECS isolation, descriptor-set conventions on Vulkan, GPU/resource lifetime rules and runtime-safe interop boundaries.
- Do not modify generated build output or third-party sources. Runtime logs/reports go under logs/ or other ignored output. Regenerate clangd after source/CMake changes during implementation.
- No mixed C#/Rust project scripts, C# deletion, engine rewrite, initial WebGL fallback, arbitrary Rust hot reload, browser multiplayer or full desktop-renderer parity in the first release.
- No speed, payload, browser coverage or agent productivity claims without measurements. Track edit-to-ready time, compiler correction cycles, cold/warm builds, compressed bytes, time to first interactive frame, memory and p95 frame times.

## Primary references checked during planning

- Rust Emscripten target and ABI compatibility: https://doc.rust-lang.org/rustc/platform-support/wasm32-unknown-emscripten.html
- Rust FFI layouts: https://doc.rust-lang.org/stable/reference/type-layout.html
- Emscripten WebGPU / Emdawnwebgpu: https://emscripten.org/docs/porting/multimedia_and_graphics/WebGPU-support.html
- Emscripten browser loop and storage: https://emscripten.org/docs/porting/emscripten-runtime-environment.html
- Slang WGSL target constraints: https://docs.shader-slang.org/en/stable/external/slang/docs/user-guide/a2-03-wgsl-target-specific.html
- .NET browser native linking: https://learn.microsoft.com/en-us/aspnet/core/blazor/webassembly-native-dependencies?view=aspnetcore-10.0
- itch.io package requirements: https://itch.io/docs/creators/html5

These establish feasibility directions, not proof that AetherCore's combined toolchain currently works. M0 supplies that evidence.

## Design review

A fresh builder review identified an ambiguity in Rust restart-state restoration. The planner resolved it with the explicit named-scene/tick-zero contract above. This is a planner decision for review, not a user requirement or a promise of full simulation hot reload.
