#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "scene/SceneKind.hpp"

namespace aether
{
	class World;
	struct Entity;
	class ServiceContainer;
} // namespace aether

namespace aether::editor
{
	// add_component / remove_component drive it, so the two never drift. Adding a
	struct ComponentCatalogEntry
	{
		std::string name;
		std::string category;
		std::string icon;

		std::function<bool(const World&, Entity)> has;
		std::function<void(World&, Entity, ServiceContainer&)> add;
		std::function<void(World&, Entity)> remove;

		// (Create > UI > Text), never added as a loose component. The Add-Component
		bool addable = true;

		// Scene features the component depends on: adding it auto-enables them
		// when AllowedSceneFeatures(kind) permits, otherwise the add is blocked.
		SceneFeatureFlags requiredFeatures = SceneFeatureFlags::None;
		// Catalog-entry names that must not be present on the entity (physics
		// 2D/3D domain exclusivity).
		std::vector<std::string> conflictsWith;
	};

	[[nodiscard]] const std::vector<ComponentCatalogEntry>& ComponentCatalog();

	[[nodiscard]] const ComponentCatalogEntry* FindComponent(std::string_view name);

	// Empty string = the entry may be added to this entity in this scene right
	// now; otherwise a human-readable reason (feature not allowed for the scene
	// kind, or a conflicting component is present). Every add path - Inspector
	// palette, MCP add_component - consults this single implementation.
	[[nodiscard]] std::string ComponentAddBlockReason(const World& world, Entity entity, const ComponentCatalogEntry& entry);

	// Editor menus show an entry only when its required features are ACTIVE in
	// the scene and no conflicting component is present - inactive domains are
	// absent from menus rather than shown disabled. (MCP still uses
	// ComponentAddBlockReason, which allows first-use feature enabling.)
	[[nodiscard]] bool ComponentVisibleInMenu(const World& world, Entity entity, const ComponentCatalogEntry& entry);

	// Enables the entry's required features on the world after a successful add
	// (first-use enable). Callers must have passed ComponentAddBlockReason.
	void EnableComponentFeatures(World& world, const ComponentCatalogEntry& entry);
} // namespace aether::editor
