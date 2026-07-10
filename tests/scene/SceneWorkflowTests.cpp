#include <doctest/doctest.h>
#include "scene/SceneWorkflow.hpp"
#include "scene/SceneSerializer.hpp"
#include "scene/Components.hpp"
#include "scene/World.hpp"
#include "physics/PhysicsComponents.hpp"

namespace
{
	// Count the live entities in a world (SwitchScene load/clear assertions).
	std::size_t LiveEntityCount(aether::World& world)
	{
		auto& reg = world.GetRegistry();
		std::size_t count = 0;
		for (const auto handle: reg.storage<entt::entity>())
		{
			if (reg.valid(handle))
			{
				++count;
			}
		}
		return count;
	}
} // namespace

// Loads the actual committed resources/scenes/default.scene.toml through the real
// ReadSceneFile path (the headless-safe core NewScene builds on), so the template
// and this test cannot silently drift.
//
// The template is a living, editor-editable file (New Scene -> author -> Save
// writes straight back over it), so this asserts the essential invariant - a
// static platform under a dynamic falling cube - by searching for the two
// physics entities rather than assuming an exact entity count or fixed indices.
// Extra entities (lights, scripts, etc.) added via the editor must not break it.
TEST_CASE("default scene template loads: static platform + falling dynamic cube")
{
	using aether::PhysicsMotionType;
	const auto desc = aether::app::scene::ReadSceneFile("default");
	REQUIRE(desc.has_value());
	REQUIRE(!desc->entities.empty());

	const aether::app::scene::EntityRecord* platform = nullptr;
	const aether::app::scene::EntityRecord* cube = nullptr;
	for (const auto& entity: desc->entities)
	{
		if (!entity.physics.has_value())
		{
			continue;
		}
		if (entity.physics->motionType == PhysicsMotionType::Static && platform == nullptr)
		{
			platform = &entity;
		}
		else if (entity.physics->motionType == PhysicsMotionType::Dynamic && cube == nullptr)
		{
			cube = &entity;
		}
	}

	// Platform: static box, scale matches half-extents (renders == collides).
	REQUIRE(platform != nullptr);
	CHECK(platform->scale.x == doctest::Approx(platform->physics->halfExtents.x * 2.0f));
	CHECK(platform->scale.z == doctest::Approx(platform->physics->halfExtents.z * 2.0f));

	// Cube: dynamic box, dropped above the platform.
	REQUIRE(cube != nullptr);
	CHECK(cube->position.y > platform->position.y);
}

// SwitchScene is the "active project changed" primitive: it replaces the live
// scene (tearing the previous one down) and only clears the world when there is
// nothing to load. This is what makes opening a second project drop the first
// project's scene instead of leaving it running.
TEST_CASE("SwitchScene loads a scene and tears down the previous world")
{
	using namespace aether;
	World world;
	const Entity stale = world.Create();
	world.Emplace<NameComponent>(stale, NameComponent{.name = "Stale"});

	const bool loaded = app::scene::SwitchScene("default", world, app::scene::ApplySceneDeps{});

	CHECK(loaded);
	// The prior scene's entity is gone and the default template populated the world.
	CHECK(!world.GetRegistry().valid(World::ToEntt(stale)));
	CHECK(LiveEntityCount(world) > 0);
}

TEST_CASE("SwitchScene with an empty name clears the previous world")
{
	using namespace aether;
	World world;
	world.Emplace<NameComponent>(world.Create(), NameComponent{.name = "Stale"});

	const bool loaded = app::scene::SwitchScene(std::string{}, world, app::scene::ApplySceneDeps{});

	CHECK(!loaded);
	CHECK(LiveEntityCount(world) == 0);
}

TEST_CASE("SwitchScene clears the world when the named scene does not exist")
{
	using namespace aether;
	World world;
	world.Emplace<NameComponent>(world.Create(), NameComponent{.name = "Stale"});

	const bool loaded = app::scene::SwitchScene("no_such_scene_zzz", world, app::scene::ApplySceneDeps{});

	CHECK(!loaded);
	CHECK(LiveEntityCount(world) == 0);
}
