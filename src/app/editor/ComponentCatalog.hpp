#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace aether
{
	class World;
	struct Entity;
	class ServiceContainer;
} // namespace aether

namespace aether::app::editor
{
	// One menu-addable component (or component bundle, e.g. a "Cube" mesh renderer).
	// This is the SINGLE SOURCE OF TRUTH for "what can be added to an entity" - the
	// Inspector's Add-Component palette renders it and the control endpoint's
	// add_component / remove_component drive it, so the two never drift. Adding a
	// component to the engine means one entry in ComponentCatalog.cpp.
	struct ComponentCatalogEntry
	{
		std::string name;      // stable id shown to the agent + inspector, e.g. "Cube", "Point Light"
		std::string category;  // "Core", "Rendering", "Behaviors", "Physics", "Editor"
		std::string icon;      // ICON_FA_* glyph string for the inspector palette

		// Present on the entity? (a bundle reports true when its primary component is.)
		std::function<bool(const World&, Entity)> has;
		// Add it. `services` is available for entries that need meshes/materials;
		// entries seed transform-derived defaults from the entity itself.
		std::function<void(World&, Entity, ServiceContainer&)> add;
		// Remove it (the bundle's components, for composite entries).
		std::function<void(World&, Entity)> remove;

		// False for entries that exist only so scripts can type-validate a component
		// reference (IComponentRef fields), NOT to be slapped onto an arbitrary
		// entity. UI Text is the canonical example: it is authored as a UI ENTITY
		// (Create > UI > Text), never added as a loose component. The Add-Component
		// palette and scene.add_component skip these; `has` still drives ref drops.
		bool addable = true;
	};

	// The catalog, built once. Order groups entries by category for the palette.
	[[nodiscard]] const std::vector<ComponentCatalogEntry>& ComponentCatalog();

	// Case-sensitive lookup by name; nullptr if unknown.
	[[nodiscard]] const ComponentCatalogEntry* FindComponent(std::string_view name);
} // namespace aether::app::editor
