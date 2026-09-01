#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include <glm/glm.hpp>
#include <imgui.h>

#include "debug/InspectorWidgets.hpp"

namespace aether
{
	class World;
	class ServiceContainer;
	struct Entity;
} // namespace aether

namespace aether::app
{
	struct LayerContext;
}

namespace aether::editor
{
	// Component copy/paste. The clipboard is editor-global on purpose: copying values onto a
	// different entity is the whole point of it.
	void PasteComponentInto(ServiceContainer& services, World& world, Entity entity);
	[[nodiscard]] std::string_view CopiedComponentName();

	// Builds the right-click target for a component's section header. Two overloads so the
	// bespoke drawers (which hold a LayerContext) and the reflected one (which holds only the
	// service container) can both use it.
	[[nodiscard]] iw::ComponentMenuTarget MenuFor(ServiceContainer& services, World& world, Entity entity, const char* component);
	[[nodiscard]] iw::ComponentMenuTarget MenuFor(app::LayerContext& context, World& world, Entity entity, const char* component);

	class SceneSelection;

	const char* EntityDisplayName(const World& world, Entity entity);

	struct KindBadge
	{
		const char* icon;
		ImVec4 color;
	};

	KindBadge EntityKindBadge(const World& world, Entity entity);

	void ApplyWorldTransform(app::LayerContext& context, World& world, Entity entity, const glm::mat4& localToWorld);

	// a group edit (inspector transform drawer or viewport gizmo) must not move it
	[[nodiscard]] bool HasSelectedAncestor(const World& world, Entity e, const SceneSelection& selection);

	void DrawTransform(app::LayerContext& context, World& world, Entity entity);
	void DrawSkinnedMesh(World& world, Entity entity);
	void DrawMaterial(app::LayerContext& context, World& world, Entity entity);
	void DrawEffectParams(app::LayerContext& context, World& world, Entity entity);
	void DrawUiCanvas(app::LayerContext& context, World& world, Entity entity);
	void DrawUiRect(app::LayerContext& context, World& world, Entity entity);
	void DrawUiImage(app::LayerContext& context, World& world, Entity entity);
	void DrawUiText(app::LayerContext& context, World& world, Entity entity);
	void DrawCamera(app::LayerContext& context, World& world, Entity entity);

	// Snapshot a component into history before a bespoke drawer's X removes it.
	void RecordComponentRemoval(app::LayerContext& context, World& world, Entity entity, const char* componentName);
	void AddScriptToEntity(World& world, Entity entity, std::string typeName = {});
	bool AcceptScriptDropOnEntity(World& world, Entity entity);
	void DrawScript(app::LayerContext& context, World& world, Entity entity);
	void DrawSceneTransient(World& world, Entity entity);
	void DrawPhysics(app::LayerContext& context, World& world, Entity entity);
	// Spatial tools for the reflected Collider 2D section (sprite-outline -> polygon).
	void DrawCollider2DTools(app::LayerContext& context, World& world, Entity entity);
	void DrawCollisionEvents(World& world, Entity entity);
	void DrawJoint(app::LayerContext& context, World& world, Entity entity);
	void DrawMeshRenderer(app::LayerContext& context, World& world, Entity entity);
	void DrawSpriteRenderer(app::LayerContext& context, World& world, Entity entity);
	void DrawSpriteAnimator(app::LayerContext& context, World& world, Entity entity);
	void DrawHierarchy(World& world, Entity entity, SceneSelection& selection);
	void DrawTags(World& world, Entity entity, char* addTagBuf, std::size_t addTagBufSize);
} // namespace aether::editor
