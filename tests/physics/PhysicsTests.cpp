// 3D physics (Jolt) coverage. The suite had none: a change that put static bodies on a
// different object layer passed all 878 tests and still dropped a dynamic body straight
// through the floor, because nothing here ever simulated one resting on the other. These
// cases pin the invariants that failure would have broken.
#include <doctest/doctest.h>

#include <filesystem>

#include "io/FileSystem.hpp"
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

TEST_CASE("A Mesh collider with an invalid mesh_source fails cleanly - no body, no crash, no half-created state")
{
	// "Fail loudly, not silently and not with garbage" for a broken/missing source
	// asset: neither RigidBodyComponent::body nor PhysicsStateComponent should exist
	// afterward - not a body simulating some fallback shape nobody authored. Mounts a
	// real (empty) temp directory as "project" - GltfAsset::LoadFromVfsPath asserts
	// if FileSystem was never initialized at all, so an uninitialized VFS is not the
	// same failure mode this test means to exercise; a genuinely missing file under a
	// real mount is.
	if (aether::io::FileSystem::IsInitialized())
	{
		aether::io::FileSystem::Shutdown();
	}
	const std::filesystem::path root = std::filesystem::temp_directory_path() / "aethercore_collider_mesh_source_test";
	std::error_code ec;
	std::filesystem::remove_all(root, ec);
	std::filesystem::create_directories(root, ec);
	aether::io::FileSystem::Initialize();
	aether::io::FileSystem::Mount("project", root);

	{
		PhysicsFixture fx;
		const aether::Entity e = fx.world.Create();
		fx.world.Emplace<aether::TransformComponent>(e, aether::TransformComponent{.localToWorld = aether::ComposeTransform({0.0f, 5.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f})});
		fx.world.Emplace<aether::RigidBodyComponent>(e, aether::RigidBodyComponent{.motionType = aether::PhysicsMotionType::Static});
		fx.world.Emplace<aether::ColliderComponent>(e, aether::ColliderComponent{.shape = aether::PhysicsShapeType::Mesh, .meshSource = "project://assets/models/DoesNotExist.glb"});

		fx.StepSeconds(0.1f);

		CHECK_FALSE(fx.world.Get<aether::RigidBodyComponent>(e).body.IsValid());
		CHECK_FALSE(fx.world.Has<aether::PhysicsStateComponent>(e));
	}

	aether::io::FileSystem::Shutdown();
	std::filesystem::remove_all(root, ec);
}

TEST_CASE("A ConvexHull collider with an invalid mesh_source fails cleanly, on a Dynamic body")
{
	if (aether::io::FileSystem::IsInitialized())
	{
		aether::io::FileSystem::Shutdown();
	}
	const std::filesystem::path root = std::filesystem::temp_directory_path() / "aethercore_collider_mesh_source_test2";
	std::error_code ec;
	std::filesystem::remove_all(root, ec);
	std::filesystem::create_directories(root, ec);
	aether::io::FileSystem::Initialize();
	aether::io::FileSystem::Mount("project", root);

	{
		PhysicsFixture fx;
		const aether::Entity e = fx.world.Create();
		fx.world.Emplace<aether::TransformComponent>(e, aether::TransformComponent{.localToWorld = aether::ComposeTransform({0.0f, 5.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f})});
		fx.world.Emplace<aether::RigidBodyComponent>(e, aether::RigidBodyComponent{.motionType = aether::PhysicsMotionType::Dynamic});
		fx.world.Emplace<aether::ColliderComponent>(e, aether::ColliderComponent{.shape = aether::PhysicsShapeType::ConvexHull, .meshSource = "project://assets/models/DoesNotExist.glb"});

		fx.StepSeconds(0.1f);

		CHECK_FALSE(fx.world.Get<aether::RigidBodyComponent>(e).body.IsValid());
		CHECK_FALSE(fx.world.Has<aether::PhysicsStateComponent>(e));
	}

	aether::io::FileSystem::Shutdown();
	std::filesystem::remove_all(root, ec);
}

TEST_CASE("A Mesh collider on a Dynamic body is rejected even before any mesh_source is touched")
{
	// Differential proof that the rejection is specific to Mesh+non-Static, not a
	// broadly broken fixture: an ordinary Box on the SAME Dynamic motion type, in the
	// same World, still creates and falls normally.
	PhysicsFixture fx;
	const aether::Entity meshOnDynamic = fx.world.Create();
	fx.world.Emplace<aether::TransformComponent>(meshOnDynamic, aether::TransformComponent{.localToWorld = aether::ComposeTransform({0.0f, 5.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f})});
	fx.world.Emplace<aether::RigidBodyComponent>(meshOnDynamic, aether::RigidBodyComponent{.motionType = aether::PhysicsMotionType::Dynamic});
	fx.world.Emplace<aether::ColliderComponent>(meshOnDynamic, aether::ColliderComponent{.shape = aether::PhysicsShapeType::Mesh, .meshSource = ""});

	const aether::Entity controlBox = fx.MakeBody({3.0f, 5.0f, 0.0f}, aether::PhysicsMotionType::Dynamic, {0.5f, 0.5f, 0.5f});

	fx.StepSeconds(1.0f);

	CHECK_FALSE(fx.world.Get<aether::RigidBodyComponent>(meshOnDynamic).body.IsValid());
	CHECK(fx.world.Get<aether::RigidBodyComponent>(controlBox).body.IsValid());
	CHECK(fx.PositionOf(controlBox).y < 4.0f); // fell under gravity, unlike a stuck/frozen body
}

TEST_CASE("A Kinematic body driven across several ticks shoves a Dynamic body in its path at roughly its own speed")
{
	// PushKinematicTargets used to place a Kinematic body with a raw
	// SetPositionAndRotation - a teleport. Jolt still depenetrates a body that ends up
	// overlapping one placed that way, but that is a small positional correction, not
	// a genuine push: the kinematic body reports zero velocity (nothing ever set one),
	// so the collision response has no momentum to transfer. A REAL moving object -
	// exactly what a Kinematic body represents here, whether locally scripted or
	// driven by another peer's transform replication (SyncSimulationAuthority forces
	// a non-owned body Kinematic precisely so replication moves it this way) - must
	// shove a dynamic body it runs into at roughly its own travel speed, the way an
	// actually-moving object does. This is the discriminating case: not "did it move
	// at all" (a teleport's depenetration alone would satisfy that), but "did it move
	// at a speed anywhere near the pusher's own", which only MoveKinematic's real
	// Jolt-computed velocity produces.
	PhysicsFixture fx;
	fx.MakeBody({0.0f, -0.5f, 0.0f}, aether::PhysicsMotionType::Static, {10.0f, 0.5f, 10.0f});
	const aether::Entity pusher = fx.MakeBody({-3.0f, 0.5f, 0.0f}, aether::PhysicsMotionType::Kinematic, {0.5f, 0.5f, 0.5f});
	const aether::Entity target = fx.MakeBody({0.0f, 0.3f, 0.0f}, aether::PhysicsMotionType::Dynamic, {0.3f, 0.3f, 0.3f});

	constexpr float kPusherSpeed = 5.0f; // m/s
	float pusherX = -3.0f;
	for (int i = 0; i < 90; ++i) // 1.5 s - comfortably covers the ~0.44 s closing time
	{
		pusherX += kPusherSpeed * aether::PhysicsSystem::kFixedTimestep;
		fx.world.Get<aether::TransformComponent>(pusher).localToWorld = aether::ComposeTransform({pusherX, 0.5f, 0.0f}, {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f});
		fx.physics->Update(fx.world, aether::PhysicsSystem::kFixedTimestep);
	}

	const glm::vec3 targetVelocity = fx.physics->GetLinearVelocity(fx.world.Get<aether::RigidBodyComponent>(target).body);
	// A depenetration-only nudge is at most a couple of units per second in Jolt's
	// default configuration; a real push at the pusher's 5 m/s lands far above that.
	CHECK(targetVelocity.x > 2.5f);
	CHECK(fx.PositionOf(target).x > 0.5f); // actually carried forward, not just nudged aside
}

TEST_CASE_FIXTURE(PhysicsFixture, "CreateFixedConstraint welds two bodies so gravity on one drags the other along")
{
	// Asymmetric gravityFactor is the discriminator: both falling under their OWN
	// gravity would keep dx constant even completely unwelded (identical fall rate),
	// proving nothing. Only a real weld explains b - which floats on its own - being
	// dragged down by a's fall.
	const aether::Entity a = MakeBody({0.0f, 8.0f, 0.0f}, aether::PhysicsMotionType::Dynamic, {0.5f, 0.5f, 0.5f});
	const aether::Entity b = MakeBody({1.0f, 8.0f, 0.0f}, aether::PhysicsMotionType::Dynamic, {0.5f, 0.5f, 0.5f});
	world.Get<aether::RigidBodyComponent>(b).gravityFactor = 0.0f;

	const std::uint32_t handle = physics->CreateFixedConstraint(world, a, b);
	CHECK(handle != 0);
	StepSeconds(1.0f);

	CHECK(PositionOf(a).y < 7.0f); // fell under its own gravity
	CHECK(PositionOf(b).y < 7.5f); // dragged down with it despite having none of its own
}

TEST_CASE_FIXTURE(PhysicsFixture, "CreateDistanceConstraint holds two bodies at the given rest length under gravity")
{
	// worldAnchor is baked to a LOCAL offset per body at creation time (Jolt's own
	// DistanceConstraintSettings::mPoint1/mPoint2 convention, inherited unchanged
	// from JointComponent's existing Distance joint) - placing both bodies exactly
	// AT worldAnchor makes that offset zero for both, so the constraint bounds the
	// distance between their ORIGINS directly instead of some offset point neither
	// test author nor reader can see.
	const glm::vec3 pin{0.0f, 10.0f, 0.0f};
	const aether::Entity anchor = MakeBody(pin, aether::PhysicsMotionType::Static, {0.2f, 0.2f, 0.2f});
	const aether::Entity weight = MakeBody(pin, aether::PhysicsMotionType::Dynamic, {0.3f, 0.3f, 0.3f});

	const std::uint32_t handle = physics->CreateDistanceConstraint(world, weight, anchor, pin, 2.0f);
	CHECK(handle != 0);

	StepSeconds(2.0f); // let it fall to the end of its rope and settle

	// A free-falling body would have long since fallen out of the fixture entirely;
	// held at its rope's rest length instead, it hangs exactly rest-length below the pin.
	const float dist = glm::length(PositionOf(weight) - PositionOf(anchor));
	CHECK(dist == doctest::Approx(2.0f).epsilon(0.15));
}

TEST_CASE_FIXTURE(PhysicsFixture, "DestroyConstraint releases a weld so the bodies move independently again")
{
	const aether::Entity a = MakeBody({0.0f, 8.0f, 0.0f}, aether::PhysicsMotionType::Dynamic, {0.5f, 0.5f, 0.5f});
	const aether::Entity b = MakeBody({1.0f, 8.0f, 0.0f}, aether::PhysicsMotionType::Dynamic, {0.5f, 0.5f, 0.5f});
	world.Get<aether::RigidBodyComponent>(b).gravityFactor = 0.0f;

	const std::uint32_t handle = physics->CreateFixedConstraint(world, a, b);
	StepSeconds(1.0f);
	CHECK(PositionOf(b).y < 7.5f); // confirms the weld actually dragged it down first
	const float bVelocityAtDestroy = physics->GetLinearVelocity(world.Get<aether::RigidBodyComponent>(b).body).y;

	physics->DestroyConstraint(world, handle);
	StepSeconds(0.5f);

	// Freed: b keeps whatever downward velocity the weld had already given it
	// (Newton's first law, no bug) but stops ACCELERATING - unlike a, which keeps
	// gaining speed under its own still-active gravity, b's velocity barely changes
	// once nothing (gravity or a weld) is pulling on it anymore.
	const float bVelocityAfter = physics->GetLinearVelocity(world.Get<aether::RigidBodyComponent>(b).body).y;
	const float aVelocityAfter = physics->GetLinearVelocity(world.Get<aether::RigidBodyComponent>(a).body).y;
	CHECK(bVelocityAfter == doctest::Approx(bVelocityAtDestroy).epsilon(0.1));
	CHECK(aVelocityAfter < bVelocityAtDestroy - 1.0f); // a kept falling faster, unlinked
}

TEST_CASE_FIXTURE(PhysicsFixture, "Destroying a welded body's entity does not leave a dangling constraint")
{
	const aether::Entity a = MakeBody({0.0f, 5.0f, 0.0f}, aether::PhysicsMotionType::Dynamic, {0.5f, 0.5f, 0.5f});
	const aether::Entity b = MakeBody({1.0f, 5.0f, 0.0f}, aether::PhysicsMotionType::Dynamic, {0.5f, 0.5f, 0.5f});
	physics->CreateFixedConstraint(world, a, b);
	StepSeconds(0.1f);

	world.Destroy(a); // fires on_destroy<RigidBodyComponent> -> DestroyJointsTouching

	// The crash/UB this guards: stepping again with a's Jolt body gone but the
	// constraint still registered would touch freed memory. No crash, and b - freed
	// of its weld - now falls normally instead of hanging as if still anchored.
	const float startY = PositionOf(b).y;
	StepSeconds(0.5f);
	CHECK(PositionOf(b).y < startY - 0.1f);
}

TEST_CASE_FIXTURE(PhysicsFixture, "One entity can hold more than one weld at once")
{
	const aether::Entity left = MakeBody({-1.0f, 8.0f, 0.0f}, aether::PhysicsMotionType::Dynamic, {0.5f, 0.5f, 0.5f});
	const aether::Entity middle = MakeBody({0.0f, 8.0f, 0.0f}, aether::PhysicsMotionType::Dynamic, {0.5f, 0.5f, 0.5f});
	const aether::Entity right = MakeBody({1.0f, 8.0f, 0.0f}, aether::PhysicsMotionType::Dynamic, {0.5f, 0.5f, 0.5f});
	world.Get<aether::RigidBodyComponent>(left).gravityFactor = 0.0f;
	world.Get<aether::RigidBodyComponent>(right).gravityFactor = 0.0f;

	// The middle prop (real gravity) welds to BOTH neighbours (zero gravity of their
	// own) - the exact shape a three-prop line contraption needs, and the one
	// JointComponent's single-slot-per-entity model cannot express at all. If the
	// second weld had silently replaced the first (a single-slot bug), left would
	// float in place while middle+right fell together.
	CHECK(physics->CreateFixedConstraint(world, middle, left) != 0);
	CHECK(physics->CreateFixedConstraint(world, middle, right) != 0);
	StepSeconds(1.0f);

	CHECK(PositionOf(middle).y < 7.0f);
	CHECK(PositionOf(left).y < 7.5f); // dragged down despite its own zero gravity
	CHECK(PositionOf(right).y < 7.5f); // both neighbours, not just whichever welded last
}
