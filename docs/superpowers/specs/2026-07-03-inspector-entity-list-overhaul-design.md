# Inspector & Entity List Overhaul — Design

- **Date:** 2026-07-03
- **Status:** Approved (design), pending spec review
- **Spec:** 1 of 3 (this) → Spec 2: Viewport Picking & Selection Highlight → Spec 3: Scene Serialization
- **Owner:** Alex Mollard

## 1. Motivation

The debug panel's **Scene** window (entity list) and **Inspector** window are placeholders:

- **Scene window** — a flat list of entities collected by unioning ~10 component views, deduped, sorted by id, labeled `#{id} Transform, Mesh, …`, **hard-capped at 80 rows** (`InspectorPanel::kMaxSceneRows`), single-select. An entity holding a component outside the tracked set is invisible. No names, no hierarchy, no search, no icons.
- **Inspector window** — a read-only text dump of the selection (id, component summary, position, animation, rigidbody type, physics pos/scale). No editing, no grouping, no add/remove.

Entities themselves are bare: identified only by numeric id + component presence. There is no name, and parent/child structure exists only implicitly in two ad-hoc components (`SpawnedEntitiesComponent` on the logical entity, `ParentEntityComponent` on spawned mesh entities) created by `load_model`.

This spec turns the two panels into a real **outliner + live editor** and lays the ECS foundation (identity, hierarchy, shared selection) that Specs 2 and 3 build on.

## 2. Goals

1. Give entities first-class **identity** (`NameComponent`, auto-named) and **hierarchy** (`HierarchyComponent`, one canonical parent+children representation).
2. Replace the flat Scene list with a **hierarchical outliner**: tree, search/filter, type badges, create/rename/delete, drag-drop reparent, multi-select, no row cap.
3. Replace the read-only Inspector with **editable collapsible per-component sections**, add/remove component, and a physics-aware transform editor.
4. Introduce a **shared `SceneSelection` service** so outliner, inspector, and (later) the viewport agree on the selection.
5. Do it as a **full migration with no backwards-compatibility layer** — `HierarchyComponent` fully replaces `SpawnedEntitiesComponent` and `ParentEntityComponent`, which are deleted.

## 3. Non-goals / explicit scope boundaries

- **Viewport picking & selection highlight** — deferred to **Spec 2**. `SceneSelection` is designed so Spec 2 only has to *write* into it.
- **Disk serialization / write-back** — deferred to **Spec 3**, including the stable asset-reference system needed to serialize components that currently hold raw pointers (`MeshComponent.mesh`, `PipelineComponent.pipeline`, `SkinnedMeshComponent.animDb`, `Material` texture slots).
- **Hierarchical transform math** — `HierarchyComponent` drives display / selection / reparent only. The *existing* "propagate parent transform to spawned children" behavior is preserved verbatim; local-vs-world transform inheritance is **not** introduced here.
- **Simultaneous multi-entity editing** in the inspector. Multi-select drives bulk delete/reparent in the outliner; the inspector edits the **primary** selection only.
- **Asset swapping** (change an entity's mesh/pipeline/texture) — needs an asset picker; later.
- **Physics body recreation** (motion-type / shape changes) — needs Jolt body rebuild; shown read-only.

## 4. ECS Foundation

### 4.1 `NameComponent`

```cpp
// scene/Components.hpp
struct NameComponent
{
    std::string name;
};
```

- **Auto-naming at creation** (so the outliner never shows a bare id):
  - `load_model` → model file stem (e.g. `fox`).
  - `create_mesh` / `add_mesh` → primitive kind (`"Cube"`, `"Sphere"`, `"Plane"`, …).
  - `entity_create` → `"Entity"`.
- **`.das` bindings:** `set_name(world, id, name)`, `get_name(world, id) : string`.
- **Editable** from the outliner (F2 / double-click inline edit) and the inspector header.
- **Fallback:** an entity without a `NameComponent` still renders in the outliner using a derived label, but the auto-naming above means the common paths always have one.

### 4.2 `HierarchyComponent` + `SetParent`

```cpp
// scene/Components.hpp
struct HierarchyComponent
{
    Entity parent{};              // {0} == root
    std::vector<Entity> children; // ordered
};
```

One component holds **both** sides; the invariant is maintained exclusively through a helper so parent and children never disagree:

```cpp
// scene/EcsHelpers.hpp (or scene/Hierarchy.hpp)
namespace aether::ecs
{
    // Re-parents `child` under `parent` ({0} parent == detach to root).
    // - removes child from its previous parent's children
    // - appends to the new parent's children
    // - sets child.parent
    // - no-op (returns false) if it would create a cycle (parent is a
    //   descendant of child) or child == parent
    bool SetParent(World& world, Entity child, Entity parent);

    // Detaches `entity` and recursively destroys it and its subtree.
    void DestroyHierarchy(World& world, Entity entity);

    // Visits roots (entities with no HierarchyComponent, or parent == {0}).
    // Used by the outliner.
}
```

- `SetParent` ensures a `HierarchyComponent` exists on both entities (emplace-on-demand).
- **Cycle guard:** reparenting an entity under one of its own descendants is rejected.
- **Delete semantics:** deleting a node deletes its subtree (`DestroyHierarchy`); the outliner delete + Delete-key use this.

### 4.3 Full migration (delete the old components)

`SpawnedEntitiesComponent` and `ParentEntityComponent` are **removed** from `scene/Components.hpp`. Every consumer moves to `HierarchyComponent`:

| File | Current use | Replacement |
|------|-------------|-------------|
| `engine/assets/AssetManager.cpp` (~805) | `Emplace<ParentEntityComponent>` on each spawned mesh entity | `ecs::SetParent(world, meshEntity, parentEntity)` |
| `engine/animation/AnimationCompiler.cpp` (~26) | reads `SpawnedEntitiesComponent` to find child SMCs | iterate `HierarchyComponent.children` |
| `app/scripting/modules/AnimationModule.cpp` (~32, 42) | `ForEachSpawnedSmc`, first spawned SMC via `entityIds` | iterate `HierarchyComponent.children` |
| `app/scripting/modules/WorldModule.cpp` | `load_model` populates `SpawnedEntitiesComponent`; `set_euler`/`set_transform` read it to propagate | `load_model` calls `SetParent`; propagation walks `HierarchyComponent.children` |
| `app/debug/InspectorPanel.cpp` | collection + display | rewritten (Sections 5–6) |

No compatibility shim, no aliasing — the old types cease to exist. `children` is `std::vector<Entity>` (was `std::vector<uint32_t>`); call sites adapt.

> **Preserved behavior:** `set_euler`/`set_transform` today overwrite every spawned child's transform with the parent's composed matrix. That exact behavior is kept — only the child-list *source* changes to `HierarchyComponent.children`.

### 4.4 `SceneSelection` service

```cpp
// app/debug/SceneSelection.hpp  (editor-only)
class SceneSelection
{
public:
    void Select(Entity e);          // replace selection, set primary
    void AddToSelection(Entity e);  // ctrl-click
    void ToggleSelection(Entity e);
    void SelectRange(...);          // shift-click (outliner supplies order)
    void Clear();
    bool Contains(Entity e) const;
    Entity Primary() const;
    const std::vector<Entity>& All() const;
private:
    std::vector<Entity> m_selected;
    Entity m_primary;
};
```

- **Owned by `DebugLayer`**, registered into the `ServiceContainer` in `OnAttach`, unregistered in `OnDetach`. Reached by panels via `context.Get<SceneSelection>()`.
- Replaces `InspectorPanel::m_selectedSceneEntity`.
- Prunes dead entities each frame (entities destroyed out from under it).
- Spec 2's viewport picking writes into the same service; Spec 1 already reads `Primary()`/`All()` from it.

## 5. Entity List — the "Scene" outliner

The window keeps the name **"Scene"** (preserves the saved dock layout) but becomes a proper outliner.

- **Toolbar row:** search/filter `InputTextWithHint`; a `+` **create** menu (empty entity / cube / sphere / plane); a small set of type filter toggles; live entity count.
- **Tree body:** recurse from roots (`HierarchyComponent.parent == {0}` or no component) using `TreeNodeEx`; childless nodes get `ImGuiTreeNodeFlags_Leaf`. Each row:
  - **Type badge** — a color-coded letter glyph derived from components (Mesh / Skinned / Physics / Empty), reusing the `colors::` namespace. **No new font/icon dependency.** (A real icon font is optional future polish.)
  - **Name** + muted `#id`.
  - **Multi-select** via ctrl (toggle) / shift (range), routed through `SceneSelection`.
- **Entity source change:** enumerate **all live entities** from the registry (`World::GetRegistry()` all-entities storage) instead of unioning component views. This removes the **80-row cap** and the untracked-component blind spot. In search mode the result collapses to a flat, `ImGuiListClipper`-friendly list for large scenes.
- **Context menu (right-click):** Rename · Create child · Add component ▸ · Delete (subtree).
- **Drag-drop reparent:** `DragDropSource`/`Target` carrying the entity id; drop on a node → `SetParent`; drop on empty space → detach to root. Cycle-guarded via `SetParent`.
- **Keyboard:** `Delete` deletes the selection (subtree-aware), `F2` renames the primary.

## 6. Inspector — editable component sections

- **Header:** editable name field, `#id`, kind badge, **Add Component** menu, **Delete entity** button.
- **One `CollapsingHeader` per present component**, each drawn by a dedicated `Draw<Component>(...)` function; a remove-"x" where removal is safe:
  - **Transform** — decompose the `mat4` to T / R(°, YXZ) / S via the shared helper (§7), three `DragFloat3`, recompose on edit through the same child-propagation path as `set_transform`. **Physics-aware:** if the entity has `PhysicsStateComponent`, position edits also write `prevPosition`/`currPosition` (a true teleport) so physics doesn't stomp the edit next frame.
  - **Material** — `ColorEdit4` base color; metallic / roughness / occlusion sliders; emissive; alpha flags (double-sided / blend / mask) + cutoff. **⚠ Risk (§8):** edits mutate the CPU `Material`; showing them on-GPU requires a `MaterialBuffer` re-upload path. Resolved in planning — if a cheap update exists, material is fully live; otherwise it renders **read-only** for Spec 1 rather than silently no-op'ing.
  - **Skinned mesh** — clip combo, playback speed, anim-time scrub, looping. Fully editable, visibly works.
  - **Physics** — motion type + live state; position teleport as above. Motion-type/shape changes are read-only (need Jolt rebuild → out of scope).
  - **Mesh / Pipeline** — read-only identity (asset swap needs a picker → later).
  - **Hierarchy** — parent (click-to-select) + children list (click-to-select) + unparent button.
  - **Tags** — surface the existing `TagSlots` on the entity with add/remove (the tag system already exists; low-effort, high-value).
- **Add Component** menu is limited to safely default-constructible components (Transform, Material, Name, Hierarchy, physics body descriptors). Asset-bearing components (Mesh/Pipeline) are excluded.
- **Multi-select:** inspector edits the **primary** only; when `SceneSelection.All().size() > 1` it shows a `"Editing primary of N selected"` note. Simultaneous multi-edit is future work.

## 7. Shared plumbing / refactors

- **Hoist TRS math:** move `ComposeTransform` / `DecomposeTRS` out of `WorldModule.cpp`'s anonymous namespace into `scene/TransformUtils.hpp`. The `.das` bindings and the inspector both consume it — one implementation, not two.
- **Selection lifecycle** in `DebugLayer` (register/unregister in attach/detach).
- **Panel file layout:** keep the `DebugPanel` pattern (own window, `Load/SaveSettings`). Split so neither file grows unwieldy — proposed:
  - `app/debug/HierarchyPanel.{hpp,cpp}` — the outliner. Class is `HierarchyPanel` but it draws the window still **titled "Scene"** (so the saved dock layout and `DebugLayer`'s `DockBuilderDockWindow("Scene", …)` mapping are unchanged).
  - `app/debug/InspectorPanel.{hpp,cpp}` — the editor shell + header.
  - `app/debug/ComponentDrawers.{hpp,cpp}` — the per-component `Draw*` functions.
  - `app/debug/SceneSelection.hpp` — the selection service.
  - The existing single `InspectorPanel` (which draws *both* Scene and Inspector windows today) is split accordingly; `DebugLayer` registers both panels.

## 8. Risks & open questions (resolved during planning)

1. **Material GPU propagation.** Does editing `MaterialComponent.material` require re-uploading to `MaterialBuffer` (the draw uses a `materialSlot` push constant)? Read `MaterialBuffer`'s update API in planning. Fallback: material section is read-only for Spec 1.
2. **All-entity enumeration API.** Confirm the exact EnTT call for "iterate every live entity" against the vendored version (`registry.storage<entt::entity>()` vs `each`). Purely mechanical.
3. **`load_model` reload.** On script reload the world is rebuilt; confirm spawned children are destroyed/recreated cleanly so `HierarchyComponent` links don't dangle. (Expected fine — same lifetime as the old `entityIds`.)

## 9. Testing / verification

The repo has **no unit-test harness**. Verification is:

1. **Build** clean (the migration touches engine + app + scripting; a green build proves the component removal is complete).
2. **Manual verification via the debug panel** (`/run`): outliner shows the hierarchy with names/badges; search filters; create/rename/delete/reparent work and survive; drag-drop reparent respects the cycle guard; inspector edits (transform on a static prop, transform-teleport on a physics body, skinned-mesh scrub on the fox, material color) reflect live in the viewport; multi-select bulk delete/reparent works.
3. Optional: a tiny standalone check for the pure-logic pieces (`SetParent` cycle guard, subtree delete, TRS compose/decompose round-trip). Not required — no framework exists — but recommended if a lightweight harness is cheap to stand up.

## 10. File change map

**New**
- `app/debug/SceneSelection.hpp`
- `app/debug/HierarchyPanel.{hpp,cpp}`
- `app/debug/ComponentDrawers.{hpp,cpp}`
- `scene/TransformUtils.hpp`

**Modified**
- `scene/Components.hpp` — add `NameComponent`, `HierarchyComponent`; **remove** `SpawnedEntitiesComponent`, `ParentEntityComponent`.
- `scene/EcsHelpers.hpp` — add `SetParent` / `DestroyHierarchy` / root iteration.
- `engine/assets/AssetManager.cpp` — `SetParent` instead of `ParentEntityComponent`.
- `engine/animation/AnimationCompiler.cpp`, `app/scripting/modules/AnimationModule.cpp` — read `HierarchyComponent.children`.
- `app/scripting/modules/WorldModule.cpp` — `load_model`/`set_*` use `HierarchyComponent`; add `set_name`/`get_name`; consume shared `TransformUtils`.
- `app/debug/InspectorPanel.{hpp,cpp}` — reduced to the editor shell + header.
- `app/layers/DebugLayer.{hpp,cpp}` — own/register `SceneSelection`; register `HierarchyPanel`; update default dock mapping if window set changes.
- Build files (`CMakeLists` / source lists) for the new translation units.

**Removed symbols**
- `SpawnedEntitiesComponent`, `ParentEntityComponent`, `InspectorPanel::kMaxSceneRows`, `InspectorPanel::m_selectedSceneEntity`.

## 11. Follow-on specs

- **Spec 2 — Viewport Picking & Selection Highlight:** screen-ray → mesh/AABB pick writes `SceneSelection`; selected entities outlined via the existing debug-line renderer.
- **Spec 3 — Scene Serialization:** save/load entity + component state, including the stable asset-reference system required to serialize pointer-bearing components.

## 12. Addendum — 2026-07-04 (v2)

The material-system overhaul landed after this spec was approved, resolving §8 risk 1 in the *good* direction: per-entity `MaterialInstanceComponent` + `MaterialSystem::AssignMaterial` give a cheap copy-on-write edit path with next-frame GPU visibility. **§6 Material is therefore fully live-edited** (factors, flags — which re-resolve the pipeline — and an Effect Params section for effect-driven entities), not read-only. This requires two small engine additions executed with the plan: `MaterialRegistry::TryDescribe` (reconstruct a `MaterialAsset` from a live slot) and seeding instances **from the entity's current material** instead of defaults — the default-seed behavior was a latent bug that wiped textures on first edit, in the inspector and the `entity_material_set_*` das bindings alike. Texture *swapping* remains out of scope (asset picker, later spec).

Further v2 deltas: §5's "no new font/icon dependency" is superseded — a Font Awesome 6 Free-Solid subset is merged into the ImGui atlas for entity-kind badges and toolbar glyphs (decision 2026-07-04); the outliner gains a micro-animation juice pass (selection pulse, spawn flash, empty states — motion only, no jokey content); the default dock layout moves the Inspector to the right side (classic editor arrangement); §9 is out of date — the repo now has a doctest harness (`EngineTests`), and the pure-logic pieces (hierarchy helpers, TRS round-trip, TryDescribe) get unit tests. Undo/redo is confirmed deferred to its own spec; inspector edits funnel through small helpers to keep that retrofit clean.
