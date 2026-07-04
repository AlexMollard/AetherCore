# Material System Overhaul — Session Handoff

**Date:** 2026-07-04
**Branch:** `master` (working tree clean, everything committed)
**Status:** All 4 phases IMPLEMENTED. Phases ① & ② runtime-verified by the user; ③ & ④ built + adversarially reviewed but **not yet run on a GPU**.

This document is a self-contained bootstrap for a fresh session. It replaces the need to re-read the whole transcript.

---

## 1. TL;DR

The material path was rebuilt in four phases, each spec'd → planned → implemented → adversarially reviewed → committed:

| Phase | What it delivered | Review | GPU-run |
|---|---|---|---|
| ① Material Core | content-addressed `MaterialRegistry`, dedup, ref-counting, `MaterialHandle`, `PackMaterial`, frames-in-flight slot recycling | clean | ✅ user |
| ② Authoring + instances | das `make_material`/`bind_material`/`material_set_*`/`entity_material_set_*`, per-entity copy-on-write `MaterialInstanceComponent` | 3 fixes | ✅ user |
| ③ Permutations + effects | `PipelineCache` (pipeline dedup), per-entity `EffectParamBuffer` (real effect params, retires the PBR-field hack) | 2 fixes | ⏳ pending |
| ④ Texture asset layer | `TextureHandle` + ref-counted/deduped `TextureRegistry`, GPU ABI frozen | 0 defects | ⏳ pending |

**Build is clean; 40/40 doctest cases pass; shaders compile; the 80-byte `GpuMaterial` ABI is verified untouched.**

Specs: `docs/superpowers/specs/2026-07-0{3,4}-*.md`. Plans: `docs/superpowers/plans/2026-07-0{3,4}-*.md`.

---

## 2. What to do next (pick up here)

Ordered by value. None are blockers — the overhaul is complete and shippable as-is.

1. **User runs ③ & ④ on the GPU** (see checklists in §6). This is the only real gap — the two render/asset-path phases can't be verified in-agent.
2. ~~**Magenta fallback texture (small, flagged).**~~ **DONE (2026-07-04, code-complete; needs GPU verify — see §6 ④).** Chose the **synthesized in-binary** route over a committed asset: a fallback that depends on an asset load succeeding is self-defeating, and the AssetPacker manages a fixed asset set (`assets.pak is up to date, skipping`), so a loose PNG could silently not get packed. New `Texture::CreateSolidColor(rgba, …)` builds a 1×1 magenta texture via the existing `UploadRgbaToGpuImage` helper; new `TextureRegistry::InitializeDefault(TextureResource&&)` overload installs it as the default (bypassing the sink path load, refcount 1, not indexed in `m_hashToEntry` so it's never a dedup target). Wired in `AssetSubsystem::Init` via `AE_EXPECT_OR_THROW`. Only STALE/missing handles resolve to magenta; a material with no texture authored still packs `kNoTexture` and skips the sample. Unit test added (41/41 pass).
3. **RT-quad-with-effect test case (user suggested, parked).** The render-to-texture infra (`RenderTargetService::CreateCameraRenderTarget` / `GetRenderTargetBindlessSlot`) EXISTS but is **dormant — called from nowhere**. `effectParamBufferAddr` is now wired into RT passes (so effects would be correct if used), but a das RT-quad demo would exercise never-run engine code with two real concerns: (a) transient bindless-slot stability across frames (`GetRenderTargetBindlessSlot` re-queries the graph each call), (b) render-graph mutation from the game/script thread. Treat "harden the dormant RT path" as its own task before building the demo on it.
4. **④b streaming** (explicitly out of ④ scope): async texture load + placeholder-while-loading + residency/eviction. The `TextureRegistry` mutex already makes a future async completion race-free at pack time; `CreateTextureAsync`/`ReadFileAsync` exist. This is a separate spec.

---

## 3. Architecture cheat-sheet

Everything lives in `src/engine/material/`. Three parallel registries, all the same shape (content/path-addressed table over an injected sink, opaque `{index,generation}` handle, ref-count, generation guard, deferred slot free):

- **`MaterialRegistry`** (sink = `MaterialBuffer`) — content-addressed by the packed 80-byte `GpuMaterial`. `Acquire/Release/ResolveSlot/InitializeDefault`. Now also **cascades texture refs** (a material slot ref-counts its textures).
- **`TextureRegistry`** (sink = `AssetTextureSink`) — **path**-addressed (resolve-before-hash, so `foo.png`/`foo.texture` dedup). `Acquire/AddRef/Release/ResolveSlot/InitializeDefault/ReleaseAll`.
- **`PipelineCache`** (factory = `AssetManager::CreateGraphicsPipeline`) — content-addressed by `MaterialTemplate` (program+blend+cull+depthWrite). Node-stable `unordered_multimap` so `PipelineComponent` raw pointers stay valid. NOT ref-counted (app-lifetime pipelines).

Per-entity mutable state:
- **`EffectParamBuffer`** — per-entity (NO dedup) BDA SSBO of 32-byte `EffectParams` (tint/speed/scale/intensity); freed via `on_destroy<EffectParamsComponent>`.

Data flow: `MaterialAsset` (authoring, holds 5 `TextureHandle`s + `MaterialTemplate templateDesc`) → `MaterialSystem::AssignMaterial(world, entity, MaterialRegistry&, PipelineCache&, asset)` → acquires material handle (→ `MaterialComponent{handle, gpuSlot}`), resolves pipeline (→ `PipelineComponent`), and (effect-override) leaves the pipeline alone if the entity has an `EffectParamsComponent`. `PackMaterial(asset, TextureRegistry&)` is the single CPU→GPU packer: resolves texture handles → raw bindless heap index, emits the frozen 80-byte `GpuMaterial`.

Ownership: `AssetSubsystem` owns (member-init order matters — sinks before registries): `MaterialBuffer` → `AssetTextureSink` → `TextureRegistry` → `MaterialRegistry` → `PipelineCache` → `EffectParamBuffer` → `MaterialAuthoring`. `AssetManager` gets pointers to them via `Initialize(...)`.

Lifecycle hooks (entt `on_destroy`): `MaterialSystem::ConnectLifecycle` releases material handles (→ cascades texture refs); `EffectSystem::ConnectLifecycle` frees effect param slots. Both `DisconnectLifecycle`'d in `AssetSubsystem::Shutdown` before teardown.

**ABI-frozen (do NOT touch without a plan):** `GpuMaterial.hpp` (80 bytes, offset asserts), `shaders/gltf_mesh.slang`, `shaders/include/GpuMaterial.slangh`. `InstanceData` (104B) and `FrameConstants` (768B) reused reserved padding for `effectParamIndex`/`effectParamBufferAddr` — same size, asserts hold.

---

## 4. Build, test, run

Primary build dir is `build-vs2022-msvc` (others exist but this one has a live cache).

```
cmake --build build-vs2022-msvc --config Debug --target <Engine|App|EngineTests|App_CompileShaders>
./build-vs2022-msvc/tests/Debug/EngineTests.exe        # 40 cases, all material/texture logic
cmake --build build-vs2022-msvc --config Debug --target App_CompileShaders
```

Tests: 9 files under `tests/material/` (doctest). Registry/cascade/instance/pipeline/effect-buffer/texture logic is pure-CPU and fully unit-tested via fake sinks (`FakeSlotSink`, `FakeTextureSink`, `FakePipelineFactory`). GPU behavior (rendering, residency, dedup on real bindless slots) is the runtime hand-off.

Scripts hot-reload from `resources/scripts/` (F5 in-app) — `sandbox.das` is the showcase. The app is GPU-driven and **cannot run in-agent**.

**Build gotcha:** if the Vulkan SDK version bumps, the CMake cache holds stale paths and the build fails with misleading `volk.h`/`vulkan_core.h`-not-found errors. Fix: `cmake -S . -B build-vs2022-msvc -U "Vulkan_*" -U "AETHERCORE_SLANG_ROOT" -U "SLANGC_EXECUTABLE" -U "FIND_PACKAGE_MESSAGE_DETAILS_Vulkan"` then rebuild.

---

## 5. Non-obvious gotchas (learned this session)

- **entt empty tag types:** `World::EmplaceOrReplace<T>` returns `T&`, which entt can't provide for a truly empty type. Give tags a trivial member.
- **Entity id 0:** `Entity::IsValid()`/`World::Destroy` treat id 0 as null, but entt hands id 0 to the FIRST `World::Create()`. Unit tests burn one entity first (`(void)world.Create();`).
- **`Entity` is a bare uint32 = the full packed entt value (index+generation)**, so `registry.valid()` correctly rejects stale/recycled entities (`MaterialAuthoring::Rebind` and `ReleaseModelTextures` rely on this).
- **`AetherError` has no `.what()`** — format it directly with `{}` in log macros.
- **`TextureRegistry` is non-assignable** (mutex + reference member) → teardown uses `ReleaseAll()`, not reassignment.
- **Texture cascade is per material SLOT, not per handle-holder:** fresh material acquire `AddRef`s each texture once + stores the handles; a **dedup-hit does NOT AddRef**; Release-to-zero drops them. (The ④ plan said "AddRef on dedup-hit too" — that would leak, since Release only drops at refcount zero. The header comment at `MaterialRegistry.hpp` SlotEntry.textures documents the correct per-slot rule.)
- **Effect-override rule:** `AssignMaterial` early-returns before emplacing `PipelineComponent` if the entity has an `EffectParamsComponent`, so `set_material` on an effect entity repaints its surface without dropping the effect pipeline. `set_entity_effect` is the sole writer of an effect entity's pipeline.
- **Cull-mode parity:** phase ③ made `doubleSided` actually drive culling (`doubleSided?None:Back`). Pre-③ everything was `None` (two-sided). Fix kept the primitive default material + material-less glTF templates two-sided so single-sided sandbox plane walls stay visible; authored `set_material` materials back-cull per spec.

---

## 6. GPU runtime hand-off checklists

**Phase ③ (permutations/effects):**
- Three plasma orbs (overhead in `sandbox.das`) tint/pulse/scale **independently** — proves the per-entity `EffectParamBuffer` (the multi-entity bug ③ fixes). If they looked identical, it's broken.
- `set_effect_color/speed/scale/intensity` animate live with **no material-slot churn** (no "MaterialBuffer full" warnings).
- Painted cubes / rainbow (vertex-color) cubes / GLTF models render unchanged.
- Single-sided plane **walls visible from both sides** (cull-parity fix).
- `set_material` on a plasma orb recolors it without dropping the plasma pipeline.
- Repeated F5 reloads: no pipeline/param-slot leak. App exit: no `VkShaderEXT`/pipeline validation-layer leak.

**Phase ④ (textures):**
- GLTF models (Fox, Human) render textured, **identical to before** (albedo/normal/ORM).
- **Dedup:** loading the same textured asset on many entities does NOT multiply bindless sampled-image slots; two spellings (`.png`/`.texture`) share one slot.
- **No leak:** destroying/reloading textured scenes repeatedly shows no monotonic growth in used sampled-image slots.
- Material presets (TOML + binary `.material`) still resolve their five texture maps (note: `LoadMaterialPreset` has no in-tree callers, so this is latent).
- Shutdown: no validation errors (deferred bindless-slot frees all retire).
- **Magenta fallback (item 2):** free a texture that a live material still references (e.g. destroy a textured entity whose material another entity shares, or F5-reload a scene mid-frame) → the stale surface samples **1×1 magenta**, not base colour. Untextured materials stay untextured (no magenta on base-colour-only surfaces). Shutdown: the extra default entry retires its bindless slot cleanly (`ReleaseAll`).

**Sandbox demo added this session (missing-texture + 2nd effect pipeline):**
- **Missing-texture cube:** the slowly spinning cube (`g_missing_cube`, front-centre near the effects row) references a texture path that does not exist → renders **flat magenta on every face**. This exercises the fallback *from script* end to end: `TextureRegistry::Acquire` now returns a `TextureHandle::Broken()` on load failure (was an invalid handle), which packs to the magenta default instead of silently untextured. The rest of Zone M (no-texture materials) must stay its normal colour — only a *requested-but-missing* map goes magenta. New das `set_material_texture(world, e, path)`.
- **Molten orb:** the 4th orb in the overhead effects row (`g_molten`, `shaders://molten.spv`) must look like flowing lava — a dark crust broken by white-hot veins — **clearly different** from the 3 plasma orbs, animating via its own `EffectParams` slot. Proves a second runtime effect pipeline coexists with plasma over the same per-entity params path.
- Shutdown after all the above: **no validation errors** (the broken handle touches no refcount; molten's pipeline retires like plasma's).

---

## 7. Commit trail (this session, on `master`)

```
3d9f359 docs(material ph4): fix stale SlotEntry.textures comment
4428524 feat(material ph4): MaterialAsset holds TextureHandles; registry cascade; migrate loaders (ABI frozen)
09856d5 feat(material ph4): TextureHandle + TextureRegistry (path-addressed, ref-counted, TDD)
61085ad feat(sandbox): three independent plasma orbs; wire effectParamBufferAddr into render-target passes
3be96c5 fix(material ph3): address render-path review findings (cull parity + null-BDA guard)
1e9df8b feat(material ph3): plumb effectParamIndex + effectParamBufferAddr; plasma reads EffectParams
e5f9f21 refactor(material ph3): route app pipelines through PipelineCache; EffectDef registry
6597055 feat(material ph3): AssignMaterial resolves pipeline via PipelineCache; AssetSubsystem owns cache+EffectParamBuffer
4770951 feat(material ph3): EffectParamBuffer + EffectParamsComponent + MaterialAsset.templateDesc
2ff5b57 feat(material ph3): MaterialTemplate + PipelineCache (TDD) + EffectParams record
91238f8 feat(material ph3): rename reserved paddings to effect-param carriers (ABI size-frozen)
876839d docs(material): implementation plans for phases 3 and 4
ce375f2 fix(material): address phase-2 review findings
0e01f03 feat(material): phase 2 - das authoring API + per-entity material instances
34668fe docs(material): design specs for phases 2/3/4
0e6474d fix(material): defer material slot recycling by frames-in-flight
b521af1 AssignMaterial acquires before releasing the old handle
578baa4 Material Core system implementation (phase 1, pre-session)
```

Cross-session memory is under `.claude/.../memory/` — see `project_material_system.md`, `reference_build_and_test.md`, `user_alex_workflow.md`.
