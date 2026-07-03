# Inspector & Entity List Overhaul Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn the debug panel's flat "Scene" list and read-only "Inspector" into a real hierarchical outliner + live-editing inspector, backed by new `NameComponent`/`HierarchyComponent` ECS identity and a shared `SceneSelection` service.

**Architecture:** Add identity/hierarchy components and a cycle-guarded `SetParent` helper; fully migrate the two legacy relationship components (`SpawnedEntitiesComponent`, `ParentEntityComponent`) onto `HierarchyComponent` with no compat layer; register a `SceneSelection` service in the `ServiceContainer` that all panels share; split the monolithic `InspectorPanel` into a `HierarchyPanel` (outliner) + `InspectorPanel` (editor shell) + `ComponentDrawers`.

**Tech Stack:** C++20, EnTT v3.16 (ECS), Dear ImGui (docking), glm, daScript bindings, CMake presets.

---

## Verification model (read first)

The repo has **no unit-test harness** (confirmed — no Catch2/gtest/doctest, no `tests/` tree). Per the approved spec §9, every task's verification gate is:

1. **Build succeeds**, and
2. Where behavior changes, **manual verification via the `/run` skill** (launches the `App` target and drives the debug panel).

**Build command (used in every task — referred to as “BUILD”):**
```bash
# First time only (configures the worktree build dir):
cmake --preset vs2022-clang
# Every build:
cmake --build --preset vs2022-clang --config Debug --target App
```
Expected: `App.vcxproj -> …\App.exe` with no errors. If clang-cl is unavailable, substitute `vs2022-msvc`.

**Manual-check command (referred to as “RUN”):** invoke the `/run` skill, or launch `build-vs2022-clang/src/app/Debug/App.exe`. The debug panel toggles with **F1**; the sandbox scene (`resources/scripts/sandbox.das`) spawns the fox, floor, walls, and toys used in the checks below.

**Commit convention:** each commit ends with the repo footer:
```
Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
```

---

## File Structure

**New files**
| File | Responsibility |
|------|----------------|
| `src/engine/scene/TransformUtils.hpp` | `ComposeTransform` / `DecomposeTRS` (hoisted out of `WorldModule.cpp`). Single TRS implementation. |
| `src/engine/scene/Hierarchy.hpp` | `SetParent`, `DetachFromParent`, `DestroyHierarchy`, `IsAncestor` — the only sanctioned way to mutate `HierarchyComponent`. |
| `src/app/debug/SceneSelection.hpp` | Shared multi-select state; registered as a service. |
| `src/app/debug/HierarchyPanel.hpp` / `.cpp` | The outliner (draws the window titled **"Scene"**). |
| `src/app/debug/ComponentDrawers.hpp` / `.cpp` | Per-component `Draw*` functions used by the inspector. |

**Modified files**
| File | Change |
|------|--------|
| `src/engine/scene/Components.hpp` | Add `NameComponent`, `HierarchyComponent`; **remove** `SpawnedEntitiesComponent`, `ParentEntityComponent`. |
| `src/engine/assets/AssetManager.cpp` | Parent spawned mesh entities via `SetParent` instead of `ParentEntityComponent`. |
| `src/engine/animation/AnimationCompiler.cpp` | Read child list from `HierarchyComponent.children`. |
| `src/app/scripting/modules/AnimationModule.cpp` | Read child list from `HierarchyComponent.children`. |
| `src/app/scripting/modules/WorldModule.cpp` | Use `TransformUtils`; parent + auto-name in `load_model`/`create_mesh`/`entity_create`; propagate via `HierarchyComponent.children`; add `set_name`/`get_name`. |
| `src/app/debug/InspectorPanel.hpp` / `.cpp` | Reduce to the editor shell + header; stop drawing the "Scene" window. |
| `src/app/layers/DebugLayer.hpp` / `.cpp` | Own + register `SceneSelection`; register `HierarchyPanel`. |
| `src/app/CMakeLists.txt` | Add the new `.cpp` translation units. |

---

# Phase A — ECS Foundation

## Task 1: Add identity + hierarchy components and helpers

**Files:**
- Modify: `src/engine/scene/Components.hpp`
- Create: `src/engine/scene/Hierarchy.hpp`

- [ ] **Step 1: Add the two components.** In `src/engine/scene/Components.hpp`, add `#include <string>` and `#include "scene/Entity.hpp"` to the include block, then add these structs inside `namespace aether` (near the top, after the `TransformComponent`):

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

> **Migration note:** `ParentEntityComponent` and `SpawnedEntitiesComponent` are **left in place** through Tasks 1–4 so the tree stays green on every commit. They are deleted only in Task 5, once every reader and writer has moved to `HierarchyComponent`. Do not remove them here.

- [ ] **Step 2: Create `src/engine/scene/Hierarchy.hpp`:**

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

- [ ] **Step 3: BUILD** (compiles — additive only; legacy structs still present).
Expected: success.

- [ ] **Step 4: Commit:**
```bash
git add src/engine/scene/Components.hpp src/engine/scene/Hierarchy.hpp
git commit -m "feat(scene): add NameComponent, HierarchyComponent and SetParent helpers"
```

---

## Task 2: Hoist TRS math into a shared header

**Files:**
- Create: `src/engine/scene/TransformUtils.hpp`
- Modify: `src/app/scripting/modules/WorldModule.cpp:26-58` (remove local copies), call sites

- [ ] **Step 1: Create `src/engine/scene/TransformUtils.hpp`** with the exact bodies currently in `WorldModule.cpp`'s anonymous namespace, promoted into `namespace aether`:

```cpp
#pragma once

#include <cmath>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace aether
{
	// Compose a TRS matrix from pos / euler(degrees) / scale — YXZ rotation order.
	inline glm::mat4 ComposeTransform(glm::vec3 pos, glm::vec3 rotEulerDeg, glm::vec3 scale)
	{
		glm::mat4 t = glm::translate(glm::mat4(1.0f), pos);
		glm::mat4 r = glm::rotate(glm::mat4(1.0f), glm::radians(rotEulerDeg.y), glm::vec3(0, 1, 0));
		r = r * glm::rotate(glm::mat4(1.0f), glm::radians(rotEulerDeg.x), glm::vec3(1, 0, 0));
		r = r * glm::rotate(glm::mat4(1.0f), glm::radians(rotEulerDeg.z), glm::vec3(0, 0, 1));
		glm::mat4 s = glm::scale(glm::mat4(1.0f), scale);
		return t * r * s;
	}

	// Extract YXZ euler angles (degrees) + per-axis scale from a TRS matrix.
	inline void DecomposeTRS(const glm::mat4& m, glm::vec3& pos, glm::vec3& eulerDeg, glm::vec3& scale)
	{
		pos = glm::vec3(m[3]);
		float sx = glm::length(glm::vec3(m[0]));
		float sy = glm::length(glm::vec3(m[1]));
		float sz = glm::length(glm::vec3(m[2]));
		scale = {sx, sy, sz};
		glm::vec3 c2 = sz > 1e-6f ? glm::vec3(m[2]) / sz : glm::vec3(0, 0, 1);
		float sinX = glm::clamp(-c2.y, -1.0f, 1.0f);
		float rotXRad = std::asin(sinX);
		float rotYRad = std::atan2(c2.x, c2.z);
		float rotZRad = 0.0f;
		float cosX = std::cos(rotXRad);
		if (std::abs(cosX) > 1e-4f)
		{
			glm::vec3 c0 = sx > 1e-6f ? glm::vec3(m[0]) / sx : glm::vec3(1, 0, 0);
			glm::vec3 c1 = sy > 1e-6f ? glm::vec3(m[1]) / sy : glm::vec3(0, 1, 0);
			rotZRad = std::atan2(c0.y, c1.y);
		}
		eulerDeg = {glm::degrees(rotXRad), glm::degrees(rotYRad), glm::degrees(rotZRad)};
	}
} // namespace aether
```

- [ ] **Step 2: Delete the two functions** from `WorldModule.cpp`'s top anonymous namespace (lines ~26-58) and add `#include "scene/TransformUtils.hpp"` to its includes.

- [ ] **Step 3: Qualify the call sites.** In `WorldModule.cpp`, the anon-namespace helpers were called unqualified. Prefix each with `aether::`:
  - `das_set_euler`: `tc->localToWorld = aether::ComposeTransform(pos, e, scale);` and both `stc->localToWorld = aether::ComposeTransform(...)`.
  - `das_set_transform`: `const auto xform = aether::ComposeTransform(...)`.
  - `das_get_euler` and `das_for_each_with_tag_transform`: `aether::DecomposeTRS(...)`.

- [ ] **Step 4: BUILD.** Expected: success.

- [ ] **Step 5: Commit:**
```bash
git add src/engine/scene/TransformUtils.hpp src/app/scripting/modules/WorldModule.cpp
git commit -m "refactor(scene): hoist TRS compose/decompose into TransformUtils.hpp"
```

---

## Task 3: Populate hierarchy + names at spawn (writers)

**Files:**
- Modify: `src/engine/assets/AssetManager.cpp:~803-806`
- Modify: `src/app/scripting/modules/WorldModule.cpp` (`load_model`, `create_mesh`, `entity_create`)

This task makes the new components **populated** while the legacy ones are still written (transitional — readers migrate in Task 4).

- [ ] **Step 1: Parent spawned mesh entities in AssetManager.** In `AssetManager.cpp`, add `#include "scene/Hierarchy.hpp"`. Replace the `ParentEntityComponent` emplace (~805) — keep it for now, add the hierarchy link beside it:

```cpp
			if (parentEntityId != 0)
			{
				m_world->Emplace<ParentEntityComponent>(entity, ParentEntityComponent{.parentId = parentEntityId});
				aether::ecs::SetParent(*m_world, entity, aether::Entity{parentEntityId});
			}
```

- [ ] **Step 2: Parent + name in `load_model`.** In `WorldModule.cpp`'s `das_load_model`, add `#include "scene/Hierarchy.hpp"` and, in the loop that pushes spawned mesh entities, add the hierarchy link and a name derived from the model path stem:

```cpp
			for (aether::Entity meshEntity: meshEntities)
			{
				if (auto tc = w->TryGet<aether::TransformComponent>(meshEntity))
				{
					tc->localToWorld = xform * tc->localToWorld;
				}
				aether::ecs::SetParent(*w, meshEntity, aether::Entity{id});
				ctx.sceneEntities.push_back(meshEntity);
			}

			// Name the parent entity after the model file stem (e.g. "fox").
			{
				std::string stem = path;
				const auto slash = stem.find_last_of("/\\");
				if (slash != std::string::npos) stem = stem.substr(slash + 1);
				const auto dot = stem.find_last_of('.');
				if (dot != std::string::npos) stem = stem.substr(0, dot);
				w->EmplaceOrReplace<aether::NameComponent>(aether::Entity{id}, aether::NameComponent{.name = stem});
			}
```

- [ ] **Step 3: Name primitives + bare entities.** In `das_create_mesh`, after resolving `primType`, capture a display string and name the entity in `das_add_mesh` (the entity is known there). Simplest: name in `das_add_mesh` from the primitive kind cached on the `CachedMesh`. If `CachedMesh` has no kind field, name generically in `das_add_mesh`:

```cpp
	// in das_add_mesh, after the EmplaceOrReplace calls:
	if (!w->TryGet<aether::NameComponent>(e))
	{
		w->Emplace<aether::NameComponent>(e, aether::NameComponent{.name = "Mesh"});
	}
```
And in `das_entity_create`, give a default name:
```cpp
	uint32_t das_entity_create(aether::World* w)
	{
		aether::Entity e = w->Create();
		w->Emplace<aether::NameComponent>(e, aether::NameComponent{.name = "Entity"});
		ActiveContext().sceneEntities.push_back(e);
		return e.id;
	}
```
> Note: to name primitives precisely ("Cube"/"Sphere"/…), thread the kind string from `das_create_mesh` into the `CachedMesh` entry and copy it in `das_add_mesh`. Optional polish; the generic "Mesh" default is acceptable for Spec 1.

- [ ] **Step 4: BUILD.** Expected: success (legacy + new components both written).

- [ ] **Step 5: Commit:**
```bash
git add src/engine/assets/AssetManager.cpp src/app/scripting/modules/WorldModule.cpp
git commit -m "feat(scene): populate HierarchyComponent + NameComponent at spawn"
```

---

## Task 4: Migrate readers to HierarchyComponent

**Files:**
- Modify: `src/engine/animation/AnimationCompiler.cpp:~26`
- Modify: `src/app/scripting/modules/AnimationModule.cpp:~32-45`
- Modify: `src/app/scripting/modules/WorldModule.cpp` (`das_set_euler`, `das_set_transform` propagation)

- [ ] **Step 1: AnimationCompiler.** Replace the `SpawnedEntitiesComponent` read with a `HierarchyComponent` read:
```cpp
		const auto sec = world.TryGet<HierarchyComponent>(Entity{entityId});
```
and update the subsequent iteration to use `sec->children` (a `std::vector<Entity>`), passing `child` / `child.id` where `entityIds` ids were used. (Add `#include "scene/Components.hpp"` if not already included.)

- [ ] **Step 2: AnimationModule.** In `AnimationModule.cpp`, the two `SpawnedEntitiesComponent` reads become `HierarchyComponent`:
```cpp
		const auto sec = w->TryGet<aether::HierarchyComponent>(e);
		if (sec && !sec->children.empty())
		{
			return w->TryGet<aether::SkinnedMeshComponent>(sec->children.front());
		}
```
and in `ForEachSpawnedSmc`:
```cpp
	void ForEachSpawnedSmc(aether::World* w, uint32_t id, auto&& f)
	{
		const auto sec = w->TryGet<aether::HierarchyComponent>(aether::Entity{id});
		if (!sec)
		{
			return;
		}
		for (const aether::Entity child: sec->children)
		{
			if (auto smc = w->TryGet<aether::SkinnedMeshComponent>(child))
			{
				f(*smc);
			}
		}
	}
```
(Match the existing body shape; the key change is `entityIds` → `children` and `Entity{eid}` → `child`.)

- [ ] **Step 3: WorldModule propagation.** In `das_set_euler` and `das_set_transform`, replace the `SpawnedEntitiesComponent` propagation with `HierarchyComponent.children`:
```cpp
		const auto sec = w->TryGet<aether::HierarchyComponent>(aether::Entity{id});
		if (sec)
		{
			for (const aether::Entity child: sec->children)
			{
				if (auto stc = w->TryGet<aether::TransformComponent>(child))
				{
					stc->localToWorld = aether::ComposeTransform(pos, e, scale); // set_euler
					// (set_transform uses: stc->localToWorld = xform;)
				}
			}
		}
```

- [ ] **Step 4: BUILD.** Expected: success.

- [ ] **Step 5: RUN — regression check.** F1, confirm the **fox still animates** (proves AnimationModule/Compiler still find the skinned child through the hierarchy) and moving objects (plasma light target, toys) still move. No crash.

- [ ] **Step 6: Commit:**
```bash
git add src/engine/animation/AnimationCompiler.cpp src/app/scripting/modules/AnimationModule.cpp src/app/scripting/modules/WorldModule.cpp
git commit -m "refactor(scene): read child links from HierarchyComponent"
```

---

## Task 5: Delete the legacy components (finish the migration)

**Files:**
- Modify: `src/engine/scene/Components.hpp`
- Modify: `src/engine/assets/AssetManager.cpp`, `src/app/scripting/modules/WorldModule.cpp` (remove now-dead legacy writes)

- [ ] **Step 1: Remove dead writes.** Delete the `m_world->Emplace<ParentEntityComponent>(...)` line in `AssetManager.cpp` (keep the `SetParent` call). In `WorldModule.cpp`'s `das_load_model`, delete the `SpawnedEntitiesComponent` block (the `sec->entityIds` bookkeeping at the end) — the hierarchy now carries this.

- [ ] **Step 2: Delete the structs.** Remove `ParentEntityComponent` and `SpawnedEntitiesComponent` from `Components.hpp`.

- [ ] **Step 3: Prove zero references.**
Run: `git grep -n "SpawnedEntitiesComponent\|ParentEntityComponent"`
Expected: **no matches** (outside this plan/spec docs).

- [ ] **Step 4: BUILD.** Expected: success — migration complete, single source of truth.

- [ ] **Step 5: Commit:**
```bash
git add -A
git commit -m "refactor(scene)!: remove SpawnedEntitiesComponent/ParentEntityComponent"
```

---

## Task 6: `.das` name bindings

**Files:**
- Modify: `src/app/scripting/modules/WorldModule.cpp`

- [ ] **Step 1: Add binding functions** (in the anonymous namespace, near `das_entity_create`):
```cpp
	// set_name(world, entity_id, name)
	void das_set_name(aether::World* w, uint32_t id, const char* name)
	{
		w->EmplaceOrReplace<aether::NameComponent>(aether::Entity{id}, aether::NameComponent{.name = name ? name : ""});
	}

	// get_name(world, entity_id) -> string
	char* das_get_name(aether::World* w, uint32_t id, das::Context* ctx)
	{
		const auto nc = w->TryGet<aether::NameComponent>(aether::Entity{id});
		return ctx->stringHeap->allocateString(nc ? nc->name.c_str() : "", nc ? static_cast<uint32_t>(nc->name.size()) : 0u);
	}
```
> Confirm the exact daScript string-return idiom against a sibling binding that returns a string in this codebase; match it. If none exists, expose only `set_name` for Spec 1 and drop `get_name`.

- [ ] **Step 2: Register them** in the `WorldModule` ctor beside the transform binds:
```cpp
				Bind<das_set_name>(lib, "set_name", SE::modifyExternal);
				Bind<das_get_name>(lib, "get_name", SE::accessExternal);
```

- [ ] **Step 3: BUILD.** Expected: success.

- [ ] **Step 4: RUN.** Add a temporary `set_name(world, g_player, "Player")` in `sandbox.das` `on_attach`, F5 to reload, confirm no script error (visual confirmation of the name lands in Task 8). Revert the temporary line.

- [ ] **Step 5: Commit:**
```bash
git add src/app/scripting/modules/WorldModule.cpp
git commit -m "feat(scripting): add set_name/get_name das bindings"
```

---

## Task 7: `SceneSelection` service

**Files:**
- Create: `src/app/debug/SceneSelection.hpp`
- Modify: `src/app/layers/DebugLayer.hpp`, `src/app/layers/DebugLayer.cpp`

- [ ] **Step 1: Create `src/app/debug/SceneSelection.hpp`:**
```cpp
#pragma once

#include <algorithm>
#include <vector>

#include "scene/Entity.hpp"
#include "scene/World.hpp"

namespace aether::app
{
	// Shared editor selection state (multi-select + a "primary" for the inspector).
	// Registered in the ServiceContainer by DebugLayer; read by the outliner,
	// the inspector, and (Spec 2) viewport picking.
	class SceneSelection
	{
	public:
		void Select(Entity e)
		{
			m_selected.clear();
			if (e.IsValid())
			{
				m_selected.push_back(e);
			}
			m_primary = e;
		}

		void AddToSelection(Entity e)
		{
			if (e.IsValid() && !Contains(e))
			{
				m_selected.push_back(e);
			}
			m_primary = e;
		}

		void ToggleSelection(Entity e)
		{
			if (!e.IsValid())
			{
				return;
			}
			const auto it = std::find(m_selected.begin(), m_selected.end(), e);
			if (it != m_selected.end())
			{
				m_selected.erase(it);
				m_primary = m_selected.empty() ? Entity{} : m_selected.back();
			}
			else
			{
				m_selected.push_back(e);
				m_primary = e;
			}
		}

		void Clear()
		{
			m_selected.clear();
			m_primary = {};
		}

		[[nodiscard]] bool Contains(Entity e) const
		{
			return std::find(m_selected.begin(), m_selected.end(), e) != m_selected.end();
		}

		[[nodiscard]] Entity Primary() const
		{
			return m_primary;
		}

		[[nodiscard]] const std::vector<Entity>& All() const
		{
			return m_selected;
		}

		// Drops entities that are no longer alive (call once per frame).
		void Prune(const World& world)
		{
			const auto dead = [&](Entity e) { return !e.IsValid() || !world.GetRegistry().valid(World::ToEntt(e)); };
			std::erase_if(m_selected, dead);
			if (dead(m_primary))
			{
				m_primary = m_selected.empty() ? Entity{} : m_selected.back();
			}
		}

	private:
		std::vector<Entity> m_selected;
		Entity m_primary{};
	};
} // namespace aether::app
```

- [ ] **Step 2: Own + register it in `DebugLayer`.** In `DebugLayer.hpp`, add `#include "debug/SceneSelection.hpp"` and a member `SceneSelection m_selection;`. In `DebugLayer.cpp` `OnAttach` (before creating panels), register it into the container; unregister in `OnDetach`:
```cpp
	// OnAttach, top:
	context.services.Register<SceneSelection>(m_selection);
	// OnDetach, before m_panels.clear():
	context.services.Unregister<SceneSelection>();
```
And in `OnUpdate`, prune once per frame:
```cpp
	m_selection.Prune(context.Get<World>());
```
(Add `#include "scene/World.hpp"` to `DebugLayer.cpp` if needed.)

- [ ] **Step 3: BUILD.** Expected: success (service registered, not yet consumed).

- [ ] **Step 4: Commit:**
```bash
git add src/app/debug/SceneSelection.hpp src/app/layers/DebugLayer.hpp src/app/layers/DebugLayer.cpp
git commit -m "feat(debug): add shared SceneSelection service"
```

---

# Phase B — Entity List (the outliner)

## Task 8: Extract `HierarchyPanel`, flat all-entity list

**Files:**
- Create: `src/app/debug/HierarchyPanel.hpp`, `src/app/debug/HierarchyPanel.cpp`
- Modify: `src/app/debug/InspectorPanel.cpp` (remove the "Scene" window block), `InspectorPanel.hpp`
- Modify: `src/app/layers/DebugLayer.cpp` (register `HierarchyPanel`), `src/app/CMakeLists.txt`

- [ ] **Step 1: Create `HierarchyPanel.hpp`:**
```cpp
#pragma once

#include "debug/DebugPanel.hpp"

namespace aether::app
{
	// The scene outliner. Draws the window titled "Scene" (dock mapping unchanged).
	class HierarchyPanel final : public DebugPanel
	{
	public:
		std::string_view GetName() const override
		{
			return "Scene Outliner";
		}

		void OnImGui(LayerContext& context) override;
	};
} // namespace aether::app
```
> `GetName()` differs from the window title on purpose — `DebugLayer` iterates panels by `GetName()`, and the window title is what `ImGui::Begin` uses. Keep the `Begin("Scene")` title so the saved dock node still matches.

- [ ] **Step 2: Create `HierarchyPanel.cpp`** — flat list of every live entity, names + `#id`, selection through `SceneSelection`:
```cpp
#include "debug/HierarchyPanel.hpp"

#include <string>

#include <entt/entt.hpp>
#include <imgui.h>

#include "debug/SceneSelection.hpp"
#include "layers/AppLayer.hpp"
#include "scene/Components.hpp"
#include "scene/World.hpp"
#include "utils/Profiler.hpp"

namespace aether::app
{
	namespace
	{
		std::string EntityLabel(const World& world, Entity e)
		{
			const auto* nc = world.TryGet<NameComponent>(e);
			const std::string name = (nc && !nc->name.empty()) ? nc->name : std::string("Entity");
			return name + "  ##" + std::to_string(e.id);
		}
	} // namespace

	void HierarchyPanel::OnImGui(LayerContext& context)
	{
		AE_PROFILE_ZONE();
		World& world = context.Get<World>();
		auto& selection = context.Get<SceneSelection>();
		auto& reg = world.GetRegistry();

		ImGui::Begin("Scene");
		{
			std::size_t count = 0;
			for (const auto handle: reg.storage<entt::entity>())
			{
				if (reg.valid(handle))
				{
					++count;
				}
			}
			ImGui::Text("%zu entities", count);

			ImGui::BeginChild("SceneList", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
			for (const auto handle: reg.storage<entt::entity>())
			{
				if (!reg.valid(handle))
				{
					continue;
				}
				const Entity e = World::FromEntt(handle);
				const bool selected = selection.Contains(e);
				const std::string label = std::string("#") + std::to_string(e.id) + "  " + EntityLabel(world, e);
				if (ImGui::Selectable(label.c_str(), selected))
				{
					selection.Select(e);
				}
			}
			ImGui::EndChild();
		}
		ImGui::End();
	}
} // namespace aether::app
```

- [ ] **Step 3: Remove the "Scene" window from `InspectorPanel`.** Delete the entire `ImGui::Begin("Scene") { … } ImGui::End();` block from `InspectorPanel::OnImGui` and the `CollectSceneEntities`/`SceneEntityLabel`/`kMaxSceneRows` members it used. Change the inspector's selection source from `m_selectedSceneEntity` to `context.Get<SceneSelection>().Primary()` (full editor comes in Phase C; for now just read the primary). Remove `m_selectedSceneEntity` from `InspectorPanel.hpp`.

- [ ] **Step 4: Register the panel + add to build.** In `DebugLayer.cpp` add `#include "debug/HierarchyPanel.hpp"` and `m_panels.push_back(std::make_unique<HierarchyPanel>());` (near the InspectorPanel push). In `src/app/CMakeLists.txt`, add `debug/HierarchyPanel.cpp` to the `App` sources list (match how the other `debug/*.cpp` files are listed).

- [ ] **Step 5: BUILD.** Expected: success.

- [ ] **Step 6: RUN.** F1 → the **Scene** window now lists **every** entity (well past the old 80 cap), each showing `#id Name`. Clicking one selects it; the Inspector reflects the same primary. The fox parent shows its model-stem name.

- [ ] **Step 7: Commit:**
```bash
git add src/app/debug/HierarchyPanel.hpp src/app/debug/HierarchyPanel.cpp src/app/debug/InspectorPanel.hpp src/app/debug/InspectorPanel.cpp src/app/layers/DebugLayer.cpp src/app/CMakeLists.txt
git commit -m "feat(debug): extract HierarchyPanel with uncapped all-entity list"
```

---

## Task 9: Tree rendering + type badges

**Files:**
- Modify: `src/app/debug/HierarchyPanel.cpp`

- [ ] **Step 1: Add a badge helper** (anonymous namespace in `HierarchyPanel.cpp`). Include `Color.hpp` and `physics/PhysicsComponents.hpp`:
```cpp
		// Returns a short bracket-tag + color describing the entity's dominant kind.
		struct Badge { const char* tag; ImVec4 color; };
		Badge KindBadge(const World& world, Entity e)
		{
			if (world.Has<SkinnedMeshComponent>(e)) return {"[S]", {0.55f, 0.75f, 1.0f, 1.0f}};
			if (world.Has<RigidBodyComponent>(e))   return {"[P]", {1.0f, 0.72f, 0.35f, 1.0f}};
			if (world.Has<MeshComponent>(e))         return {"[M]", {0.65f, 0.9f, 0.65f, 1.0f}};
			return {"[ ]", {0.6f, 0.6f, 0.6f, 1.0f}};
		}
```
(Add `#include "physics/PhysicsComponents.hpp"` for `RigidBodyComponent`.)

- [ ] **Step 2: Replace the flat loop with a recursive tree.** Add a recursive draw function and drive it from roots:
```cpp
		void DrawNode(World& world, SceneSelection& selection, Entity e)
		{
			const auto* h = world.TryGet<HierarchyComponent>(e);
			const bool hasKids = h && !h->children.empty();

			ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
			if (selection.Contains(e)) flags |= ImGuiTreeNodeFlags_Selected;
			if (!hasKids) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;

			const Badge b = KindBadge(world, e);
			ImGui::PushID(static_cast<int>(e.id));
			ImGui::TextColored(b.color, "%s", b.tag);
			ImGui::SameLine();
			const bool open = ImGui::TreeNodeEx("node", flags, "%s  #%u",
				world.TryGet<NameComponent>(e) && !world.Get<NameComponent>(e).name.empty()
					? world.Get<NameComponent>(e).name.c_str() : "Entity", e.id);
			if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
			{
				selection.Select(e);
			}
			if (open && hasKids)
			{
				for (const Entity c: h->children) DrawNode(world, selection, c);
				ImGui::TreePop();
			}
			ImGui::PopID();
		}
```
In `OnImGui`, replace the `Selectable` loop with: iterate all valid entities, and for each **root** (no `HierarchyComponent` or `parent == {0}`) call `DrawNode`:
```cpp
			for (const auto handle: reg.storage<entt::entity>())
			{
				if (!reg.valid(handle)) continue;
				const Entity e = World::FromEntt(handle);
				const auto* h = world.TryGet<HierarchyComponent>(e);
				if (!h || !h->parent.IsValid())
				{
					DrawNode(world, selection, e);
				}
			}
```

- [ ] **Step 3: BUILD.** Expected: success.

- [ ] **Step 4: RUN.** The fox's spawned mesh children now nest under the fox node; badges show `[S]`/`[M]`/`[P]` in color; expand/collapse works; clicking a row (not the arrow) selects it.

- [ ] **Step 5: Commit:**
```bash
git add src/app/debug/HierarchyPanel.cpp
git commit -m "feat(debug): render entity hierarchy as a tree with type badges"
```

---

## Task 10: Search/filter, count, create menu

**Files:**
- Modify: `src/app/debug/HierarchyPanel.hpp`, `src/app/debug/HierarchyPanel.cpp`

- [ ] **Step 1: Add state** to `HierarchyPanel.hpp`: `char m_search[64] = {};`.

- [ ] **Step 2: Toolbar.** At the top of `OnImGui` (inside `Begin("Scene")`), draw a `+` create menu and a search box:
```cpp
			if (ImGui::Button("+"))
			{
				ImGui::OpenPopup("CreateEntity");
			}
			if (ImGui::BeginPopup("CreateEntity"))
			{
				if (ImGui::MenuItem("Empty entity"))
				{
					Entity e = world.Create();
					world.Emplace<NameComponent>(e, NameComponent{.name = "Entity"});
					selection.Select(e);
				}
				ImGui::EndPopup();
			}
			ImGui::SameLine();
			ImGui::SetNextItemWidth(-FLT_MIN);
			ImGui::InputTextWithHint("##search", "Search…", m_search, sizeof(m_search));
```

- [ ] **Step 3: Filter mode.** When `m_search[0] != '\0'`, skip the tree and draw a flat, filtered list (case-insensitive substring match on name or `#id`), each row a `Selectable` like Task 8. Otherwise draw the tree (Task 9). Add a small `std::string` lowercase helper in the anon namespace.

- [ ] **Step 4: BUILD.** Expected: success.

- [ ] **Step 5: RUN.** Typing `fox` filters to matching rows; clearing restores the tree; `+ → Empty entity` adds a selectable "Entity".

- [ ] **Step 6: Commit:**
```bash
git add src/app/debug/HierarchyPanel.hpp src/app/debug/HierarchyPanel.cpp
git commit -m "feat(debug): outliner search filter + create-entity menu"
```

---

## Task 11: Multi-select, context menu, keyboard

**Files:**
- Modify: `src/app/debug/HierarchyPanel.cpp`

- [ ] **Step 1: Ctrl-select.** In the tree/flat click handlers, branch on `ImGui::GetIO().KeyCtrl`:
```cpp
			if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
			{
				if (ImGui::GetIO().KeyCtrl) selection.ToggleSelection(e);
				else selection.Select(e);
			}
```
(Shift-range is optional for Spec 1; Ctrl covers the multi-select need for bulk ops.)

- [ ] **Step 2: Per-node context menu.** Inside `DrawNode`, after the tree node, add:
```cpp
			if (ImGui::BeginPopupContextItem("ctx"))
			{
				if (ImGui::MenuItem("Rename")) { selection.Select(e); /* F2 flow below */ }
				if (ImGui::MenuItem("Create child"))
				{
					Entity child = world.Create();
					world.Emplace<NameComponent>(child, NameComponent{.name = "Entity"});
					ecs::SetParent(world, child, e);
					selection.Select(child);
				}
				ImGui::Separator();
				if (ImGui::MenuItem("Delete"))
				{
					ecs::DestroyHierarchy(world, e);
					selection.Clear();
				}
				ImGui::EndPopup();
			}
```
(Add `#include "scene/Hierarchy.hpp"`.)

- [ ] **Step 3: Inline rename.** Add `Entity m_renaming{}; char m_renameBuf[64] = {};` to the header. When `Rename` is chosen (or `F2` with a primary), set `m_renaming = e` and seed the buffer from the name. While `m_renaming == e`, draw an `InputText` in place of the label; commit on Enter/focus-loss into `NameComponent`, then clear `m_renaming`.

- [ ] **Step 4: Delete/F2 keys.** After the tree, when the "Scene" window is focused:
```cpp
			if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
			{
				if (ImGui::IsKeyPressed(ImGuiKey_Delete))
				{
					for (const Entity e: selection.All()) ecs::DestroyHierarchy(world, e);
					selection.Clear();
				}
				if (ImGui::IsKeyPressed(ImGuiKey_F2) && selection.Primary().IsValid())
				{
					m_renaming = selection.Primary();
					// seed m_renameBuf from the name
				}
			}
```
> Deleting while iterating the registry: collect the selection into a local vector before destroying, since `DestroyHierarchy` mutates storage.

- [ ] **Step 5: BUILD.** Expected: success.

- [ ] **Step 6: RUN.** Ctrl-click selects multiple; right-click → Create child / Delete works; Delete key removes the selection (and its subtree); F2 / context Rename edits the name inline and it sticks in the tree.

- [ ] **Step 7: Commit:**
```bash
git add src/app/debug/HierarchyPanel.hpp src/app/debug/HierarchyPanel.cpp
git commit -m "feat(debug): outliner multi-select, context menu, rename + delete keys"
```

---

## Task 12: Drag-drop reparent

**Files:**
- Modify: `src/app/debug/HierarchyPanel.cpp`

- [ ] **Step 1: Drag source + drop target** inside `DrawNode`, right after the tree node is drawn:
```cpp
			if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None))
			{
				ImGui::SetDragDropPayload("AETHER_ENTITY", &e.id, sizeof(e.id));
				ImGui::Text("Move %s", world.TryGet<NameComponent>(e) ? world.Get<NameComponent>(e).name.c_str() : "entity");
				ImGui::EndDragDropSource();
			}
			if (ImGui::BeginDragDropTarget())
			{
				if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("AETHER_ENTITY"))
				{
					const std::uint32_t draggedId = *static_cast<const std::uint32_t*>(p->Data);
					ecs::SetParent(world, Entity{draggedId}, e); // cycle-guarded; no-op if invalid
				}
				ImGui::EndDragDropTarget();
			}
```

- [ ] **Step 2: Drop-on-empty to unparent.** After the root loop, add an invisible full-child drop target that calls `ecs::SetParent(world, Entity{draggedId}, Entity{})` (detach to root).

- [ ] **Step 3: BUILD.** Expected: success.

- [ ] **Step 4: RUN.** Drag one entity onto another → it nests; drag a child onto empty space → it returns to root; dragging a parent onto its own descendant is silently rejected (cycle guard) — no crash, no broken tree.

- [ ] **Step 5: Commit:**
```bash
git add src/app/debug/HierarchyPanel.cpp
git commit -m "feat(debug): drag-drop reparenting in the outliner"
```

---

# Phase C — Inspector

## Task 13: Inspector shell + header + drawer scaffold

**Files:**
- Create: `src/app/debug/ComponentDrawers.hpp`, `src/app/debug/ComponentDrawers.cpp`
- Modify: `src/app/debug/InspectorPanel.cpp`, `src/app/CMakeLists.txt`

- [ ] **Step 1: Create `ComponentDrawers.hpp`:**
```cpp
#pragma once

namespace aether { class World; struct Entity; }

namespace aether::app
{
	struct LayerContext;

	// Each draws one CollapsingHeader section for `entity` if that component is present.
	void DrawTransform(LayerContext& context, World& world, Entity entity);
	void DrawSkinnedMesh(World& world, Entity entity);
	void DrawMaterial(World& world, Entity entity);   // read-only (material system mid-rewrite)
	void DrawPhysics(World& world, Entity entity);
	void DrawMeshPipeline(World& world, Entity entity);
	void DrawHierarchy(World& world, Entity entity, class SceneSelection& selection);
	void DrawTags(World& world, Entity entity);
} // namespace aether::app
```

- [ ] **Step 2: Create `ComponentDrawers.cpp`** with empty-but-guarded stubs that compile (each `TryGet`s its component and early-returns if absent). Real bodies land in Tasks 14–17. Example stub:
```cpp
#include "debug/ComponentDrawers.hpp"
#include <imgui.h>
#include "layers/AppLayer.hpp"
#include "scene/Components.hpp"
#include "scene/World.hpp"

namespace aether::app
{
	void DrawSkinnedMesh(World& world, Entity entity) { (void)world; (void)entity; }
	// … one stub per declared function …
}
```

- [ ] **Step 3: Rewrite `InspectorPanel::OnImGui`** as the editor shell reading the primary selection:
```cpp
	void InspectorPanel::OnImGui(LayerContext& context)
	{
		AE_PROFILE_ZONE();
		World& world = context.Get<World>();
		auto& selection = context.Get<SceneSelection>();
		const Entity e = selection.Primary();

		ImGui::Begin("Inspector");
		if (!e.IsValid() || !world.GetRegistry().valid(World::ToEntt(e)))
		{
			ImGui::TextDisabled("No selection");
			ImGui::End();
			return;
		}

		// Header: editable name + id
		if (auto* nc = world.TryGet<NameComponent>(e))
		{
			char buf[64];
			std::snprintf(buf, sizeof(buf), "%s", nc->name.c_str());
			ImGui::SetNextItemWidth(-60.0f);
			if (ImGui::InputText("##name", buf, sizeof(buf)))
			{
				nc->name = buf;
			}
		}
		else if (ImGui::SmallButton("Add name"))
		{
			world.Emplace<NameComponent>(e, NameComponent{.name = "Entity"});
		}
		ImGui::SameLine();
		ImGui::TextDisabled("#%u", e.id);

		if (selection.All().size() > 1)
		{
			ImGui::TextDisabled("Editing primary of %zu selected", selection.All().size());
		}
		ImGui::Separator();

		DrawTransform(context, world, e);
		DrawSkinnedMesh(world, e);
		DrawMaterial(world, e);
		DrawPhysics(world, e);
		DrawMeshPipeline(world, e);
		DrawHierarchy(world, e, selection);
		DrawTags(world, e);

		ImGui::End();
	}
```
Add includes for `SceneSelection.hpp`, `ComponentDrawers.hpp`, `<cstdio>`. Drop the now-unused physics/animation includes the old body used if they're no longer referenced.

- [ ] **Step 4: Add `ComponentDrawers.cpp` to `src/app/CMakeLists.txt`.**

- [ ] **Step 5: BUILD.** Expected: success.

- [ ] **Step 6: RUN.** Selecting an entity shows an editable name field + `#id`; multi-select shows the "primary of N" note. Sections are empty (stubs) — fleshed out next.

- [ ] **Step 7: Commit:**
```bash
git add src/app/debug/ComponentDrawers.hpp src/app/debug/ComponentDrawers.cpp src/app/debug/InspectorPanel.cpp src/app/CMakeLists.txt
git commit -m "feat(debug): inspector editor shell with editable name header"
```

---

## Task 14: Transform drawer (physics-aware)

**Files:**
- Modify: `src/app/debug/ComponentDrawers.cpp`

- [ ] **Step 1: Implement `DrawTransform`:**
```cpp
	void DrawTransform(LayerContext& context, World& world, Entity entity)
	{
		auto* tc = world.TryGet<TransformComponent>(entity);
		if (!tc)
		{
			return;
		}
		if (!ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen))
		{
			return;
		}

		glm::vec3 pos{}, euler{}, scale{};
		DecomposeTRS(tc->localToWorld, pos, euler, scale);
		bool changed = false;
		changed |= ImGui::DragFloat3("Position", &pos.x, 0.05f);
		changed |= ImGui::DragFloat3("Rotation", &euler.x, 0.5f);
		changed |= ImGui::DragFloat3("Scale", &scale.x, 0.02f, 0.001f, 1000.0f);

		if (changed)
		{
			tc->localToWorld = ComposeTransform(pos, euler, scale);
			// Propagate to children like set_transform does.
			if (const auto* h = world.TryGet<HierarchyComponent>(entity))
			{
				for (const Entity c: h->children)
				{
					if (auto* stc = world.TryGet<TransformComponent>(c))
					{
						stc->localToWorld = tc->localToWorld;
					}
				}
			}
			// Physics-aware teleport so the body doesn't stomp the edit next frame.
			if (auto* ps = world.TryGet<PhysicsStateComponent>(entity))
			{
				ps->prevPosition = pos;
				ps->currPosition = pos;
			}
		}
	}
```
Add includes: `scene/TransformUtils.hpp`, `scene/Hierarchy.hpp` (for `HierarchyComponent`; it's in Components.hpp already), `physics/PhysicsComponents.hpp`.

- [ ] **Step 2: BUILD.** Expected: success.

- [ ] **Step 3: RUN.** Select a **static wall/floor** → drag Position/Rotation/Scale, it moves live and stays. Select a **physics toy** (dynamic body) → drag Position; it teleports and does **not** snap back next frame. Fox children follow the fox's transform edits.

- [ ] **Step 4: Commit:**
```bash
git add src/app/debug/ComponentDrawers.cpp
git commit -m "feat(debug): editable physics-aware transform section"
```

---

## Task 15: Skinned-mesh, hierarchy, tags drawers

**Files:**
- Modify: `src/app/debug/ComponentDrawers.cpp`

- [ ] **Step 1: `DrawSkinnedMesh`:**
```cpp
	void DrawSkinnedMesh(World& world, Entity entity)
	{
		auto* smc = world.TryGet<SkinnedMeshComponent>(entity);
		if (!smc || !ImGui::CollapsingHeader("Skinned Mesh"))
		{
			return;
		}
		int clip = static_cast<int>(smc->clipIndex);
		if (ImGui::InputInt("Clip", &clip)) smc->clipIndex = static_cast<std::uint32_t>(std::max(0, clip));
		ImGui::DragFloat("Speed", &smc->playbackSpeed, 0.01f, -4.0f, 4.0f);
		ImGui::DragFloat("Time", &smc->animTime, 0.01f, 0.0f, 1000.0f);
		ImGui::Checkbox("Looping", &smc->looping);
	}
```

- [ ] **Step 2: `DrawHierarchy`** (parent/children navigation + unparent):
```cpp
	void DrawHierarchy(World& world, Entity entity, SceneSelection& selection)
	{
		auto* h = world.TryGet<HierarchyComponent>(entity);
		if (!h || !ImGui::CollapsingHeader("Hierarchy"))
		{
			return;
		}
		if (h->parent.IsValid())
		{
			ImGui::Text("Parent:");
			ImGui::SameLine();
			if (ImGui::SmallButton((std::string("#") + std::to_string(h->parent.id)).c_str()))
			{
				selection.Select(h->parent);
			}
			ImGui::SameLine();
			if (ImGui::SmallButton("Unparent"))
			{
				ecs::SetParent(world, entity, Entity{});
			}
		}
		else
		{
			ImGui::TextDisabled("Root");
		}
		ImGui::Text("Children: %zu", h->children.size());
		for (const Entity c: h->children)
		{
			if (ImGui::SmallButton((std::string("#") + std::to_string(c.id)).c_str()))
			{
				selection.Select(c);
			}
			ImGui::SameLine();
		}
		ImGui::NewLine();
	}
```
(Add `#include "debug/SceneSelection.hpp"`, `#include "scene/Hierarchy.hpp"`, `<string>`.)

- [ ] **Step 3: `DrawTags`** using the existing `TagSlots` API. Confirm the query/list signatures in `scene/TagSlots.hpp`; wire "add tag by name" (`TagGetId`/`TagCreate` + `TagAdd`) and a remove per listed tag (`TagRemove`). Keep it a single collapsing section.

- [ ] **Step 4: BUILD.** Expected: success.

- [ ] **Step 5: RUN.** Fox: Skinned Mesh section scrubs the animation (drag Time / change Speed visibly changes the pose). Hierarchy section jumps selection to parent/children. Tags add/remove reflected.

- [ ] **Step 6: Commit:**
```bash
git add src/app/debug/ComponentDrawers.cpp
git commit -m "feat(debug): skinned-mesh, hierarchy, tag inspector sections"
```

---

## Task 16: Read-only material / mesh / physics sections

**Files:**
- Modify: `src/app/debug/ComponentDrawers.cpp`

> **Material is read-only for Spec 1** — the material system is mid-rewrite, so no live-edit path. Show values only.

- [ ] **Step 1: `DrawMaterial` (read-only):**
```cpp
	void DrawMaterial(World& world, Entity entity)
	{
		const auto* mc = world.TryGet<MaterialComponent>(entity);
		if (!mc || !ImGui::CollapsingHeader("Material"))
		{
			return;
		}
		const Material& m = mc->material;
		ImGui::ColorButton("Base", ImVec4(m.baseColorFactor.r, m.baseColorFactor.g, m.baseColorFactor.b, m.baseColorFactor.a));
		ImGui::SameLine();
		ImGui::Text("Base color");
		ImGui::Text("Metallic %.2f   Roughness %.2f", m.metallicFactor, m.roughnessFactor);
		ImGui::Text("Emissive %.2f %.2f %.2f", m.emissiveFactor.r, m.emissiveFactor.g, m.emissiveFactor.b);
		ImGui::TextDisabled("Editing disabled (material system rewrite in progress)");
	}
```

- [ ] **Step 2: `DrawMeshPipeline` (read-only identity):**
```cpp
	void DrawMeshPipeline(World& world, Entity entity)
	{
		const bool hasMesh = world.Has<MeshComponent>(entity);
		const bool hasPipe = world.Has<PipelineComponent>(entity);
		if ((!hasMesh && !hasPipe) || !ImGui::CollapsingHeader("Render"))
		{
			return;
		}
		ImGui::Text("Mesh: %s", hasMesh ? "present" : "none");
		ImGui::Text("Pipeline: %s", hasPipe ? "present" : "none");
		ImGui::TextDisabled("Asset swapping: later spec");
	}
```

- [ ] **Step 3: `DrawPhysics`** (state + read-only motion type; teleport already handled in Transform):
```cpp
	void DrawPhysics(World& world, Entity entity)
	{
		const auto* rb = world.TryGet<RigidBodyComponent>(entity);
		const auto* ps = world.TryGet<PhysicsStateComponent>(entity);
		if ((!rb && !ps) || !ImGui::CollapsingHeader("Physics"))
		{
			return;
		}
		if (rb)
		{
			const char* motion = rb->motionType == PhysicsMotionType::Static ? "Static"
				: rb->motionType == PhysicsMotionType::Kinematic ? "Kinematic" : "Dynamic";
			ImGui::Text("Motion: %s", motion);
			ImGui::TextDisabled("(motion/shape changes need body rebuild — out of scope)");
		}
		if (ps)
		{
			ImGui::Text("Pos: %.2f %.2f %.2f", ps->currPosition.x, ps->currPosition.y, ps->currPosition.z);
			ImGui::Text("Scale: %.2f %.2f %.2f", ps->scale.x, ps->scale.y, ps->scale.z);
		}
	}
```
(Add `#include "material/Material.hpp"`, `#include "physics/PhysicsComponents.hpp"` if not already included.)

- [ ] **Step 4: BUILD.** Expected: success.

- [ ] **Step 5: RUN.** Material shows the base-color swatch + values with the "editing disabled" note; Render shows mesh/pipeline presence; Physics shows motion type + live position on a toy.

- [ ] **Step 6: Commit:**
```bash
git add src/app/debug/ComponentDrawers.cpp
git commit -m "feat(debug): read-only material/render/physics inspector sections"
```

---

## Task 17: Add/remove component menu + final pass

**Files:**
- Modify: `src/app/debug/InspectorPanel.cpp`

- [ ] **Step 1: Add-component menu** in the header (after the name row):
```cpp
		if (ImGui::Button("Add Component"))
		{
			ImGui::OpenPopup("AddComponent");
		}
		if (ImGui::BeginPopup("AddComponent"))
		{
			if (!world.Has<TransformComponent>(e) && ImGui::MenuItem("Transform"))
			{
				world.Emplace<TransformComponent>(e);
			}
			if (!world.Has<HierarchyComponent>(e) && ImGui::MenuItem("Hierarchy"))
			{
				world.Emplace<HierarchyComponent>(e);
			}
			if (!world.Has<NameComponent>(e) && ImGui::MenuItem("Name"))
			{
				world.Emplace<NameComponent>(e, NameComponent{.name = "Entity"});
			}
			ImGui::EndPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Delete entity"))
		{
			ecs::DestroyHierarchy(world, e);
			selection.Clear();
			ImGui::End();
			return;
		}
```
(Add `#include "scene/Hierarchy.hpp"`.) Asset-bearing components (Mesh/Pipeline/Material) are intentionally excluded — they need an asset picker.

- [ ] **Step 2: Remove-component affordance.** For safely-removable sections (e.g. Hierarchy, Name), add a right-aligned `SmallButton("x")` on the collapsing header row that calls `world.Remove<T>(e)`. (Do not offer remove for Transform on entities the renderer/physics rely on — keep the menu conservative.)

- [ ] **Step 3: BUILD.** Expected: success.

- [ ] **Step 4: RUN — full acceptance pass:**
  - Outliner: tree with names + badges, search filters, `+` creates, Ctrl multi-select, right-click create-child/delete, F2 rename, drag-drop reparent (cycle-guarded), no 80-cap.
  - Inspector: name edit; Transform edits a static prop and teleports a physics body; skinned-mesh scrub on the fox; hierarchy nav; tags; read-only material/render/physics; Add Component adds Transform/Hierarchy/Name; Delete entity removes the subtree.
  - Reload (`F5`) — no crash, scene rebuilds, hierarchy/names re-populate.

- [ ] **Step 5: Commit:**
```bash
git add src/app/debug/InspectorPanel.cpp
git commit -m "feat(debug): add/remove component menu and inspector polish"
```

---

## Self-Review notes (author)

- **Spec coverage:** §4.1 Name → T1/T3/T6/T13; §4.2 Hierarchy+SetParent → T1; §4.3 full migration → T3–T5; §4.4 SceneSelection → T7; §5 outliner (tree/search/badges/create/rename/delete/reparent/multi-select/no-cap) → T8–T12; §6 inspector (header/transform/skinned/material/physics/mesh/hierarchy/tags/add-remove/multi-select note) → T13–T17; §7 TransformUtils hoist + file split → T2/T8/T13. All spec sections map to tasks.
- **Deviation from spec (approved by user):** material is **read-only** (system mid-rewrite) rather than live-edited; the spec's live-edit path is dropped. Reflected in T16.
- **Type consistency:** `SetParent`/`DetachFromParent`/`DestroyHierarchy`/`IsAncestor` (Hierarchy.hpp) used verbatim across T3–T17; `SceneSelection::{Select,AddToSelection,ToggleSelection,Clear,Contains,Primary,All,Prune}` consistent T7→T17; `ComposeTransform`/`DecomposeTRS` (aether::) consistent T2→T14; `HierarchyComponent.children` (vector<Entity>) consistent everywhere.
- **Known confirmations deferred to execution (flagged inline, not placeholders):** daScript string-return idiom for `get_name` (T6 Step 1); `TagSlots` list/query signatures (T15 Step 3); exact `src/app/CMakeLists.txt` source-list style.
