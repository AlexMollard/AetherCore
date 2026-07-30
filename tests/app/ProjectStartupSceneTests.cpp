#include <doctest/doctest.h>

#include <filesystem>
#include <string>
#include <system_error>

#include "io/FileUtil.hpp"
#include "project/ProjectStartupScene.hpp"

using namespace aether;

namespace
{
	struct TempProject
	{
		std::filesystem::path root;
		std::filesystem::path projectFile;
		std::filesystem::path scenesDir;

		explicit TempProject(const std::string& name)
		{
			root = std::filesystem::temp_directory_path() / ("aethercore_startupscene_" + name);
			std::error_code ec;
			std::filesystem::remove_all(root, ec);
			projectFile = root / "ProjectSettings.toml";
			scenesDir = root / "scenes";
			REQUIRE(io::file_util::CreateDirectories(scenesDir).has_value());
		}

		~TempProject()
		{
			std::error_code ec;
			std::filesystem::remove_all(root, ec);
		}

		void AddScene(const std::string& sceneName) const
		{
			REQUIRE(io::file_util::WriteText(scenesDir / (sceneName + ".scene.toml"), "version = 1\n").has_value());
		}

		[[nodiscard]] std::string ProjectFileText() const
		{
			auto text = io::file_util::ReadText(projectFile);
			return text ? *text : std::string{};
		}
	};
} // namespace

TEST_CASE("WriteProjectStartupScene records the scene in the project file") {
    const TempProject project("write");
    REQUIRE(io::file_util::WriteText(project.projectFile, "[project]\nname = \"Demo\"\nversion = 1\n").has_value());

    std::string error;
    REQUIRE(app::WriteProjectStartupScene(project.projectFile, "Title", error));

    CHECK(app::ReadProjectStartupScene(project.projectFile) == "Title");
}

TEST_CASE("WriteProjectStartupScene preserves every other key in the project file") {
    const TempProject project("preserve");
    REQUIRE(io::file_util::WriteText(project.projectFile,
                                     "[app]\nstartupscene = \"Old\"\ntargetfps = 0\n\n"
                                     "[paths]\nassets = \"assets\"\nscenes = \"scenes\"\n\n"
                                     "[project]\nname = \"Demo\"\nversion = 1\n")
                .has_value());

    std::string error;
    REQUIRE(app::WriteProjectStartupScene(project.projectFile, "Arena", error));

    const std::string text = project.ProjectFileText();
    CHECK(app::ReadProjectStartupScene(project.projectFile) == "Arena");
    CHECK(text.find("assets") != std::string::npos);
    CHECK(text.find("Demo") != std::string::npos);
    CHECK(text.find("targetfps") != std::string::npos);
    CHECK(text.find("Old") == std::string::npos);
}

TEST_CASE("ValidateProjectStartupScene rejects a scene the project does not have") {
    const TempProject project("validate");
    project.AddScene("Title");

    std::string error;
    CHECK(app::ValidateProjectStartupScene(project.scenesDir, "Title", error));

    error.clear();
    CHECK_FALSE(app::ValidateProjectStartupScene(project.scenesDir, "Ghost", error));
    CHECK(error.find("Ghost") != std::string::npos);
}

TEST_CASE("ValidateProjectStartupScene rejects an unset startup scene") {
    const TempProject project("unset");

    std::string error;
    CHECK_FALSE(app::ValidateProjectStartupScene(project.scenesDir, "", error));
    CHECK_FALSE(error.empty());
}

TEST_CASE("ProjectHasScene ignores the rebuildable .scene.bin cache") {
    const TempProject project("bin");
    // A fresh clone has the authored .toml and no .bin; a stale tree can have the .bin
    // alone. Only the authored file counts, or publish would pass on one machine and
    // ship an unloadable scene from another.
    REQUIRE(io::file_util::WriteText(project.scenesDir / "Cached.scene.bin", "binary").has_value());

    CHECK_FALSE(app::ProjectHasScene(project.scenesDir, "Cached"));
}
