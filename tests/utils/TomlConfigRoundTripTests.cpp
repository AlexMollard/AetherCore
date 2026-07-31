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
	REQUIRE(config.Load(original));
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
	REQUIRE(first.Load(
	        "[app]\nstartupscene = \"Title\"\n\n[paths]\nassets = \"assets\"\nscenes = \"scenes\"\n\n[project]\nname = \"Whisper\"\nversion = 1\n"));
	first.Set("app.startupscene", "Arena");

	std::ostringstream out;
	first.Save(out, "AetherCore project file.");

	TomlConfig second;
	REQUIRE(second.Load(out.str()));

	CHECK(second.GetString("app.startupscene") == "Arena");
	CHECK(second.GetString("paths.assets") == "assets");
	CHECK(second.GetString("paths.scenes") == "scenes");
	CHECK(second.GetString("project.name") == "Whisper");
}

// The corruption that destroyed a project file. Load decodes a quoted string to its raw
// value, so Save has to re-quote it - and a Windows path re-quoted without escaping is
// `"D:\AetherCore\..."`, where \A is an invalid TOML escape. The document then stops
// parsing, the next Load yields nothing, and the next Save writes a file containing only
// the keys it explicitly Set.
TEST_CASE("A backslash path survives repeated load/save cycles")
{
	const std::string original =
	        "[app]\nstartupscene = \"Title\"\n\n"
	        "[publish]\noutputroot = \"D:\\\\AetherCore\\\\projects\\\\Whisper\\\\Builds\"\n\n"
	        "[project]\nname = \"Whisper\"\n";

	std::string text = original;
	for (int cycle = 0; cycle < 3; ++cycle)
	{
		TomlConfig config;
		REQUIRE_MESSAGE(config.Load(text), "document stopped parsing on cycle " << cycle);
		// Only the publish keys are re-Set, exactly as the publish dialog does. Everything
		// else is re-emitted from the loaded values, which is where the escaping was lost.
		config.Set("publish.cleanoutput", true);

		std::ostringstream out;
		config.Save(out, "AetherCore project file.");
		text = out.str();

		INFO("cycle " << cycle << " wrote:\n" << text);
		CHECK(config.GetString("publish.outputroot") == "D:\\AetherCore\\projects\\Whisper\\Builds");
		CHECK(text.find("startupscene = \"Title\"") != std::string::npos);
		CHECK(text.find("name = \"Whisper\"") != std::string::npos);
	}
}

TEST_CASE("A quote inside a value survives a load/save cycle")
{
	TomlConfig first;
	REQUIRE(first.Load("[project]\nname = \"He said \\\"hi\\\"\"\n"));

	std::ostringstream out;
	first.Save(out);

	TomlConfig second;
	REQUIRE(second.Load(out.str()));
	CHECK(second.GetString("project.name") == "He said \"hi\"");
}

// Load must report failure rather than leave an empty config that looks successfully
// loaded - every read-modify-write caller keys its refuse-to-save decision off this.
TEST_CASE("Load reports failure on a document that does not parse")
{
	TomlConfig config;
	CHECK_FALSE(config.Load("[app\nthis is = = not toml\n"));
	CHECK_FALSE(config.Has("app.startupscene"));
}
