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

		// Scene features the component implies; adding it auto-enables them on
		// the world (metadata bookkeeping only - never a permission check).
		SceneFeatureFlags requiredFeatures = SceneFeatureFlags::None;
		// Catalog-entry names that must not be present on the entity (physics
		// 2D/3D domain exclusivity).
		std::vector<std::string> conflictsWith;
	};

	[[nodiscard]] const std::vector<ComponentCatalogEntry>& ComponentCatalog();

	[[nodiscard]] const ComponentCatalogEntry* FindComponent(std::string_view name);

	// Empty string = the entry may be added to this entity right now; otherwise
	// a human-readable reason. The only block is a conflicting component on the
	// same entity (2D/3D physics exclusivity). Every add path - Inspector
	// palette, MCP add_component - consults this single implementation.
	[[nodiscard]] std::string ComponentAddBlockReason(const World& world, Entity entity, const ComponentCatalogEntry& entry);

	// Editor menus show every addable entry that doesn't conflict with a
	// component already on the entity. No scene-kind or feature filtering.
	[[nodiscard]] bool ComponentVisibleInMenu(const World& world, Entity entity, const ComponentCatalogEntry& entry);

	// Enables the entry's required features on the world after a successful add
	// (first-use enable). Callers must have passed ComponentAddBlockReason.
	void EnableComponentFeatures(World& world, const ComponentCatalogEntry& entry);
} // namespace aether::editor
