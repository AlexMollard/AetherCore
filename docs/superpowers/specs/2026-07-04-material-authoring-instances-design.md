# Material Authoring API + Instances — Design Spec (②)

**Date:** 2026-07-04
**Status:** Approved design, pre-implementation
**Scope:** Sub-project ② of the material-system overhaul. Builds a stateful, named, reusable **authoring layer** over the ①-merged `MaterialRegistry`/`MaterialSystem` core. Shader-permutation binding + effects semantic rework (③) and the texture/streaming asset layer (④) remain separate specs that depend on this one. This spec touches **no** GPU/render/shader code and adds **no** new slot allocator or parallel material type — everything reduces to producing a `MaterialAsset` fed to the existing `MaterialSystem::AssignMaterial` (`src/engine/material/MaterialSystem.cpp:31-49`).

---

## 1. Problem

① landed the immutable, content-addressed `MaterialRegistry` and a **minimal preview painting surface** so scenes wouldn't regress: `set_material` / `set_material_color` (`src/app/scripting/modules/WorldModule.cpp:458-477`, bound at `:526-527`, called from `resources/scripts/sandbox.das:122,132,170`), plus per-entity effect assets (`src/app/effects/EffectManager.hpp:27-30`; `src/app/scripting/modules/EffectsModule.cpp:38,50-81`). That preview surface is **whole-asset, stateless, and anonymous**, and that is exactly what ② must replace:

- **Every paint re-specifies the entire surface.** `das_set_material` builds a fresh `MaterialAsset` from three scalars and drops every other field back to its default (`WorldModule.cpp:465-469`). A script cannot "make this cube's existing material a little rougher" — it must restate base color + metallic + roughness together or lose them. There is no partial edit.
- **No named / shareable material.** A material exists only transiently inside one binding call; two entities that should share an *authored* material (not merely dedup by identical content) have no handle to name it by. Authoring intent ("these 40 crates use the `rusted_metal` material") is inexpressible.
- **Effects hand-roll their own per-entity mutable copy.** `EffectMaterialComponent` (`EffectManager.hpp:27-30`) + `MutateEffectMaterial` (`EffectsModule.cpp:46-57`) is a bespoke copy-then-reacquire pattern living in app code. It is the *right* pattern, but it is duplicated, effects-only, and undiscoverable.
- **Idempotent edits still re-pack + re-acquire.** Each effect setter calls `AssignMaterial` → `Acquire` → `PackMaterial` + FNV hash + multimap probe every call (`EffectsModule.cpp:56`; `MaterialRegistry.cpp:37-54`), even when it writes the value the field already holds.

① explicitly deferred "New das authoring API … and ergonomic per-entity parameter overrides / material instances → ②" (`docs/superpowers/specs/2026-07-03-material-core-design.md:46-49`). This is that spec.

## 2. Goals / Non-goals

**Goals (②):**
- A **stateful authoring handle**: build a material once, edit individual fields, reuse it by an opaque script id across many entities. Partial edits, no whole-asset restate.
- **One engine-side generalization of the effects copy-on-write component** so per-entity mutable materials are first-class, not app-local plumbing.
- **Cheap idempotent edits**: a setter that writes an unchanged field does no pack, no hash, no acquire, no slot churn.
- Keep everything on ①'s backbone: `MaterialAsset` → `MaterialSystem::AssignMaterial` → `MaterialRegistry`. No second material type, no second slot allocator, no new render path. Reuse ①'s acquire-before-release and deferred-free machinery verbatim.
- **Zero breakage**: `sandbox.das` and the effects scripts keep compiling and behaving identically.

**Non-goals (deferred):**
- **Material→pipeline-variant/permutation binding and the effects *semantic* rework → ③.** ② consolidates the effects *mechanism* but does **not** touch the emissive=color / metallic=speed / roughness=scale / occlusion=intensity field-overloading hack (`EffectsModule.cpp:62-80`); the fields written stay byte-identical.
- **Texture asset layer / `TextureHandle` / streaming → ④.** ② keeps texture refs as raw bindless heap-index `uint32`s exactly as `MaterialAsset` holds them today (`src/engine/material/MaterialAsset.hpp`).
- **Any change to `GpuMaterial`, the 80-byte ABI, the shaders, `WorldRenderer`, `MaterialComponent`, `MaterialBuffer`, or `MaterialRegistry`'s public API.** ② is a layer *above* the registry; the registry stays sealed.

## 3. Decided model

**Two script-facing concepts, one engine backbone.**

1. **Named / shared authoring material** — build once, get an opaque id, edit fields, *bind* it (by id) to any number of entities. Editing the id re-acquires and re-binds every entity currently bound to it.
2. **Per-entity material instance** — a material owned by exactly one entity, seeded from a starting asset, then mutated in place cheaply (the generalization of today's effects component). This is what animated per-entity parameters use.

Both reduce to the operation ① already ships: mutate a CPU `MaterialAsset`, then call `MaterialSystem::AssignMaterial(world, entity, registry, asset)`. ② adds **only**: (a) storage for the authoring `MaterialAsset` between edits, keyed by id or entity; (b) a **change gate** so an unchanged edit is free; (c) the entity↔id bookkeeping so editing a shared material re-binds its entities. No new GPU state exists anywhere in ②.

**Id representation: opaque `uint32`**, matching the pervasive `Entity{uint32}` / mesh-handle convention (`das_add_mesh(World*, uint32_t, uint32_t)`, `WorldModule.cpp:427`; mesh handles are `uint32` indices into `meshCache`, `SceneContext.hpp:65`). Not the two-word `MaterialHandle`, not a per-call string. An optional string-alias→id map sits *on top* for readable scripts.

## 4. Design

### 4.A `MaterialInstanceComponent` — generalize the effects component (engine)

Promote the app-local `EffectMaterialComponent` (`EffectManager.hpp:27-30`) to a first-class engine component in `src/engine/scene/Components.hpp`, next to `MaterialComponent` (`Components.hpp:34-38`):

```cpp
struct MaterialInstanceComponent
{
    MaterialAsset asset{};
};
```

The single storage location for "an entity's editable material." `Components.hpp` already includes `material/MaterialHandle.hpp` (`Components.hpp:9`); this adds `#include "material/MaterialAsset.hpp"`. No `dirty` flag — change detection is done at the setter (§4.B) where the written field is known. The effect setters already mutate exactly these fields on exactly a `MaterialAsset` and re-acquire (`EffectsModule.cpp:62-80`, `:56`); moving the struct's *definition* into the engine changes storage location and namespace, not a written field or acquire call.

### 4.B Typed field setters with a scalar change-gate (engine — `MaterialSystem`)

Add per-field editors to the `MaterialSystem` namespace (`MaterialSystem.hpp:11-25`), each gating on the field about to be written:

```cpp
bool EnsureInstance(World&, Entity, const MaterialAsset& seed);   // returns true if created
void SetMetallic(World&, Entity, MaterialRegistry&, float value); // + SetBaseColor/Roughness/Emissive/Occlusion
```

Gate (illustrative `SetMetallic`): `if (inst->asset.metallicFactor == v) return;` then write + `AssignMaterial`. **Why scalar compare, not double-`PackMaterial`:** ① already dedups a same-content re-assign into a cheap registry hit with no slot churn (`tests/material/MaterialSystemTests.cpp:11-31`; `MaterialSystem.cpp:35-48`). The gate's only job is to skip the pack+hash+probe on a **no-op**; the cheapest oracle is comparing the one field being written. A pack-before/pack-after `memcmp` would add a full `PackMaterial` to every *real* edit (the animated case, where the value changes every call and the gate never fires) — strictly worse. `EnsureInstance` returns `bool`, **not** `MaterialAsset&`: a reference into entt storage dangles on the next `emplace`/`remove` and bypasses the gate.

### 4.C `MaterialAuthoring` — the named/shared material table (engine service)

New engine service `src/engine/material/MaterialAuthoring.{hpp,cpp}`: `uint32` id → `{ MaterialAsset asset; std::vector<Entity> boundEntities }`, over a `MaterialRegistry&`. **Owns no GPU state, allocates no slots** — `Create`/`Bind`/edits delegate through `MaterialSystem::AssignMaterial(world, entity, m_registry, entry.asset)` (`MaterialSystem.cpp:31-49`); it never calls `AllocateSlot`/`FreeSlot`/`Acquire`/`Release` itself. Sharing is content-honest: two ids with identical content dedup onto one GPU slot (`MaterialRegistry.cpp:44-54`) but stay two authored materials that can diverge. **No `MaterialHandle` cached in `Entry`** — the registry hands out handles per entity via `AssignMaterial` onto each `MaterialComponent` (`MaterialSystem.cpp:48`); caching one would be wrong when bound entities dedup to different slots and would risk caching the **invalid** handle `Acquire` returns on a full sink (`MaterialRegistry.cpp:56-63`). By always going through `AssignMaterial`, ② inherits ①'s full-buffer fallback (invalid handle → default slot, `MaterialRegistry.cpp:95-104`) with no corrupted "shared slot" invariant, because ② asserts none. **No mutex**: all das calls run on the game thread (`g_activeContext` is `thread_local`, set only around a script invocation, `SceneContext.hpp:76`); the render thread never touches this table. The registry's `std::mutex` (`MaterialRegistry.hpp:43`) exists for its off-thread `ResolveSlot` caller — a caller this table does not have.

### 4.D Seeding — no readback from `MaterialComponent`

An instance/authored material is seeded from an **explicit** `MaterialAsset`, never recovered from an entity's current GPU material. `MaterialComponent` holds only `{ MaterialHandle handle; uint32 gpuSlot }` (`Components.hpp:34-38`) — no asset to read back — and the registry is one-way: a `SlotEntry` stores the packed `GpuMaterial` (`MaterialRegistry.hpp:33-40`), and `PackMaterial` is lossy (bools collapse into `flags`). So: `EnsureInstance` keeps an existing instance else seeds from the caller's `seed` (effects pass `effect->material`, `EffectManager.hpp:21`; generic setters seed from the registry default); `MaterialAuthoring::Create` always takes an explicit `seed`. This closes the impossible "read back from `MaterialComponent`" seeding path.

### 4.E Reachability — reuse the existing registry plumbing verbatim

Already solved by ①'s merge; not re-invented. das bindings reach the registry via `ctx.assets->GetMaterialRegistry()` (`WorldModule.cpp:446,469`; `EffectsModule.cpp:39,56`), where `ctx.assets` is an `AssetManager*` (`SceneContext.hpp:36`) holding `MaterialRegistry* m_materialRegistry` (`AssetManager.hpp:99`) plumbed via `Initialize` (`AssetManager.hpp:70`; caller `AssetSubsystem.cpp:57`); the registry is owned by `AssetSubsystem` (`AssetSubsystem.hpp:83-86`). `MaterialAuthoring` is plumbed **identically — not owned by `AssetManager`**: own it in `AssetSubsystem` declared **after** `m_materialRegistry` (`AssetSubsystem.hpp:83-86` is the exact precedent) with a `GetMaterialAuthoring()` getter mirroring `GetMaterialRegistry()` (`AssetSubsystem.hpp:57-60`); add a `MaterialAuthoring&` param to `AssetManager::Initialize` (`AssetManager.hpp:70`), store a `MaterialAuthoring*`, add a getter (`AssetManager.hpp:72-75` template), update the one caller (`AssetSubsystem.cpp:57`). das then calls `ctx.assets->GetMaterialAuthoring()`.

### 4.F das binding surface

New `WorldModule` bindings alongside the preview ones (`WorldModule.cpp:526-527`), reusing `to_glm` (`WorldModule.cpp:113,466`); no `das::float4` helper exists, so base color is `float3` + implicit `a=1`, as `das_set_material` already does (`WorldModule.cpp:466`). Named: `make_material(color,metallic,roughness):uint` → `Create`; `bind_material(world,entity,id)` → `Bind`; `material_set_{color,metallic,roughness,emissive}(id,…)` → edits that re-bind every entity under the id. Per-entity: `entity_material_set_{metallic,roughness,emissive,color}(world,entity,…)` → §4.B setters. **Naming avoids the das overload collision:** the legacy `set_material(world,uint,float3,float,float)` (`WorldModule.cpp:458`, `sandbox.das:132`) keeps its exact signature; the new entry is `bind_material`/`make_material`, not an overloaded `set_material`, sidestepping arity ambiguity. **Shims retained unchanged:** `set_material`/`set_material_color` (`WorldModule.cpp:458-477`) stay as thin one-shot wrappers so `sandbox.das:122,132,170` are byte-identical.

### 4.G Re-bind / re-acquire path — inherits ①'s in-flight safety

Every re-bind and instance edit re-acquires through `AssignMaterial` → `Acquire` then `Release` (`MaterialSystem.cpp:44-45`), which **acquires the new slot before releasing the old** (`MaterialSystem.cpp:35-48`, pinned by `MaterialSystemTests.cpp:33-51`); `Release`'s only free is `m_sink.FreeSlot` (`MaterialRegistry.cpp:92`), deferred `kMaxFramesInFlight+1` frames by the buffer's `DeferredSlotFreeList`, clocked by `AssetSubsystem::AdvanceFrame` → `m_materialBuffer.AdvanceFrame` (`AssetSubsystem.cpp:68-71`). **② adds no direct sink frees**, so it cannot free a slot the render thread is mid-read on — the property is inherited, not re-implemented.

### 4.H Effects migration — mechanism only, byte-identical

Delete `EffectMaterialComponent` + its alias (`EffectManager.hpp:27-30`; `EffectsModule.cpp:15`), replace with `MaterialInstanceComponent`; `EffectData::material` (`EffectManager.hpp:21`) stays the seed. `das_set_entity_effect`'s emplace (`EffectsModule.cpp:38`) becomes `EnsureInstance(*w,e,effect->material)`, then the existing `AssignMaterial` (`:39`) is unchanged. `MutateEffectMaterial` + its four setters (`:46-81`) route through §4.B; **fields written stay exactly** `emissiveFactor`/`metallicFactor`/`roughnessFactor`/`occlusionStrength` (`:62,68,74,80`) — the semantic hack is untouched, deferred to ③. `sandbox.das:444-446` output is byte-identical; the only change is the no-op short-circuit.

### 4.I Teardown / leak safety — explicit `ReleaseAll` before the buffer shuts down

`AssetSubsystem::Shutdown` (`AssetSubsystem.cpp:98-118`) runs before member destruction: disconnects the hook (`:105`), resets the AssetManager (`:108`), then shuts the buffer down (`:112`). A `MaterialAuthoring` destructor that released via the registry → `m_sink.FreeSlot` would free into a **dead buffer**. Fix: add `MaterialAuthoring::ReleaseAll(world)` and call it in `Shutdown` **before line 112**, mirroring the `DisconnectLifecycle` precedent at `:105`; it releases each entry's `boundEntities` through the registry while the buffer is alive. Ordinary entity destruction is already covered by ①'s `on_destroy<MaterialComponent>` hook (`MaterialSystem.cpp:20-23`).

## 5. Migration

~7 files, no render/shader files touched: (1) `Components.hpp` — add component + include; (2) `MaterialSystem.{hpp,cpp}` — `EnsureInstance` + gated setters; (3) `MaterialAuthoring.{hpp,cpp}` (new); (4) `AssetSubsystem.{hpp,cpp}` — own after registry (`:86`), getter, `ReleaseAll` before `:112`; (5) `AssetManager.{hpp,cpp}` — `Initialize` param + getter, caller `AssetSubsystem.cpp:57`; (6) `WorldModule.cpp` — new bindings, keep shims; (7) `EffectManager.hpp` + `EffectsModule.cpp` — swap component + route setters. Implementation ends at clean compile + passing `EngineTests` + a runtime hand-off checklist.

## 6. Risks

- **Overload ambiguity** — avoided by naming (`make_material`/`bind_material`, not overloaded `set_material`), §4.F.
- **Invalid handle on full buffer** — never cached (§4.C); entity resolves to default slot (`MaterialRegistry.cpp:95-104`); leak-count tests account for entries that never acquired (`Release` guarded no-op, `MaterialRegistry.cpp:78,82`).
- **Impossible seed** — closed by §4.D (explicit seed only).
- **Free into dead buffer** — closed by `ReleaseAll` before `Shutdown()`'s `:112` (§4.I).
- **In-flight free** — closed by §4.G (registry-routed deferred free + acquire-before-release).
- **Reference invalidation** — `EnsureInstance` returns `bool`, not `MaterialAsset&` (§4.B).
- **Effects drift** — mechanism-only; semantic hack untouched (§4.H); `MaterialSystemTests.cpp:11-31` pins the no-churn behavior.

## 7. Acceptance / verification

- Clean Debug build on `build-vs2022-msvc`; `EngineTests` passes (`cmake --build build-vs2022-msvc --config Debug --target EngineTests`, then `./build-vs2022-msvc/tests/Debug/EngineTests.exe`).
- **New pure-CPU doctest suites over `FakeSlotSink`** (`tests/material/FakeSlotSink.hpp`, used by `MaterialSystemTests.cpp:12`): `MaterialInstanceTests.cpp` (no-op `SetMetallic` leaves alloc/free/write counts unchanged; changed value re-acquires like `MaterialSystemTests.cpp:33-51`; `EnsureInstance` seeds once, idempotent) and `MaterialAuthoringTests.cpp` (N binds of one unedited material → `allocCount==1`; edit re-binds all bound entities together; two identical seeds dedup then diverge on edit; `ReleaseAll` frees exactly the live-slot count, entries with failed acquires contribute zero frees).
- **Runtime hand-off checklist:** `sandbox.das` unchanged (painted cubes/spheres, plasma `:444-446` identical); `make_material`+`bind_material` share one slot until edited, edit updates all bound entities; per-frame `entity_material_set_*` runs with no unbounded slot growth; repeated scene reload → no slot growth (no leak across `ReleaseAll`).