#pragma once

#include <filesystem>
#include <string>
#include "scripting/AgentDiagnostics.hpp"

namespace aether::app::scripting
{
	struct GameplayRunRequest
	{
		std::filesystem::path project;
		std::filesystem::path module;
		std::filesystem::path managedDir;
		std::filesystem::path output;
		std::string scene;
		std::uint32_t ticks = 60;
		float step = 1.0f / 60.0f;
		nlohmann::json launchConfig = nlohmann::json::object();
	};

	// Synchronous, owner-thread execution in an isolated child process. No GPU is created.
	[[nodiscard]] aether::scripting::AgentReport RunGameplay(const GameplayRunRequest& request);
}
