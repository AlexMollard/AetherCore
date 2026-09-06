// Coverage for RagdollBuilder: the parts of building and simulating a ragdoll that are
// decidable without a GPU or a live editor - bone/joint resolution off a synthetic
// skeleton, Swing Twist's cone limit and Hinge's bend limit each respecting their OWN
// configured value at the boundary (not some shared default), and the whole assembled
// ragdoll actually behaving like one (collapses, comes to rest, pushes a crate) rather
// than a setter echoing a field back.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <memory>

#include "assets/GltfAsset.hpp"
#include "physics/PhysicsComponents.hpp"
#include "physics/PhysicsSystem.hpp"
#include "physics/RagdollBuilder.hpp"
#include "scene/Components.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/TransformUtils.hpp"
#include "scene/World.hpp"

namespace
{
	// A small, hand-built humanoid skeleton - not the real Human.gltf. Bone/joint
	// resolution only needs bind-pose node names and positions, and a synthetic
	// fixture is deterministic and independent of that asset's own future edits; the
	// real asset is exercised by the live editor smoke test instead (see the worker's
	// report). Names alone (not the array order) drive resolution - see
	// RagdollBuilder.cpp's FindNodeByRole - so this only has to be a recognisable
	// skeleton, not match Human.gltf's exact hierarchy.
	aether::assets::GltfAsset MakeHumanoidSkeleton(float scale = 1.0f)
	{
		aether::assets::GltfAsset asset;
		auto add = [&](const char* name, std::int32_t parent, glm::vec3 t)
		{
			aether::assets::GltfNode node;
			node.name = name;
			node.parentIndex = parent;
			node.translation = t * scale;
			asset.nodes.push_back(node);
		};
		add("Hips", -1, {0.0f, 1.0f, 0.0f});
		add("Spine", 0, {0.0f, 0.15f, 0.0f});
		add("Neck", 1, {0.0f, 0.45f, 0.0f});
		add("HeadTop_End", 2, {0.0f, 0.25f, 0.0f});
		add("LeftArm", 1, {0.20f, 0.40f, 0.0f});
		add("LeftForeArm", 4, {0.28f, 0.0f, 0.0f});
		add("LeftHand", 5, {0.26f, 0.0f, 0.0f});
		add("RightArm", 1, {-0.20f, 0.40f, 0.0f});
		add("RightForeArm", 7, {-0.28f, 0.0f, 0.0f});
		add("RightHand", 8, {-0.26f, 0.0f, 0.0f});
		add("LeftUpLeg", 0, {0.10f, -0.05f, 0.0f});
		add("LeftLeg", 10, {0.0f, -0.45f, 0.0f});
		add("LeftFoot", 11, {0.0f, -0.45f, 0.0f});
		add("RightUpLeg", 0, {-0.10f, -0.05f, 0.0f});
		add("RightLeg", 13, {0.0f, -0.45f, 0.0f});
		add("RightFoot", 14, {0.0f, -0.45f, 0.0f});
		return asset;
	}

	struct RagdollFixture
	{
		aether::World world;
		aether::PhysicsSystem* physics = nullptr;
		aether::assets::GltfAsset skeleton = MakeHumanoidSkeleton();

		RagdollFixture()
		{
			world.SetSceneKind(aether::SceneKind::Scene3D);
			world.SetSceneFeatures(aether::DefaultSceneFeatures(aether::SceneKind::Scene3D));
			auto system = std::make_unique<aether::PhysicsSystem>();
			physics = system.get();
			world.RegisterSystem(std::move(system));
		}

		aether::Entity MakeStaticBox(glm::vec3 pos, glm::vec3 halfExtents)
		{
			const aether::Entity e = world.Create();
			world.Emplace<aether::TransformComponent>(e, aether::TransformComponent{.localToWorld = aether::ComposeTransform(pos, {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f})});
			world.Emplace<aether::RigidBodyComponent>(e, aether::RigidBodyComponent{.motionType = aether::PhysicsMotionType::Static});
			world.Emplace<aether::ColliderComponent>(e, aether::ColliderComponent{.shape = aether::PhysicsShapeType::Box, .halfExtents = halfExtents});
			return e;
		}

		aether::Entity MakeDynamicBox(glm::vec3 pos, glm::vec3 halfExtents)
		{
			const aether::Entity e = world.Create();
			world.Emplace<aether::TransformComponent>(e, aether::TransformComponent{.localToWorld = aether::ComposeTransform(pos, {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f})});
			world.Emplace<aether::RigidBodyComponent>(e, aether::RigidBodyComponent{.motionType = aether::PhysicsMotionType::Dynamic});
			world.Emplace<aether::ColliderComponent>(e, aether::ColliderComponent{.shape = aether::PhysicsShapeType::Box, .halfExtents = halfExtents});
			return e;
		}

		aether::Entity SpawnRagdollAt(glm::vec3 pos)
		{
			const aether::Entity e = world.Create();
			world.Emplace<aether::TransformComponent>(e, aether::TransformComponent{.localToWorld = aether::ComposeTransform(pos, {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f})});
			REQUIRE(aether::SpawnRagdoll(world, e, skeleton));
			return e;
		}

		void StepSeconds(float seconds)
		{
			const int frames = static_cast<int>(seconds / aether::PhysicsSystem::kFixedTimestep + 0.5f);
			for (int i = 0; i < frames; ++i)
			{
				physics->Update(world, aether::PhysicsSystem::kFixedTimestep);
			}
		}
	};

	// Assumes `rot` is (very nearly) a pure rotation around `axis` - true for a body
	// jointed only by a Hinge, which constrains every other axis to zero by construction.
	float SignedDegreesAroundAxis(const glm::quat& rot, const glm::vec3& axis)
	{
		const glm::vec3 v(rot.x, rot.y, rot.z);
		const float angle = 2.0f * std::atan2(glm::dot(v, axis), rot.w);
		return glm::degrees(angle);
	}
} // namespace

TEST_CASE("Ragdoll bones are parented under the ragdoll's own source entity, not left at root")
{
	// Eleven bones per ragdoll, previously all registered as independent world roots -
	// the worst single offender for a Hierarchy panel flooded with runtime spawns. The
	// bones ARE this ragdoll, so the parent is semantically correct, not merely tidy.
	RagdollFixture fx;
	const aether::Entity root = fx.SpawnRagdollAt({0.0f, 5.0f, 0.0f});
	const auto& rag = fx.world.Get<aether::RagdollComponent>(root);
	REQUIRE(rag.bones.size() == 11);

	const auto& roots = fx.world.Roots();
	for (const aether::Entity bone: rag.bones)
	{
		if (bone == root)
		{
			continue;
		}
		const auto* h = fx.world.TryGet<aether::HierarchyComponent>(bone);
		REQUIRE(h != nullptr);
		CHECK(h->parent == root);
		// Not double-booked as a world root once it has a parent.
		CHECK(std::find(roots.begin(), roots.end(), bone) == roots.end());

		// Parenting (Hierarchy.hpp's SetParent/InsertChildAt) must never move anything -
		// unlike SetWorldTransform's delta-to-subtree propagation, it only touches
		// HierarchyComponent bookkeeping. A humanoid ragdoll's bones sit within a couple
		// of metres of the spawn point; a bone reset to parent-local zero (a bug this
		// guards against) would read as y == 0, nowhere near the 5.0 spawn height.
		const glm::vec3 bonePos(fx.world.Get<aether::TransformComponent>(bone).localToWorld[3]);
		CHECK(bonePos.y > 3.0f);
		CHECK(bonePos.y < 7.0f);
	}
	REQUIRE(fx.world.Has<aether::HierarchyComponent>(root));
	CHECK(fx.world.Get<aether::HierarchyComponent>(root).children.size() == 10); // every bone but the root itself
}

TEST_CASE("Ragdoll builder resolves every bone and joints them to their skeletal parent")
{
	RagdollFixture fx;
	const aether::Entity root = fx.SpawnRagdollAt({0.0f, 5.0f, 0.0f});
	const auto& rag = fx.world.Get<aether::RagdollComponent>(root);
	// Pelvis (root) + Chest + Head + 2x(UpperArm, Forearm) + 2x(Thigh, Shin) = 11.
	CHECK(rag.bones.size() == 11);

	int jointCount = 0;
	for (const aether::Entity bone: rag.bones)
	{
		CHECK(fx.world.Has<aether::RigidBodyComponent>(bone));
		CHECK(fx.world.Has<aether::ColliderComponent>(bone));
		CHECK(fx.world.Has<aether::RagdollBoneComponent>(bone));
		if (fx.world.Has<aether::JointComponent>(bone))
		{
			++jointCount;
		}
	}
	// Every bone except the root is jointed to its parent - a tree of 11 bones has 10 edges.
	CHECK(jointCount == 10);
	CHECK(root == rag.bones[0]);
}

TEST_CASE("Ragdoll builder returns false and changes nothing for an unrecognisable skeleton")
{
	RagdollFixture fx;
	aether::assets::GltfAsset empty;
	const aether::Entity e = fx.world.Create();
	fx.world.Emplace<aether::TransformComponent>(e, aether::TransformComponent{});
	CHECK_FALSE(aether::SpawnRagdoll(fx.world, e, empty));
	CHECK_FALSE(fx.world.Has<aether::RagdollComponent>(e));
	CHECK_FALSE(fx.world.Has<aether::RigidBodyComponent>(e));
}

TEST_CASE("Ragdoll builder spawns a full 11-bone ragdoll from a Quaternius-named skeleton")
{
	// The naming-convention regression this exists to catch: a real CC0 rig
	// (QuaterniusMan.glb) uses Torso/Abdomen/UpperArm.L/LowerArm.L/Foot.L, sharing
	// almost no vocabulary with the Mixamo table's hips/spine/leftArm/leftForeArm.
	// Before kQuaterniusBoneDefs existed, only Pelvis ("hips") and Head's start
	// ("neck") resolved by coincidence - every limb's parentRole lookup then
	// cascaded to a skip, so this rig silently built a one-bone "ragdoll" and
	// SpawnRagdoll still reported success. Feet are deliberately parented to the
	// ROOT node here, not to LowerLeg.{L,R} - QuaterniusMan.glb is an IK rig where
	// the foot/pole-target nodes hang off the skeleton root independently of the FK
	// leg chain, and FindNodeByRole matches by name across the whole skeleton, not
	// by hierarchy, so this must resolve exactly the same as a rig where they nest
	// normally.
	aether::assets::GltfAsset asset;
	auto add = [&](const char* name, std::int32_t parent, glm::vec3 t)
	{
		aether::assets::GltfNode node;
		node.name = name;
		node.parentIndex = parent;
		node.translation = t;
		asset.nodes.push_back(node);
	};
	add("Bone", -1, {0.0f, 0.0f, 0.0f});
	add("Body", 0, {0.0f, 1.0f, 0.0f});
	add("Hips", 1, {0.0f, 0.0f, 0.0f});
	add("Abdomen", 2, {0.0f, 0.15f, 0.0f});
	add("Torso", 3, {0.0f, 0.15f, 0.0f});
	add("Neck", 4, {0.0f, 0.30f, 0.0f});
	add("Head", 5, {0.0f, 0.10f, 0.0f});
	add("Head_end", 6, {0.0f, 0.15f, 0.0f});
	add("Shoulder.L", 4, {0.20f, 0.10f, 0.0f});
	add("UpperArm.L", 8, {0.08f, 0.0f, 0.0f});
	add("LowerArm.L", 9, {0.28f, 0.0f, 0.0f});
	add("Palm.L", 10, {0.26f, 0.0f, 0.0f});
	add("Shoulder.R", 4, {-0.20f, 0.10f, 0.0f});
	add("UpperArm.R", 12, {-0.08f, 0.0f, 0.0f});
	add("LowerArm.R", 13, {-0.28f, 0.0f, 0.0f});
	add("Palm.R", 14, {-0.26f, 0.0f, 0.0f});
	add("UpperLeg.L", 1, {0.10f, -0.05f, 0.0f});
	add("LowerLeg.L", 16, {0.0f, -0.45f, 0.0f});
	add("UpperLeg.R", 1, {-0.10f, -0.05f, 0.0f});
	add("LowerLeg.R", 18, {0.0f, -0.45f, 0.0f});
	add("Foot.L", 0, {0.10f, 0.15f, 0.0f}); // parented to root Bone, IK-style
	add("Foot.R", 0, {-0.10f, 0.15f, 0.0f});

	aether::World world;
	world.SetSceneKind(aether::SceneKind::Scene3D);
	world.SetSceneFeatures(aether::DefaultSceneFeatures(aether::SceneKind::Scene3D));
	auto system = std::make_unique<aether::PhysicsSystem>();
	world.RegisterSystem(std::move(system));

	const aether::Entity root = world.Create();
	world.Emplace<aether::TransformComponent>(root, aether::TransformComponent{.localToWorld = aether::ComposeTransform({0.0f, 5.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f})});
	REQUIRE(aether::SpawnRagdoll(world, root, asset));

	const auto& rag = world.Get<aether::RagdollComponent>(root);
	CHECK(rag.bones.size() == 11);
	for (const aether::Entity bone: rag.bones)
	{
		REQUIRE(world.Has<aether::RigidBodyComponent>(bone));
		REQUIRE(world.Has<aether::ColliderComponent>(bone));
	}
}

TEST_CASE("Ragdoll builder refuses a rig that matches neither convention in full, not a partial blend of both")
{
	// A rig with SOME Mixamo words and SOME Quaternius words (but missing enough of
	// either table that neither resolves completely) must fail outright, not build
	// from whichever roles happen to resolve across the two tables - that cross-
	// convention blending is exactly what could bind an arm to a leg on some third
	// rig by coincidence.
	aether::assets::GltfAsset asset;
	auto add = [&](const char* name, std::int32_t parent, glm::vec3 t)
	{
		aether::assets::GltfNode node;
		node.name = name;
		node.parentIndex = parent;
		node.translation = t;
		asset.nodes.push_back(node);
	};
	add("Hips", -1, {0.0f, 1.0f, 0.0f});     // Mixamo-style
	add("Neck", 0, {0.0f, 0.45f, 0.0f});     // shared by both conventions
	add("UpperArm.L", 0, {0.20f, 0.40f, 0.0f}); // Quaternius-style, but no matching
	                                             // LowerArm.L/Palm.L/other-side/legs
	RagdollFixture fx;
	fx.skeleton = asset;
	aether::Entity e = fx.world.Create();
	fx.world.Emplace<aether::TransformComponent>(e, aether::TransformComponent{});
	CHECK_FALSE(aether::SpawnRagdoll(fx.world, e, asset));
	CHECK_FALSE(fx.world.Has<aether::RagdollComponent>(e));
	CHECK_FALSE(fx.world.Has<aether::RigidBodyComponent>(e));
}

TEST_CASE("Swing Twist joint clamps its swing to its own configured cone limit")
{
	// A body pinned to the world by a Swing Twist joint, spun hard enough that an
	// unconstrained body would swing far past either limit - the limit itself, not the
	// spin, should decide where each one stops.
	auto measureFinalSwingDeg = [](float swingLimitDeg) -> float
	{
		aether::World world;
		world.SetSceneKind(aether::SceneKind::Scene3D);
		world.SetSceneFeatures(aether::DefaultSceneFeatures(aether::SceneKind::Scene3D));
		auto system = std::make_unique<aether::PhysicsSystem>();
		aether::PhysicsSystem* physics = system.get();
		world.RegisterSystem(std::move(system));

		const glm::vec3 anchor{0.0f, 2.0f, 0.0f};
		const aether::Entity arm = world.Create();
		world.Emplace<aether::TransformComponent>(arm, aether::TransformComponent{.localToWorld = aether::ComposeTransform(anchor + glm::vec3(0.0f, -0.3f, 0.0f), {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f})});
		world.Emplace<aether::RigidBodyComponent>(arm, aether::RigidBodyComponent{.motionType = aether::PhysicsMotionType::Dynamic, .gravityFactor = 0.0f, .initialAngularVelocity = {0.0f, 14.0f, 0.0f}});
		world.Emplace<aether::ColliderComponent>(arm, aether::ColliderComponent{.shape = aether::PhysicsShapeType::Capsule, .radius = 0.05f, .halfHeight = 0.3f});
		world.Emplace<aether::JointComponent>(arm,
		        aether::JointComponent{
		                .type = aether::JointType::SwingTwist,
		                .target = aether::Entity{},
		                .anchor = anchor,
		                .axis = {0.0f, 0.0f, 1.0f},
		                .swingLimit = glm::radians(swingLimitDeg),
		        });

		const int frames = static_cast<int>(1.5f / aether::PhysicsSystem::kFixedTimestep + 0.5f);
		for (int i = 0; i < frames; ++i)
		{
			physics->Update(world, aether::PhysicsSystem::kFixedTimestep);
		}

		const glm::quat rot = world.Get<aether::PhysicsStateComponent>(arm).currRotation;
		const glm::vec3 twistNow = rot * glm::vec3(0.0f, 0.0f, 1.0f);
		const float swingRad = std::acos(glm::clamp(glm::dot(glm::vec3(0.0f, 0.0f, 1.0f), twistNow), -1.0f, 1.0f));
		return glm::degrees(swingRad);
	};

	const float narrow = measureFinalSwingDeg(20.0f);
	const float wide = measureFinalSwingDeg(70.0f);
	CHECK(std::abs(narrow - 20.0f) <= 5.0f);
	CHECK(std::abs(wide - 70.0f) <= 5.0f);
}

TEST_CASE("Hinge joint bends up to its configured limit and blocks hyperextension past it")
{
	auto measureFinalBendDeg = [](float minLimitDeg, float maxLimitDeg, float angularVelRadPerSec) -> float
	{
		aether::World world;
		world.SetSceneKind(aether::SceneKind::Scene3D);
		world.SetSceneFeatures(aether::DefaultSceneFeatures(aether::SceneKind::Scene3D));
		auto system = std::make_unique<aether::PhysicsSystem>();
		aether::PhysicsSystem* physics = system.get();
		world.RegisterSystem(std::move(system));

		const glm::vec3 anchor{0.0f, 2.0f, 0.0f};
		const glm::vec3 axis{1.0f, 0.0f, 0.0f};
		const aether::Entity shin = world.Create();
		world.Emplace<aether::TransformComponent>(shin, aether::TransformComponent{.localToWorld = aether::ComposeTransform(anchor + glm::vec3(0.0f, -0.25f, 0.0f), {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f})});
		world.Emplace<aether::RigidBodyComponent>(shin, aether::RigidBodyComponent{.motionType = aether::PhysicsMotionType::Dynamic, .gravityFactor = 0.0f, .initialAngularVelocity = axis * angularVelRadPerSec});
		world.Emplace<aether::ColliderComponent>(shin, aether::ColliderComponent{.shape = aether::PhysicsShapeType::Capsule, .radius = 0.05f, .halfHeight = 0.25f});
		world.Emplace<aether::JointComponent>(shin,
		        aether::JointComponent{
		                .type = aether::JointType::Hinge,
		                .target = aether::Entity{},
		                .anchor = anchor,
		                .axis = axis,
		                .minLimit = glm::radians(minLimitDeg),
		                .maxLimit = glm::radians(maxLimitDeg),
		        });

		const int frames = static_cast<int>(1.5f / aether::PhysicsSystem::kFixedTimestep + 0.5f);
		for (int i = 0; i < frames; ++i)
		{
			physics->Update(world, aether::PhysicsSystem::kFixedTimestep);
		}
		return SignedDegreesAroundAxis(world.Get<aether::PhysicsStateComponent>(shin).currRotation, axis);
	};

	// Drive hard in both directions. Which velocity sign reaches which configured limit
	// depends on Jolt's own hinge-angle sign convention for this body/axis order, which
	// is not this test's concern - what min_limit/max_limit promise is the MAGNITUDE
	// each direction stops at: one is already at its limit (max = 0, so it can barely
	// move at all - no hyperextension), the other drives all the way to min's magnitude.
	const float driveA = measureFinalBendDeg(-140.0f, 0.0f, -20.0f);
	const float driveB = measureFinalBendDeg(-140.0f, 0.0f, 20.0f);
	const float nearZero = std::min(std::abs(driveA), std::abs(driveB));
	const float nearLimit = std::max(std::abs(driveA), std::abs(driveB));
	CHECK(nearZero <= 6.0f);
	CHECK(std::abs(nearLimit - 140.0f) <= 6.0f);
}

TEST_CASE("A dropped ragdoll collapses and comes to rest without falling through the floor")
{
	RagdollFixture fx;
	fx.MakeStaticBox({0.0f, 0.0f, 0.0f}, {5.0f, 0.1f, 5.0f});
	const aether::Entity root = fx.SpawnRagdollAt({0.0f, 3.0f, 0.0f});
	fx.StepSeconds(3.0f);

	const auto& rag = fx.world.Get<aether::RagdollComponent>(root);
	float minY = 1e9f;
	float maxY = -1e9f;
	for (const aether::Entity bone: rag.bones)
	{
		const float y = fx.world.Get<aether::PhysicsStateComponent>(bone).currPosition.y;
		CHECK(y > -1.0f);
		CHECK(y < 3.0f);
		minY = std::min(minY, y);
		maxY = std::max(maxY, y);
	}
	// Collapsed - the ragdoll's overall vertical extent shrank a lot from a standing
	// pose (~1.8 m of bone positions) once gravity and the ground take over.
	CHECK(maxY - minY < 1.0f);
}

TEST_CASE("A ragdoll dropped onto a dynamic crate moves it")
{
	RagdollFixture fx;
	fx.MakeStaticBox({0.0f, 0.0f, 0.0f}, {5.0f, 0.1f, 5.0f});
	const glm::vec3 crateStart{0.0f, 0.6f, 0.0f};
	const aether::Entity crate = fx.MakeDynamicBox(crateStart, {0.4f, 0.4f, 0.4f});
	fx.SpawnRagdollAt({0.0f, 2.5f, 0.0f});
	fx.StepSeconds(2.0f);

	const glm::vec3 crateEnd = fx.world.Get<aether::PhysicsStateComponent>(crate).currPosition;
	CHECK(glm::length(crateEnd - crateStart) > 0.1f);
}

TEST_CASE("Ragdoll builder auto-detects a centimetre-authored skeleton and normalises it")
{
	// Discovered live against the real Human.gltf: some Mixamo-exported rigs carry
	// bind-pose translations in centimetres (Hips ~104 units up) with no accompanying
	// scale node to say so. Fed through unchanged, a "1.0" capsule radius heuristic
	// against a "104"-unit skeleton builds a ragdoll roughly 100x too large, spawned
	// 100x too high - exactly what exploded the first live smoke-test attempt.
	aether::World world;
	world.SetSceneKind(aether::SceneKind::Scene3D);
	world.SetSceneFeatures(aether::DefaultSceneFeatures(aether::SceneKind::Scene3D));
	auto system = std::make_unique<aether::PhysicsSystem>();
	world.RegisterSystem(std::move(system));

	const aether::assets::GltfAsset cmSkeleton = MakeHumanoidSkeleton(100.0f);
	const aether::Entity root = world.Create();
	world.Emplace<aether::TransformComponent>(root, aether::TransformComponent{.localToWorld = aether::ComposeTransform({0.0f, 5.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f})});
	REQUIRE(aether::SpawnRagdoll(world, root, cmSkeleton));

	const auto& rag = world.Get<aether::RagdollComponent>(root);
	CHECK(rag.bones.size() == 11);
	for (const aether::Entity bone: rag.bones)
	{
		// Every bone should land within a few metres of the spawn point (a person-sized
		// ragdoll, not one ~100x too large) and use a person-sized capsule radius, not a
		// multi-metre one.
		const glm::vec3 pos = world.Get<aether::TransformComponent>(bone).localToWorld[3];
		CHECK(glm::length(pos - glm::vec3(0.0f, 5.0f, 0.0f)) < 3.0f);
		const auto& collider = world.Get<aether::ColliderComponent>(bone);
		CHECK(collider.radius < 1.0f);
	}
}
