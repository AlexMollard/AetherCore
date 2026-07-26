// TileSetAsset / TileMapAsset coverage: stable ids, TOML + binary round trips,
// sparse chunk behaviour, revisions, palette dedupe.
#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <system_error>

#include "assets/SpriteAtlasAsset.hpp"
#include "assets/TileAssetStore.hpp"
#include "assets/TileMapAsset.hpp"
#include "assets/TileSetAsset.hpp"

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
		auto dir = base / "aethercore_tile_asset_tests";
		std::filesystem::create_directories(dir);
		return dir;
	}
} // namespace

TEST_CASE("TileSetAsset round-trips through TOML with stable ids")
{
	aether::TileSetAsset tileSet;
	tileSet.name = "Dungeon";
	tileSet.cellSize = 0.5f;

	// NOTE: AddTile returns a reference into the tiles vector; later AddTile
	// calls may reallocate, so configure each tile before adding the next and
	// keep only the ids around.
	tileSet.AddTile("project://assets/dungeon.spriteatlas.toml", aether::AssetObjectId{0x1111}, "Floor").properties["footstep"] = "stone";
	tileSet.AddTile("project://assets/dungeon.spriteatlas.toml", aether::AssetObjectId{0x2222}, "Wall").collision = aether::TileCollisionKind::Full;
	{
		auto& torch = tileSet.AddTile("project://assets/dungeon.spriteatlas.toml", aether::AssetObjectId{0x3333}, "Torch");
		torch.animationFrames = {aether::AssetObjectId{0x3333}, aether::AssetObjectId{0x4444}, aether::AssetObjectId{0x5555}};
		torch.animationFps = 6.0f;
	}
	const aether::AssetObjectId floorId = tileSet.tiles[0].id;
	const aether::AssetObjectId wallId = tileSet.tiles[1].id;
	const aether::AssetObjectId torchId = tileSet.tiles[2].id;

	CHECK(floorId.IsValid());
	CHECK(floorId != wallId);
	CHECK(wallId != torchId);

	const auto path = TempDir() / "dungeon.tileset.toml";
	REQUIRE(tileSet.Save(path).has_value());
	const auto loaded = aether::TileSetAsset::Load(path);
	REQUIRE(loaded.has_value());

	CHECK(loaded->name == "Dungeon");
	CHECK(loaded->cellSize == doctest::Approx(0.5f));
	REQUIRE(loaded->tiles.size() == 3);
	CHECK(loaded->tiles[0].id == floorId);
	CHECK(loaded->tiles[0].atlasPath == "project://assets/dungeon.spriteatlas.toml");
	CHECK(loaded->tiles[0].spriteId == aether::AssetObjectId{0x1111});
	CHECK(loaded->tiles[0].properties.at("footstep") == "stone");
	CHECK(loaded->tiles[1].collision == aether::TileCollisionKind::Full);
	REQUIRE(loaded->tiles[2].animationFrames.size() == 3);
	CHECK(loaded->tiles[2].animationFrames[1] == aether::AssetObjectId{0x4444});
	CHECK(loaded->tiles[2].animationFps == doctest::Approx(6.0f));

	CHECK(loaded->Find(wallId) != nullptr);
	CHECK(loaded->Find(aether::AssetObjectId{0xdead}) == nullptr);

	CHECK_FALSE(aether::TileSetAsset::Load(TempDir() / "missing.tileset.toml").has_value());
}

TEST_CASE("TileMap cells map to sparse chunks with floor division")
{
	// Pin the cell -> chunk convention for negatives.
	CHECK(aether::ChunkKeyFor({0, 0}) == aether::TileChunkKey{0, 0});
	CHECK(aether::ChunkKeyFor({31, 31}) == aether::TileChunkKey{0, 0});
	CHECK(aether::ChunkKeyFor({32, 0}) == aether::TileChunkKey{1, 0});
	CHECK(aether::ChunkKeyFor({-1, -1}) == aether::TileChunkKey{-1, -1});
	CHECK(aether::ChunkKeyFor({-33, 5}) == aether::TileChunkKey{-2, 0});
	CHECK(aether::CellIndexInChunk({0, 0}) == 0);
	CHECK(aether::CellIndexInChunk({-1, -1}) == static_cast<std::size_t>(31 * 32 + 31));

	aether::TileMapAsset map;
	map.layers.emplace_back();

	const std::uint32_t cell = aether::tilecell::Make(4, true, false);
	map.SetCell(0, {-1, -1}, cell);
	CHECK(map.GetCell(0, {-1, -1}) == cell);
	CHECK(map.GetCell(0, {0, 0}) == aether::tilecell::kEmpty);
	CHECK(map.layers[0].chunks.size() == 1);
	CHECK(map.TotalCellCount() == 1);

	// Revision bumps on change, not on an identical write.
	const std::uint32_t before = map.layers[0].chunks.at({-1, -1}).revision;
	map.SetCell(0, {-1, -1}, cell);
	CHECK(map.layers[0].chunks.at({-1, -1}).revision == before);
	map.SetCell(0, {-2, -1}, aether::tilecell::Make(1));
	CHECK(map.layers[0].chunks.at({-1, -1}).revision == before + 1);

	// Clearing the last cells prunes the chunk.
	map.SetCell(0, {-1, -1}, aether::tilecell::kEmpty);
	map.SetCell(0, {-2, -1}, aether::tilecell::kEmpty);
	CHECK(map.layers[0].chunks.empty());
	CHECK(map.TotalCellCount() == 0);

	// Palette dedupes by tile id.
	CHECK(map.PaletteIndexFor(aether::AssetObjectId{7}) == 0);
	CHECK(map.PaletteIndexFor(aether::AssetObjectId{9}) == 1);
	CHECK(map.PaletteIndexFor(aether::AssetObjectId{7}) == 0);
}

TEST_CASE("TileMapAsset round-trips through the binary format")
{
	aether::TileMapAsset map;
	map.tileSetPath = "project://assets/dungeon.tileset.toml";
	map.cellSize = 0.5f;
	const std::uint16_t floorIndex = map.PaletteIndexFor(aether::AssetObjectId{0x1111});
	const std::uint16_t wallIndex = map.PaletteIndexFor(aether::AssetObjectId{0x2222});

	map.layers.resize(2);
	map.layers[0].name = "Ground";
	map.layers[0].tint = {0.9f, 0.8f, 0.7f, 1.0f};
	map.layers[0].sortingLayer = -1;
	map.layers[1].name = "Walls";
	map.layers[1].opacity = 0.75f;
	map.layers[1].orderInLayer = 3;
	map.layers[1].collision = true;
	map.layers[1].visible = false;

	for (int x = -40; x < 40; ++x)
	{
		map.SetCell(0, {x, 0}, aether::tilecell::Make(floorIndex));
	}
	map.SetCell(1, {5, 5}, aether::tilecell::Make(wallIndex, true, true));

	const auto path = TempDir() / "room.tilemap";
	REQUIRE(map.Save(path).has_value());
	const auto loaded = aether::TileMapAsset::Load(path);
	REQUIRE(loaded.has_value());

	CHECK(loaded->tileSetPath == map.tileSetPath);
	CHECK(loaded->cellSize == doctest::Approx(0.5f));
	REQUIRE(loaded->tilePalette.size() == 2);
	CHECK(loaded->tilePalette[1] == aether::AssetObjectId{0x2222});
	REQUIRE(loaded->layers.size() == 2);
	CHECK(loaded->layers[0].name == "Ground");
	CHECK(loaded->layers[0].tint.r == doctest::Approx(0.9f));
	CHECK(loaded->layers[0].sortingLayer == -1);
	CHECK(loaded->layers[1].opacity == doctest::Approx(0.75f));
	CHECK(loaded->layers[1].orderInLayer == 3);
	CHECK_FALSE(loaded->layers[1].visible);

	CHECK(loaded->TotalCellCount() == 81);
	CHECK(loaded->GetCell(0, {-40, 0}) == aether::tilecell::Make(floorIndex));
	CHECK(loaded->GetCell(0, {39, 0}) == aether::tilecell::Make(floorIndex));
	const std::uint32_t wallCell = loaded->GetCell(1, {5, 5});
	CHECK(aether::tilecell::PaletteIndex(wallCell) == wallIndex);
	CHECK((wallCell & aether::tilecell::kFlipX) != 0);
	CHECK((wallCell & aether::tilecell::kFlipY) != 0);

	// x in [-40, 39] spans chunk columns floor(-40/32) = -2 through floor(39/32) = 1.
	CHECK(loaded->layers[0].chunks.size() == 4);
}

// Not a test: run explicitly (-tc="generate tile demo assets" -nts) to author
// the TestingProject demo tile assets through the real serialization path.
TEST_CASE("generate tile demo assets" * doctest::skip())
{
	const std::filesystem::path repo = std::filesystem::path(AETHER_TESTS_SOURCE_DIR).parent_path();
	const std::filesystem::path outDir = repo / "projects/TestingProject/assets/tilemaps";
	std::filesystem::create_directories(outDir);

	// tex_DebugUVTiles.png is a 2048x2048 8x8 labelled grid.
	constexpr std::int32_t kTextureSize = 2048;
	aether::SpriteSliceSettings slice;
	slice.cellWidth = kTextureSize / 8;
	slice.cellHeight = kTextureSize / 8;
	aether::SpriteAtlasAsset atlas = aether::SpriteAtlasAsset::SliceGrid("project://assets/textures/tex_DebugUVTiles.png", kTextureSize, kTextureSize, slice);
	REQUIRE(atlas.sprites.size() == 64);
	atlas.pixelsPerUnit = static_cast<float>(kTextureSize) / 8.0f; // 1 world unit per tile
	REQUIRE(atlas.Save(outDir / "uvtiles.spriteatlas.toml").has_value());

	aether::TileSetAsset tileSet;
	tileSet.name = "UV Tiles";
	tileSet.cellSize = 1.0f;
	for (std::size_t i = 0; i < 16; ++i) // first two atlas rows as paintable tiles
	{
		auto& tile = tileSet.AddTile("project://assets/tilemaps/uvtiles.spriteatlas.toml", atlas.sprites[i].id, atlas.sprites[i].name);
		tile.collision = i < 8 ? aether::TileCollisionKind::Full : aether::TileCollisionKind::None; // first row solid
	}
	REQUIRE(tileSet.Save(outDir / "uvtiles.tileset.toml").has_value());

	aether::TileMapAsset map;
	map.tileSetPath = "project://assets/tilemaps/uvtiles.tileset.toml";
	map.cellSize = 1.0f;
	map.layers.emplace_back();
	map.layers[0].name = "Ground";
	// A solid tile floor and a floating platform, plus a decorative row.
	for (int x = -12; x <= 12; ++x)
	{
		map.SetCell(0, {x, -6}, aether::tilecell::Make(map.PaletteIndexFor(tileSet.tiles[static_cast<std::size_t>((x + 12) % 8)].id)));
	}
	for (int x = 2; x <= 6; ++x)
	{
		map.SetCell(0, {x, -2}, aether::tilecell::Make(map.PaletteIndexFor(tileSet.tiles[3].id)));
	}
	for (int x = -8; x <= -4; ++x)
	{
		map.SetCell(0, {x, 1}, aether::tilecell::Make(map.PaletteIndexFor(tileSet.tiles[10].id), (x % 2) == 0)); // decorative, some flipped
	}
	REQUIRE(map.Save(outDir / "uvtiles.tilemap").has_value());
	CHECK(map.TotalCellCount() == 35);
}

TEST_CASE("TileAssetStore caches and invalidates")
{
	const auto setPath = (TempDir() / "cache.tileset.toml").generic_string();
	aether::TileSetAsset tileSet;
	tileSet.name = "Cache";
	tileSet.AddTile("a.toml", aether::AssetObjectId{1}, "T");

	aether::TileAssetStore store;
	REQUIRE(store.SaveTileSet(setPath, tileSet).has_value());
	const auto first = store.LoadTileSet(setPath);
	REQUIRE(first.has_value());
	const auto second = store.LoadTileSet(setPath);
	REQUIRE(second.has_value());
	CHECK(*first == *second); // same cached pointer

	store.Invalidate(setPath);
	const auto third = store.LoadTileSet(setPath);
	REQUIRE(third.has_value());
	CHECK((*third)->name == "Cache");
}

TEST_CASE("FlushDirtyTileMaps persists in-memory tile edits to disk")
{
	const auto mapPath = (TempDir() / "flush.atlm").generic_string();

	// Seed a saved map with one cell, then load it into the store (clean cache).
	aether::TileMapAsset seed;
	seed.layers.emplace_back();
	seed.SetCell(0, {2, 3}, aether::tilecell::Make(0));
	seed.SetCell(0, {5, 5}, aether::tilecell::Make(0));

	aether::TileAssetStore store;
	REQUIRE(store.SaveTileMap(mapPath, seed).has_value());
	CHECK_FALSE(store.AnyTileMapDirty()); // SaveTileMap leaves the cache clean

	// Edit through the editor-facing mutable accessor (as painting/erasing does).
	aether::TileMapAsset* live = store.MutableTileMap(mapPath);
	REQUIRE(live != nullptr);
	live->SetCell(0, {5, 5}, aether::tilecell::kEmpty); // erase a tile
	live->SetCell(0, {9, 1}, aether::tilecell::Make(0)); // paint a new one
	CHECK(store.AnyTileMapDirty());

	// Before flushing, the on-disk file still holds the old cells.
	{
		const auto onDisk = aether::TileMapAsset::Load(mapPath);
		REQUIRE(onDisk.has_value());
		CHECK(onDisk->GetCell(0, {5, 5}) != aether::tilecell::kEmpty); // still there on disk
		CHECK(onDisk->GetCell(0, {9, 1}) == aether::tilecell::kEmpty); // not yet on disk
	}

	REQUIRE(store.FlushDirtyTileMaps().has_value());
	CHECK_FALSE(store.AnyTileMapDirty());

	// After flushing, disk matches the in-memory edits - what publish would pack.
	const auto flushed = aether::TileMapAsset::Load(mapPath);
	REQUIRE(flushed.has_value());
	CHECK(flushed->GetCell(0, {5, 5}) == aether::tilecell::kEmpty); // erase persisted
	CHECK(flushed->GetCell(0, {9, 1}) != aether::tilecell::kEmpty); // paint persisted
	CHECK(flushed->GetCell(0, {2, 3}) != aether::tilecell::kEmpty); // untouched cell intact
}

TEST_CASE("FlushDirtyTileMaps isolates one bad map and still flushes the healthy ones")
{
	const auto goodPath = (TempDir() / "flush_good.atlm").generic_string();
	const auto badPath = (TempDir() / "flush_bad.atlm").generic_string();

	aether::TileMapAsset seed;
	seed.layers.emplace_back();

	aether::TileAssetStore store;
	REQUIRE(store.SaveTileMap(goodPath, seed).has_value());
	REQUIRE(store.SaveTileMap(badPath, seed).has_value());

	// Dirty both maps through the editor-facing accessor.
	store.MutableTileMap(goodPath)->SetCell(0, {1, 1}, aether::tilecell::Make(0));
	store.MutableTileMap(badPath)->SetCell(0, {2, 2}, aether::tilecell::Make(0));
	CHECK(store.AnyTileMapDirty());

	// Make badPath unwritable as a file by replacing it with a directory - Save's
	// ofstream cannot open a directory, so that one map fails while the other saves.
	std::error_code ec;
	std::filesystem::remove(badPath, ec);
	std::filesystem::create_directory(badPath, ec);
	REQUIRE(std::filesystem::is_directory(badPath, ec));

	const auto result = store.FlushDirtyTileMaps();
	CHECK_FALSE(result.has_value()); // surfaced the failure

	// The healthy map flushed and is clean; the bad one stays dirty for a later retry.
	CHECK(store.AnyTileMapDirty());
	const auto good = aether::TileMapAsset::Load(goodPath);
	REQUIRE(good.has_value());
	CHECK(good->GetCell(0, {1, 1}) != aether::tilecell::kEmpty);

	std::filesystem::remove_all(badPath, ec); // cleanup
}

TEST_CASE("Snapshot/RestoreTileMaps reverts play-time tile edits")
{
	const auto path = (TempDir() / "playrevert.atlm").generic_string();
	aether::TileMapAsset seed;
	seed.layers.emplace_back();
	seed.SetCell(0, {0, 0}, aether::tilecell::Make(0));

	aether::TileAssetStore store;
	REQUIRE(store.SaveTileMap(path, seed).has_value());

	// Snapshot at "play start", then simulate a script painting/erasing during play.
	auto snapshot = store.SnapshotTileMaps();
	aether::TileMapAsset* live = store.MutableTileMap(path);
	REQUIRE(live != nullptr);
	live->SetCell(0, {0, 0}, aether::tilecell::kEmpty);  // erase the authored tile
	live->SetCell(0, {7, 7}, aether::tilecell::Make(0)); // paint a runtime tile
	CHECK(store.MutableTileMap(path)->GetCell(0, {7, 7}) != aether::tilecell::kEmpty);

	// "Stop" restores the snapshot: play-time edits are gone, authored state is back.
	store.RestoreTileMaps(std::move(snapshot));
	const aether::TileMapAsset* reverted = store.MutableTileMap(path);
	REQUIRE(reverted != nullptr);
	CHECK(reverted->GetCell(0, {0, 0}) != aether::tilecell::kEmpty); // authored tile restored
	CHECK(reverted->GetCell(0, {7, 7}) == aether::tilecell::kEmpty); // runtime paint reverted
	CHECK_FALSE(store.AnyTileMapDirty()); // snapshot was clean, so restore is clean
}
