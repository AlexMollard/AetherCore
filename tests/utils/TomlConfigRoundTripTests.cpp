#include <doctest/doctest.h>

#include <sstream>
#include <string>

#include "utils/TomlConfig.hpp"

using namespace aether;

// A project file is user data. Editing one key from the editor must never drop the
// rest - a lossy round trip here silently destroys a project's paths and settings.
TEST_CASE("Editing one key preserves every other project setting")
{
	const std::string original =
	        "# AetherCore project file.\n"
	        "\n"
	        "[app]\n"
	        "startupscene = \"Title\"\n"
	        "targetfps = 0\n"
	        "\n"
	        "[graphics]\n"
	        "asynccompute = true\n"
	        "fxaa = false\n"
	        "vsync = false\n"
	        "\n"
	        "[paths]\n"
	        "assets = \"assets\"\n"
	        "prefabs = \"assets/prefabs\"\n"
	        "scenes = \"scenes\"\n"
	        "scripts = \"scripts\"\n"
	        "\n"
	        "[project]\n"
	        "name = \"Whisper\"\n"
	        "version = 1\n";

	TomlConfig config;
	config.Load(original);
	config.Set("app.startupscene", "Arena");

	std::ostringstream out;
	config.Save(out, "AetherCore project file.");
	const std::string saved = out.str();

	INFO("saved file was:\n" << saved);

	// The edited key took.
	CHECK(saved.find("startupscene = \"Arena\"") != std::string::npos);

	// ...and nothing else was lost.
	CHECK(saved.find("[paths]") != std::string::npos);
	CHECK(saved.find("assets = \"assets\"") != std::string::npos);
	CHECK(saved.find("prefabs = \"assets/prefabs\"") != std::string::npos);
	CHECK(saved.find("scenes = \"scenes\"") != std::string::npos);
	CHECK(saved.find("scripts = \"scripts\"") != std::string::npos);
	CHECK(saved.find("[project]") != std::string::npos);
	CHECK(saved.find("name = \"Whisper\"") != std::string::npos);
	CHECK(saved.find("[graphics]") != std::string::npos);
	CHECK(saved.find("targetfps") != std::string::npos);
}

// Reload what we just wrote: the values must survive a full round trip, not merely
// appear somewhere in the text.
TEST_CASE("A saved project file reloads with the same values")
{
	TomlConfig first;
	first.Load(
	        "[app]\nstartupscene = \"Title\"\n\n[paths]\nassets = \"assets\"\nscenes = \"scenes\"\n\n[project]\nname = \"Whisper\"\nversion = 1\n");
	first.Set("app.startupscene", "Arena");

	std::ostringstream out;
	first.Save(out, "AetherCore project file.");

	TomlConfig second;
	second.Load(out.str());

	CHECK(second.GetString("app.startupscene") == "Arena");
	CHECK(second.GetString("paths.assets") == "assets");
	CHECK(second.GetString("paths.scenes") == "scenes");
	CHECK(second.GetString("project.name") == "Whisper");
}
