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
	// Editable-field access for a component type, so an agent can read and write the
	// same fields a user edits in the Inspector. This is the field-level companion to
	// ComponentCatalog (which only adds/removes whole components): the control
	// server's get_component / set_component drive it, and it's the single place a new
	// editable field is registered, so the MCP and the inspector never drift.
	//
	// `read` fills `out` (a json object) with the component's current field values and
	// returns false if the component is absent. `write` applies the fields PRESENT in
	// `values` (a partial update - unknown/omitted keys are ignored) and returns the
	// names it actually applied. Both receive the service container for components that
	// need registries (e.g. Material goes through MaterialSystem).
	struct ComponentFieldSet
	{
		std::string name;   // matches the ComponentCatalog name ("Point Light", "Material", ...)
		std::string fields; // "name:type, ..." hint listing the editable fields for discovery
		std::function<bool(const World&, Entity, ServiceContainer&, nlohmann::json&)> read;
		std::function<std::vector<std::string>(World&, Entity, const nlohmann::json&, ServiceContainer&)> write;
	};

	// All components with editable fields, in a stable order.
	[[nodiscard]] const std::vector<ComponentFieldSet>& ComponentFieldSets();

	// Case-sensitive lookup by component name; nullptr if the type has no editable fields.
	[[nodiscard]] const ComponentFieldSet* FindComponentFields(std::string_view name);
} // namespace aether::editor
