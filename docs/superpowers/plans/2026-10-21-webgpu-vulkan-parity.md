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
The user authorized incremental commits when continuing Task 2. No automatic CI.

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

- [x] Pin native/baked geometry, material and color-space reference cases.
- [x] Reuse portable cooked mesh streams; bind textures per material, preserving
  alpha and sampler semantics. Extract owned draw data on the game thread.
- [x] Handle authored linked materials, scene dependencies and skinning, testing
  multi-primitive models and script-driven movement in both language runtimes.

### Task 3: Lighting, shadows, backgrounds and HDR

**Files:** shared Slang include adapters, WebGPU render passes, common frame
constants/extraction and same-scene capture fixtures. Consume Tasks 1/2.

- [x] Split pure shading math from BDA/resource access; test both targets compile.
- [x] Add shared material lighting variants, light extraction, shadow passes and
  explicit HDR intermediates; preserve native equations and effect settings.
- [x] Compare stable feature fixtures at fixed tick/camera/exposure, including
  2D lighting and camera backgrounds. Verify limit failures before allocation.

### Task 4: Complete effects and custom shader portability

**Files:** native effect/pass inventory, shared Slang, WebGPU render scheduling,
material-graph compiler and shader cook/diagnostic paths. Consume Task 3.

- [x] Enumerate every native pass and material variant as a tracked parity case.
- [ ] Port post effects, procedural/specialized shading and particles through
  shared equations; preserve ordering, history reset and blending semantics.
- [ ] Add custom/material-graph WGSL compilation and actionable unsupported
  feature diagnostics; prove intentional visual regressions fail comparison.

### Task 5: Full 3D gameplay and scenes

**Files:** physics/animation browser closure, Web scene application and services,
native/browser Rust/C# gameplay fixtures. Consume Tasks 2â€“4.

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

## Task 2 qualification checkpoint

Tasks 1 and 2 are implemented and qualified. Saved perspective/orthographic scenes
now load shared native-cooked model streams, linked materials (TOML, authored and
cooked folders, and binary presets), includes/prefab composition and skin palettes.
Owned game-thread draw extraction follows live entity transforms and materials.
Texture color/data spaces, alpha mask/blend/additive modes, vertex colors,
mirrored winding, backfaces, UV scrolling and complete mip chains are covered.

The mip filter is shared with the published native BCn cooker, including its
existing encoded-byte/alpha behavior. Loose native PNG uploads have a different
filter; this is not a claim that those paths or BCn/RGBA compression are identical.
Both source and frame validation retain explicit mesh/texture/skin budgets.

Native fixes made while establishing shared semantics:

- Step/CubicSpline animation sampling now evaluates the intended keys/tangents.
  CPU tests execute the actual native shader functions against the portable pose
  evaluator. Browser scene application clamps stale clip indices like native.
- Authored presets preserve additive and specialized material flags; linked
  materials retain inherited textures unless explicitly overridden/cleared.
- Prefab root overrides preserve the placed scene-node identity on both paths.
- Browser captures survive a declined frame until a GPU submission accepts them;
  a development-only rejection fixture makes this regression deterministic.

Qualification on this Windows host:

- Hardware Chrome 155.0.8059.85 / NVIDIA Blackwell / Playwright 1.62.1: all 38
  browser cases passed, plus Node foundation and native-import/shared-memory/source
  audits. Cases read actual GPU pixels for authored models/materials, minification,
  alpha modes/winding, includes/prefabs, two animated primitives and capture retry.
- Native full gauntlet: 1,512 cases passed; editor and runtime Vulkan smokes each
  reported zero errors, warnings or advisories. Managed SDK: 168 passed.
- Both C# and Rust passed unchanged production Web 2D publishing, relocation and
  iframe checks. Private copies of those packages then passed authored 3D model
  movement using real gameplay scripts and simulation ticks, actual GPU captures,
  diagnostics checks and stop with zero frames in flight. Original verified
  packages were hash-checked unchanged; qualifier browsers/servers were closed.
- Project Python: 54 cases, 48 passed and six opt-in skips. MCP: 26 passed.
  Browser-harness unit tests: seven passed. Clangd and graphify were refreshed.
- Fresh review found stale clip-index handling, which was reproduced and fixed.
  Subsequent mip/shared-loader/capture review found no remaining P1/P2 issues.

The private 3D language qualifier is reproducible with
`python tools/web-runtime/authored-language-checks.py --playwright <module-path>`.
It runs the existing publisher first, stages modified copies under `.temp`, and
labels them private qualification; it does not bypass the released export gate.

At this checkpoint surface shading was unlit. Normal/MR/AO maps and specialized
flags were carried for Tasks 3/4 to implement lighting, shadows, HDR and effects. Full 3D
physics/animation APIs and scripted prefab execution remain Task 5. Production
`webgpu-2d-v1` still rejects 3D; Task 6 must qualify and unlock full publishing.
These fixtures do not establish complete native/browser visual equivalence or
cross-browser/device support. External-host distribution qualification remains
pending the previously deferred authorized destination.

Commits: `af460da` (foundation), `935959e` (pose/native sampling), `3125d93`
(material flags), `e57c39e` (authored Web rendering), `6de11e8` (shared material
loading/capture retry), followed by the language qualifier/checkpoint commit.

Local evidence (untracked): `logs/task2/qualified-web/report.json`,
`logs/task2/qualified-web/browser.json`, `logs/task2/qualified-native/report.json`,
`logs/task2/authored-languages-final/report.json`,
`logs/task2/authored-languages-final/browser.json`, `logs/task2-managed-tests.log`,
`logs/task2-project-tests-final.log`, `logs/task2-mcp-tests.log`, and
`logs/task2-browser-tests.log`. The development site remains hosted at
`http://127.0.0.1:18989/`; refresh after rebuilding to inspect the current fixtures.

## Task 3 implementation checkpoint

The WebGPU frame now supports authored lighting, standard PBR maps, directional
cascades, point/spot shadows, skinned and alpha-tested casters, camera backgrounds,
linear HDR intermediates and the shared native tone-map/sky shader families. The
2D light-map shader shares native equations and samples rasterized tile-art alpha.
Display-space UI runs after scene lighting and tone mapping. Legacy unlit fixtures
retain their previous behavior, and both language packages carry the new shaders.
See [the lifecycle contract](../../architecture/webgpu-lifecycle.md) for scheduling,
ownership, limits and the persistent shadow atlas budget.

All 56 hardware browser cases pass, including asymmetric cutout shadows that
reproduced and caught a vertical shadow sampling error before its correction,
and a visible mesh whose authored shadow-casting flag is disabled.
The full native gauntlet passes 1,521 cases with zero editor/runtime Vulkan
findings. Managed SDK tests pass 168.
Both C# and Rust production 2D publishing/relocation/iframe checks and private 3D
script movement qualifications pass; the verified original packages remain
hash-checked unchanged. The development gallery remains hosted at port 18989.

All 21 same-scene native/browser comparisons pass. The maintained
`tools/web-runtime/lighting-parity-checks.py` captures matching camera/extents at
fixed tick 10 with manual ACES/exposure, compares actual GPU pixels, and independently
checks feature behavior on both renderers. All six shadow comparisons pass,
including asymmetric alpha and disabled-caster checks. Mean RGB error ranges
from 0.067 to 0.230 on the 0–255 scale; every scene's 95th percentile error is 1.
Whole-image sampling retains edges and uses unchanged tolerances.

The comparison exposed native bugs as well as browser bugs: colour attachment
clears overwrote their Vulkan union member, local lights ignored the receive-shadow
flag, the local shadow override culled two-sided cutouts, and tiled spot-light
culling assumed perspective projection for orthographic cameras. These are fixed;
the shared tile-frustum math has 122 geometric regression assertions. Native
fixtures use canonical asset baking and hash-check their authored inputs.

Local evidence: `logs/task3/qualified-web/report.json`,
`logs/task3/qualified-web/browser.json`, `logs/task3/qualified-native/report.json`,
`logs/task3/qualified-parity.json`, and
`logs/task3/qualified-languages/report.json` with its `browser.json`. The final
language run passes both C# and Rust with original production packages unchanged.
Nine comparison-helper tests, seven browser-harness tests, 48 project tests
(six opt-in skips), and 26 MCP tests pass. Clangd and graphify were refreshed.
The comparison closes its browser/server
and native jobs; the requested development gallery remains hosted at port 18989.

The public export profile remains `webgpu-2d-v1`. Automatic exposure, grading,
other post effects, specialized/custom materials and the remaining gameplay work
are still Tasks 4/5; full 3D production publishing remains gated by Task 6.

## Task 4 bounded qualification checkpoint

Task 4 is not complete. This checkpoint qualifies the bounded cases recorded in
[the effect parity ledger](../../architecture/webgpu-effect-parity.md), which
tracks every native pass and material variant with its evidence. On hardware
Chrome/NVIDIA Blackwell: 30 post controls, 19 material cases (9 pairs) and 22
scene-effect cases (10 pairs) pass native vs WebGPU comparisons with unchanged
image limits; 13 deliberate regressions fail their gates. The full Debug native
gauntlet passes 1,549 cases with zero Vulkan findings, and C#/Rust production 2D
publishing and private 3D movement pass.

Qualification fixed two shared shader defects (a precision-fragile noise hash
that diverged across compilers, and a Slang 2026.8 WGSL miscompile of negated
vector sums, now rejected at build time by `tools/shaders/wgsl_checks.py`), a
latent Play/Stop restore crash, and four fixture/oracle defects documented in
the ledger. The language qualifier now refreshes the whole Rust module tree.

Remaining Task 4 work, per the ledger: raw custom passes (behind/over/emissive
2D) are unsupported, gameplay render-to-texture cameras are missing, and the
authored `sky` mesh, UV-scroll/vertex-colour overrides, history reset and
enable/resize transitions, parameter sweeps and effect cross-products have no
qualified case. The export profile remains `webgpu-2d-v1`.
