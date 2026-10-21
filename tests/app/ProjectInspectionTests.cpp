#include <doctest/doctest.h>

#include "io/FileUtil.hpp"
#include "project/ProjectInspection.hpp"

using namespace aether;

namespace
{
	struct InspectionFixture
	{
		app::EditorProjectContext project;

		InspectionFixture()
		{
			project.root = std::filesystem::temp_directory_path() / "aether-inspection-tests";
			std::filesystem::remove_all(project.root);
			project.projectFile = project.root / "ProjectSettings.toml";
			project.scenesDir = project.root / "custom scenes";
			project.scriptsDir = project.root / "custom scripts";
			project.assetsDir = project.root / "custom assets";
			project.prefabsDir = project.root / "custom prefabs";
			project.name = "Inspection";
			std::filesystem::create_directories(project.scenesDir);
			std::filesystem::create_directories(project.scriptsDir);
			Write(project.projectFile, "[app]\nstartupscene = 'Start'\n");
			Write(project.scenesDir / "Start.scene.toml", "[scene]\nname = 'Start'\n[[entities]]\nname = 'Player'\n[[entities.scripts]]\ntype = 'PlayerController'\n");
			Write(project.scriptsDir / "AetherGame.csproj", "<Project />");
		}

		~InspectionFixture()
		{
			std::filesystem::remove_all(project.root);
		}

		void Write(const std::filesystem::path& path, std::string_view text)
		{
			REQUIRE(io::file_util::WriteText(path, text).has_value());
		}
	};
} // namespace

TEST_CASE("Project inspection describes authored inputs without runtime claims")
{
	InspectionFixture fixture;
	fixture.Write(fixture.project.scenesDir / "Cache.scene.bin", "cache");
	const auto result = app::project::InspectProject(fixture.project, "Start");
	REQUIRE(result.diagnostics.empty());
	CHECK(result.inventory.at("language") == "csharp");
	CHECK(result.inventory.at("runtimeVerified") == false);
	CHECK(result.inventory.at("scenes").at("total") == 1);
	CHECK(result.inventory.at("gameplay").at("buildEntry").at("exists") == true);
	CHECK(result.inventory.at("scene").at("entities").at(0).at("name") == "Player");
	CHECK(result.inventory.at("scene").at("entities").at(0).at("scripts").at(0).at("type") == "PlayerController");
}

TEST_CASE("Project inspection uses Rust build entry and preserves repair evidence")
{
	InspectionFixture fixture;
	fixture.project.language = app::GameplayLanguage::Rust;
	const auto missing = app::project::InspectProject(fixture.project);
	CHECK_FALSE(missing.diagnostics.empty());
	CHECK(missing.inventory.at("gameplay").at("buildEntry").at("exists") == false);
	fixture.Write(fixture.project.scriptsDir / "Cargo.toml", "[package]\nname = 'game'\n");
	CHECK(app::project::InspectProject(fixture.project).diagnostics.empty());
	std::filesystem::remove(fixture.project.scenesDir / "Start.scene.toml");
	CHECK_FALSE(app::project::InspectProject(fixture.project).diagnostics.empty());
}

TEST_CASE("Project inspection rejects scene traversal and reports cancellation")
{
	InspectionFixture fixture;
	CHECK_FALSE(app::project::InspectProject(fixture.project, "../Start").diagnostics.empty());
	std::stop_source source;
	source.request_stop();
	CHECK(app::project::InspectProject(fixture.project, {}, source.get_token()).cancelled);
}

TEST_CASE("Project inspection bounds deterministic inventories and marks nested scenes")
{
	InspectionFixture fixture;
	for (int index = 0; index < 514; ++index)
	{
		fixture.Write(fixture.project.scenesDir / ("Scene" + std::to_string(index) + ".scene.toml"), "[scene]\n");
	}
	std::filesystem::create_directories(fixture.project.scenesDir / "nested");
	fixture.Write(fixture.project.scenesDir / "nested/Hidden.scene.toml", "[scene]\n");
	const auto inventory = app::project::InspectProject(fixture.project).inventory;
	CHECK(inventory.at("scenes").at("total") == 516);
	CHECK(inventory.at("scenes").at("truncated") == true);
	CHECK(inventory.at("scenes").at("entries").size() == 512);
	CHECK(inventory.at("scenes").at("entries").at(0).at("name") == "Scene0");
	std::filesystem::remove_all(fixture.project.scenesDir);
	std::filesystem::create_directories(fixture.project.scenesDir / "nested");
	fixture.Write(fixture.project.scenesDir / "nested/Hidden.scene.toml", "[scene]\n");
	CHECK(app::project::InspectProject(fixture.project).inventory.at("scenes").at("entries").at(0).at("loadableByName") == false);
}

TEST_CASE("Project inspection reports unreadable or invalid scene details")
{
	InspectionFixture fixture;
	fixture.Write(fixture.project.scenesDir / "Start.scene.toml", "not valid = [");
	const auto invalid = app::project::InspectProject(fixture.project, "Start");
	REQUIRE(invalid.diagnostics.size() == 1);
	CHECK(invalid.diagnostics[0].source->file.ends_with("Start.scene.toml"));
	fixture.Write(fixture.project.scenesDir / "Start.scene.toml", std::string(1024 * 1024 + 1, ' '));
	CHECK_FALSE(app::project::InspectProject(fixture.project, "Start").diagnostics.empty());
	std::string scene = "[scene]\nname = 'Start'\n";
	for (int index = 0; index < 201; ++index)
	{
		scene += "[[entities]]\nname = 'Entity'\n";
	}
	fixture.Write(fixture.project.scenesDir / "Start.scene.toml", scene);
	const auto bounded = app::project::InspectProject(fixture.project, "Start").inventory.at("scene");
	CHECK(bounded.at("truncated") == true);
	CHECK(bounded.at("totalEntities") == 201);
	CHECK(bounded.at("entities").size() == 200);
}

TEST_CASE("Project inspection startup requires a UTF-8 named regular file")
{
	InspectionFixture fixture;
	const std::string name = "caf\xc3\xa9";
	const auto path = fixture.project.scenesDir / std::filesystem::u8path(name + ".scene.toml");
	fixture.Write(fixture.project.projectFile, "[app]\nstartupscene = '" + name + "'\n");
	fixture.Write(path, "[scene]\nname = '" + name + "'\n");
	const auto result = app::project::InspectProject(fixture.project, name);
	CHECK(result.diagnostics.empty());
	CHECK(result.inventory.at("startupScene").at("exists") == true);
	std::filesystem::remove(path);
	std::filesystem::create_directory(path);
	CHECK(app::project::InspectProject(fixture.project).inventory.at("startupScene").at("exists") == false);
}
