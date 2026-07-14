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
	};

	[[nodiscard]] const std::vector<ComponentCatalogEntry>& ComponentCatalog();

	[[nodiscard]] const ComponentCatalogEntry* FindComponent(std::string_view name);
} // namespace aether::editor
