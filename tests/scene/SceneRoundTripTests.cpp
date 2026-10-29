// Saving a scene serialises a capture, not the file it came from. If that text is not a
// fixed point, every save rewrites parts of the scene nobody edited - which shows up as
// diff noise in review, as merge conflicts between two people who touched nothing in
// common, and as autosave recovery copies that look like unsaved work.
//
// Re-serialisation must settle after schema upgrades, and preserve scene parenting.
#include <doctest/doctest.h>

#include <string>

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
