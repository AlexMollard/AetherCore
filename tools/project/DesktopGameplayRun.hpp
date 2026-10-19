#pragma once

#include "scripting/GameplayRun.hpp"

namespace aether::project_tool
{
	[[nodiscard]] aether::scripting::AgentReport RunDesktopGameplay(const app::scripting::GameplayRunRequest& request);
}
