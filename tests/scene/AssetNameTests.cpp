// An asset name - a scene, a prefab, a tileset - is pasted straight into a filename. Saving a
// scene as "bad:name" wrote it into an NTFS alternate data stream, left a 0-byte file called
// "bad", and still reported success, so the work was not where the user had been told it was.
// The same predicate now guards prefab and tileset names.
#include <doctest/doctest.h>

#include "scene/SceneSerializer.hpp"

using aether::app::scene::IsValidAssetName;

TEST_CASE("Ordinary asset names are accepted")
{
	CHECK(IsValidAssetName("Testing"));
	CHECK(IsValidAssetName("Level 1"));
	CHECK(IsValidAssetName("boss-fight_02"));
	CHECK(IsValidAssetName("scene.with.dots"));
}

TEST_CASE("Names that would not land where they say are rejected")
{
	// The one that caused real damage: a colon starts an NTFS stream.
	CHECK_FALSE(IsValidAssetName("bad:name"));
	// Separators would write outside the scenes directory.
	CHECK_FALSE(IsValidAssetName("sub/dir"));
	CHECK_FALSE(IsValidAssetName("sub\\dir"));
	CHECK_FALSE(IsValidAssetName("../escape"));
	// Wildcards and the rest of the Windows-reserved set.
	CHECK_FALSE(IsValidAssetName("what?"));
	CHECK_FALSE(IsValidAssetName("star*"));
	CHECK_FALSE(IsValidAssetName("pipe|name"));
	CHECK_FALSE(IsValidAssetName("quote\"name"));
	CHECK_FALSE(IsValidAssetName("angle<name>"));
	// Control characters and the empty name.
	CHECK_FALSE(IsValidAssetName(std::string_view{"tab\there"}));
	CHECK_FALSE(IsValidAssetName(""));
	// A name of only dots is a directory, not a scene.
	CHECK_FALSE(IsValidAssetName("."));
	CHECK_FALSE(IsValidAssetName(".."));
}
