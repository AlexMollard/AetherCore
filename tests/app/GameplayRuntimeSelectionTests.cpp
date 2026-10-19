#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

#include "RuntimeProjectSettings.hpp"
#include "PlaySession.hpp"
#include "GameplayPrerequisites.hpp"
#include "editor/EditorProjectContext.hpp"
#include "editor/publish/PublishPlan.hpp"
#include "utils/EngineSettings.hpp"
#include "utils/SettingsService.hpp"

namespace
{
	struct RuntimeLanguageLayout
	{
		std::filesystem::path root = std::filesystem::temp_directory_path() / "aether_runtime_gameplay_selection";
		RuntimeLanguageLayout()
		{
			std::filesystem::remove_all(root);
			std::filesystem::create_directories(root);
		}
		~RuntimeLanguageLayout()
		{
			std::error_code ec;
			std::filesystem::remove_all(root, ec);
		}
		void Write(const std::filesystem::path& path, std::string_view text)
		{
			std::filesystem::create_directories(path.parent_path());
			std::ofstream file(path);
			file << text;
		}
	};
}

TEST_CASE("Runtime gameplay language defaults to C# for legacy projects")
{
	RuntimeLanguageLayout layout;
	layout.Write(layout.root / "project" / "ProjectSettings.toml", "[project]\nname = 'Legacy'\n");
	const auto selected = aether::app::ResolveRuntimeGameplaySettings({layout.root / "project", layout.root / "exe", {}});
	REQUIRE(selected);
	CHECK(selected->language == aether::app::GameplayLanguage::CSharp);
}

TEST_CASE("Runtime gameplay language selects Rust and rejects unknown explicit values")
{
	RuntimeLanguageLayout layout;
	const auto projectFile = layout.root / "project" / "ProjectSettings.toml";
	layout.Write(projectFile, "[scripting]\nlanguage = 'rust'\n");
	const auto selected = aether::app::ResolveRuntimeGameplaySettings({layout.root / "project", layout.root / "exe", {}});
	REQUIRE(selected);
	CHECK(selected->language == aether::app::GameplayLanguage::Rust);
	layout.Write(projectFile, "[scripting]\nlanguage = 'python'\n");
	CHECK_FALSE(aether::app::ResolveRuntimeGameplaySettings({layout.root / "project", layout.root / "exe", {}}));
}

TEST_CASE("Moved published packages use shipped gameplay language instead of build machine sources")
{
	RuntimeLanguageLayout layout;
	layout.Write(layout.root / "source" / "ProjectSettings.toml", "[scripting]\nlanguage = 'csharp'\n");
	layout.Write(layout.root / "moved" / "data" / "project.pak", "pak");
	layout.Write(layout.root / "moved" / "data" / "config" / "EngineSettings.toml", "[scripting]\nlanguage = 'rust'\n");
	const auto selected = aether::app::ResolveRuntimeGameplaySettings({{}, layout.root / "moved", layout.root / "source"});
	REQUIRE(selected);
	CHECK(selected->language == aether::app::GameplayLanguage::Rust);
	const auto module = aether::app::ResolveRuntimeGameplayModule(layout.root / "moved", aether::app::GameplayLanguage::Rust);
	CHECK(module.parent_path() == layout.root / "moved" / "data" / "scripts" / "rust");
	CHECK(module.is_absolute());
	CHECK(aether::app::ResolveRuntimeGameplayModule(layout.root / "moved", aether::app::GameplayLanguage::CSharp)
	      == layout.root / "moved" / "data" / "scripts" / "managed" / "AetherGame.dll");
}

TEST_CASE("Runtime source projects reject mixed gameplay manifests before selecting a backend")
{
	RuntimeLanguageLayout layout;
	layout.Write(layout.root / "project" / "ProjectSettings.toml", "[scripting]\nlanguage = 'rust'\n");
	layout.Write(layout.root / "project" / "scripts" / "Cargo.toml", "[package]\nname = 'game'\n");
	layout.Write(layout.root / "project" / "scripts" / "AetherGame.csproj", "<Project />");
	CHECK_FALSE(aether::app::ResolveRuntimeGameplaySettings({layout.root / "project", layout.root / "exe", {}}));
}

TEST_CASE("Runtime source validation honors the authored scripts path")
{
	RuntimeLanguageLayout layout;
	const auto projectRoot = layout.root / "project";
	layout.Write(projectRoot / "ProjectSettings.toml", "[scripting]\nlanguage = 'rust'\n[paths]\nscripts = 'gameplay/custom'\n");
	layout.Write(projectRoot / "gameplay" / "custom" / "Cargo.toml", "[package]\nname = 'game'\n");
	const auto selected = aether::app::ResolveRuntimeGameplaySettings({projectRoot, layout.root / "exe", {}});
	REQUIRE(selected);
	CHECK(selected->language == aether::app::GameplayLanguage::Rust);
	layout.Write(projectRoot / "gameplay" / "custom" / "AetherGame.csproj", "<Project />");
	CHECK_FALSE(aether::app::ResolveRuntimeGameplaySettings({projectRoot, layout.root / "exe", {}}));
	CHECK_FALSE(aether::app::ResolveRuntimeGameplaySettings({{}, layout.root / "exe", projectRoot}));
}

TEST_CASE("Gameplay language is authored project state and cannot be overridden by user settings")
{
	aether::EngineSettings settings;
	CHECK(settings.scripting.language == "csharp");
	CHECK(aether::IsProjectOnlySettingKey("scripting.language"));
	CHECK(aether::SettingsHomeFor("scripting.language") == aether::SettingsHome::Project);
	aether::EngineSettingsIO::Apply("[scripting]\nlanguage = 'rust'\n", settings);
	CHECK(settings.scripting.language == "rust");
	aether::EngineSettingsIO::Apply("[scripting]\nlanguage = 'csharp'\n", settings, aether::SettingsScope::UserOverridable);
	CHECK(settings.scripting.language == "rust");
	const auto serialized = aether::EngineSettingsIO::Serialize(settings);
	CHECK(serialized.find("rust") != std::string::npos);
	RuntimeLanguageLayout layout;
	const auto projectFile = layout.root / "project" / "ProjectSettings.toml";
	layout.Write(projectFile, "[project]\nname = 'RustGame'\n[scripting]\nlanguage = 'rust'\n");
	std::string error;
	CHECK(aether::EngineSettingsIO::SaveProjectOverrides(settings, aether::EngineSettings{}, projectFile, error));
	const auto reloaded = aether::app::ResolveRuntimeGameplaySettings({layout.root / "project", layout.root / "exe", {}});
	REQUIRE(reloaded);
	CHECK(reloaded->language == aether::app::GameplayLanguage::Rust);
}

TEST_CASE("Publish planning preserves per-project gameplay language")
{
	aether::app::EditorProjectContext project;
	project.root = "project";
	project.name = "RustGame";
	project.language = aether::app::GameplayLanguage::Rust;
	const auto plan = aether::editor::MakePublishPlan(project, {});
	CHECK(plan.language == aether::app::GameplayLanguage::Rust);
	project.language = aether::app::GameplayLanguage::CSharp;
	CHECK(aether::editor::MakePublishPlan(project, {}).language == aether::app::GameplayLanguage::CSharp);
}

TEST_CASE("Gameplay operations reject descriptor and configured language changes until reopen")
{
	RuntimeLanguageLayout layout;
	const auto projectRoot = layout.root / "project";
	const auto projectFile = projectRoot / "ProjectSettings.toml";
	layout.Write(projectFile, "[scripting]\nlanguage = 'csharp'\n");
	CHECK(aether::app::ValidateProjectGameplaySelection(projectRoot, aether::app::GameplayLanguage::CSharp, "csharp"));
	const auto configuredChange = aether::app::ValidateProjectGameplaySelection(projectRoot, aether::app::GameplayLanguage::CSharp, "rust");
	REQUIRE_FALSE(configuredChange);
	CHECK(configuredChange.error().message.find("Reopen") != std::string::npos);
	CHECK_FALSE(aether::app::ValidateProjectGameplaySelection(projectRoot, aether::app::GameplayLanguage::CSharp, ""));
	layout.Write(projectFile, "[scripting]\nlanguage = 'rust'\n");
	const auto authoredChange = aether::app::ValidateProjectGameplaySelection(projectRoot, aether::app::GameplayLanguage::CSharp, "csharp");
	REQUIRE_FALSE(authoredChange);
	CHECK(authoredChange.error().message.find("Reopen") != std::string::npos);
	layout.Write(projectFile, "[scripting]\nlanguage = 'unknown'\n");
	CHECK_FALSE(aether::app::ValidateProjectGameplaySelection(projectRoot, aether::app::GameplayLanguage::CSharp));
	layout.Write(projectFile, "[scripting]\nlanguage = 'csharp'\n[paths]\nscripts = 'gameplay'\n");
	layout.Write(projectRoot / "gameplay" / "Cargo.toml", "[package]\nname = 'game'\n");
	layout.Write(projectRoot / "gameplay" / "AetherGame.csproj", "<Project />");
	CHECK_FALSE(aether::app::ValidateProjectGameplaySelection(projectRoot, aether::app::GameplayLanguage::CSharp));
}

TEST_CASE("Play preflight uses active backend and current configured language")
{
	RuntimeLanguageLayout layout;
	const auto projectRoot = layout.root / "project";
	layout.Write(projectRoot / "ProjectSettings.toml", "[scripting]\nlanguage = 'csharp'\n");
	aether::ServiceContainer services;
	aether::app::ProjectScriptingSettings selected;
	aether::app::EditorProjectContext project;
	project.root = projectRoot;
	aether::EngineSettings values;
	aether::SettingsService settings(values, values, values, services);
	services.Register(selected);
	services.Register(project);
	services.Register(settings);
	CHECK(aether::app::ValidatePlaySessionGameplay(services));
	settings.Values().scripting.language = "rust";
	CHECK_FALSE(aether::app::ValidatePlaySessionGameplay(services));
	selected.language = aether::app::GameplayLanguage::Rust;
	layout.Write(projectRoot / "ProjectSettings.toml", "[scripting]\nlanguage = 'rust'\n");
	const auto rust = aether::app::ValidatePlaySessionGameplay(services);
	const auto prerequisites = aether::app::BuildGameplayPrerequisiteReport(selected.language, aether::app::InspectGameplayPrerequisites(selected.language));
	CHECK(rust.has_value() == (prerequisites.status == aether::scripting::AgentJobStatus::Passed));
}
