#include <doctest/doctest.h>
#include "scene/SceneWorkflow.hpp"
#include "scene/SceneSerializer.hpp"
#include "physics/PhysicsComponents.hpp"

// Loads the actual committed resources/scenes/default.scene.toml through the real
// ReadSceneFile path (the headless-safe core NewScene builds on), so the template
// and this test cannot silently drift.
TEST_CASE("default scene template loads: static platform + falling dynamic cube")
{
	const auto desc = aether::app::scene::ReadSceneFile("default");
	REQUIRE(desc.has_value());
	REQUIRE(desc->entities.size() == 2);
	// Platform: static box, scale matches half-extents (renders == collides).
	REQUIRE(desc->entities[0].physics.has_value());
	CHECK(desc->entities[0].physics->motionType == aether::PhysicsMotionType::Static);
	CHECK(desc->entities[0].scale.x == doctest::Approx(desc->entities[0].physics->halfExtents.x * 2.0f));
	CHECK(desc->entities[0].scale.z == doctest::Approx(desc->entities[0].physics->halfExtents.z * 2.0f));
	// Cube: dynamic box, dropped above the platform.
	REQUIRE(desc->entities[1].physics.has_value());
	CHECK(desc->entities[1].physics->motionType == aether::PhysicsMotionType::Dynamic);
	CHECK(desc->entities[1].position.y == doctest::Approx(4.0));
}
