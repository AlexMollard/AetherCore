#pragma once

#include <filesystem>

#include "ProjectScripting.hpp"
#include "scripting/AgentDiagnostics.hpp"

namespace aether::app
{
	struct GameplayPrerequisites
	{
		std::filesystem::path cargo;
		std::filesystem::path rustc;
		std::filesystem::path rustSdkManifest;
		std::filesystem::path cmake;
	};

	// Presence checks only. Backend compilation and loading are separate operations.
	[[nodiscard]] GameplayPrerequisites InspectGameplayPrerequisites(GameplayLanguage language);
	[[nodiscard]] ::aether::scripting::AgentReport BuildGameplayPrerequisiteReport(GameplayLanguage language, const GameplayPrerequisites& prerequisites);
	[[nodiscard]] nlohmann::json DescribeGameplayCapabilities(GameplayLanguage language);
	[[nodiscard]] nlohmann::json DescribeProjectGameplayCapabilities(const std::filesystem::path& root, std::string_view requestedLanguage = {});
} // namespace aether::app
