#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

#include "ProjectScripting.hpp"
#include "project/ProjectCommon.hpp"
#include "io/FileUtil.hpp"
#include "utils/TomlConfig.hpp"
#include "utils/EngineSettings.hpp"

using namespace aether::app;

namespace
{
	struct ProjectFixture
	{
		std::filesystem::path root = std::filesystem::temp_directory_path() / "aether-project-scripting-tests";

		ProjectFixture()
		{
			std::filesystem::remove_all(root);
			std::filesystem::create_directories(root / "scripts");
		}

		~ProjectFixture()
		{
			std::error_code ec;
			std::filesystem::remove_all(root, ec);
		}

		void Write(std::filesystem::path path, std::string_view contents)
		{
			std::ofstream(root / path) << contents;
		}
	};
} // namespace

TEST_CASE("Project scripting defaults legacy descriptors to CSharp")
{
	const auto settings = ParseProjectScriptingSettings("[project]\nname = 'Legacy'\n");
	REQUIRE(settings);
	CHECK(settings->language == GameplayLanguage::CSharp);
	CHECK(ReadProjectScriptingSettings({})->language == GameplayLanguage::CSharp);
	CHECK(GameplayLanguageName(GameplayLanguage::CSharp) == "csharp");
	CHECK(GameplayProfileName(GameplayLanguage::CSharp) == "desktop-csharp-v1");
	CHECK(GameplayProfileName(GameplayLanguage::Rust) == "desktop-rust-v1");
}

TEST_CASE("Project scripting accepts only one strictly typed supported language")
{
	for (const auto value: {"'rust'", "\"rust\""})
	{
		const auto parsed = ParseProjectScriptingSettings(std::string("[scripting]\nlanguage = ") + value);
		REQUIRE(parsed);
		CHECK(parsed->language == GameplayLanguage::Rust);
	}
	for (const auto text: {"[scripting]\nlanguage = 'Rust'",
	             "[scripting]\nlanguage = 'lua'",
	             "[scripting]\nlanguage = 3",
	             "[scripting]\nlanguage = ['rust']",
	             "[scripting]\nlanguage = true",
	             "[scripting]\nlanguage = 'rust'\nlanguage = 'csharp'",
	             "[[scripting]]\nlanguage = 'rust'",
	             "scripting = 'rust'",
	             "[scripting.language]\nx = 'rust'",
	             "broken = ["})
	{
		CHECK_FALSE(ParseProjectScriptingSettings(text));
	}
	CHECK_FALSE(ParseGameplayLanguage("C#"));
	CHECK_FALSE(ParseGameplayLanguage("rust,csharp"));
	CHECK_FALSE(ParseGameplayLanguage(""));
}

TEST_CASE("Project scripting distinguishes absent descriptors from legacy descriptors")
{
	ProjectFixture fixture;
	CHECK_FALSE(ReadProjectScriptingSettings(fixture.root / "missing.toml"));
	fixture.Write("ProjectSettings.toml", "[scripting]\nlanguage = 'rust'");
	const auto parsed = ReadProjectScriptingSettings(fixture.root / "ProjectSettings.toml");
	REQUIRE(parsed);
	CHECK(parsed->language == GameplayLanguage::Rust);
}

TEST_CASE("Project scripting rejects mixed or mismatched gameplay manifests")
{
	ProjectFixture fixture;
	CHECK(ValidateProjectGameplayManifests(GameplayLanguage::Rust, fixture.root / "scripts"));
	fixture.Write("scripts/Cargo.toml", "[package]\nname='game'");
	CHECK(ValidateProjectGameplayManifests(GameplayLanguage::Rust, fixture.root / "scripts"));
	CHECK_FALSE(ValidateProjectGameplayManifests(GameplayLanguage::CSharp, fixture.root / "scripts"));
	fixture.Write("scripts/AetherGame.csproj", "<Project />");
	CHECK_FALSE(ValidateProjectGameplayManifests(GameplayLanguage::Rust, fixture.root / "scripts"));
	CHECK_FALSE(ValidateProjectGameplayManifests(GameplayLanguage::CSharp, fixture.root / "scripts"));
}

TEST_CASE("Rust project scaffolding remains Rust only and round trips metadata")
{
	ProjectFixture fixture;
	std::string error;
	REQUIRE(project::WriteProjectDescriptor(fixture.root, "Rust game", error, project::ProjectTemplate::Blank2D, GameplayLanguage::Rust));
	const auto descriptor = project::ReadProjectDescriptor(fixture.root);
	REQUIRE(descriptor);
	CHECK(descriptor->language == GameplayLanguage::Rust);
	CHECK(descriptor->kind == ProjectKind::Scene2D);
	CHECK(std::filesystem::exists(fixture.root / "scripts/Cargo.toml"));
	CHECK(std::filesystem::exists(fixture.root / "scripts/src/lib.rs"));
	CHECK_FALSE(std::filesystem::exists(fixture.root / "scripts/AetherGame.csproj"));
	CHECK_FALSE(std::filesystem::exists(fixture.root / "scripts/Player.cs"));
	REQUIRE(project::EnsureGameSolution(fixture.root, error));
	CHECK_FALSE(std::filesystem::exists(fixture.root / "AetherGame.slnx"));
	const auto source = aether::io::file_util::ReadText(fixture.root / "scripts/src/lib.rs");
	REQUIRE(source);
	CHECK(source->find("AE_GAMEPLAY_UNSUPPORTED") != std::string::npos);
}

TEST_CASE("Project creation rejects incompatible language and templates before writing")
{
	ProjectFixture fixture;
	std::string error;
	const auto multiplayerRoot = fixture.root / "multiplayer";
	CHECK_FALSE(project::WriteProjectDescriptor(multiplayerRoot, "No", error, project::ProjectTemplate::Multiplayer2D, GameplayLanguage::Rust));
	CHECK_FALSE(std::filesystem::exists(multiplayerRoot));
	fixture.Write("scripts/Cargo.toml", "[package]\nname='kept'");
	CHECK_FALSE(project::WriteProjectDescriptor(fixture.root, "No", error));
	CHECK_FALSE(std::filesystem::exists(fixture.root / "ProjectSettings.toml"));
	CHECK_FALSE(std::filesystem::exists(fixture.root / "scripts/Player.cs"));
}

TEST_CASE("CSharp project scaffolding remains the default with legacy script assets")
{
	ProjectFixture fixture;
	std::string error;
	REQUIRE(project::WriteProjectDescriptor(fixture.root, "Legacy game", error));
	const auto descriptor = project::ReadProjectDescriptor(fixture.root);
	REQUIRE(descriptor);
	CHECK(descriptor->language == GameplayLanguage::CSharp);
	CHECK(std::filesystem::exists(fixture.root / "scripts/AetherGame.csproj"));
	CHECK(std::filesystem::exists(fixture.root / "scripts/Player.cs"));
	CHECK_FALSE(std::filesystem::exists(fixture.root / "scripts/Cargo.toml"));
	fixture.Write("ProjectSettings.toml", "[project]\nname = 'legacy'\n[paths]\nscripts = 'scripts'");
	REQUIRE(project::ReadProjectDescriptor(fixture.root));
	CHECK(project::ReadProjectDescriptor(fixture.root)->language == GameplayLanguage::CSharp);
}

TEST_CASE("Project descriptor validation honors configured scripts path")
{
	ProjectFixture fixture;
	std::filesystem::create_directories(fixture.root / "gameplay");
	fixture.Write("gameplay/Cargo.toml", "[package]\nname='game'");
	fixture.Write("ProjectSettings.toml", "[project]\nname = 'Rust'\n[scripting]\nlanguage = 'rust'\n[paths]\nscripts = 'gameplay'");
	REQUIRE(project::ReadProjectDescriptor(fixture.root));
	fixture.Write("gameplay/AetherGame.csproj", "<Project />");
	CHECK_FALSE(project::ReadProjectDescriptor(fixture.root));
	fixture.Write("ProjectSettings.toml", "[scripting]\nlanguage = 5");
	CHECK_FALSE(project::ReadProjectDescriptor(fixture.root));
}

TEST_CASE("Rust blank3d project retains the starter scene type and round trips")
{
	ProjectFixture fixture;
	std::string error;
	REQUIRE(project::WriteProjectDescriptor(fixture.root, "Rust 3D", error, project::ProjectTemplate::Blank3D, GameplayLanguage::Rust));
	const auto descriptor = project::ReadProjectDescriptor(fixture.root);
	REQUIRE(descriptor);
	CHECK(descriptor->kind == ProjectKind::Scene3D);
	CHECK(descriptor->language == GameplayLanguage::Rust);
	const auto scene = aether::io::file_util::ReadText(fixture.root / "scenes/default.scene.toml");
	REQUIRE(scene);
	CHECK(scene->find("type = 'Player'") != std::string::npos);
}

TEST_CASE("Project creation does not replace another language descriptor")
{
	ProjectFixture fixture;
	fixture.Write("ProjectSettings.toml", "[scripting]\nlanguage = 'rust'\n");
	std::string error;
	CHECK_FALSE(project::WriteProjectDescriptor(fixture.root, "No", error));
	const auto settings = ReadProjectScriptingSettings(fixture.root / "ProjectSettings.toml");
	REQUIRE(settings);
	CHECK(settings->language == GameplayLanguage::Rust);
	CHECK_FALSE(std::filesystem::exists(fixture.root / "scripts/Player.cs"));
}

TEST_CASE("Recent projects retain their authored gameplay language")
{
	ProjectFixture fixture;
	fixture.Write("ProjectSettings.toml", "[project]\nname = 'Rust recent'\n[scripting]\nlanguage = 'rust'\n");
	aether::TomlConfig config;
	config.Set("launcher.recent_0.path", fixture.root.generic_string());
	config.Set("launcher.recent_0.name", "Rust recent");
	const auto recents = project::LoadRecentProjects(config);
	REQUIRE(recents.size() == 1);
	CHECK(recents.front().language == GameplayLanguage::Rust);
}

TEST_CASE("Project gameplay language survives settings serialization and excludes user overrides")
{
	aether::EngineSettings original;
	original.scripting.language = "rust";
	original.app.startupScene = "AgentLevel";
	original.app.autoplay = true;
	const auto serialized = aether::EngineSettingsIO::Serialize(original);
	aether::TomlConfig parsed;
	REQUIRE(parsed.Load(serialized));
	aether::EngineSettings restored;
	aether::EngineSettingsIO::Apply(serialized, restored);
	CHECK(restored.scripting.language == "rust");
	CHECK(restored.app.startupScene == "AgentLevel");
	CHECK(restored.app.autoplay);
	const auto user = aether::EngineSettingsIO::SerializeOverrides(original, aether::EngineSettings{});
	CHECK(user.find("language") == std::string::npos);
	aether::EngineSettingsIO::Apply("[scripting]\nlanguage='csharp'", restored, aether::SettingsScope::UserOverridable);
	CHECK(restored.scripting.language == "rust");
}
