#pragma once

#include <span>
#include <nlohmann/json.hpp>
#include "scripting/GameplayHost.hpp"

namespace aether::app::scripting
{
	// Empty declarations preserve legacy scalar inference unless strict metadata is requested.
	Expected<std::map<std::string, ScriptPropertyValue>> ParseGameplayScriptProperties(const nlohmann::json& input, std::span<const ScriptPropertyInfo> declarations = {}, bool requireDeclared = false);
} // namespace aether::app::scripting
