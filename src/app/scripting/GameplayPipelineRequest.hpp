#pragma once

#include "scripting/AgentDiagnostics.hpp"

namespace aether::app::scripting
{
	[[nodiscard]] Expected<nlohmann::json> ValidateGameplayPipelineRequest(const nlohmann::json& request);
}
