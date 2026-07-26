// TileMapSystem extraction: chunk culling, dirty-only rebuilds, animation
// patching, sort interleaving with sprites, and the 100k-tile phase gate.
#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <system_error>

#include "assets/SpriteAssetStore.hpp"
#include "assets/SpriteAtlasAsset.hpp"
#include "assets/TileAssetStore.hpp"
#include "material/TextureRegistry.hpp"
#include "rendering/RenderFramePacket.hpp"
#include "rendering/SpriteSystem.hpp"
#include "rendering/TileMapSystem.hpp"
#include "scene/Components.hpp"
#include "scene/TransformUtils.hpp"
#include "scene/World.hpp"
#include "../material/FakeTextureSink.hpp"

using namespace aether;

namespace
{
	std::filesystem::path TempDir()
	{
		// std::filesystem, not getenv("TEMP"): it already consults the platform's temp-dir
		// variables and needs no deprecated CRT call to do it.
		std::error_code ec;
		std::filesystem::path base = std::filesystem::temp_directory_path(ec);
		if (ec)
		{
			base = ".";
		}
		auto dir = base / "aethercore_tilemap_extract_tests";
		std::filesystem::create_directories(dir);
		return dir;
	}

	struct TileWorldFixture
	{
		FakeTextureSink sink;
		TextureRegistry textures{sink};
		SpriteAssetStore sprites;
		TileAssetStore tiles;
		TileMapSystem system;
		World world;

		std::string atlasPath;
		std::string tileSetPath;
		std::string tileMapPath;
		AssetObjectId grassTileId{};
		AssetObjectId waterTileId{}; // animated (2 frames)
		std::uint16_t grassIndex = 0;
		std::uint16_t waterIndex = 0;

		TileWorldFixture()
		{
			textures.InitializeDefault("fallback.png");
			system.Initialize(textures, sprites, tiles);

			// Atlas with three 32x32 regions in a 96x32 texture.
			SpriteAtlasAsset atlas;
			atlas.texturePath = "tiles.png";
			atlas.textureWidth = 96;
			atlas.textureHeight = 32;
			atlas.pixelsPerUnit = 32.0f;
			for (int i = 0; i < 3; ++i)
			{
				auto& region = atlas.AddManualRegion({i * 32, 0, 32, 32}, "tile" + std::to_string(i));
				(void) region;
			}
			atlas.RecalculateUvs(96, 32);
			atlasPath = (TempDir() / "tiles.spriteatlas.toml").generic_string();
			REQUIRE(sprites.SaveAtlas(atlasPath, atlas).has_value());
			const auto atlasLoaded = sprites.LoadAtlas(atlasPath);
			REQUIRE(atlasLoaded.has_value());

			TileSetAsset tileSet;
			tileSet.name = "Terrain";
			tileSet.cellSize = 1.0f;
			tileSet.AddTile(atlasPath, (*atlasLoaded)->sprites[0].id, "Grass");
			{
				auto& water = tileSet.AddTile(atlasPath, (*atlasLoaded)->sprites[1].id, "Water");
				water.animationFrames = {(*atlasLoaded)->sprites[1].id, (*atlasLoaded)->sprites[2].id};
				water.animationFps = 1.0f;
			}
			grassTileId = tileSet.tiles[0].id;
			waterTileId = tileSet.tiles[1].id;
			tileSetPath = (TempDir() / "terrain.tileset.toml").generic_string();
			REQUIRE(tiles.SaveTileSet(tileSetPath, tileSet).has_value());

			TileMapAsset map;
			map.tileSetPath = tileSetPath;
			map.cellSize = 1.0f;
			grassIndex = map.PaletteIndexFor(grassTileId);
			waterIndex = map.PaletteIndexFor(waterTileId);
			map.layers.emplace_back();
			tileMapPath = (TempDir() / "terrain.tilemap").generic_string();
			REQUIRE(tiles.SaveTileMap(tileMapPath, map).has_value());
		}

		[[nodiscard]] TileMapAsset& Map()
		{
			TileMapAsset* map = tiles.MutableTileMap(tileMapPath);
			REQUIRE(map != nullptr);
			return *map;
		}

		Entity MakeTileMapEntity()
		{
			const Entity entity = world.Create();
			world.Emplace<TransformComponent>(entity);
			world.Emplace<TileMapComponent>(entity, TileMapComponent{.tilemapPath = tileMapPath});
			return entity;
		}
	};
} // namespace

TEST_CASE("tile extraction culls by chunk and rebuilds only dirty chunks")
{
	TileWorldFixture fx;
	TileMapAsset& map = fx.Map();
	// Two chunks: cells in chunk (0,0) and chunk (2,0) (x >= 64).
	map.SetCell(0, {1, 1}, tilecell::Make(fx.grassIndex));
	map.SetCell(0, {2, 1}, tilecell::Make(fx.grassIndex, true));
	map.SetCell(0, {70, 1}, tilecell::Make(fx.grassIndex));
	fx.MakeTileMapEntity();

	// Full view: both chunks extract (3 instances).
	Render2DFrameData frame;
	fx.system.Extract(fx.world, View2DBounds{}, 0.0f, frame);
	CHECK(frame.sprites.size() == 3);
	const std::uint64_t afterFirst = fx.system.RebuildCount();
	CHECK(afterFirst == 2); // one rebuild per chunk

	// Flip flag landed on the flipped cell's instance.
	int flipped = 0;
	for (const auto& instance: frame.sprites)
	{
		if ((static_cast<std::uint32_t>(instance.flags) & static_cast<std::uint32_t>(SpriteInstanceFlags::FlipX)) != 0u)
		{
			++flipped;
		}
	}
	CHECK(flipped == 1);

	// Second extract: nothing dirty, zero rebuilds.
	frame.sprites.clear();
	fx.system.Extract(fx.world, View2DBounds{}, 0.0f, frame);
	CHECK(frame.sprites.size() == 3);
	CHECK(fx.system.RebuildCount() == afterFirst);

	// A view covering only chunk (0,0) extracts 2 instances.
	frame.sprites.clear();
	fx.system.Extract(fx.world, View2DBounds{.min = {0.0f, 0.0f}, .max = {10.0f, 10.0f}, .valid = true}, 0.0f, frame);
	CHECK(frame.sprites.size() == 2);

	// Editing one cell dirties exactly one chunk.
	map.SetCell(0, {3, 3}, tilecell::Make(fx.grassIndex));
	frame.sprites.clear();
	fx.system.Extract(fx.world, View2DBounds{}, 0.0f, frame);
	CHECK(frame.sprites.size() == 4);
	CHECK(fx.system.RebuildCount() == afterFirst + 1);
}

TEST_CASE("animated tiles patch UVs per frame without rebuilding the cache")
{
	TileWorldFixture fx;
	TileMapAsset& map = fx.Map();
	map.SetCell(0, {0, 0}, tilecell::Make(fx.waterIndex));
	fx.MakeTileMapEntity();

	Render2DFrameData frameA;
	fx.system.Extract(fx.world, View2DBounds{}, 0.0f, frameA); // frame 0
	REQUIRE(frameA.sprites.size() == 1);
	const std::uint64_t rebuilds = fx.system.RebuildCount();

	Render2DFrameData frameB;
	fx.system.Extract(fx.world, View2DBounds{}, 1.0f, frameB); // 1s at 1 fps -> frame 1
	REQUIRE(frameB.sprites.size() == 1);
	CHECK(fx.system.RebuildCount() == rebuilds); // animation never rebuilds
	CHECK(frameA.sprites[0].uvRect != frameB.sprites[0].uvRect);
}

TEST_CASE("tiles interleave with sprites through the shared sort")
{
	TileWorldFixture fx;
	TileMapAsset& map = fx.Map();
	map.layers[0].sortingLayer = 0;
	map.SetCell(0, {0, 0}, tilecell::Make(fx.grassIndex));
	const Entity mapEntity = fx.MakeTileMapEntity();

	// One sprite below (layer -1) and one above (layer 1).
	SpriteSystem spriteSystem;
	spriteSystem.Initialize(fx.textures, fx.sprites);
	const Entity below = fx.world.Create();
	fx.world.Emplace<TransformComponent>(below);
	fx.world.Emplace<SpriteRendererComponent>(below, SpriteRendererComponent{.texturePath = "b.png", .sortingLayer = -1});
	const Entity above = fx.world.Create();
	fx.world.Emplace<TransformComponent>(above);
	fx.world.Emplace<SpriteRendererComponent>(above, SpriteRendererComponent{.texturePath = "a.png", .sortingLayer = 1});

	Render2DFrameData frame;
	spriteSystem.Extract(fx.world, frame);
	fx.system.Extract(fx.world, View2DBounds{}, 0.0f, frame);
	Finalize2DFrame(frame);

	REQUIRE(frame.sprites.size() == 3);
	CHECK(frame.sprites[0].entityId == below.id);
	CHECK(frame.sprites[1].entityId == mapEntity.id);
	CHECK(frame.sprites[2].entityId == above.id);
	spriteSystem.Shutdown();
}

TEST_CASE("100k tile world: bounded visible work and single-chunk edits")
{
	TileWorldFixture fx;
	TileMapAsset& map = fx.Map();
	// 320 x 320 = 102,400 cells (10 x 10 chunks).
	for (int y = 0; y < 320; ++y)
	{
		for (int x = 0; x < 320; ++x)
		{
			map.SetCell(0, {x, y}, tilecell::Make(fx.grassIndex));
		}
	}
	CHECK(map.TotalCellCount() == 102400);
	fx.MakeTileMapEntity();

	// Full-view extract touches every cell once...
	Render2DFrameData frame;
	fx.system.Extract(fx.world, View2DBounds{}, 0.0f, frame);
	CHECK(frame.sprites.size() == 102400);
	const std::uint64_t initialRebuilds = fx.system.RebuildCount();
	CHECK(initialRebuilds == 100);

	// ...and an unchanged second pass rebuilds nothing.
	frame.sprites.clear();
	fx.system.Extract(fx.world, View2DBounds{}, 0.0f, frame);
	CHECK(fx.system.RebuildCount() == initialRebuilds);

	// A one-chunk viewport keeps resident work bounded.
	frame.sprites.clear();
	fx.system.Extract(fx.world, View2DBounds{.min = {1.0f, 1.0f}, .max = {30.0f, 30.0f}, .valid = true}, 0.0f, frame);
	CHECK(frame.sprites.size() <= 4096); // at most a 2x2 chunk neighbourhood
	CHECK(frame.sprites.size() >= 1024); // and at least the fully covered chunk

	// Editing one cell rebuilds exactly one chunk on the next full pass.
	map.SetCell(0, {100, 100}, tilecell::kEmpty);
	frame.sprites.clear();
	fx.system.Extract(fx.world, View2DBounds{}, 0.0f, frame);
	CHECK(frame.sprites.size() == 102399);
	CHECK(fx.system.RebuildCount() == initialRebuilds + 1);
}
