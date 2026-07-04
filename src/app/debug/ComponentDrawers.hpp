#pragma once

#include <cstddef>

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

	// ── Inspector component sections ───────────────────────────────────────────
	// Each draws one collapsing section for `entity` when the matching component
	// is present (guarded no-op otherwise). Called in order by InspectorPanel.

	void DrawTransform(LayerContext& context, World& world, Entity entity);
	void DrawSkinnedMesh(World& world, Entity entity);
	void DrawMaterial(LayerContext& context, World& world, Entity entity);
	void DrawEffectParams(LayerContext& context, World& world, Entity entity);
	void DrawPhysics(World& world, Entity entity);
	void DrawMeshPipeline(World& world, Entity entity);
	void DrawHierarchy(World& world, Entity entity, SceneSelection& selection);
	// addTagBuf: caller-owned input buffer for the add-by-name field (the
	// drawers are stateless free functions; InspectorPanel owns the state).
	void DrawTags(World& world, Entity entity, char* addTagBuf, std::size_t addTagBufSize);
} // namespace aether::app
