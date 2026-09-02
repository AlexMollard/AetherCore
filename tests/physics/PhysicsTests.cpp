// 3D physics (Jolt) coverage. The suite had none: a change that put static bodies on a
// different object layer passed all 878 tests and still dropped a dynamic body straight
// through the floor, because nothing here ever simulated one resting on the other. These
// cases pin the invariants that failure would have broken.
#include <doctest/doctest.h>

#include "physics/PhysicsComponents.hpp"
#include "physics/PhysicsSystem.hpp"
#include "scene/Components.hpp"
#include "scene/TransformUtils.hpp"
#include "scene/World.hpp"

namespace
{
	struct PhysicsFixture
	{
		aether::World world;
		aether::PhysicsSystem* physics = nullptr;

		PhysicsFixture()
		{
			world.SetSceneKind(aether::SceneKind::Scene3D);
			world.SetSceneFeatures(aether::DefaultSceneFeatures(aether::SceneKind::Scene3D));
			auto system = std::make_unique<aether::PhysicsSystem>();
			physics = system.get();
			world.RegisterSystem(std::move(system));
		}

		aether::Entity MakeBody(glm::vec3 pos, aether::PhysicsMotionType motion, glm::vec3 halfExtents, bool sensor = false)
		{
			const aether::Entity e = world.Create();
			world.Emplace<aether::TransformComponent>(e, aether::TransformComponent{.localToWorld = aether::ComposeTransform(pos, {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f})});
			world.Emplace<aether::RigidBodyComponent>(e, aether::RigidBodyComponent{.motionType = motion});
			world.Emplace<aether::ColliderComponent>(e,
			        aether::ColliderComponent{
			                .shape = aether::PhysicsShapeType::Box,
			                .halfExtents = halfExtents,
			                .isSensor = sensor,
			        });
			return e;
		}

		void StepSeconds(float seconds)
		{
			// Update() accumulates and runs whole fixed steps, so feeding it the timestep
			// exactly is what makes the count predictable.
			const int frames = static_cast<int>(seconds / aether::PhysicsSystem::kFixedTimestep + 0.5f);
			for (int i = 0; i < frames; ++i)
			{
				physics->Update(world, aether::PhysicsSystem::kFixedTimestep);
			}
		}

		[[nodiscard]] glm::vec3 PositionOf(aether::Entity e)
		{
			glm::vec3 pos{};
			glm::vec3 euler{};
			glm::vec3 scale{};
			aether::DecomposeTRS(world.Get<aether::TransformComponent>(e).localToWorld, pos, euler, scale);
			return pos;
		}
	};
} // namespace

TEST_CASE("A dynamic body comes to rest on a static floor")
{
	PhysicsFixture fx;
	// Floor spanning the origin, top face at y = 0.
	fx.MakeBody({0.0f, -0.5f, 0.0f}, aether::PhysicsMotionType::Static, {10.0f, 0.5f, 10.0f});
	// Half-unit cube dropped from three units up.
	const aether::Entity box = fx.MakeBody({0.0f, 3.0f, 0.0f}, aether::PhysicsMotionType::Dynamic, {0.5f, 0.5f, 0.5f});

	fx.StepSeconds(3.0f);

	const glm::vec3 rest = fx.PositionOf(box);
	// Resting on the floor puts its centre a half-extent above y = 0. Anything below zero
	// means it passed through, which is what a wrong collision layer looks like.
	CHECK(rest.y > 0.0f);
	CHECK(rest.y == doctest::Approx(0.5f).epsilon(0.2));
}

TEST_CASE("A dynamic body actually falls when there is nothing under it")
{
	// The counterpart to the case above: without it, a physics system that froze every body
	// where it was authored would pass "rests on a floor" without simulating anything.
	PhysicsFixture fx;
	const aether::Entity box = fx.MakeBody({0.0f, 3.0f, 0.0f}, aether::PhysicsMotionType::Dynamic, {0.5f, 0.5f, 0.5f});

	fx.StepSeconds(1.0f);

	CHECK(fx.PositionOf(box).y < 2.5f);
}

TEST_CASE("A static body stays put under a dynamic body's weight")
{
	PhysicsFixture fx;
	const aether::Entity floor = fx.MakeBody({0.0f, -0.5f, 0.0f}, aether::PhysicsMotionType::Static, {10.0f, 0.5f, 10.0f});
	fx.MakeBody({0.0f, 2.0f, 0.0f}, aether::PhysicsMotionType::Dynamic, {0.5f, 0.5f, 0.5f});

	fx.StepSeconds(2.0f);

	CHECK(fx.PositionOf(floor).y == doctest::Approx(-0.5f));
}

TEST_CASE("A sensor is passed straight through and does not hold a body up")
{
	// Sensors report overlaps without a collision response, so a body must fall past one.
	// This is the case that would break if sensors ever shared a collision layer with
	// ordinary geometry.
	PhysicsFixture fx;
	fx.MakeBody({0.0f, 0.0f, 0.0f}, aether::PhysicsMotionType::Static, {10.0f, 0.5f, 10.0f}, /*sensor=*/true);
	const aether::Entity box = fx.MakeBody({0.0f, 3.0f, 0.0f}, aether::PhysicsMotionType::Dynamic, {0.5f, 0.5f, 0.5f});

	fx.StepSeconds(2.0f);

	CHECK(fx.PositionOf(box).y < 0.0f);
}
