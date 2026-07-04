# Inspector & Entity List Overhaul Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn the debug panel's flat "Scene" list and read-only "Inspector" into a real hierarchical outliner + live-editing inspector — including a **live material editor** built on the new material-instance system — backed by new `NameComponent`/`HierarchyComponent` ECS identity and a shared `SceneSelection` service.

**Architecture:** Add identity/hierarchy components and a cycle-guarded `SetParent` helper; fully migrate the two legacy relationship components (`SpawnedEntitiesComponent`, `ParentEntityComponent`) onto `HierarchyComponent` with no compat layer; register a `SceneSelection` service in the `ServiceContainer`; split the monolithic `InspectorPanel` into `HierarchyPanel` (outliner) + `InspectorPanel` (editor shell) + `ComponentDrawers`; merge a Font Awesome icon subset into the ImGui font atlas; add `MaterialRegistry::TryDescribe` + seed-from-current instances so the inspector (and das setters) edit materials without clobbering textures.

**Tech Stack:** C++23, EnTT (ECS), Dear ImGui v1.92.8-docking, glm, daScript bindings, doctest (`EngineTests`), CMake presets.

---

## Revision changelog — 2026-07-04 (v2)

The v1 plan (committed 6abc734, 2026-07-03) predates the material-system overhaul (6 commits, 2026-07-04). Verified against HEAD 2688262; none of v1 had been implemented. Changes:

1. **Old Task 16 (read-only material) replaced** by Task 18 (live material editor). `MaterialComponent` is now `{MaterialHandle, gpuSlot}`; `MaterialInstanceComponent`/`EffectParamsComponent` exist; `MaterialSystem` typed setters give a copy-on-write edit path with next-frame GPU visibility.
2. **v1 Task 5 would not have built**: `InspectorPanel.cpp` references the legacy components (lines 59/63/125/130) but v1 deleted them before the panel rewrite. Task 5 now strips those references too.
3. **CMakeLists steps removed** — `src/app/CMakeLists.txt` GLOBs sources; new `.cpp` files are picked up automatically.
4. **Physics teleport extended**: `PhysicsStateComponent` carries `prevRotation/currRotation` quats + `scale`; the transform drawer writes those too, not just positions.
5. **TagSlots needs a small enumeration API** for the tag drawer (only per-tag ops exist today) — added in Task 17.
6. Line drift: WorldModule TRS helpers 29-61 (was ~26-58); AssetManager `ParentEntityComponent` emplace at 712 (was ~805).
7. **The repo now HAS a test harness** (doctest `EngineTests`, added with the material work). Pure-logic pieces get unit tests: hierarchy helpers (T1), TRS round-trip (T2), `TryDescribe`/seed-from-current (T18).
8. **Latent das bug fixed by T18**: `entity_material_set_*` on a textured entity seeds a *default* instance and wipes its textures; seed-from-current fixes script + inspector paths.
9. New tasks from the AAA/juice decisions (2026-07-04): T8 icon font (FA6 solid subset merged into the atlas — supersedes v1's "no new font dependency" stance), T14 juice pass (selection pulse, spawn flash, empty states; micro-animations only — no jokey content), T20 gains the editor dock layout (Scene left / Inspector right / Viewport center).
10. Undo/redo confirmed deferred to a later spec; drawers funnel edits through small helpers to keep the retrofit clean.

---

## Verification model (read first)

The repo has a **doctest harness**: `EngineTests` links the full `Engine` lib; test sources live in `tests/` and `tests/material/`; register new files in `tests/CMakeLists.txt`.

**Build command ("BUILD"):**
```bash
cmake --build build-vs2022-msvc --config Debug --target App
```
(`build-vs2022-msvc` is the primary configured build dir with a live cache. If the Vulkan SDK moved, purge stale cache entries first — see the known gotcha: `cmake -S . -B build-vs2022-msvc -U "Vulkan_*" -U "AETHERCORE_SLANG_ROOT" -U "SLANGC_EXECUTABLE" -U "FIND_PACKAGE_MESSAGE_DETAILS_Vulkan"`.)

**Test command ("TEST"):**
```bash
cmake --build build-vs2022-msvc --config Debug --target EngineTests
ctest --test-dir build-vs2022-msvc -C Debug --output-on-failure
```

**Manual check ("RUN"):** the app is GPU-driven and runs on Alex's machine — launch `build-vs2022-msvc/src/app/Debug/App.exe` (or `/run`), **F1** toggles the debug panel, **F5** hot-reloads `resources/scripts/sandbox.das`. Tasks marked RUN end with a concise hand-off checklist instead of an agent-side claim of visual correctness.

**Commit convention:** one commit per task, footer:
```
Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
```

---

## File Structure

**New files**
| File | Responsibility |
|------|----------------|
| `src/engine/scene/TransformUtils.hpp` | `ComposeTransform` / `DecomposeTRS` (hoisted from `WorldModule.cpp`). |
| `src/engine/scene/Hierarchy.hpp` | `SetParent`, `DetachFromParent`, `DestroyHierarchy`, `IsAncestor` — the only sanctioned way to mutate `HierarchyComponent`. |
| `src/app/debug/SceneSelection.hpp` | Shared multi-select state; registered as a service. |
| `src/app/debug/HierarchyPanel.hpp/.cpp` | The outliner (window titled **"Scene"**). |
| `src/app/debug/ComponentDrawers.hpp/.cpp` | Per-component `Draw*` functions used by the inspector. |
| `src/app/debug/Icons.hpp` | FA6 codepoint defines (no external header dep). |
| `resources/fonts/fa-solid-900.ttf` | Font Awesome 6 Free-Solid (OFL) — packed into assets.pak automatically. |
| `tests/scene/HierarchyTests.cpp`, `tests/scene/TransformUtilsTests.cpp`, `tests/material/MaterialDescribeTests.cpp` | doctest coverage for the pure-logic pieces. |

**Modified files**
| File | Change |
|------|--------|
| `src/engine/scene/Components.hpp` | Add `NameComponent`, `HierarchyComponent`; **remove** `SpawnedEntitiesComponent`, `ParentEntityComponent` (T5). |
| `src/engine/assets/AssetManager.cpp` (:712) | Parent spawned mesh entities via `SetParent`. |
| `src/engine/animation/AnimationCompiler.cpp` (:26), `src/app/scripting/modules/AnimationModule.cpp` (:32, :42) | Read children from `HierarchyComponent`. |
| `src/app/scripting/modules/WorldModule.cpp` | Use `TransformUtils`; parent + auto-name in spawn paths; propagate via `HierarchyComponent.children`; add `set_name`/`get_name`. |
| `src/app/debug/InspectorPanel.hpp/.cpp` | Editor shell + header; stops drawing the "Scene" window. |
| `src/app/layers/DebugLayer.hpp/.cpp` | Own + register `SceneSelection`; register `HierarchyPanel`; dock layout V3 (T20). |
| `src/engine/imgui/ImguiSubsystem.cpp` | Merge-load the icon font after Roboto. |
| `src/engine/material/MaterialRegistry.hpp/.cpp`, `MaterialSystem.cpp` | `TryDescribe`; `GetOrSeedInstance` seeds from current. |
| `src/engine/scene/TagSlots.hpp/.cpp` | Minimal tag-enumeration accessor. |
| `resources/scripts/sandbox.das` | `set_name` on key entities (fox, floor, zone props). |
| `tests/CMakeLists.txt` | Register new test files. |

Services reachable from panels: `World`, `MaterialRegistry`, `MaterialBuffer`, `EffectParamBuffer`, `AssetManager` (→ `GetPipelineCache()`), `ImguiSubsystem` (AetherCore.cpp:65-115). `PipelineCache` is **not** a service — go through `AssetManager`.

---

# Phase A — ECS Foundation

## Task 1: Identity + hierarchy components, helpers, tests

**Files:** modify `src/engine/scene/Components.hpp`; create `src/engine/scene/Hierarchy.hpp`, `tests/scene/HierarchyTests.cpp`; modify `tests/CMakeLists.txt`.

- [x] **Step 1: Add the two components** in `Components.hpp` (add `#include <string>`, `#include <vector>`, `#include "scene/Entity.hpp"` as needed), after `TransformComponent`:

```cpp
	// Human-readable display name (auto-assigned at spawn, editable in the inspector).
	struct NameComponent
	{
		std::string name;
	};

	// Canonical scene-graph link. Both sides are kept consistent exclusively
	// through aether::ecs::SetParent (see scene/Hierarchy.hpp) — never mutate
	// parent/children directly.
	struct HierarchyComponent
	{
		Entity parent{};              // {0} == root
		std::vector<Entity> children; // ordered
	};
```

> **Migration note:** `ParentEntityComponent` and `SpawnedEntitiesComponent` stay through Tasks 1-4 so the tree is green on every commit; deleted in Task 5.

- [x] **Step 2: Create `src/engine/scene/Hierarchy.hpp`** (verbatim from v1 — verified against the real `World` API):

```cpp
#pragma once

#include <algorithm>
#include <vector>

#include "scene/Components.hpp"
#include "scene/Entity.hpp"
#include "scene/World.hpp"

namespace aether::ecs
{
	// True if `possibleAncestor` is `entity` itself or any ancestor of it.
	inline bool IsAncestor(World& world, Entity entity, Entity possibleAncestor)
	{
		Entity cur = entity;
		while (cur.IsValid())
		{
			if (cur == possibleAncestor)
			{
				return true;
			}
			const auto* h = world.TryGet<HierarchyComponent>(cur);
			if (!h)
			{
				break;
			}
			cur = h->parent;
		}
		return false;
	}

	// Removes `child` from its current parent's child list and clears its parent.
	inline void DetachFromParent(World& world, Entity child)
	{
		auto* ch = world.TryGet<HierarchyComponent>(child);
		if (!ch || !ch->parent.IsValid())
		{
			return;
		}
		if (auto* ph = world.TryGet<HierarchyComponent>(ch->parent))
		{
			auto& kids = ph->children;
			kids.erase(std::remove(kids.begin(), kids.end(), child), kids.end());
		}
		ch->parent = {};
	}

	// Re-parents `child` under `parent` (parent == {0} detaches to root).
	// Returns false (no-op) if child == parent or it would create a cycle.
	inline bool SetParent(World& world, Entity child, Entity parent)
	{
		if (!child.IsValid() || child == parent)
		{
			return false;
		}
		if (parent.IsValid() && IsAncestor(world, parent, child))
		{
			return false; // cycle: parent is a descendant of child
		}

		DetachFromParent(world, child);

		// Set the child side first; emplacing on the parent below may reallocate
		// the HierarchyComponent pool and invalidate this pointer.
		auto* ch = world.TryGet<HierarchyComponent>(child);
		if (!ch)
		{
			ch = &world.Emplace<HierarchyComponent>(child);
		}
		ch->parent = parent;

		if (parent.IsValid())
		{
			auto* ph = world.TryGet<HierarchyComponent>(parent);
			if (!ph)
			{
				ph = &world.Emplace<HierarchyComponent>(parent);
			}
			ph->children.push_back(child);
		}
		return true;
	}

	// Recursively destroys `entity` and its whole subtree, keeping parent links tidy.
	inline void DestroyHierarchy(World& world, Entity entity)
	{
		std::vector<Entity> kids; // copy — the loop mutates the source vector
		if (const auto* h = world.TryGet<HierarchyComponent>(entity))
		{
			kids = h->children;
		}
		for (const Entity c: kids)
		{
			DestroyHierarchy(world, c);
		}
		DetachFromParent(world, entity);
		world.Destroy(entity);
	}
} // namespace aether::ecs
```

- [x] **Step 3: doctest coverage** — `tests/scene/HierarchyTests.cpp` (+ register in `tests/CMakeLists.txt`, matching the existing style): reparent moves the child between parents' lists; `SetParent(child, descendant)` returns false and mutates nothing; self-parent rejected; `DetachFromParent` → root; `DestroyHierarchy` kills the subtree and removes the node from its parent's list; parent `{0}` semantics.
- [x] **Step 4: BUILD + TEST.** Expected: green (additive only).
- [x] **Step 5: Commit** `feat(scene): add NameComponent, HierarchyComponent and SetParent helpers`.

---

## Task 2: Hoist TRS math into a shared header (+ round-trip test)

**Files:** create `src/engine/scene/TransformUtils.hpp`, `tests/scene/TransformUtilsTests.cpp`; modify `src/app/scripting/modules/WorldModule.cpp` (delete locals at :29-61, qualify call sites), `tests/CMakeLists.txt`.

- [x] **Step 1:** Create `TransformUtils.hpp` with the exact bodies from `WorldModule.cpp`'s anonymous namespace (lines 29-61), promoted into `namespace aether` (v1 Task 2 snippet is verbatim-correct).
- [x] **Step 2:** Delete the two local functions; add `#include "scene/TransformUtils.hpp"`; prefix call sites with `aether::` in `das_set_euler`, `das_set_transform`, `das_get_euler`, `das_get_scale`, `das_for_each_with_tag_transform` (any other `ComposeTransform`/`DecomposeTRS` users `git grep` finds).
- [x] **Step 3:** `tests/scene/TransformUtilsTests.cpp`: compose→decompose round-trip across representative pos/euler/scale sets (including negative angles, near-gimbal ±90° X), tolerance ~1e-3.
- [x] **Step 4: BUILD + TEST.**
- [x] **Step 5: Commit** `refactor(scene): hoist TRS compose/decompose into TransformUtils.hpp`.

---

## Task 3: Populate hierarchy + names at spawn (writers)

**Files:** modify `src/engine/assets/AssetManager.cpp` (:712), `src/app/scripting/modules/WorldModule.cpp`.

- [x] **Step 1: AssetManager.** Add `#include "scene/Hierarchy.hpp"`; beside the legacy emplace at :712 (keep it until T5):

```cpp
			if (parentEntityId != 0)
			{
				m_world->Emplace<ParentEntityComponent>(entity, ParentEntityComponent{.parentId = parentEntityId});
				aether::ecs::SetParent(*m_world, entity, aether::Entity{parentEntityId});
			}
```

- [x] **Step 2: `das_load_model`** — SetParent each spawned mesh entity under `Entity{id}` (keep the `SpawnedEntitiesComponent` bookkeeping until T5), and name the parent from the model path stem (e.g. `"fox"`), `EmplaceOrReplace<NameComponent>`.
- [x] **Step 3: Names for primitives + bare entities.** `das_add_mesh`: if no `NameComponent`, emplace one — use the primitive kind if cheap to thread through the mesh cache, else `"Mesh"`. `das_entity_create`: emplace `NameComponent{"Entity"}`.
- [x] **Step 4: BUILD.**
- [x] **Step 5: Commit** `feat(scene): populate HierarchyComponent + NameComponent at spawn`.

---

## Task 4: Migrate readers to HierarchyComponent

**Files:** modify `src/engine/animation/AnimationCompiler.cpp` (:26), `src/app/scripting/modules/AnimationModule.cpp` (:32, :42), `src/app/scripting/modules/WorldModule.cpp` (`das_set_euler` :158, `das_set_transform` :184).

- [x] **Step 1:** All five read sites switch `SpawnedEntitiesComponent`/`entityIds` (`vector<uint32_t>`) → `HierarchyComponent`/`children` (`vector<Entity>`); pass `child`/`child.id` accordingly. Propagation behavior in `set_euler`/`set_transform` is preserved verbatim (children get the parent's composed matrix).
- [x] **Step 2: BUILD.**
- [ ] **Step 3: RUN (hand-off): fox still animates; plasma target + toys still move; no crash.**
- [x] **Step 4: Commit** `refactor(scene): read child links from HierarchyComponent`.

---

## Task 5: Delete the legacy components (finish the migration)

**Files:** modify `src/engine/scene/Components.hpp`, `src/engine/assets/AssetManager.cpp`, `src/app/scripting/modules/WorldModule.cpp`, **`src/app/debug/InspectorPanel.cpp`**.

- [x] **Step 1:** Remove the legacy writes: `Emplace<ParentEntityComponent>` (AssetManager :712 block), the `SpawnedEntitiesComponent` bookkeeping in `das_load_model` (:264-274).
- [x] **Step 2:** **Strip InspectorPanel's legacy references** (v2 correction): the two `Has<>` lines in `ComponentSummary` (:59, :63) and the two `View<>` unions in `CollectSceneEntities` (:125, :130). The panel is rewritten in Phase C; this just keeps the build green.
- [x] **Step 3:** Delete both structs from `Components.hpp`.
- [x] **Step 4:** `git grep -n "SpawnedEntitiesComponent\|ParentEntityComponent"` → no matches outside docs.
- [x] **Step 5: BUILD + TEST.**
- [x] **Step 6: Commit** `refactor(scene)!: remove SpawnedEntitiesComponent/ParentEntityComponent`.

---

## Task 6: `.das` name bindings + sandbox names

**Files:** modify `src/app/scripting/modules/WorldModule.cpp`, `resources/scripts/sandbox.das`.

- [x] **Step 1:** `das_set_name(World*, uint32_t, const char*)` → `EmplaceOrReplace<NameComponent>`; register `Bind<das_set_name>(lib, "set_name", SE::modifyExternal)`. Add `get_name` **only if** the daScript string-return idiom (`ctx->stringHeap->allocateString`) checks out against the vendored daScript (no existing binding returns a string — verify or drop).
- [x] **Step 2:** In `sandbox.das`, `set_name` the memorable actors: fox, floor, walls, plasma light target, Zone M gallery groups. (Doubles as binding verification and makes the outliner demo read well.)
- [ ] **Step 3: BUILD.** F5-reload check happens at the Phase B RUN gate.
- [x] **Step 4: Commit** `feat(scripting): add set_name das binding + name sandbox actors`.

---

## Task 7: `SceneSelection` service

**Files:** create `src/app/debug/SceneSelection.hpp`; modify `src/app/layers/DebugLayer.hpp/.cpp`.

- [x] **Step 1:** Create `SceneSelection` exactly per the v1 snippet (Select / AddToSelection / ToggleSelection / Clear / Contains / Primary / All / Prune-dead-entities; `std::vector<Entity>` + primary).
- [x] **Step 2:** `DebugLayer` member `SceneSelection m_selection;` — `context.services.Register<SceneSelection>(m_selection)` in `OnAttach` (before panel creation), `Unregister` in `OnDetach`, `m_selection.Prune(context.Get<World>())` in `OnUpdate`.
- [x] **Step 3: BUILD.**
- [x] **Step 4: Commit** `feat(debug): add shared SceneSelection service`.

---

# Phase B — Entity List (the outliner)

## Task 8: Icon font foundation

**Files:** add `resources/fonts/fa-solid-900.ttf`; create `src/app/debug/Icons.hpp`; modify `src/engine/imgui/ImguiSubsystem.cpp` (font load, ~:219-233).

- [x] **Step 1:** Vendor Font Awesome 6 Free-Solid TTF (OFL-1.1) into `resources/fonts/` (pak pipeline picks it up beside Roboto).
- [x] **Step 2:** `Icons.hpp` — small self-contained codepoint defines (no IconFontCppHeaders dependency), e.g. `ICON_FA_CUBE "\xef\x86\xb2"` (U+F1B2), person-running, weight-hanging, bolt, tag, magnifying-glass, plus, trash, palette, sitemap, eye, gears, film, wand.
- [x] **Step 3:** In `ImguiSubsystem` after the Roboto load: second `AddFontFromMemoryTTF` with `ImFontConfig{MergeMode=true, PixelSnapH=true, GlyphMinAdvanceX=15}` and static glyph range `{0xE000, 0xF8FF, 0}` (FA6 solid glyphs live in the PUA); read via the same `assets://fonts/…` VFS path.
- [x] **Step 4: BUILD.** Fallback if the TTF can't be obtained: keep `Icons.hpp` names but map to colored Unicode `●■▲◆` and proceed — call sites don't change.
- [x] **Step 5: Commit** `feat(imgui): merge FA6 solid icon subset into the font atlas`.

---

## Task 9: Extract `HierarchyPanel`, flat uncapped list

**Files:** create `src/app/debug/HierarchyPanel.hpp/.cpp`; modify `src/app/debug/InspectorPanel.hpp/.cpp`, `src/app/layers/DebugLayer.cpp`.

- [x] **Step 1:** `HierarchyPanel : DebugPanel` (`GetName()` = "Scene Outliner", window still `Begin("Scene")` so the saved dock mapping holds). Body per v1 Task 8: entity count + `BeginChild` list over `reg.storage<entt::entity>()` (skip invalid), rows `#id Name` via `Selectable`, click → `selection.Select(e)`.
- [x] **Step 2:** InspectorPanel: delete the whole `Begin("Scene")` block + `CollectSceneEntities`/`SceneEntityLabel`/`kMaxSceneRows`/`m_selectedSceneEntity`; selection source becomes `context.Get<SceneSelection>().Primary()`.
- [x] **Step 3:** DebugLayer: `m_panels.push_back(std::make_unique<HierarchyPanel>());` near the InspectorPanel push. (No CMake edit — sources are GLOBed.)
- [x] **Step 4: BUILD.**
- [x] **Step 5: Commit** `feat(debug): extract HierarchyPanel with uncapped all-entity list`.

---

## Task 10: Tree rendering + icon badges

**Files:** modify `src/app/debug/HierarchyPanel.cpp`.

- [ ] **Step 1:** Kind badge helper returning `{icon, color}` from components — skinned (`ICON_FA_PERSON_RUNNING`, blue), physics (`ICON_FA_WEIGHT_HANGING`, orange), effect (`ICON_FA_WAND_MAGIC_SPARKLES`, purple), mesh (`ICON_FA_CUBE`, green), empty (dim dot). Include `physics/PhysicsComponents.hpp`.
- [x] **Step 2:** Recursive `DrawNode` per v1 Task 9 (TreeNodeEx `OpenOnArrow|SpanAvailWidth`, leaf flags for childless, click-not-toggle selects), driven from roots (`!h || !h->parent.IsValid()`); icon drawn colored before the label, muted `#id` after.
- [x] **Step 3:** Enable tree indent guides with `ImGuiTreeNodeFlags_DrawLinesToNodes` (present in 1.92.8).
- [x] **Step 4: BUILD.**
- [x] **Step 5: Commit** `feat(debug): render entity hierarchy as a tree with icon badges`.

---

## Task 11: Toolbar — search, create, count, filter chips

**Files:** modify `src/app/debug/HierarchyPanel.hpp/.cpp`.

- [x] **Step 1:** Toolbar row: `ICON_FA_PLUS` create menu (Empty entity; Cube/Sphere/Plane via the primitive-mesh spawn path used by `das_create_mesh`/`PrimitiveMeshes` — auto-named, selected on create), `ICON_FA_MAGNIFYING_GLASS` search `InputTextWithHint` (`m_search[64]`), right-aligned live entity count.
- [x] **Step 2:** Filter mode: when searching, flat `ImGuiListClipper` list (case-insensitive substring on name or `#id`); tree otherwise.
- [x] **Step 3:** Component-type filter chips (toggle buttons: mesh/skinned/physics/effect) applied in both modes.
- [x] **Step 4: BUILD.**
- [x] **Step 5: Commit** `feat(debug): outliner toolbar — search, create menu, type filters`.

---

## Task 12: Multi-select, context menu, keyboard

**Files:** modify `src/app/debug/HierarchyPanel.hpp/.cpp`.

- [x] **Step 1:** Ctrl-click `ToggleSelection`; **shift-click range** over the currently visible row order (panel records the flattened visible list each frame); plain click `Select`.
- [x] **Step 2:** Context menu per node: Rename (→ inline edit), Create child (new named entity + `SetParent`), Delete (subtree via `DestroyHierarchy`).
- [x] **Step 3:** Inline rename state (`m_renaming`, `m_renameBuf`): InputText swaps in for the label, commits to `NameComponent` on Enter/defocus.
- [x] **Step 4:** Window-focused keys: Delete (collect selection to a local vector first, then destroy), F2 (rename primary).
- [x] **Step 5: BUILD.**
- [x] **Step 6: Commit** `feat(debug): outliner multi-select, context menu, rename + delete keys`.

---

## Task 13: Drag-drop reparent

**Files:** modify `src/app/debug/HierarchyPanel.cpp`.

- [x] **Step 1:** `BeginDragDropSource` with `AETHER_ENTITY` payload (entity id) + drag label; `BeginDragDropTarget` on each node → `ecs::SetParent(world, Entity{draggedId}, e)` (cycle guard makes bad drops silent no-ops).
- [x] **Step 2:** Empty-space drop target after the root loop → detach to root (`SetParent(world, dragged, {})`).
- [x] **Step 3: BUILD.**
- [ ] **Step 4: RUN (hand-off): tree with names + icons; search filters; + creates; ctrl/shift multi-select; right-click ops; F2 rename sticks; Delete removes subtree; drag-drop nests / unparents / rejects cycles; F5 reload rebuilds names + hierarchy; entity count well past 80.**
- [x] **Step 5: Commit** `feat(debug): drag-drop reparenting in the outliner`.

---

## Task 14: Juice pass (micro-animations)

**Files:** modify `src/app/debug/HierarchyPanel.hpp/.cpp` (and small shared helpers if the inspector reuses them).

- [x] **Step 1: Selection pulse** — on selection change, record `ImGui::GetTime()`; selected-row highlight lerps from an accent-bright tint to the normal selection color over ~0.2s (drawlist rect behind the row, eased).
- [x] **Step 2: Spawn flash** — panel tracks seen entity ids (`unordered_set` + first-seen time); rows younger than ~0.75s get a fading glow tint. Set is pruned with dead ids.
- [x] **Step 3: Hover + rows** — subtle alternating row tint and a hover brighten (drawlist, respecting the theme's flat look).
- [x] **Step 4: Empty states** — centered, dimmed: outliner "No entities match" (search mode with zero hits); inspector "Nothing selected".
- [x] **Step 5: BUILD.**
- [x] **Step 6: Commit** `feat(debug): outliner micro-animations + empty states`.

---

# Phase C — Inspector

## Task 15: Editor shell + header + drawer scaffold

**Files:** create `src/app/debug/ComponentDrawers.hpp/.cpp`; modify `src/app/debug/InspectorPanel.cpp`.

- [x] **Step 1:** `ComponentDrawers.hpp` declares: `DrawTransform(LayerContext&, World&, Entity)`, `DrawSkinnedMesh`, `DrawMaterial(LayerContext&, World&, Entity)`, `DrawEffectParams(LayerContext&, World&, Entity)`, `DrawPhysics`, `DrawMeshPipeline`, `DrawHierarchy(World&, Entity, SceneSelection&)`, `DrawTags`. `.cpp` = guarded stubs (TryGet + early-return).
- [x] **Step 2:** InspectorPanel shell: no-selection empty state; header = kind icon + editable name InputText (emplace-on-edit if missing) + muted `#id` + "Editing primary of N selected" note; then the Draw* list.
- [x] **Step 3:** **Add Component palette**: button opens a popup with an InputText filter over safely-default-constructible components (Transform, Name, Hierarchy) — type-to-filter, click/Enter adds. `Delete entity` button (DestroyHierarchy + selection.Clear).
- [x] **Step 4: BUILD.**
- [x] **Step 5: Commit** `feat(debug): inspector editor shell, header, add-component palette`.

---

## Task 16: Transform drawer (per-axis colors, physics-aware)

**Files:** modify `src/app/debug/ComponentDrawers.cpp`.

- [x] **Step 1:** `DrawVec3Row(label, glm::vec3&, resetValue, speed)` helper — per-axis colored badge (X red / Y green / Z blue accent bar or mini-button that resets that axis), `DragFloat` per axis, row reset button; returns changed.
- [x] **Step 2:** `DrawTransform`: `DecomposeTRS` → Position/Rotation/Scale rows → on change `ComposeTransform`; propagate to `HierarchyComponent.children` exactly like `set_transform`; **physics-aware teleport** if `PhysicsStateComponent` present:

```cpp
		if (auto* ps = world.TryGet<PhysicsStateComponent>(entity))
		{
			const glm::quat q = glm::quat(glm::radians(euler)); // matches YXZ compose order — verify axis order against ComposeTransform
			ps->prevPosition = pos;   ps->currPosition = pos;
			ps->prevRotation = q;     ps->currRotation = q;
			ps->scale = scale;
		}
```

- [x] **Step 3: BUILD.**
- [ ] **Step 4: RUN (hand-off): static wall edit sticks; dynamic toy teleports (position AND rotation) without snap-back; fox children follow parent edits.**
- [x] **Step 5: Commit** `feat(debug): editable physics-aware transform section with per-axis rows`.

---

## Task 17: Skinned-mesh, hierarchy, tags drawers (+ TagSlots enumeration)

**Files:** modify `src/app/debug/ComponentDrawers.cpp`, `src/engine/scene/TagSlots.hpp/.cpp`.

- [x] **Step 1:** `DrawSkinnedMesh`: clip InputInt, playback-speed drag (−4..4), anim-time scrub, looping checkbox (v1 snippet valid).
- [x] **Step 2:** `DrawHierarchy`: parent button (click-to-select) + Unparent; children as click-to-select buttons (v1 snippet valid).
- [x] **Step 3:** TagSlots: add a minimal read API (e.g. `void ForEachTag(const std::function<void(const std::string&, uint32_t)>&)` over the name→id map — match the existing file's style). `DrawTags`: list entity's tags (ForEachTag + TagHas) with per-tag remove, add-by-name field (TagCreate/TagGetId + TagAdd).
- [x] **Step 4: BUILD.**
- [x] **Step 5: Commit** `feat(debug): skinned-mesh, hierarchy, tag inspector sections`.

---

## Task 18: Material live editor (flagship)

**Files:** modify `src/engine/material/MaterialRegistry.hpp/.cpp`, `src/engine/material/MaterialSystem.cpp`, `src/app/debug/ComponentDrawers.cpp`; create `tests/material/MaterialDescribeTests.cpp`; modify `tests/CMakeLists.txt`.

- [x] **Step 1: `MaterialRegistry::TryDescribe(MaterialHandle, MaterialAsset& out) const`** — validate handle (alive + generation), reconstruct: factors + flags unpacked from the slot's stored `GpuMaterial` (inverse of `PackMaterial` for the factor/flag fields), the 5 stored `TextureHandle`s copied directly, `templateDesc` left default. Returns false for stale/invalid handles.
- [x] **Step 2: Seed-from-current** — `GetOrSeedInstance` gains a registry + current-handle path: when creating the instance, seed `asset` via `TryDescribe(currentHandle)` when the entity has a valid `MaterialComponent` (falls back to default asset otherwise). Typed setters pass the registry through. **Fixes the latent das clobber bug** (`entity_material_set_*` on textured entities).
- [x] **Step 3: doctest** — pack→describe round-trip for factors/flags/textures; describe-stale-handle fails; seed-from-current preserves texture handles when a setter touches one factor.
- [x] **Step 4: `DrawMaterial`** — requires `MaterialComponent`; seeds/reads `MaterialInstanceComponent.asset`:
  - `ColorEdit4` baseColorFactor; sliders metallic/roughness (0-1), occlusion (0-1); `ColorEdit3` emissive; alpha-cutoff drag (shown when alphaMask); checkboxes doubleSided / alphaBlend / alphaMask / modulateVertexColor.
  - Any change → **one** `MaterialSystem::AssignMaterial(world, e, registry, context.Get<AssetManager>().GetPipelineCache(), inst.asset)` (dedup-safe; flags re-resolve the pipeline; effect-driven entities keep their pipeline via the existing override).
  - Read-only texture rows: albedo/normal/metallicRoughness/occlusion/emissive — valid/broken/none per `TextureHandle` state. Thumbnails only if an ImageView accessor is already cheap via `ImguiSubsystem::RegisterTexture`; otherwise defer.
  - Footer: `GPU slot %u` from `MaterialComponent.gpuSlot`.
- [x] **Step 5: `DrawEffectParams`** — if `EffectParamsComponent`: tint `ColorEdit4`, speed/scale/intensity drags → mutate CPU `params` then `context.Get<EffectParamBuffer>().Write(paramSlot, params)` (same path as the das setters; skip when `paramSlot` invalid).
- [x] **Step 6: BUILD + TEST.**
- [ ] **Step 7: RUN (hand-off): recolor one of the 36 dedup crimson cubes → only it changes (splits to its own slot); edit fox roughness → textures stay; Zone M shared-material pulse (sandbox.das:629) unaffected by per-entity edits; plasma entity tint/speed scrub live. This is also the outstanding GPU verify for material phases 3-4.**
- [x] **Step 8: Commit** `feat(material,debug): live material editor with seed-from-current instances`.

---

## Task 19: Physics + render read-only drawers

**Files:** modify `src/app/debug/ComponentDrawers.cpp`.

- [x] **Step 1:** `DrawPhysics`: motion type text (Static/Kinematic/Dynamic), live `currPosition`/`scale`; note "(motion/shape changes need body rebuild — out of scope)".
- [x] **Step 2:** `DrawMeshPipeline`: mesh/pipeline presence lines; "Asset swapping: later spec".
- [x] **Step 3: BUILD.**
- [x] **Step 4: Commit** `feat(debug): read-only physics/render inspector sections`.

---

## Task 20: Final pass — remove-x, dock layout V3, acceptance

**Files:** modify `src/app/debug/InspectorPanel.cpp`, `src/app/debug/ComponentDrawers.cpp`, `src/app/layers/DebugLayer.cpp`, this plan + the spec.

- [x] **Step 1:** Remove-"x" on safely-removable section headers (Name, Hierarchy — conservative list).
- [x] **Step 2:** **Dock layout V3**: bump the dockspace id string (`AetherDebugDockSpaceV3`) and rebuild the default layout — Scene left ~20%, **Inspector right ~27%**, bottom row Performance/Lighting/Day-Night/Textures, right stack Render Graph/Debug/Tonemap/Post Processing, Viewport center.
- [x] **Step 3: BUILD.**
- [ ] **Step 4: RUN — full acceptance (hand-off):** outliner (tree/icons/search/chips/create/rename/delete/reparent/multi-select/no-cap/juice), inspector (header/palette/transform/skinned/material-live/effects/physics/render/hierarchy/tags/remove-x), F5 reload clean, new dock layout, fox intact.
- [x] **Step 5:** Tick all checkboxes here; finalize the spec addendum; update memory notes.
- [x] **Step 6: Commit** `feat(debug): inspector polish, editor dock layout, acceptance pass`.

---

## Self-review notes (v2)

- **Spec coverage:** identity §4.1 → T1/T3/T6; hierarchy §4.2 → T1; migration §4.3 → T3-T5; selection §4.4 → T7; outliner §5 → T9-T14 (badges upgraded to icon font per 2026-07-04 decision); inspector §6 → T15-T19 (material upgraded read-only → live per decision; physics teleport extended to rotation+scale); plumbing §7 → T2/T9/T15; risks §8.1 resolved (cheap GPU update exists — instance + AssignMaterial); §9 updated (doctest harness exists).
- **New engine surface:** `ecs::SetParent` family (T1), `TryDescribe` + seed-from-current (T18), TagSlots enumeration (T17), icon-font merge (T8) — each is small, tested where pure-logic.
- **Known execution-time confirmations:** daScript string-return idiom (T6); FA6 TTF availability (T8 — fallback defined); `ImGuiTreeNodeFlags_DrawLinesToNodes` exact name in 1.92.8 (T10 — skip if absent); quat axis-order vs `ComposeTransform` YXZ (T16); thumbnail ImageView accessor cost (T18 — defer if not cheap).
