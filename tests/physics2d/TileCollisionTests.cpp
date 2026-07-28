// Tile collision: greedy rect merging and incremental per-chunk static bodies.
#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <system_error>

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
		// std::filesystem, not getenv("TEMP"): it already consults the platform's temp-dir
		// variables and needs no deprecated CRT call to do it.
		std::error_code ec;
		std::filesystem::path base = std::filesystem::temp_directory_path(ec);
		if (ec)
		{
			base = ".";
		}
		auto dir = base / "aethercore_tile_collision_tests";
		std::filesystem::create_directories(dir);
		return dir;
	}

} // namespace

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

		AssetObjectId platformTileId{};
		std::uint16_t platformIndex = 0;

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
			// A jump-through platform: the same whole-cell solid, but only from above.
			TileDefinition& platform = tileSet.AddTile("atlas.toml", AssetObjectId{2}, "Platform");
			platform.collision = TileCollisionKind::Full;
			platform.oneWay = TileOneWay::Up;
			platformTileId = platform.id;
			const auto setPath = (TempDir() / "solid.tileset.toml").generic_string();
			REQUIRE(tiles.SaveTileSet(setPath, tileSet).has_value());

			TileMapAsset map;
			map.tileSetPath = setPath;
			map.cellSize = 1.0f;
			solidIndex = map.PaletteIndexFor(solidTileId);
			platformIndex = map.PaletteIndexFor(platformTileId);
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

TEST_CASE("a one-way tile platform lets a body rise through it and lands it on top")
{
	// Regression for Whisper's arena, where every floating platform was painted
	// with the same two-way solid tile as the floor. A two-way platform is a
	// CEILING to anything under it: the jump fires, the head hits the underside
	// 0.7 units up, and the whole arc is swallowed - wherever a platform
	// overhangs, and only there. A one-way Up tile is solid from above only.
	TileCollisionFixture fx;
	TileMapAsset& map = fx.Map();
	for (int x = -5; x <= 5; ++x)
	{
		map.SetCell(0, {x, 0}, tilecell::Make(fx.solidIndex));    // floor: y [0, 1]
		map.SetCell(0, {x, 3}, tilecell::Make(fx.platformIndex)); // platform: y [3, 4]
	}
	fx.MakeTileMapEntity();
	const Entity jumper = fx.MakeFallingBox({0.0f, 1.5f});
	fx.StepSeconds(0.5f); // settle on the floor
	REQUIRE(fx.HeightOf(jumper) == doctest::Approx(1.5f).epsilon(0.05));

	fx.physics->SetLinearVelocity(fx.world.Get<RigidBody2DComponent>(jumper).body, {0.0f, 12.0f});
	fx.StepSeconds(4.0f);

	// Through the platform and resting on its top (y = 4 + half the unit box).
	// A two-way platform caps the rise at 2.5 and drops it back to 1.5.
	CHECK(fx.HeightOf(jumper) == doctest::Approx(4.5f).epsilon(0.05));
}

TEST_CASE("drop-through drops a body through the one-way platform it stands on")
{
	TileCollisionFixture fx;
	TileMapAsset& map = fx.Map();
	for (int x = -5; x <= 5; ++x)
	{
		map.SetCell(0, {x, 0}, tilecell::Make(fx.solidIndex));
		map.SetCell(0, {x, 3}, tilecell::Make(fx.platformIndex));
	}
	fx.MakeTileMapEntity();
	const Entity rider = fx.MakeFallingBox({0.0f, 4.5f});
	fx.StepSeconds(0.5f);
	REQUIRE(fx.HeightOf(rider) == doctest::Approx(4.5f).epsilon(0.05));

	fx.physics->SetDropThrough(fx.world, rider, 0.5f);
	fx.StepSeconds(2.0f);

	CHECK(fx.HeightOf(rider) == doctest::Approx(1.5f).epsilon(0.05));
}

TEST_CASE("a ground ray still finds a one-way platform from above")
{
	// The player's grounded check is a short downward ray; a jump-through
	// platform has to answer it or standing on one silently disables jumping.
	TileCollisionFixture fx;
	TileMapAsset& map = fx.Map();
	for (int x = -5; x <= 5; ++x)
	{
		map.SetCell(0, {x, 3}, tilecell::Make(fx.platformIndex));
	}
	fx.MakeTileMapEntity();
	fx.physics->FlushPendingOnly(fx.world);

	const Physics2DSystem::RayHit2D above = fx.physics->CastRay({0.5f, 4.65f}, {0.0f, -1.0f}, 0.85f);
	CHECK(above.hit);
	CHECK(above.point.y == doctest::Approx(4.0f).epsilon(0.01));
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
