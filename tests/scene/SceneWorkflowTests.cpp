#include <doctest/doctest.h>
#include "scene/SceneWorkflow.hpp"
#include "scene/SceneSerializer.hpp"
#include "scene/Components.hpp"
#include "scene/World.hpp"
#include "physics/PhysicsComponents.hpp"

namespace
{
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
}

// writes straight back over it), so this asserts the essential invariant - a
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

	REQUIRE(platform != nullptr);
	CHECK(platform->scale.x == doctest::Approx(platform->physics->halfExtents.x * 2.0f));
	CHECK(platform->scale.z == doctest::Approx(platform->physics->halfExtents.z * 2.0f));

	REQUIRE(cube != nullptr);
	CHECK(cube->position.y > platform->position.y);
}

TEST_CASE("SwitchScene loads a scene and tears down the previous world")
{
	using namespace aether;
	World world;
	const Entity stale = world.Create();
	world.Emplace<NameComponent>(stale, NameComponent{.name = "Stale"});

	const bool loaded = app::scene::SwitchScene("default", world, app::scene::ApplySceneDeps{}, app::scene::OnLoadFailure::ClearWorld);

	CHECK(loaded);
	CHECK(!world.GetRegistry().valid(World::ToEntt(stale)));
	CHECK(LiveEntityCount(world) > 0);
}

TEST_CASE("SwitchScene with an empty name clears the previous world under ClearWorld")
{
	using namespace aether;
	World world;
	world.Emplace<NameComponent>(world.Create(), NameComponent{.name = "Stale"});

	const bool loaded = app::scene::SwitchScene(std::string{}, world, app::scene::ApplySceneDeps{}, app::scene::OnLoadFailure::ClearWorld);

	CHECK(!loaded);
	CHECK(LiveEntityCount(world) == 0);
}

TEST_CASE("SwitchScene clears the world when the named scene does not exist, under ClearWorld")
{
	using namespace aether;
	World world;
	world.Emplace<NameComponent>(world.Create(), NameComponent{.name = "Stale"});

	const bool loaded = app::scene::SwitchScene("no_such_scene_zzz", world, app::scene::ApplySceneDeps{}, app::scene::OnLoadFailure::ClearWorld);

	CHECK(!loaded);
	CHECK(LiveEntityCount(world) == 0);
}

// The other half of the contract, and the reason it became a parameter: opening a scene
// that cannot be loaded must not cost the user the scene they already had open. Before
// this, a corrupt or misnamed scene wiped the live world and left a save poised to
// overwrite the real file with nothing.
TEST_CASE("SwitchScene keeps the current world when the named scene does not exist, under KeepCurrent")
{
    using namespace aether;
    World world;
    const Entity kept = world.Create();
    world.Emplace<NameComponent>(kept, NameComponent{.name = "Keep"});
    const std::size_t before = LiveEntityCount(world);

    const bool loaded = app::scene::SwitchScene("no_such_scene_zzz", world, app::scene::ApplySceneDeps{}, app::scene::OnLoadFailure::KeepCurrent);

    CHECK(!loaded);
    CHECK(world.GetRegistry().valid(World::ToEntt(kept)));
    CHECK(LiveEntityCount(world) == before);
}

TEST_CASE("SwitchScene keeps the current world for an empty name, under KeepCurrent")
{
    using namespace aether;
    World world;
    const Entity kept = world.Create();
    world.Emplace<NameComponent>(kept, NameComponent{.name = "Keep"});

    const bool loaded = app::scene::SwitchScene(std::string{}, world, app::scene::ApplySceneDeps{}, app::scene::OnLoadFailure::KeepCurrent);

    CHECK(!loaded);
    CHECK(world.GetRegistry().valid(World::ToEntt(kept)));
}
