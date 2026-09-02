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

// The predicate is only useful if the save functions actually consult it. These call the two
// that had the bug; a rejected name returns before touching the filesystem, so the cases are
// side-effect free and do not need a scratch directory.
TEST_CASE("The save functions refuse a name that would not land where it says")
{
	const aether::app::scene::SceneDescription empty;

	CHECK_FALSE(aether::app::scene::SaveSceneFile("bad:name", empty));
	CHECK_FALSE(aether::app::scene::SaveSceneFile("sub/dir", empty));
	CHECK_FALSE(aether::app::scene::SaveSceneFile("", empty));

	// Prefab saving has no control method - it is reached only through the hierarchy's
	// context menu, which the UI automation cannot drive - so this is the only place the
	// guard on that path is exercised at all.
	CHECK_FALSE(aether::app::scene::SavePrefabFile("bad:name", empty));
	CHECK_FALSE(aether::app::scene::SavePrefabFile("../escape", empty));
	CHECK_FALSE(aether::app::scene::SavePrefabFile("", empty));
}
