// A scene name is pasted straight into a filename. Saving as "bad:name" wrote the scene into
// an NTFS alternate data stream, left a 0-byte file called "bad", and still reported success
// - so the work was not where the user had been told it was.
#include <doctest/doctest.h>

#include "scene/SceneSerializer.hpp"

using aether::app::scene::IsValidSceneName;

TEST_CASE("Ordinary scene names are accepted")
{
	CHECK(IsValidSceneName("Testing"));
	CHECK(IsValidSceneName("Level 1"));
	CHECK(IsValidSceneName("boss-fight_02"));
	CHECK(IsValidSceneName("scene.with.dots"));
}

TEST_CASE("Names that would not land where they say are rejected")
{
	// The one that caused real damage: a colon starts an NTFS stream.
	CHECK_FALSE(IsValidSceneName("bad:name"));
	// Separators would write outside the scenes directory.
	CHECK_FALSE(IsValidSceneName("sub/dir"));
	CHECK_FALSE(IsValidSceneName("sub\\dir"));
	CHECK_FALSE(IsValidSceneName("../escape"));
	// Wildcards and the rest of the Windows-reserved set.
	CHECK_FALSE(IsValidSceneName("what?"));
	CHECK_FALSE(IsValidSceneName("star*"));
	CHECK_FALSE(IsValidSceneName("pipe|name"));
	CHECK_FALSE(IsValidSceneName("quote\"name"));
	CHECK_FALSE(IsValidSceneName("angle<name>"));
	// Control characters and the empty name.
	CHECK_FALSE(IsValidSceneName(std::string_view{"tab\there"}));
	CHECK_FALSE(IsValidSceneName(""));
	// A name of only dots is a directory, not a scene.
	CHECK_FALSE(IsValidSceneName("."));
	CHECK_FALSE(IsValidSceneName(".."));
}
