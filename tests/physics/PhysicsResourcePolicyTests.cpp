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
