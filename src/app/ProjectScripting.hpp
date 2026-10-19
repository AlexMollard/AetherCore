#pragma once

#include <filesystem>
#include <string_view>

#include "utils/Expected.hpp"

namespace aether::app
{
	enum class GameplayLanguage
	{
		CSharp,
		Rust,
	};

	struct ProjectScriptingSettings
	{
		GameplayLanguage language = GameplayLanguage::CSharp;
	};

	[[nodiscard]] std::string_view GameplayLanguageName(GameplayLanguage language);
	[[nodiscard]] std::string_view GameplayProfileName(GameplayLanguage language);
	[[nodiscard]] Expected<GameplayLanguage> ParseGameplayLanguage(std::string_view language);
	[[nodiscard]] Expected<ProjectScriptingSettings> ParseProjectScriptingSettings(std::string_view text);
	[[nodiscard]] Expected<ProjectScriptingSettings> ReadProjectScriptingSettings(const std::filesystem::path& projectFile);
	[[nodiscard]] Expected<void> ValidateProjectGameplayManifests(GameplayLanguage language, const std::filesystem::path& scriptsDir);
} // namespace aether::app
