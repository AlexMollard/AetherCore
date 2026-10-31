#include <Jolt/Jolt.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Collision/ContactListener.h>

#include <doctest/doctest.h>

#include <cstdio>
#include <cstdlib>
#include <array>
#include <cmath>
#include <string_view>

#if defined(_WIN32)
#include <windows.h>
#endif

#include "physics/PhysicsResourcePolicy.hpp"
#include "physics/PhysicsSystem.hpp"
#include "scene/TransformUtils.hpp"
#include "scene/World.hpp"

namespace
{
	bool PhysicsProbeEnabled(const char* name)
	{
		const char* value = std::getenv(name);
		return value != nullptr && std::string_view(value) == "1";
	}

	class ProbeBroadPhase final: public JPH::BroadPhaseLayerInterface
	{
	public:
		JPH::uint GetNumBroadPhaseLayers() const override { return 1; }
		JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer) const override { return JPH::BroadPhaseLayer(0); }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
		const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer) const override { return "Probe"; }
#endif
	};
	class ProbeBroadPhaseFilter final: public JPH::ObjectVsBroadPhaseLayerFilter
	{
	public:
		bool ShouldCollide(JPH::ObjectLayer, JPH::BroadPhaseLayer) const override { return true; }
	};
	class ProbePairFilter final: public JPH::ObjectLayerPairFilter
	{
	public:
		bool ShouldCollide(JPH::ObjectLayer, JPH::ObjectLayer) const override { return true; }
	};
	class ProbeContacts final: public JPH::ContactListener
	{
	public:
		void OnContactAdded(const JPH::Body&, const JPH::Body&, const JPH::ContactManifold&, JPH::ContactSettings&) override { ++added; }
		unsigned added = 0;
	};
}

TEST_CASE("Physics resource settings round trip and sanitize before allocating")
{
	aether::EngineSettings settings;
	aether::EngineSettingsIO::Apply("[physics]\nscratchArenaKiB=2048\nscratchFallbackKiB=4096\nmaxBodies=128\nmaxBodyPairs=256\nmaxContactConstraints=64\n", settings);
	aether::EngineSettings restored;
	aether::EngineSettingsIO::Apply(aether::EngineSettingsIO::Serialize(settings), restored);
	CHECK(restored.physics.scratchArenaKiB == 2048);
	CHECK(restored.physics.scratchFallbackKiB == 4096);
	CHECK(restored.physics.maxBodies == 128);
	CHECK(restored.physics.maxBodyPairs == 256);
	CHECK(restored.physics.maxContactConstraints == 64);
	settings.physics.scratchArenaKiB = -1;
	settings.physics.scratchFallbackKiB = -1;
	settings.physics.maxBodies = 2147483647;
	aether::EngineSettingsIO::Sanitize(settings);
	CHECK(settings.physics.scratchArenaKiB == 1);
	CHECK(settings.physics.scratchFallbackKiB == 0);
	CHECK(settings.physics.maxBodies == 64536);
	CHECK(aether::SettingMetadata("physics.scratchFallbackKiB").restartRequired);
}

TEST_CASE("Physics fallback budget rejects aggregate exhaustion and retains its high water")
{
	aether::PhysicsScratchBudget budget(1024);
	REQUIRE(budget.TryAcquire(600));
	CHECK_FALSE(budget.TryAcquire(500));
	CHECK(budget.LiveBytes() == 600);
	REQUIRE(budget.TryAcquire(424));
	budget.Release(600);
	REQUIRE(budget.TryAcquire(600));
	CHECK_FALSE(budget.TryAcquire(1));
	budget.Release(1024);
	CHECK(budget.LiveBytes() == 0);
	CHECK(budget.HighWaterBytes() == 1024);
}

TEST_CASE("Physics saturation counts every error bit while bounding repeated diagnostics")
{
	aether::PhysicsSaturationStats stats;
	CHECK(aether::RecordPhysicsSaturation(stats, 3));
	for (int step = 0; step < 59; ++step)
	{
		CHECK_FALSE(aether::RecordPhysicsSaturation(stats, 3));
	}
	CHECK(aether::RecordPhysicsSaturation(stats, 3));
	CHECK(aether::RecordPhysicsSaturation(stats, 4));
	CHECK(stats.saturatedSteps == 62);
	CHECK(stats.manifoldCacheFullSteps == 61);
	CHECK(stats.bodyPairCacheFullSteps == 61);
	CHECK(stats.contactConstraintsFullSteps == 1);
	CHECK(stats.diagnostics == 3);
	CHECK_FALSE(aether::RecordPhysicsSaturation(stats, 0));
	CHECK(stats.lastErrorFlags == 0);
	CHECK(stats.observedErrorFlags == 7);
}

TEST_CASE("Physics scratch fallback is bounded and observable during real fixed steps")
{
	using namespace aether;
	World world;
	world.SetSceneKind(SceneKind::Scene3D);
	world.SetSceneFeatures(DefaultSceneFeatures(SceneKind::Scene3D));
	EngineSettings::Physics settings;
	settings.scratchArenaKiB = 1;
	settings.scratchFallbackKiB = 16384;
	auto owned = std::make_unique<PhysicsSystem>(settings);
	auto& physics = *owned;
	world.RegisterSystem(std::move(owned));
	for (int index = 0; index < 128; ++index)
	{
		const auto entity = world.Create();
		world.Emplace<TransformComponent>(entity, TransformComponent{.localToWorld = ComposeTransform({float(index) * 3, 2, 0}, {}, {1, 1, 1})});
		world.Emplace<RigidBodyComponent>(entity, RigidBodyComponent{.motionType = PhysicsMotionType::Dynamic});
		world.Emplace<ColliderComponent>(entity);
	}
	for (int frame = 0; frame < 120; ++frame) { physics.Update(world, PhysicsSystem::kFixedTimestep); }
	const auto stats = physics.GetResourceStats();
	CHECK(stats.scratchHighWaterBytes > 1024);
	CHECK(stats.scratchFallbackHighWaterBytes > 0);
	CHECK(stats.scratchFallbackHighWaterBytes <= 16384 * 1024);
	CHECK(stats.scratchArenaCapacityBytes == 1024);
	CHECK(stats.scratchFallbackCapacityBytes == 16384 * 1024);
	CHECK(stats.scratchFallbackLiveBytes == 0);
	CHECK(stats.saturation.saturatedSteps == 0);
	MESSAGE("Measured 128 bodies / 120 steps: scratch high water=" << stats.scratchHighWaterBytes
	        << " fallback high water=" << stats.scratchFallbackHighWaterBytes << " fallback allocations=" << stats.scratchFallbackAllocations);
}

TEST_CASE("Physics duplicate pair constraints suppress contacts until the last constraint is removed")
{
	using namespace aether;
	const auto campaign = [](bool reverseEndpoints, bool authoredFirst, bool suppressSecond)
	{
		World world;
		world.SetSceneKind(SceneKind::Scene3D);
		world.SetSceneFeatures(DefaultSceneFeatures(SceneKind::Scene3D));
		auto owned = std::make_unique<PhysicsSystem>();
		auto& physics = *owned;
		world.RegisterSystem(std::move(owned));
		const auto makeBody = [&](float x)
		{
			const auto entity = world.Create();
			world.Emplace<TransformComponent>(entity, TransformComponent{.localToWorld = ComposeTransform({x, 5, 0}, {}, {1, 1, 1})});
			world.Emplace<RigidBodyComponent>(entity, RigidBodyComponent{.motionType = PhysicsMotionType::Dynamic, .gravityFactor = 0, .allowSleeping = false});
			world.Emplace<ColliderComponent>(entity);
			world.Emplace<CollisionEventsComponent>(entity);
			return entity;
		};
		const auto a = makeBody(0);
		const auto b = makeBody(0.25f);
		std::uint32_t first = 0;
		if (authoredFirst)
		{
			world.Emplace<JointComponent>(a, JointComponent{.type = JointType::Fixed, .target = b});
		}
		else
		{
			first = physics.CreateFixedConstraint(world, a, b);
			REQUIRE(first != 0);
		}
		JointEntry entry;
		entry.target = reverseEndpoints ? a : b;
		entry.collideConnected = !suppressSecond;
		const auto second = physics.AddScriptJoint(world, reverseEndpoints ? b : a, entry);
		REQUIRE(second != 0);
		REQUIRE(first != second);
		const auto removeFirst = [&]()
		{
			if (authoredFirst) { world.Remove<JointComponent>(a); }
			else { physics.DestroyConstraint(world, first); }
		};
		const auto countContacts = [&]()
		{
			unsigned contacts = 0;
			for (int frame = 0; frame < 6; ++frame)
			{
				physics.Update(world, PhysicsSystem::kFixedTimestep);
				// Joins the worker, synchronizes the ECS, and drains this step's events.
				physics.Update(world, 0);
				contacts += static_cast<unsigned>(world.Get<CollisionEventsComponent>(a).collisionEnter.size());
			}
			return contacts;
		};
		REQUIRE(countContacts() == 0);
		bool expectContacts = false;
		bool removeSurvivor = false;
		SUBCASE("keep both constraints") {}
		SUBCASE("remove only one constraint")
		{
			removeFirst();
			expectContacts = !suppressSecond;
			removeSurvivor = suppressSecond;
		}
		SUBCASE("remove both constraints restores collision")
		{
			removeFirst();
			physics.DestroyConstraint(world, second);
			physics.DestroyConstraint(world, second); // repeated destruction is a no-op
			expectContacts = true;
		}
		const auto contacts = countContacts();
		if (expectContacts)
		{
			CHECK(contacts > 0);
		}
		else
		{
			CHECK(contacts == 0);
		}
		if (removeSurvivor)
		{
			physics.DestroyConstraint(world, second);
			CHECK(countContacts() > 0);
		}
		CHECK(physics.GetResourceStats().saturation.observedErrorFlags == 0);
	};
	SUBCASE("same endpoint order") { campaign(false, false, true); }
	SUBCASE("reversed endpoint order") { campaign(true, false, true); }
	SUBCASE("authored and script joints share suppression") { campaign(true, true, true); }
	SUBCASE("collideConnected joint holds no suppression claim") { campaign(false, false, false); }
}

TEST_CASE("Physics duplicate joint cleanup restores collision before a body index is reused")
{
	using namespace aether;
	World world;
	world.SetSceneKind(SceneKind::Scene3D);
	world.SetSceneFeatures(DefaultSceneFeatures(SceneKind::Scene3D));
	auto owned = std::make_unique<PhysicsSystem>();
	auto& physics = *owned;
	world.RegisterSystem(std::move(owned));
	const auto makeBody = [&](float x)
	{
		const auto entity = world.Create();
		world.Emplace<TransformComponent>(entity, TransformComponent{.localToWorld = ComposeTransform({x, 5, 0}, {}, {1, 1, 1})});
		world.Emplace<RigidBodyComponent>(entity, RigidBodyComponent{.gravityFactor = 0, .allowSleeping = false});
		world.Emplace<ColliderComponent>(entity);
		world.Emplace<CollisionEventsComponent>(entity);
		return entity;
	};
	const auto a = makeBody(0);
	const auto b = makeBody(0.25f);
	world.Emplace<JointComponent>(a, JointComponent{.target = b});
	const auto script = physics.CreateFixedConstraint(world, b, a);
	physics.Update(world, PhysicsSystem::kFixedTimestep);
	physics.Update(world, 0);
	REQUIRE(world.Get<CollisionEventsComponent>(b).collisionEnter.empty());
	const auto oldIndex = JPH::BodyID(world.Get<RigidBodyComponent>(a).body.value).GetIndex();
	SUBCASE("entity destruction") { world.Destroy(a); }
	SUBCASE("explicit body removal") { physics.RemoveBody(world, a); }
	physics.DestroyConstraint(world, script); // already released by body cleanup
	REQUIRE(world.Get<ScriptJointsComponent>(b).joints.empty());
	const auto replacement = makeBody(0);
	physics.FlushPendingOnly(world);
	REQUIRE(JPH::BodyID(world.Get<RigidBodyComponent>(replacement).body.value).GetIndex() == oldIndex);
	unsigned contacts = 0;
	for (int frame = 0; frame < 6; ++frame)
	{
		physics.Update(world, PhysicsSystem::kFixedTimestep);
		physics.Update(world, 0);
		contacts += static_cast<unsigned>(world.Get<CollisionEventsComponent>(replacement).collisionEnter.size());
	}
	CHECK(contacts > 0);
	CHECK(physics.GetResourceStats().saturation.observedErrorFlags == 0);
}

TEST_CASE("Physics static motion transition retires touching joints before replacing its body")
{
	using namespace aether;
	World world;
	world.SetSceneKind(SceneKind::Scene3D);
	world.SetSceneFeatures(DefaultSceneFeatures(SceneKind::Scene3D));
	auto owned = std::make_unique<PhysicsSystem>();
	auto& physics = *owned;
	world.RegisterSystem(std::move(owned));
	const auto makeBody = [&](float x, PhysicsMotionType motion, float gravity)
	{
		const auto entity = world.Create();
		world.Emplace<TransformComponent>(entity, TransformComponent{.localToWorld = ComposeTransform({x, 8, 0}, {}, {1, 1, 1})});
		world.Emplace<RigidBodyComponent>(entity, RigidBodyComponent{.motionType = motion, .gravityFactor = gravity});
		world.Emplace<ColliderComponent>(entity, ColliderComponent{.halfExtents = {0.2f, 0.2f, 0.2f}});
		return entity;
	};
	const auto changed = makeBody(0, PhysicsMotionType::Static, 1);
	const auto partner = makeBody(1, PhysicsMotionType::Dynamic, 0);
	// Exercise both ownership directions: the authored joint targets the replaced
	// body, while the script joint belongs to it. RebuildBody retires both kinds,
	// then only the persistent authored declaration is rebuilt on the next flush.
	world.Emplace<JointComponent>(partner, JointComponent{.target = changed});
	const auto script = physics.CreateFixedConstraint(world, changed, partner);
	physics.FlushPendingOnly(world);
	const auto oldBody = world.Get<RigidBodyComponent>(changed).body;
	const auto oldConstraint = world.Get<JointComponent>(partner).constraintId;
	REQUIRE(oldBody.IsValid());
	REQUIRE(oldConstraint != 0);
	REQUIRE(world.Get<ScriptJointsComponent>(changed).joints.front().created);
	PhysicsMotionType requested = PhysicsMotionType::Dynamic;
	SUBCASE("static to dynamic") {}
	SUBCASE("static to kinematic") { requested = PhysicsMotionType::Kinematic; }
	physics.SetBodyMotionType(world, changed, requested);
	const bool authoredRetired = world.Get<JointComponent>(partner).constraintId == 0;
	const bool scriptRetired = world.Get<ScriptJointsComponent>(changed).joints.empty();
	CHECK_FALSE(world.Get<RigidBodyComponent>(changed).body.IsValid());
	CHECK(authoredRetired);
	CHECK(scriptRetired);
	if (!authoredRetired || !scriptRetired)
	{
		// RED must diagnose stale bookkeeping without integrating a constraint whose
		// endpoint has already been freed. Remove it while the worker is still idle.
		world.Remove<JointComponent>(partner);
		physics.DestroyConstraint(world, script);
		return;
	}
	physics.DestroyConstraint(world, script); // retired script handles remain no-ops
	physics.FlushPendingOnly(world);
	REQUIRE(world.Get<RigidBodyComponent>(changed).body.IsValid());
	CHECK(world.Get<RigidBodyComponent>(changed).body.value != oldBody.value);
	const auto rebuilt = world.Get<JointComponent>(partner).constraintId;
	REQUIRE(rebuilt != 0);
	CHECK(rebuilt != oldConstraint);
	for (int frame = 0; frame < 60; ++frame)
	{
		if (requested == PhysicsMotionType::Kinematic)
		{
			world.Get<TransformComponent>(changed).localToWorld = ComposeTransform({float(frame + 1) / 60, 8, 0}, {}, {1, 1, 1});
		}
		physics.Update(world, PhysicsSystem::kFixedTimestep);
		physics.Update(world, 0);
	}
	const auto a = world.Get<PhysicsStateComponent>(changed).currPosition;
	const auto b = world.Get<PhysicsStateComponent>(partner).currPosition;
	CHECK(glm::length(a - b) == doctest::Approx(1).epsilon(0.1));
	if (requested == PhysicsMotionType::Dynamic)
	{
		CHECK(a.y < 7);
		CHECK(b.y < 7.5f); // dragged down without gravity of its own
	}
	else
	{
		CHECK(a.x > 0.9f);
		CHECK(b.x > 1.8f); // moved by the rebuilt authored weld
	}
	CHECK(physics.GetResourceStats().saturation.observedErrorFlags == 0);
}

TEST_CASE("Physics bounded mixed ECS workload preserves joints CCD contacts and character motion")
{
	using namespace aether;
	World world;
	world.SetSceneKind(SceneKind::Scene3D);
	world.SetSceneFeatures(DefaultSceneFeatures(SceneKind::Scene3D));
	EngineSettings::Physics settings;
	// Exercise the bounded fallback with actual contact/CCD/character allocations.
	settings.scratchArenaKiB = 1;
	settings.scratchFallbackKiB = 16384;
	auto owned = std::make_unique<PhysicsSystem>(settings);
	auto& physics = *owned;
	world.RegisterSystem(std::move(owned));
	const auto makeBody = [&](glm::vec3 position, PhysicsMotionType motion, glm::vec3 halfExtents)
	{
		const auto entity = world.Create();
		world.Emplace<TransformComponent>(entity, TransformComponent{.localToWorld = ComposeTransform(position, {}, {1, 1, 1})});
		world.Emplace<RigidBodyComponent>(entity, RigidBodyComponent{.motionType = motion});
		world.Emplace<ColliderComponent>(entity, ColliderComponent{.halfExtents = halfExtents});
		world.Emplace<CollisionEventsComponent>(entity);
		return entity;
	};
	const auto positionOf = [&](Entity entity) { return world.Get<PhysicsStateComponent>(entity).currPosition; };
	const auto floor = makeBody({0, -0.5f, 14}, PhysicsMotionType::Static, {20, 0.5f, 20});
	const auto sensor = makeBody({-10, 1.5f, 14}, PhysicsMotionType::Static, {1, 0.1f, 16});
	world.Get<ColliderComponent>(sensor).isSensor = true;
	constexpr int kLanes = 8;
	std::array<Entity, kLanes> weldA, weldB, anchors, weights, projectiles, controls, characters, drops;
	std::array<bool, kLanes> floorContacts{}, sensorContacts{};
	for (int lane = 0; lane < kLanes; ++lane)
	{
		const float offset = float(lane) * 4;
		weldA[lane] = makeBody({-50 + offset, 30, -50}, PhysicsMotionType::Dynamic, {0.25f, 0.25f, 0.25f});
		weldB[lane] = makeBody({-49 + offset, 30, -50}, PhysicsMotionType::Dynamic, {0.25f, 0.25f, 0.25f});
		world.Get<RigidBodyComponent>(weldB[lane]).gravityFactor = 0;
		REQUIRE(physics.CreateFixedConstraint(world, weldA[lane], weldB[lane]) != 0);
		const glm::vec3 pin{-50 + offset, 10, -20};
		anchors[lane] = makeBody(pin, PhysicsMotionType::Static, {0.1f, 0.1f, 0.1f});
		weights[lane] = makeBody(pin, PhysicsMotionType::Dynamic, {0.2f, 0.2f, 0.2f});
		world.Emplace<JointComponent>(weights[lane], JointComponent{.type = JointType::Distance, .target = anchors[lane], .anchor = pin, .distance = 2});
		makeBody({0, 2, 100 + offset}, PhysicsMotionType::Static, {0.05f, 2, 1});
		projectiles[lane] = makeBody({-2, 2, 100 + offset}, PhysicsMotionType::Dynamic, {0.1f, 0.1f, 0.1f});
		controls[lane] = makeBody({-2, 2, 140 + offset}, PhysicsMotionType::Dynamic, {0.1f, 0.1f, 0.1f});
		for (const auto entity: {projectiles[lane], controls[lane]})
		{
			auto& rigid = world.Get<RigidBodyComponent>(entity);
			rigid.gravityFactor = 0;
			rigid.linearDamping = 0;
			rigid.continuousCollision = true;
			rigid.initialVelocity = {240, 0, 0};
		}
		characters[lane] = world.Create();
		world.Emplace<TransformComponent>(characters[lane], TransformComponent{.localToWorld = ComposeTransform({0, 2, offset}, {}, {1, 1, 1})});
		world.Emplace<CharacterControllerComponent>(characters[lane]);
		drops[lane] = makeBody({-10, 3, offset}, PhysicsMotionType::Dynamic, {0.25f, 0.25f, 0.25f});
	}
	for (int frame = 0; frame < 240; ++frame)
	{
		for (const auto entity: characters)
		{
			auto& controller = world.Get<CharacterControllerComponent>(entity);
			if (frame == 60) { controller.desiredVelocity = {1, 0, 0}; }
			if (frame == 120)
			{
				REQUIRE(controller.isGrounded);
				CHECK(controller.groundEntity == floor.id);
				controller.pendingJumpSpeed = 6;
			}
		}
		physics.Update(world, PhysicsSystem::kFixedTimestep);
		physics.Update(world, 0);
		for (int lane = 0; lane < kLanes; ++lane)
		{
			const auto& events = world.Get<CollisionEventsComponent>(drops[lane]);
			for (const auto entity: events.collisionEnter) { floorContacts[lane] = floorContacts[lane] || entity == floor; }
			for (const auto entity: events.triggerEnter) { sensorContacts[lane] = sensorContacts[lane] || entity == sensor; }
			// Four metres per step clears both the 20 cm projectile and 10 cm wall;
			// endpoint-only collision cannot explain the blocked projectile.
			CHECK(positionOf(projectiles[lane]).x < 0);
			if (frame == 0) { CHECK(positionOf(controls[lane]).x > 1); }
			if (frame == 126)
			{
				CHECK_FALSE(world.Get<CharacterControllerComponent>(characters[lane]).isGrounded);
				CHECK(positionOf(characters[lane]).y > 0.4f);
			}
		}
		const auto stats = physics.GetResourceStats();
		CHECK(stats.scratchFallbackLiveBytes == 0);
		CHECK(stats.saturation.observedErrorFlags == 0);
	}
	for (int lane = 0; lane < kLanes; ++lane)
	{
		CAPTURE(lane);
		CHECK(positionOf(weldB[lane]).y < 25); // dragged despite having no gravity
		CHECK(glm::length(positionOf(weldB[lane]) - positionOf(weldA[lane])) == doctest::Approx(1).epsilon(0.05));
		CHECK(world.Get<JointComponent>(weights[lane]).constraintId != 0);
		CHECK(glm::length(positionOf(weights[lane]) - positionOf(anchors[lane])) == doctest::Approx(2).epsilon(0.1));
		CHECK(floorContacts[lane]);
		CHECK(sensorContacts[lane]);
		CHECK(positionOf(drops[lane]).y == doctest::Approx(0.25).epsilon(0.2));
		const auto& controller = world.Get<CharacterControllerComponent>(characters[lane]);
		CHECK(controller.isGrounded);
		CHECK(controller.groundEntity == floor.id);
		CHECK(controller.pendingJumpSpeed == 0);
		CHECK(positionOf(characters[lane]).x > 2.5f);
		for (const auto entity: {weldA[lane], weldB[lane], weights[lane], projectiles[lane], controls[lane], characters[lane], drops[lane]})
		{
			const auto position = positionOf(entity);
			CHECK(std::isfinite(position.x));
			CHECK(std::isfinite(position.y));
			CHECK(std::isfinite(position.z));
		}
	}
	const auto stats = physics.GetResourceStats();
	CHECK(stats.saturation.steps == 240);
	CHECK(stats.scratchHighWaterBytes > 1024);
	CHECK(stats.scratchHighWaterBytes <= stats.scratchArenaCapacityBytes + stats.scratchFallbackCapacityBytes);
	CHECK(stats.scratchFallbackHighWaterBytes > 0);
	CHECK(stats.scratchFallbackHighWaterBytes <= stats.scratchFallbackCapacityBytes);
	MESSAGE("Measured mixed ECS workload: 74 bodies including 8 character inner bodies, 16 joints, 240 fixed steps; scratch high water="
	        << stats.scratchHighWaterBytes << " fallback high water=" << stats.scratchFallbackHighWaterBytes);
}

TEST_CASE("Physics contact saturation is observable in an assertion-disabled Jolt build")
{
	using namespace aether;
	if (!PhysicsSystem::SupportsRecoverableSaturation())
	{
		MESSAGE("Saturation qualification requires assertion-disabled Jolt; pinned Jolt asserts before returning errors in this build");
		return;
	}
	World world;
	world.SetSceneKind(SceneKind::Scene3D);
	world.SetSceneFeatures(DefaultSceneFeatures(SceneKind::Scene3D));
	EngineSettings::Physics settings;
	settings.maxBodyPairs = 8;
	settings.maxContactConstraints = 8;
	int pairCount = 32;
	bool separatedSpheres = false;
	std::uint32_t requiredFlag = 0;
	SUBCASE("combined small-capacity contact workload") {}
	SUBCASE("body-pair cache without narrow-phase contacts")
	{
		settings.maxBodyPairs = 4;
		settings.maxContactConstraints = 4;
		pairCount = 64;
		separatedSpheres = true;
		requiredFlag = 2;
	}
	SUBCASE("solver constraints with ample shared contact-cache storage")
	{
		settings.maxBodyPairs = 1024;
		settings.maxContactConstraints = 4;
		requiredFlag = 4;
	}
	auto owned = std::make_unique<PhysicsSystem>(settings);
	auto& physics = *owned;
	world.RegisterSystem(std::move(owned));
	for (int pair = 0; pair < pairCount; ++pair)
	{
		for (int side = 0; side < 2; ++side)
		{
			const auto entity = world.Create();
			// Sphere AABBs overlap, but sqrt(1.5^2 + 1.5^2) exceeds their
			// summed radii plus speculative distance: only body pairs are cached.
			const glm::vec3 position = separatedSpheres
			        ? glm::vec3{float(pair) * 8 + float(side) * 1.5f, float(side) * 1.5f, 0}
			        : glm::vec3{float(pair) * 4, float(side) * 0.5f, 0};
			world.Emplace<TransformComponent>(entity, TransformComponent{.localToWorld = ComposeTransform(position, {}, {1, 1, 1})});
			world.Emplace<RigidBodyComponent>(entity, RigidBodyComponent{.motionType = PhysicsMotionType::Dynamic, .gravityFactor = 0});
			auto& collider = world.Emplace<ColliderComponent>(entity);
			if (separatedSpheres)
			{
				collider.shape = PhysicsShapeType::Sphere;
				collider.radius = 1;
			}
		}
	}
	physics.Update(world, PhysicsSystem::kFixedTimestep);
	const auto stats = physics.GetResourceStats();
	CHECK(stats.saturation.saturatedSteps > 0);
	CHECK(stats.saturation.observedErrorFlags != 0);
	if (requiredFlag) { CHECK((stats.saturation.observedErrorFlags & requiredFlag) != 0); }
	CHECK(stats.saturation.diagnostics > 0);
	MESSAGE("Measured saturation flags=" << stats.saturation.observedErrorFlags << " scratch high water=" << stats.scratchHighWaterBytes);
}

TEST_CASE("Pinned Jolt manifold exhaustion with one sensor body pair")
{
	using namespace aether;
	if (!PhysicsProbeEnabled("AETHER_TEST_PHYSICS_MANIFOLD"))
	{
		MESSAGE("Manifold campaign requires AETHER_TEST_PHYSICS_MANIFOLD=1");
		return;
	}
	if (!PhysicsSystem::SupportsRecoverableSaturation())
	{
		MESSAGE("Manifold campaign requires assertion-disabled Jolt");
		return;
	}
	// Lease the engine's normal process-wide Jolt registration. The raw scene is
	// upstream-path evidence: ECS colliders do not expose compound/reduction knobs.
	World runtimeLease;
	runtimeLease.SetSceneKind(SceneKind::Scene3D);
	runtimeLease.SetSceneFeatures(DefaultSceneFeatures(SceneKind::Scene3D));
	EngineSettings::Physics leaseSettings;
	leaseSettings.maxBodies = 4;
	leaseSettings.maxBodyPairs = 4;
	leaseSettings.maxContactConstraints = 4;
	runtimeLease.RegisterSystem(std::make_unique<PhysicsSystem>(leaseSettings));

	JPH::uint maxContacts = 4;
	bool exhaust = true;
	SUBCASE("normal capacity observes all subshape contacts")
	{
		maxContacts = 64;
		exhaust = false;
	}
	SUBCASE("manifold capacity exceeded without pair or solver exhaustion") {}
	ProbeBroadPhase layers;
	ProbeBroadPhaseFilter broadFilter;
	ProbePairFilter pairFilter;
	ProbeContacts contacts;
	JPH::PhysicsSystem simulation;
	simulation.Init(4, 0, 4, maxContacts, layers, broadFilter, pairFilter);
	simulation.SetGravity(JPH::Vec3::sZero());
	simulation.SetContactListener(&contacts);
	JPH::TempAllocatorImpl scratch(1024 * 1024);
	JPH::JobSystemSingleThreaded jobs(JPH::cMaxPhysicsJobs);
	JPH::StaticCompoundShapeSettings compound;
	JPH::RefConst<JPH::Shape> smallSphere = new JPH::SphereShape(0.1f);
	for (int index = 0; index < 32; ++index)
	{
		compound.AddShape(JPH::Vec3(float(index % 8) * 0.1f - 0.35f, float(index / 8) * 0.1f - 0.15f, 0),
		        JPH::Quat::sIdentity(), smallSphere.GetPtr());
	}
	const auto compoundResult = compound.Create();
	REQUIRE_FALSE(compoundResult.HasError());
	JPH::BodyCreationSettings staticSettings(compoundResult.Get(), JPH::RVec3::sZero(), JPH::Quat::sIdentity(), JPH::EMotionType::Static, 0);
	JPH::RefConst<JPH::Shape> sensorSphere = new JPH::SphereShape(1.0f);
	JPH::BodyCreationSettings sensorSettings(sensorSphere.GetPtr(), JPH::RVec3::sZero(), JPH::Quat::sIdentity(), JPH::EMotionType::Dynamic, 0);
	sensorSettings.mIsSensor = true;
	sensorSettings.mUseManifoldReduction = false;
	auto& bodies = simulation.GetBodyInterface();
	struct BodyCleanup
	{
		JPH::BodyInterface& bodies;
		JPH::BodyID ids[2];
		~BodyCleanup()
		{
			for (const auto id: ids)
			{
				if (!id.IsInvalid())
				{
					bodies.RemoveBody(id);
					bodies.DestroyBody(id);
				}
			}
		}
	} cleanup{bodies, {bodies.CreateAndAddBody(staticSettings, JPH::EActivation::DontActivate),
	        bodies.CreateAndAddBody(sensorSettings, JPH::EActivation::Activate)}};
	REQUIRE_FALSE(cleanup.ids[0].IsInvalid());
	REQUIRE_FALSE(cleanup.ids[1].IsInvalid());
	const auto flags = static_cast<std::uint32_t>(simulation.Update(PhysicsSystem::kFixedTimestep, 1, &scratch, &jobs));
	MESSAGE("Measured raw-Jolt manifold flags=" << flags << " contacts=" << contacts.added);
	if (exhaust)
	{
		CHECK((flags & 1u) != 0);
		CHECK((flags & 6u) == 0);
		PhysicsSaturationStats stats;
		CHECK(RecordPhysicsSaturation(stats, flags));
		CHECK(stats.manifoldCacheFullSteps == 1);
		CHECK(stats.bodyPairCacheFullSteps == 0);
		CHECK(stats.contactConstraintsFullSteps == 0);
	}
	else
	{
		CHECK(flags == 0);
		CHECK(contacts.added == 32);
	}
}

TEST_CASE("Physics scratch exhaustion child probe")
{
	using namespace aether;
	const char* mode = std::getenv("AETHER_TEST_PHYSICS_SCRATCH_CHILD");
	if (mode == nullptr || (std::string_view(mode) != "control" && std::string_view(mode) != "exhaust"))
	{
		MESSAGE("Scratch child probe requires the bounded verify_physics_scratch.py runner");
		return;
	}
#if defined(_WIN32)
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
	const bool exhaust = std::string_view(mode) == "exhaust";
	World world;
	world.SetSceneKind(SceneKind::Scene3D);
	world.SetSceneFeatures(DefaultSceneFeatures(SceneKind::Scene3D));
	EngineSettings::Physics settings;
	if (exhaust)
	{
		settings.scratchArenaKiB = 1;
		settings.scratchFallbackKiB = 0;
	}
	auto owned = std::make_unique<PhysicsSystem>(settings);
	auto& physics = *owned;
	world.RegisterSystem(std::move(owned));
	const auto entity = world.Create();
	world.Emplace<TransformComponent>(entity);
	world.Emplace<RigidBodyComponent>(entity);
	world.Emplace<ColliderComponent>(entity);
	std::puts(exhaust ? "PHYSICS_SCRATCH_PROBE_READY exhaust" : "PHYSICS_SCRATCH_PROBE_READY control");
	std::fflush(stdout);
	physics.Update(world, PhysicsSystem::kFixedTimestep);
	const auto stats = physics.GetResourceStats();
	CHECK(stats.scratchFallbackLiveBytes == 0);
	std::puts("PHYSICS_SCRATCH_PROBE_SURVIVED");
	std::fflush(stdout);
	CHECK_FALSE(exhaust);
}
