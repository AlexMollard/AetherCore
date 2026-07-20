// Tile collision: greedy rect merging and incremental per-chunk static bodies.
#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>

#include "assets/TileAssetStore.hpp"
#include "physics2d/Physics2DComponents.hpp"
#include "physics2d/Physics2DSystem.hpp"
#include "physics2d/TileMapCollision.hpp"
#include "scene/Components.hpp"
#include "scene/TransformUtils.hpp"
#include "scene/World.hpp"

using namespace aether;

namespace
{
	std::filesystem::path TempDir()
	{
		const char* base = std::getenv("TEMP");
		auto dir = std::filesystem::path(base != nullptr ? base : ".") / "aethercore_tile_collision_tests";
		std::filesystem::create_directories(dir);
		return dir;
	}

	std::array<std::uint32_t, kTileChunkCellCount> CellsWith(std::initializer_list<glm::ivec2> solidCells)
	{
		std::array<std::uint32_t, kTileChunkCellCount> cells{};
		for (const glm::ivec2 cell: solidCells)
		{
			cells[static_cast<std::size_t>(cell.y) * kTileChunkSize + static_cast<std::size_t>(cell.x)] = tilecell::Make(0);
		}
		return cells;
	}

	const auto kAnySolid = [](std::uint32_t cell) { return !tilecell::Empty(cell); };
} // namespace

TEST_CASE("greedy merge covers solid cells exactly once")
{
	// Empty chunk -> nothing.
	CHECK(MergeSolidCells(CellsWith({}), kAnySolid).empty());

	// Full chunk -> a single rect.
	std::array<std::uint32_t, kTileChunkCellCount> full{};
	full.fill(tilecell::Make(0));
	const auto fullRects = MergeSolidCells(full, kAnySolid);
	REQUIRE(fullRects.size() == 1);
	CHECK(fullRects[0] == TileRect{0, 0, kTileChunkSize, kTileChunkSize});

	// One row -> one rect.
	auto row = CellsWith({});
	for (int x = 0; x < kTileChunkSize; ++x)
	{
		row[static_cast<std::size_t>(x)] = tilecell::Make(0);
	}
	const auto rowRects = MergeSolidCells(row, kAnySolid);
	REQUIRE(rowRects.size() == 1);
	CHECK(rowRects[0] == TileRect{0, 0, kTileChunkSize, 1});

	// L-shape -> two rects covering 5 cells total.
	const auto lShape = MergeSolidCells(CellsWith({{0, 0}, {0, 1}, {0, 2}, {1, 2}, {2, 2}}), kAnySolid);
	std::int32_t covered = 0;
	for (const TileRect& rect: lShape)
	{
		covered += rect.w * rect.h;
	}
	CHECK(covered == 5);
	CHECK(lShape.size() == 2);

	// Checkerboard 4x4 -> 8 unit rects.
	const auto checker = MergeSolidCells(CellsWith({{0, 0}, {2, 0}, {1, 1}, {3, 1}, {0, 2}, {2, 2}, {1, 3}, {3, 3}}), kAnySolid);
	CHECK(checker.size() == 8);
	for (const TileRect& rect: checker)
	{
		CHECK(rect.w == 1);
		CHECK(rect.h == 1);
	}
}

namespace
{
	struct TileCollisionFixture
	{
		World world;
		Physics2DSystem* physics = nullptr;
		TileAssetStore tiles;
		std::string tileMapPath;
		AssetObjectId solidTileId{};
		std::uint16_t solidIndex = 0;

		TileCollisionFixture()
		{
			world.SetSceneKind(SceneKind::Scene2D);
			world.SetSceneFeatures(DefaultSceneFeatures(SceneKind::Scene2D));
			auto system = std::make_unique<Physics2DSystem>();
			physics = system.get();
			physics->SetTileAssets(&tiles);
			world.RegisterSystem(std::move(system));

			TileSetAsset tileSet;
			tileSet.name = "Solid";
			tileSet.AddTile("atlas.toml", AssetObjectId{1}, "Block").collision = TileCollisionKind::Full;
			solidTileId = tileSet.tiles[0].id;
			const auto setPath = (TempDir() / "solid.tileset.toml").generic_string();
			REQUIRE(tiles.SaveTileSet(setPath, tileSet).has_value());

			TileMapAsset map;
			map.tileSetPath = setPath;
			map.cellSize = 1.0f;
			solidIndex = map.PaletteIndexFor(solidTileId);
			map.layers.emplace_back();
			tileMapPath = (TempDir() / "solid.tilemap").generic_string();
			REQUIRE(tiles.SaveTileMap(tileMapPath, map).has_value());
		}

		TileMapAsset& Map()
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

		Entity MakeFallingBox(glm::vec2 position)
		{
			const Entity entity = world.Create();
			world.Emplace<TransformComponent>(entity, TransformComponent{.localToWorld = ComposeTransform({position.x, position.y, 0.0f}, {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f})});
			world.Emplace<RigidBody2DComponent>(entity);
			world.Emplace<Collider2DComponent>(entity, Collider2DComponent{.size = {1.0f, 1.0f}});
			return entity;
		}

		void StepSeconds(float seconds)
		{
			const int frames = static_cast<int>(seconds / Physics2DSystem::kFixedTimestep + 0.5f);
			for (int i = 0; i < frames; ++i)
			{
				physics->Update(world, Physics2DSystem::kFixedTimestep);
			}
		}

		[[nodiscard]] float HeightOf(Entity entity)
		{
			glm::vec3 pos{}, euler{}, scale{};
			DecomposeTRS(world.Get<TransformComponent>(entity).localToWorld, pos, euler, scale);
			return pos.y;
		}
	};
} // namespace

TEST_CASE("a dynamic box rests on painted tile ground")
{
	TileCollisionFixture fx;
	TileMapAsset& map = fx.Map();
	for (int x = -5; x <= 5; ++x)
	{
		map.SetCell(0, {x, 0}, tilecell::Make(fx.solidIndex)); // row of solid tiles y in [0, 1)
	}
	fx.MakeTileMapEntity();
	const Entity box = fx.MakeFallingBox({0.0f, 4.0f});

	fx.StepSeconds(3.0f);

	// Tile row occupies y [0, 1]; the unit box rests with its centre at ~1.5.
	CHECK(fx.HeightOf(box) == doctest::Approx(1.5f).epsilon(0.05));
}

TEST_CASE("clearing a tile removes its collision on the next flush")
{
	TileCollisionFixture fx;
	TileMapAsset& map = fx.Map();
	for (int x = -3; x <= 3; ++x)
	{
		map.SetCell(0, {x, 0}, tilecell::Make(fx.solidIndex));
	}
	fx.MakeTileMapEntity();
	fx.physics->FlushPendingOnly(fx.world);

	// A ray from above strikes the solid row's surface (tile collision is a
	// one-sided chain outline now, so probe the surface, not the interior)...
	CHECK(fx.physics->CastRay({0.5f, 3.0f}, {0.0f, -1.0f}, 5.0f).hit);

	// ...then the tiles under it are cleared and the body rebuilds without them.
	for (int x = -3; x <= 3; ++x)
	{
		map.SetCell(0, {x, 0}, tilecell::kEmpty);
	}
	fx.physics->FlushPendingOnly(fx.world);
	CHECK_FALSE(fx.physics->CastRay({0.5f, 3.0f}, {0.0f, -1.0f}, 5.0f).hit);
}

TEST_CASE("editing one chunk leaves other chunk bodies untouched")
{
	TileCollisionFixture fx;
	TileMapAsset& map = fx.Map();
	map.SetCell(0, {1, 1}, tilecell::Make(fx.solidIndex));   // chunk (0,0)
	map.SetCell(0, {70, 1}, tilecell::Make(fx.solidIndex));  // chunk (2,0)
	fx.MakeTileMapEntity();
	fx.physics->FlushPendingOnly(fx.world);

	CHECK(fx.physics->CastRay({1.5f, 4.0f}, {0.0f, -1.0f}, 5.0f).hit);
	CHECK(fx.physics->CastRay({70.5f, 4.0f}, {0.0f, -1.0f}, 5.0f).hit);

	// Edit only chunk (0,0): the far chunk's collision persists, the near one updates.
	map.SetCell(0, {1, 1}, tilecell::kEmpty);
	fx.physics->FlushPendingOnly(fx.world);
	CHECK_FALSE(fx.physics->CastRay({1.5f, 4.0f}, {0.0f, -1.0f}, 5.0f).hit);
	CHECK(fx.physics->CastRay({70.5f, 4.0f}, {0.0f, -1.0f}, 5.0f).hit);
}

TEST_CASE("a driven box crosses chunk seams without snagging")
{
	// Regression for the ghost-corner stall at chunk borders (the CoinDash
	// patroller froze at x=64.425 where two chunks' merged rects met). Tile
	// collision is chain outlines now: a flat-bottomed box driven along a
	// multi-chunk ground line must keep moving across every seam.
	TileCollisionFixture fx;
	TileMapAsset& map = fx.Map();
	for (int x = 0; x <= 80; ++x) // spans chunks (0,0), (1,0), and (2,0)
	{
		map.SetCell(0, {x, 0}, tilecell::Make(fx.solidIndex));
	}
	fx.MakeTileMapEntity();
	const Entity walker = fx.MakeFallingBox({2.0f, 1.6f});
	fx.StepSeconds(0.5f); // settle onto the ground

	const auto body = fx.world.Get<RigidBody2DComponent>(walker).body;
	glm::vec3 pos{}, euler{}, scale{};
	const int frames = static_cast<int>(12.0f / Physics2DSystem::kFixedTimestep);
	for (int i = 0; i < frames; ++i)
	{
		fx.physics->SetLinearVelocity(body, {8.0f, fx.physics->GetLinearVelocity(body).y});
		fx.physics->Update(fx.world, Physics2DSystem::kFixedTimestep);
		DecomposeTRS(fx.world.Get<TransformComponent>(walker).localToWorld, pos, euler, scale);
		if (pos.x > 76.0f)
		{
			break; // reached the far end - stop before running off the line
		}
	}

	// Any seam snag (x = 32 or 64) leaves the box far short; it must reach the
	// far end still ON the ground (a bounce or wedge would change y).
	CHECK(pos.x > 76.0f);
	CHECK(pos.y == doctest::Approx(1.5f).epsilon(0.1));
}
