#pragma once

#include <stop_token>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "editor/EditorProjectContext.hpp"
#include "scripting/AgentDiagnostics.hpp"

namespace aether::app::project
{
	struct ProjectInspection
	{
		nlohmann::json inventory;
		std::vector<aether::scripting::AgentDiagnostic> diagnostics;
		bool cancelled = false;
	};

	// Authored files only. Does not load gameplay modules or invoke a compiler.
	[[nodiscard]] ProjectInspection InspectProject(const EditorProjectContext& project, std::string_view scene = {}, std::stop_token stop = {});
} // namespace aether::app::project
