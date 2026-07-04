# Material Core — Design Spec (①)

**Date:** 2026-07-03
**Status:** Approved design, pre-implementation
**Scope:** Sub-project ① of the material-system overhaul. Foundation only — the
data model + registry. Authoring API/instances (②), shader-permutation binding
(③), and texture/streaming (④) are separate specs that depend on this one.

---

## 1. Problem

The current material path is correct enough to ship but structurally jerry-rigged,
to the point that runtime behavior isn't predictable from the code:

- **Two hand-synced structs.** `Material` (CPU authoring, `src/engine/material/Material.hpp`)
  and `GpuMaterial` (80-byte GPU record, `src/engine/material/GpuMaterial.hpp`) are
  copied field-by-field in `AssetManager::RegisterMaterial` — twice, once per branch
  (`src/engine/assets/AssetManager.cpp:315-354`). Byte-offset `static_assert`s are the
  only guard against drift.
- **GPU slot smuggled into the authoring struct.** `Material::materialSlot`
  (`Material.hpp:39`) fuses "what the surface looks like" with "where it lives in the
  GPU buffer," and overloads `kNoTexture` as the null-slot sentinel.
- **`RegisterMaterial(Material&)` mutates its argument** and doubles as create-and-update
  via the "slot already valid" branch. No explicit update, **no dedup, no ref-counting** —
  N distinctly-colored entities = N slots, silently exhausting the 4096-slot buffer (it
  warns and renders default: `AssetManager.cpp:335`).
- **Unpredictable default behavior.** Every `add_mesh` primitive shares one lazily-registered
  gray singleton (`src/app/scripting/modules/WorldModule.cpp:401-415`). That *should* make
  every cube render gray (the shader drops vertex color when a material is present,
  `shaders/gltf_mesh.slang:66`), yet scenes treat cubes as rainbow casters. The behavior is
  emergent, not designed.

## 2. Goals / Non-goals

**Goals (①):**
- One source of truth for material data: a single CPU→GPU packing function.
- Handle-based materials with **content-addressed dedup** and **ref-counting**.
- Remove the GPU slot from the authoring struct.
- Predictable, explicit default-material and vertex-color behavior.
- Preserve the bleeding-edge Vulkan path: material data via **buffer device address**,
  texture refs as **descriptor-heap indices** (VK_EXT_descriptor_heap, Vulkan 1.4). No
  per-material descriptor sets, ever.

**Non-goals (deferred):**
- **New das authoring API** (`create_material` / `set_material` / `set_base_color`) and
  ergonomic per-entity parameter overrides / material instances → ②. ① is engine-side
  only: it keeps the *existing* internal material paths (add_mesh default, GLTF load,
  effects) working through the registry, with no new scripting surface.
- Material→pipeline-variant/permutation system, effects rework → ③.
- Texture asset layer / `TextureHandle` / streaming → ④. ① keeps texture refs as raw
  heap-index `uint`s, exactly as today.

## 3. Decided model

**Immutable + content-addressed.** A material, once created, is not mutated in place.
`Acquire` content-hashes the packed record; identical content shares one GPU slot and
bumps a refcount (paint 500 red cubes → 1 slot). Per-entity variation is an instance
concern (②), not in-place mutation. This is the Unreal-style asset model and gives free
dedup — important now that we are about to let scenes paint many entities.

## 4. Design

### 4.A Data model — three distinct types, one packing path

- **`MaterialAsset`** (new; replaces `Material`) — pure authoring data: PBR factors,
  texture refs (heap-index `uint`s for now), flags. **No `materialSlot`.**
- **`GpuMaterial`** — unchanged 80-byte packed record; keep the offset `static_assert`s.
- **`MaterialHandle`** — opaque `{ uint32 index; uint32 generation }`. The only thing
  callers store/pass. Not a slot, not a pointer.

Single source of truth:

```cpp
GpuMaterial PackMaterial(const MaterialAsset& asset);
```

The two field-by-field copies in `RegisterMaterial` collapse into this one function.

### 4.B `MaterialRegistry` — the core of ①

New type owning the existing `MaterialBuffer` (kept as-is: mapped SSBO, free-list,
BDA — `src/engine/material/MaterialBuffer.{hpp,cpp}`). The registry adds what's missing:

- `MaterialHandle Acquire(const MaterialAsset&)` — pack → hash the 80-byte record →
  if present, bump refcount, return existing handle; else allocate a slot, `Write`,
  store `{packedBytes, hash, refcount=1, generation}`, return a fresh handle.
- `void Release(MaterialHandle)` — decrement refcount; at zero, free the slot and
  **bump that slot's generation** (stale handles become detectably invalid).
- `uint32 ResolveSlot(MaterialHandle) const` — validate generation; return the GPU
  slot, or the default slot if stale/invalid.
- **Hash-collision safety:** on a hash hit, compare the stored packed bytes before
  treating as a dedup match (no false sharing).
- **One default material**, registered at init; handed to anything without an explicit
  material. Replaces the lazy WorldModule singleton.
- Thread-safety at the registry (MaterialBuffer already locks its free-list).

Registry state is pure CPU logic over a slot allocator: `hash → slotEntry`, plus a
per-slot `{refcount, generation, packedBytes}` table sized to `MaterialBuffer::kMaxMaterials`
(4096).

### 4.C ECS + render integration

- `MaterialComponent` holds a **`MaterialHandle`**, not a `Material`
  (`src/engine/scene/Components.hpp:32`).
- `WorldRenderer::Flush` calls `registry.ResolveSlot(handle)` for `materialIndex`
  (replaces `src/engine/rendering/WorldRenderer.cpp:47-50`).
- **Lifecycle (leak-proof):** the ECS is `entt` (`src/engine/scene/World.hpp:16`).
  - Assignment (internal paths only in ①: add_mesh default, GLTF load, effects):
    `Release` the old handle if present, then `Acquire` + store the new one.
  - Register `registry.on_destroy<MaterialComponent>()` to `Release` the handle when a
    component is removed or the registry is cleared. This covers entity destruction and
    scene teardown without manual bookkeeping.

### 4.D Vertex-color vs. default material (resolves the gray/rainbow ambiguity)

Replace the shader's implicit `hasMaterial ? white : vertexColor` special-case
(`gltf_mesh.slang:66`) with an explicit **`kModulateVertexColor`** bit in
`GpuMaterial.flags`:

- The **default primitive material** sets the bit → rainbow cubes stay `vertexColor ×
  neutralBase`.
- A **painted** material (explicit base color) clears it → reads as a solid color.

Behavior becomes designed and predictable. Cost: one flag bit + a few lines in
`gltf_mesh.slang`.

### 4.E Migration (incremental, compile-checked)

Every current `RegisterMaterial` caller moves to `registry.Acquire`:
- GLTF import + binary/preset load (`src/engine/assets/AssetManager.cpp`).
- das default material (`src/app/scripting/modules/WorldModule.cpp:401-415`).
- Effects/plasma (`src/app/scripting/modules/EffectsModule.cpp`) — wrapped minimally
  now (its material becomes a registry handle), fully reworked in ③.
- `Material` → `MaterialAsset` rename; `materialSlot` deleted.

~6-8 files, one coherent change. Because the app is GPU-driven and cannot be run in this
environment, implementation ends at a clean compile + a hand-off checklist for runtime
verification.

### 4.F Testing

There is **no project-owned test harness** today (all test files under
`build-ninja-clang/_deps/` are third-party). The registry's logic (dedup, refcount,
generation invalidation, default fallback, hash-collision compare) is pure CPU and is
the highest-value thing to test. **Open decision for the implementation plan:**
1. (Recommended, AAA) add a minimal test target (e.g. a `MaterialRegistry` test exe/CTest
   entry) exercising the registry with a mock/no-op slot backend, or
2. gate registry invariants behind `AE_ASSERT` self-checks + a one-shot debug validation
   at startup.

Shader/vertex-color behavior requires the user's GPU run regardless.

## 5. Risks

- **Refcount leaks** if an assignment path bypasses the on_destroy hook. Mitigation:
  route all assignment through one helper; the entt observer is the backstop.
- **MaterialBuffer as a shared slot backend** — the registry must be the sole allocator
  once introduced, or dedup/refcount accounting drifts. Migration removes all direct
  `AllocateSlot`/`RegisterMaterial` callers.
- **Effects coupling** — EffectsModule mutates material fields today; ① only needs it to
  hold a handle. Keep its behavior byte-identical; defer the real rework to ③.

## 6. Acceptance / verification

- Builds clean on `build-vs2022-msvc` Debug (+ `App_CompileShaders`).
- Registry unit checks pass (per §4.F decision).
- Hand-off runtime checklist for the user:
  - Existing scenes render unchanged (rainbow cubes still rainbow, GLTF models correct).
  - Painting an entity a solid color works and dedups (many same-colored entities do not
    exhaust the material buffer — watch for the "MaterialBuffer is full" warning).
  - Destroying/reloading scenes does not leak slots over time.
