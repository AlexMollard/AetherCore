#include <ostream>

#include <doctest/doctest.h>

#include "assets/AssetTypes.hpp"

using namespace aether;

TEST_CASE("2D asset sources have stable type-separated identities")
{
	const AssetSource atlas = MakeSpriteAtlasSource("project://sprites/hero.atlas.toml");
	const AssetSource animation = MakeSpriteAnimationSource("project://sprites/hero.atlas.toml");

	CHECK(ComputeAssetId(atlas) == ComputeAssetId(atlas));
	CHECK(ComputeAssetId(atlas) != ComputeAssetId(animation));
	CHECK(std::string_view{AssetTypeName(atlas.type)} == "SpriteAtlas");
}

TEST_CASE("asset object identity survives authored rename and reorder")
{
	const AssetId atlas = ComputeAssetId(MakeSpriteAtlasSource("project://sprites/hero.atlas.toml"));
	const AssetObjectId idle = ComputeAssetObjectId(atlas, "019f65a8-72ad-7000-8000-000000000001");
	const AssetObjectId run = ComputeAssetObjectId(atlas, "019f65a8-72ad-7000-8000-000000000002");

	CHECK(idle.IsValid());
	CHECK(idle == ComputeAssetObjectId(atlas, "019f65a8-72ad-7000-8000-000000000001"));
	CHECK(idle != run);
}
