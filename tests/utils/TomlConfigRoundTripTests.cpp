#include <doctest/doctest.h>

#include <sstream>
#include <string>

#include "utils/TomlConfig.hpp"
#include "utils/EngineSettings.hpp"
#include "utils/TomlParse.hpp"
#include "io/FileUtil.hpp"
#include <filesystem>

using namespace aether;

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

TEST_CASE("Project settings save preserves typed arrays and lighting precision")
{
	const auto path = std::filesystem::temp_directory_path() / "aether_toml_project_roundtrip_test.toml";
	const std::string original =
	        "[publish.web]\n"
	        "additional_assets = [\"project://assets/credits.txt\", \"project://assets/with space.txt\", "
	        "\"C:\\\\assets\\\\quoted\\\"file.txt\"]\n"
	        "options = { label = \"true\", count = 2 }\n"
	        "[graphics]\n"
	        "bloomstrength = 0.035\nfilmgrain = 0.015\nexposurespeed = 0.8\n"
	        "[project]\nname = \"123\"\nversion = 1\n";
	REQUIRE(io::file_util::WriteText(path, original).has_value());
	EngineSettings shipped;
	EngineSettings values = shipped;
	EngineSettingsIO::Apply(original, values);
	std::string error;
	REQUIRE_MESSAGE(EngineSettingsIO::SaveProjectOverrides(values, shipped, path, error), error);
	const auto saved = io::file_util::ReadText(path);
	REQUIRE(saved);
	const auto parsed = ParseTomlDocument(*saved);
	REQUIRE(parsed);
	const auto* assets = (*parsed)["publish"]["web"]["additional_assets"].as_array();
	REQUIRE(assets);
	REQUIRE(assets->size() == 3);
	CHECK((*assets)[0].value<std::string>() == "project://assets/credits.txt");
	CHECK((*assets)[1].value<std::string>() == "project://assets/with space.txt");
	CHECK((*assets)[2].value<std::string>() == "C:\\assets\\quoted\"file.txt");
	CHECK((*parsed)["publish"]["web"]["options"]["label"].value<std::string>() == "true");
	CHECK((*parsed)["publish"]["web"]["options"]["count"].value<int>() == 2);
	CHECK((*parsed)["project"]["name"].value<std::string>() == "123");
	CHECK((*parsed)["project"]["version"].value<int>() == 1);
	EngineSettings reloaded = shipped;
	EngineSettingsIO::Apply(*saved, reloaded);
	CHECK(reloaded.graphics.bloomStrength == 0.035f);
	CHECK(reloaded.graphics.filmGrain == 0.015f);
	CHECK(reloaded.graphics.exposureSpeed == 0.8f);
	std::error_code ec;
	std::filesystem::remove(path, ec);
}

TEST_CASE("Changing a configuration scalar type marks the new value dirty")
{
	TomlConfig config;
	REQUIRE(config.Load("value = 1\n"));
	config.Set("value", "1");
	CHECK(config.IsDirty());
	std::ostringstream stringOutput;
	config.Save(stringOutput);
	const auto stringDocument = ParseTomlDocument(stringOutput.str());
	REQUIRE(stringDocument);
	CHECK((*stringDocument)["value"].value<std::string>() == "1");

	config.MarkClean();
	config.Set("value", 1);
	CHECK(config.IsDirty());
	std::ostringstream integerOutput;
	config.Save(integerOutput);
	const auto integerDocument = ParseTomlDocument(integerOutput.str());
	REQUIRE(integerDocument);
	CHECK((*integerDocument)["value"].value<int>() == 1);
}
