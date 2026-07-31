#include <doctest/doctest.h>

#include <filesystem>
#include <string>

#include "editor/EditorProjectContext.hpp"
#include "editor/publish/PublishPlan.hpp"

using namespace aether;

namespace
{
	app::EditorProjectContext MakeProject(const std::string& name)
	{
		app::EditorProjectContext project;
		project.root = std::filesystem::path("D:/work") / name;
		project.name = name;
		project.projectFile = project.root / "ProjectSettings.toml";
		project.scenesDir = project.root / "scenes";
		project.scriptsDir = project.root / "scripts";
		project.loaded = true;
		return project;
	}

	editor::PublishEnvironment MakeEnv()
	{
		editor::PublishEnvironment env;
		env.bundleDir = "D:/build/src/app/Debug";
		env.runtimeExeName = "AetherGame.exe";
		env.platformName = "Windows";
		env.configName = "Debug";
		env.shippableConfig = false;
		return env;
	}
} // namespace

TEST_CASE("MakePublishPlan derives the output directory from the project and platform") {
    const editor::PublishPlan plan = editor::MakePublishPlan(MakeProject("Whisper"), MakeEnv());

    CHECK(plan.outputDir == std::filesystem::path("D:/work/Whisper/Builds/Windows/Whisper"));
    CHECK(plan.productName == "Whisper");
    CHECK(plan.runtimeExeName == "AetherGame.exe");
    CHECK(plan.bundleDir == std::filesystem::path("D:/build/src/app/Debug"));
}

TEST_CASE("MakePublishPlan sanitises a project name that is not a valid path segment") {
    const editor::PublishPlan plan = editor::MakePublishPlan(MakeProject("My: Game?/v2"), MakeEnv());

    CHECK(plan.productName == "My_ Game__v2");
    CHECK(plan.outputDir.filename() == "My_ Game__v2");
}

TEST_CASE("MakePublishPlan falls back when the project name is empty or all separators") {
    app::EditorProjectContext project = MakeProject("Whisper");
    project.name = "///";

    const editor::PublishPlan plan = editor::MakePublishPlan(project, MakeEnv());

    CHECK(plan.productName == "AetherGame");
}

TEST_CASE("MakePublishPlan carries the build configuration through for labelling") {
    editor::PublishEnvironment env = MakeEnv();
    env.configName = "Release";
    env.shippableConfig = true;

    const editor::PublishPlan plan = editor::MakePublishPlan(MakeProject("Whisper"), env);

    CHECK(plan.configName == "Release");
    CHECK(plan.shippableConfig);
}

TEST_CASE("SanitizePublishSegment strips trailing dots and spaces that Windows rejects") {
    CHECK(editor::SanitizePublishSegment("Game. ", "fallback") == "Game");
    CHECK(editor::SanitizePublishSegment("   ", "fallback") == "fallback");
}
