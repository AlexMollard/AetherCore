#pragma once

#include <cstddef>

#include <glm/glm.hpp>
#include <imgui.h>

namespace aether
{
	class World;
	struct Entity;
} // namespace aether

namespace aether::app
{
	struct LayerContext;
	class SceneSelection;

	// ── Shared entity-row helpers (outliner rows + inspector header) ──────────

	// NameComponent text, or "Entity" when unnamed/empty.
	const char* EntityDisplayName(const World& world, Entity entity);

	// Colored icon describing the entity's dominant kind (most specific wins).
	struct KindBadge
	{
		const char* icon;
		ImVec4 color;
	};
	KindBadge EntityKindBadge(const World& world, Entity entity);

	// Writes a full world transform to the entity with the editor's shared edit
	// semantics: the subtree follows with the same world-space delta (children
	// keep their relative offsets - ecs::SetWorldTransform), and every moved
	// body gets a true teleport (Jolt SetPosition/SetRotation + prev==curr
	// interpolation state). The inspector transform drawer and the viewport
	// gizmo both funnel through here; the future undo spec hooks this.
	void ApplyWorldTransform(LayerContext& context, World& world, Entity entity, const glm::mat4& localToWorld);

	// ── Inspector component sections ───────────────────────────────────────────
	// Each draws one collapsing section for `entity` when the matching component
	// is present (guarded no-op otherwise). Called in order by InspectorPanel.

	void DrawTransform(LayerContext& context, World& world, Entity entity);
	void DrawSkinnedMesh(World& world, Entity entity);
	void DrawMaterial(LayerContext& context, World& world, Entity entity);
	void DrawEffectParams(LayerContext& context, World& world, Entity entity);
	// One section per present behavior component (Bob/Spin/Orbit/MaterialPulse),
	// each live-editable and removable.
	void DrawBehaviors(World& world, Entity entity);
	// Point/spot light sections, live-editable and removable (LightSystem
	// republishes to the renderer every frame, edit mode included).
	void DrawLights(World& world, Entity entity);
	// Entity script section: type/path, attach state, and (for C# scripts) the
	// inspector-exposed, per-entity serialized properties. Removable.
	void DrawScript(LayerContext& context, World& world, Entity entity);
	// Scene-transient marker (excluded from captures), removable.
	void DrawSceneTransient(World& world, Entity entity);
	void DrawPhysics(World& world, Entity entity);
	void DrawMeshPipeline(World& world, Entity entity);
	void DrawHierarchy(World& world, Entity entity, SceneSelection& selection);
	// addTagBuf: caller-owned input buffer for the add-by-name field (the
	// drawers are stateless free functions; InspectorPanel owns the state).
	void DrawTags(World& world, Entity entity, char* addTagBuf, std::size_t addTagBufSize);
} // namespace aether::app
