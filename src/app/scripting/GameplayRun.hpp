#pragma once

#include <filesystem>
#include <string>
#include "scripting/AgentDiagnostics.hpp"

namespace aether
{
	class World;
}

namespace aether::app
{
	class ScriptComponentSystem;
}

namespace aether::app::scripting
{
	class GameplayHost;

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

	void ValidateGameplayRunConfig(const GameplayRunRequest& request);
	// Publishes copied evidence before an assertion failure unwinds the runtime.
	void WriteGameplayState(
	        const GameplayRunRequest& request, World& world, GameplayHost& host, ScriptComponentSystem& scripts, bool allowSaveReads, const nlohmann::json& observations = nlohmann::json::array(), aether::scripting::AgentReport* report = nullptr);

	// Synchronous, owner-thread execution in an isolated child process. No GPU is created.
	[[nodiscard]] aether::scripting::AgentReport RunGameplay(const GameplayRunRequest& request);
} // namespace aether::app::scripting
