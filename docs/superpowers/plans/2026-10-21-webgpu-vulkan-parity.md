# WebGPU and Vulkan parity implementation plan

> **For agentic workers:** Use superpowers:executing-plans to implement this plan task by task. Independent code/review work may use superpowers:dispatching-parallel-agents.

**Goal:** Export the same full 3D game and engine effects to WebGPU with measured
visual and gameplay equivalence to Vulkan.

**Architecture:** Share CPU scene/assets and shader math, with owned portable
frame data and explicit WebGPU bindings. Retain native optimized implementation
and reject unsupported browser features until qualified.

**Tech Stack:** C++23/26, Slang/WGSL/SPIR-V, Emscripten/Emdawnwebgpu, Rust/C#,
existing native tests and hardware Chromium automation.

**Spec:** `docs/superpowers/specs/2026-10-21-webgpu-vulkan-parity-design.md`.

## Global constraints

Preserve existing 2D/native/both-language behavior, never read ECS from GPU code,
retain GPU resources until completion, and keep production feature gates honest.
No git mutations are authorized by the current message. No automatic CI.

## Review focus

- Depth ordering must be independent of draw order; overlays stay on top.
- Nonuniform/mirrored transforms require correct normal transforms and winding.
- Invalid/nonfinite data or exhausted budgets must fail before GPU submission.
- Resize, device loss and stop with frames outstanding must retire resources.
- A build/fixture passing must never unlock unimplemented production features.

### Task 1: Shared geometry and bounded WebGPU mesh foundation

**Files:** `src/engine/mesh/PrimitiveMeshData.hpp/.cpp`,
`src/engine/mesh/PrimitiveMeshes.cpp`, `src/engine/gpu/MeshPass.hpp`,
`src/engine/rendering/RenderFramePacket.hpp`, `src/engine/webgpu/WebGpuDevice.*`,
`resources/web/mesh.slang`, `CMake/WebSources.cmake`, `CMake/WebRuntime.cmake`,
`tests/rendering/WebGpuContractTests.cpp`, `tests/mesh/PrimitiveMeshDataTests.cpp`.

**Interfaces:** `GeneratePrimitiveMesh(PrimitiveMesh)` returns shared CPU
`MeshGen::MeshData`; `RenderFramePacket.render3D` owns mesh instances;
`WebGpuDevice::UploadMesh(slot, vertices, indices)` registers immutable resources.
`PlanMeshPass` validates frame data and produces shader-layout instance records.

- [x] Write failing tests for all five native primitives, invalid IDs, index
  bounds and existing vertex/UV/color/topology conventions.
- [x] Extract existing CPU primitive data without modifying native upload bytes
  or bounding volumes; compile the same generator into the Web closure.
- [x] Add packet/layout/transform validation tests, including nonfinite input,
  singular/mirrored transforms and explicit frame budgets.
- [x] Implement indexed draws, explicit per-frame mesh bindings and depth
  attachments retained until completion. Keep the sprite pass an overlay.
- [x] Compile the Slang mesh adapter to WGSL; make Rust and managed packages
  carry it with explicit link dependencies.
- [x] Add browser fixture covering depth order, perspective, nonuniform scale,
  resize and stop. Verify on a real hardware adapter; run native gauntlet and
  C#/Rust fixture regressions. Keep production 3D export disabled.

### Task 2: Models, materials and authored 3D extraction

**Files:** Web asset cooker/readers, `WebScene.*`, shared scene/mesh/material
code, Web source closure and asset tests. Consume Task 1 mesh upload/packet API.

- [ ] Pin native/baked geometry, material and color-space reference cases.
- [ ] Reuse portable cooked mesh streams; bind textures per material, preserving
  alpha and sampler semantics. Extract owned draw data on the game thread.
- [ ] Handle authored linked materials, scene dependencies and skinning, testing
  multi-primitive models and script-driven movement in both language runtimes.

### Task 3: Lighting, shadows, backgrounds and HDR

**Files:** shared Slang include adapters, WebGPU render passes, common frame
constants/extraction and same-scene capture fixtures. Consume Tasks 1/2.

- [ ] Split pure shading math from BDA/resource access; test both targets compile.
- [ ] Add shared material lighting variants, light extraction, shadow passes and
  explicit HDR intermediates; preserve native equations and effect settings.
- [ ] Compare stable feature fixtures at fixed tick/camera/exposure, including
  2D lighting and camera backgrounds. Verify limit failures before allocation.

### Task 4: Complete effects and custom shader portability

**Files:** native effect/pass inventory, shared Slang, WebGPU render scheduling,
material-graph compiler and shader cook/diagnostic paths. Consume Task 3.

- [ ] Enumerate every native pass and material variant as a tracked parity case.
- [ ] Port post effects, procedural/specialized shading and particles through
  shared equations; preserve ordering, history reset and blending semantics.
- [ ] Add custom/material-graph WGSL compilation and actionable unsupported
  feature diagnostics; prove intentional visual regressions fail comparison.

### Task 5: Full 3D gameplay and scenes

**Files:** physics/animation browser closure, Web scene application and services,
native/browser Rust/C# gameplay fixtures. Consume Tasks 2–4.

- [ ] Build 3D physics/query/joint/animation tests using the native semantics.
- [ ] Implement portable browser execution with explicit budgets and controlled
  simulation; qualify prefab/include/scene transitions, ragdolls and callbacks.
- [ ] Verify both languages against matching native observable outcomes.

### Task 6: Production export parity qualification

**Files:** `tools/project/web_publish.py`, publish plans, asset packing, package
schemas, browser export verification and native/browser visual reports.

- [ ] Add a new profile only for implemented and qualified features; preserve
  existing `webgpu-2d-v1` package behavior.
- [ ] Validate complete shader/asset reachability and adapter requirements,
  refusing unsupported features instead of emitting visually incomplete games.
- [ ] Export both language fixtures; compare actual relocated packages against
  native captures and gameplay evidence; exercise iframe and cleanup behavior.
- [ ] Record feature coverage, numerical/pixel tolerances, performance/memory,
  browser/device environment and remaining unsupported configurations.

## Current checkpoint: mesh foundation, not full parity

Task 1 is implemented and qualified. Task 2 has the portable 100-byte MeshVertex
record and shared native cooked-model reader available in the browser closure;
authored 3D scene/material application and the remaining tasks are not complete.
Production 3D publishing remains rejected rather than omitting rendering features.

Qualification on this Windows host:

- `Run-WebRuntimeChecks.ps1 -SkipHostBuild -BrowserChecks`: passed all 33 cases
  on hardware NVIDIA Blackwell / Chrome 155.0.8059.85 / Playwright 1.62.1, plus
  Node foundation and source/import/shared-memory audits. Mesh cases exercise
  native-cooked glTF geometry, shared primitives, perspective/depth, sprite
  overlay, resize, retirement and invalid-frame rejection before submission.
- Native `Run-DebugGauntlet.ps1 -Fast`: 1,475 cases passed, both editor and
  runtime Vulkan validation smokes had zero findings. An earlier concurrent run
  hit swapchain-out-of-date advisories; the final complete gate passed. A build
  attempted while an owned publish worker held aether-project.exe hit a link
  lock; the final gate ran after that worker finished. No findings were filtered.
- Real C#/Rust desktop fixture failure/repair and compiler-error exercises passed.
  Both production Web 2D export/relocation/iframe checks passed; latest managed
  export was rebuilt after the final internal mesh validation changes.
- Managed SDK: 168 passed. Project Python: 54 cases, 48 passed and six opt-in
  skips; MCP: 26 passed; browser harness unit tests: seven passed.
- Review caught a formatter-split JavaScript operator in EM_ASM; fixed and rebuilt.
  Final review of the pre-submission validation/encoding split found no blockers.

Local evidence (untracked): `logs/web3d/final-runtime-and-browser.json`,
`logs/web3d/browser.json`, `logs/web3d/final-native-gauntlet.json`,
`logs/web3d/desktop-regression.log`, `logs/web3d/export-regression.json`,
`logs/web3d/managed-export-final.json`, `logs/web3d/managed-tests.log`,
`logs/web3d/project-tests.log`, `logs/web3d/mcp-tests.log`, and
`logs/web3d/browser-unit.log`. The fixture screenshot is a depth test, not a
representative game or a native/browser visual parity comparison.
