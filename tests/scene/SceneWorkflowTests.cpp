#include <doctest/doctest.h>
#include "scene/SceneWorkflow.hpp"
#include "scene/SceneSerializer.hpp"
#include "physics/PhysicsComponents.hpp"

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
