#include "ProjectScripting.hpp"

#include <toml++/toml.hpp>

#include "io/FileUtil.hpp"

namespace aether::app
{
	std::string_view GameplayLanguageName(GameplayLanguage language)
	{
		switch (language)
		{
			case GameplayLanguage::CSharp:
				return "csharp";
			case GameplayLanguage::Rust:
				return "rust";
		}
		return {};
	}

	std::string_view GameplayProfileName(GameplayLanguage language)
	{
		switch (language)
		{
			case GameplayLanguage::CSharp:
				return "desktop-csharp-v1";
			case GameplayLanguage::Rust:
				return "desktop-rust-v1";
		}
		return {};
	}

	Expected<GameplayLanguage> ParseGameplayLanguage(std::string_view language)
	{
		if (language == "csharp")
		{
			return GameplayLanguage::CSharp;
		}
		if (language == "rust")
		{
			return GameplayLanguage::Rust;
		}
		AE_UNEXPECTED(AetherError::Engine("Unsupported scripting language '" + std::string(language) + "'; expected 'csharp' or 'rust'."));
	}

	Expected<ProjectScriptingSettings> ParseProjectScriptingSettings(std::string_view text)
	{
		ProjectScriptingSettings settings;
		try
		{
			const toml::table document = toml::parse(text);
			const auto* scripting = document.get("scripting");
			if (scripting == nullptr)
			{
				return settings;
			}
			if (!scripting->is_table())
			{
				AE_UNEXPECTED(AetherError::Engine("ProjectSettings.toml [scripting] must be a table with one language string."));
			}
			const auto* language = scripting->as_table()->get("language");
			if (language == nullptr)
			{
				return settings;
			}
			if (!language->is_string())
			{
				AE_UNEXPECTED(AetherError::Engine("ProjectSettings.toml scripting.language must be one string: 'csharp' or 'rust'."));
			}
			AE_TRY(parsed, ParseGameplayLanguage(language->as_string()->get()));
			settings.language = *parsed;
			return settings;
		}
		catch (const toml::parse_error& error)
		{
			AE_UNEXPECTED(AetherError::Engine("ProjectSettings.toml is not readable TOML: " + std::string(error.description())));
		}
	}

	Expected<ProjectScriptingSettings> ReadProjectScriptingSettings(const std::filesystem::path& projectFile)
	{
		if (projectFile.empty())
		{
			return ProjectScriptingSettings{};
		}
		AE_TRY(text, io::file_util::ReadText(projectFile));
		return ParseProjectScriptingSettings(*text);
	}

	Expected<void> ValidateProjectGameplayManifests(GameplayLanguage language, const std::filesystem::path& scriptsDir)
	{
		if (GameplayLanguageName(language).empty())
		{
			AE_UNEXPECTED(AetherError::Engine("Unsupported project scripting language."));
		}
		std::error_code ec;
		const bool hasCSharp = std::filesystem::exists(scriptsDir / "AetherGame.csproj", ec);
		if (ec)
		{
			AE_UNEXPECTED(AetherError::Engine("Could not inspect C# gameplay manifest: " + ec.message()));
		}
		const bool hasRust = std::filesystem::exists(scriptsDir / "Cargo.toml", ec);
		if (ec)
		{
			AE_UNEXPECTED(AetherError::Engine("Could not inspect Rust gameplay manifest: " + ec.message()));
		}
		if (hasCSharp && hasRust)
		{
			AE_UNEXPECTED(AetherError::Engine("Mixed gameplay languages are unsupported: scripts contains both AetherGame.csproj and Cargo.toml. Select one language per project."));
		}
		if ((language == GameplayLanguage::CSharp && hasRust) || (language == GameplayLanguage::Rust && hasCSharp))
		{
			AE_UNEXPECTED(AetherError::Engine("Gameplay manifest does not match scripting.language = '" + std::string(GameplayLanguageName(language)) + "'. Select one matching gameplay manifest."));
		}
		return {};
	}
} // namespace aether::app
