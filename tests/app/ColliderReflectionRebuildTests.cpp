// Regression coverage for a real silent-no-op: Physics.reflect.cpp registered no
// postSet hook for RigidBodyComponent/ColliderComponent, unlike Physics2D.reflect.cpp's
// RebuildBody2D/RebuildJoint2D. PhysicsSystem::FlushPendingBodies only ever reads a
// collider's shape fields ONCE, at bake time, and nothing else revisited them - so
// scene.set_component (the Inspector, MCP, a script) could change half_extents, read
// it back correctly off the component, and get a body that kept simulating whatever
// shape it was created with, forever. These cases exercise the exact path
// ControlMethods.cpp's setComponent lambda uses: FieldDesc::set followed by
// ComponentType::postSet.
#include <doctest/doctest.h>

#include <memory>

#include "physics/PhysicsComponents.hpp"
#include "physics/PhysicsSystem.hpp"
#include "scene/Components.hpp"
#include "scene/TransformUtils.hpp"
#include "scene/World.hpp"
#include "scene/reflection/Reflection.hpp"

using namespace aether;

namespace
{
	// Drops a dynamic box (created with the DEFAULT 0.5 half-extent, matching how the
	// real bug's props were authored) onto a static floor, bakes it, then resizes
	// half_extents through the reflected field setter exactly once the body already
	// exists - and returns the height it eventually rests at.
	float SettleHeightAfterReflectedResize(float halfExtent)
	{
		World world;
		world.SetSceneKind(SceneKind::Scene3D);
		world.SetSceneFeatures(DefaultSceneFeatures(SceneKind::Scene3D));
		auto system = std::make_unique<PhysicsSystem>();
		PhysicsSystem* physics = system.get();
		world.RegisterSystem(std::move(system));

		const Entity floor = world.Create();
		world.Emplace<TransformComponent>(floor, TransformComponent{.localToWorld = ComposeTransform({0.0f, -0.5f, 0.0f}, {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f})});
		world.Emplace<RigidBodyComponent>(floor, RigidBodyComponent{.motionType = PhysicsMotionType::Static});
		world.Emplace<ColliderComponent>(floor, ColliderComponent{.shape = PhysicsShapeType::Box, .halfExtents = {10.0f, 0.5f, 10.0f}});

		const Entity box = world.Create();
		world.Emplace<TransformComponent>(box, TransformComponent{.localToWorld = ComposeTransform({0.0f, 3.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f})});
		world.Emplace<RigidBodyComponent>(box, RigidBodyComponent{.motionType = PhysicsMotionType::Dynamic});
		world.Emplace<ColliderComponent>(box, ColliderComponent{.shape = PhysicsShapeType::Box}); // default half_extents = 0.5

		// One tick bakes the body (FlushPendingBodies reads half_extents = 0.5 here).
		physics->Update(world, PhysicsSystem::kFixedTimestep);

		const reflect::ComponentType* colliderType = reflect::FindComponentType("Collider");
		REQUIRE(colliderType != nullptr);
		void* collider = colliderType->tryGetRaw(world, box);
		REQUIRE(collider != nullptr);
		const reflect::FieldDesc* halfExtentsField = colliderType->FindField("half_extents");
		REQUIRE(halfExtentsField != nullptr);

		// Exactly what scene.set_component does: FieldDesc::set, then, if registered,
		// ComponentType::postSet.
		halfExtentsField->set(collider, reflect::MakeValue(glm::vec3{halfExtent, halfExtent, halfExtent}));
		REQUIRE(colliderType->postSet);
		colliderType->postSet(world, box);

		for (int i = 0; i < 240; ++i)
		{
			physics->Update(world, PhysicsSystem::kFixedTimestep);
		}

		glm::vec3 pos{};
		glm::vec3 euler{};
		glm::vec3 scale{};
		DecomposeTRS(world.Get<TransformComponent>(box).localToWorld, pos, euler, scale);
		return pos.y;
	}
} // namespace

TEST_CASE("Resizing Collider.half_extents through the reflected setter after the body baked changes where it rests")
{
	// Without the postSet -> RebuildBody hook, both halfExtent values settle at the SAME
	// height (whatever the body was created with, ~0.5) regardless of what the field was
	// resized to afterward - which is exactly the "every prop settles at 0.4799997" defect
	// this test exists to catch. With the hook, a body resized up to a 2.0 half-extent
	// must rest measurably higher than one resized to 0.5.
	const float smallRest = SettleHeightAfterReflectedResize(0.5f);
	const float largeRest = SettleHeightAfterReflectedResize(2.0f);
	CHECK(smallRest == doctest::Approx(0.5f).epsilon(0.3));
	CHECK(largeRest > smallRest + 1.0f);
}

TEST_CASE("Collider.half_extents reflected field readback matches what was set, independent of the live shape")
{
	// Guards the OTHER half of the bug report: the field write/readback was never in
	// question (the reflection fallback genuinely applied it) - only whether Jolt's
	// shape agreed with it. This pins the field-level contract so a regression in the
	// rebuild plumbing cannot be misdiagnosed as a field-storage bug.
	World world;
	world.SetSceneKind(SceneKind::Scene3D);
	world.SetSceneFeatures(DefaultSceneFeatures(SceneKind::Scene3D));
	auto system = std::make_unique<PhysicsSystem>();
	world.RegisterSystem(std::move(system));

	const Entity box = world.Create();
	world.Emplace<TransformComponent>(box, TransformComponent{.localToWorld = ComposeTransform({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f})});
	world.Emplace<RigidBodyComponent>(box, RigidBodyComponent{.motionType = PhysicsMotionType::Dynamic});
	world.Emplace<ColliderComponent>(box, ColliderComponent{.shape = PhysicsShapeType::Box});

	const reflect::ComponentType* colliderType = reflect::FindComponentType("Collider");
	REQUIRE(colliderType != nullptr);
	void* collider = colliderType->tryGetRaw(world, box);
	REQUIRE(collider != nullptr);
	const reflect::FieldDesc* halfExtentsField = colliderType->FindField("half_extents");
	REQUIRE(halfExtentsField != nullptr);

	halfExtentsField->set(collider, reflect::MakeValue(glm::vec3{1.5f, 2.5f, 3.5f}));
	colliderType->postSet(world, box);

	const reflect::FieldValue readback = halfExtentsField->get(collider);
	CHECK(glm::vec3(readback.vec) == glm::vec3{1.5f, 2.5f, 3.5f});
}

TEST_CASE("Resizing Collider.half_extents on a body already moving preserves its velocity through the rebuild")
{
	// The asset worker's own live verification only exercised rebuilds on bodies AT
	// REST - proving the shape changes, not that motion survives. RebuildBody reads
	// the live Jolt velocity into RigidBodyComponent::initialVelocity before
	// destroying the old body (the same mechanism freeze_rotation already relies on),
	// so a body given a sideways velocity, then resized mid-flight, must keep
	// travelling at roughly the same speed afterward - not teleport-stop the way a
	// naive destroy/recreate without that seed would.
	World world;
	world.SetSceneKind(SceneKind::Scene3D);
	world.SetSceneFeatures(DefaultSceneFeatures(SceneKind::Scene3D));
	auto system = std::make_unique<PhysicsSystem>();
	PhysicsSystem* physics = system.get();
	world.RegisterSystem(std::move(system));

	const Entity box = world.Create();
	world.Emplace<TransformComponent>(box, TransformComponent{.localToWorld = ComposeTransform({0.0f, 5.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f})});
	world.Emplace<RigidBodyComponent>(box, RigidBodyComponent{.motionType = PhysicsMotionType::Dynamic, .gravityFactor = 0.0f, .initialVelocity = {6.0f, 0.0f, 0.0f}});
	world.Emplace<ColliderComponent>(box, ColliderComponent{.shape = PhysicsShapeType::Box}); // default half_extents = 0.5

	// Bake the body, then let it travel for a few ticks so it is genuinely IN MOTION
	// (not merely holding a seeded-but-never-simulated velocity) when the resize hits.
	for (int i = 0; i < 10; ++i)
	{
		physics->Update(world, PhysicsSystem::kFixedTimestep);
	}
	const float speedBefore = physics->GetLinearVelocity(world.Get<RigidBodyComponent>(box).body).x;
	REQUIRE(speedBefore == doctest::Approx(6.0f).epsilon(0.05));

	const reflect::ComponentType* colliderType = reflect::FindComponentType("Collider");
	REQUIRE(colliderType != nullptr);
	void* collider = colliderType->tryGetRaw(world, box);
	REQUIRE(collider != nullptr);
	const reflect::FieldDesc* halfExtentsField = colliderType->FindField("half_extents");
	REQUIRE(halfExtentsField != nullptr);

	halfExtentsField->set(collider, reflect::MakeValue(glm::vec3{1.5f, 1.5f, 1.5f}));
	colliderType->postSet(world, box); // triggers RebuildBody mid-flight

	// Immediately after the rebuild, before another Update() has a chance to re-bake -
	// RebuildBody's own seed (rb->initialVelocity = live Jolt velocity, read before
	// destroying the old body) is what this assertion is actually pinning.
	CHECK(world.Get<RigidBodyComponent>(box).initialVelocity.x == doctest::Approx(6.0f).epsilon(0.05));

	physics->Update(world, PhysicsSystem::kFixedTimestep); // re-bake with the new shape
	const float speedAfter = physics->GetLinearVelocity(world.Get<RigidBodyComponent>(box).body).x;
	CHECK(speedAfter == doctest::Approx(6.0f).epsilon(0.05));
}
