#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "scene/Entity.hpp"

namespace aether
{
	class World;
	class ServiceContainer;
} // namespace aether

namespace aether::editor
{
	// editable field is registered, so the MCP and the inspector never drift.
	struct ComponentFieldSet
	{
		std::string name;
		std::string fields;
		std::function<bool(const World&, Entity, ServiceContainer&, nlohmann::json&)> read;
		std::function<std::vector<std::string>(World&, Entity, const nlohmann::json&, ServiceContainer&)> write;
	};

	[[nodiscard]] const std::vector<ComponentFieldSet>& ComponentFieldSets();

	[[nodiscard]] const ComponentFieldSet* FindComponentFields(std::string_view name);
} // namespace aether::editor
