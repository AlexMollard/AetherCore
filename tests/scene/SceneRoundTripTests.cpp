// Saving a scene serialises a capture, not the file it came from. If that text is not a
// fixed point, every save rewrites parts of the scene nobody edited - which shows up as
// diff noise in review, as merge conflicts between two people who touched nothing in
// common, and as autosave recovery copies that look like unsaved work.
//
// Two properties, and they are different questions:
//   1. Re-serialising is stable  - write(parse(x)) == write(parse(write(parse(x)))).
//   2. What ships is canonical   - the scenes the engine hands a new project survive a
//      save unchanged, so a user's first diff is not full of edits they did not make.
//      Comments are the one thing that legitimately goes; see Normalised.
#include <doctest/doctest.h>

#include <cstddef>
#include <filesystem>
#include <string>

#include "io/FileUtil.hpp"
#include "scene/SceneSerializer.hpp"

using aether::app::scene::ParseToml;
using aether::app::scene::WriteToml;

namespace
{
	std::string RoundTrip(const std::string& toml)
	{
		const auto parsed = ParseToml(toml);
		REQUIRE(parsed.has_value());
		return WriteToml(*parsed, true);
	}

#ifdef AETHER_SCENES_SOURCE_DIR
	std::string ReadShippedScene(const char* name)
	{
		const std::filesystem::path path = std::filesystem::path(AETHER_SCENES_SOURCE_DIR) / name;
		const auto text = aether::io::file_util::ReadText(path);
		REQUIRE_MESSAGE(text.has_value(), "could not read " << path.string());
		return *text;
	}

	// Line endings, blank lines, and COMMENTS. A TOML serialiser rebuilds the document from
	// the parsed model, which has nowhere to keep a comment, so the explanatory blocks in
	// the shipped templates do not survive a save. That is inherent to the format, not a
	// defect - what matters is that nothing ELSE changes.
	std::string Normalised(const std::string& text)
	{
		std::string out;
		std::size_t at = 0;
		while (at <= text.size())
		{
			const std::size_t end = text.find('\n', at);
			std::string line = text.substr(at, (end == std::string::npos ? text.size() : end) - at);
			std::erase(line, '\r');
			const std::size_t firstGlyph = line.find_first_not_of(" \t");
			if (firstGlyph != std::string::npos && line[firstGlyph] != '#')
			{
				out += line;
				out += '\n';
			}
			if (end == std::string::npos)
			{
				break;
			}
			at = end + 1;
		}
		return out;
	}
#endif
} // namespace

TEST_CASE("Re-serialising a scene is a fixed point")
{
	// Whatever the first write settles on, the second must agree. If it does not, a scene
	// saved twice with no edits in between produces two different files.
	const std::string authored =
	        "[scene]\nkind = '2d'\nname = 'T'\nversion = 16\n\n"
	        "[[entities]]\nname = 'Parent'\nnode = 11\nparent = -1\nposition = [ 1, 2, 0 ]\n\n"
	        "[[entities]]\nname = 'Child'\nnode = 22\nparent = 0\nparent_node = 11\nposition = [ 3.5, 0.0, 0.0 ]\n";

	const std::string once = RoundTrip(authored);
	const std::string twice = RoundTrip(once);
	CHECK(once == twice);
}

TEST_CASE("Re-serialising preserves parenting")
{
	// parent is a positional index and parent_node is the id it resolves through. A
	// round trip that quietly reparented would be far worse than diff noise.
	const std::string authored =
	        "[scene]\nkind = '3d'\nname = 'T'\nversion = 16\n\n"
	        "[[entities]]\nname = 'Root'\nnode = 11\nparent = -1\nposition = [ 0.0, 0.0, 0.0 ]\n\n"
	        "[[entities]]\nname = 'Kid'\nnode = 22\nparent = 0\nparent_node = 11\nposition = [ 0.0, 1.0, 0.0 ]\n";

	const auto parsed = ParseToml(RoundTrip(authored));
	REQUIRE(parsed.has_value());
	REQUIRE(parsed->entities.size() == 2);
	CHECK(parsed->entities[0].parentIndex == -1);
	CHECK(parsed->entities[1].parentIndex == 0);
	CHECK(parsed->entities[1].name == "Kid");
}

#ifdef AETHER_SCENES_SOURCE_DIR
TEST_CASE("Saving a shipped template changes nothing but its comments")
{
	// If these are not, the first save of a brand new project rewrites the template it
	// was just given, and the user's first diff is full of changes they did not make.
	for (const char* name: {"default.scene.toml", "default2d.scene.toml"})
	{
		const std::string original = ReadShippedScene(name);
		const std::string rewritten = RoundTrip(original);
		INFO("scene: " << name);
		CHECK(Normalised(rewritten) == Normalised(original));
	}
}
#endif
