// The full physics-gun loop, over REAL peers with REAL PhysicsSystem instances on
// every one of them: grab (claim ownership), carry (drive it - velocity writes each
// physics tick, replicated), throw (impulse + release), and someone else picks it
// back up mid-flight. Every PIECE already has isolated coverage - NetOwnershipTests.cpp
// for the wire messages and the validation rule, NetworkContextTests.cpp for
// RequestOwnershipTransfer's contract and the ragdoll freeze-as-a-unit state
// transitions, NetVelocityTests.cpp/NetRagdollTests.cpp for the two new replication
// messages in isolation, Net3DPhysicsAuthorityTests.cpp for the base
// SyncSimulationAuthority mechanism this all sits on top of. None of those prove the
// COMPOSITION: that claiming actually flips who simulates a body, that what the new
// owner does with it is what a second peer's own Jolt body ends up doing, and that
// handing it off again does not glitch. This file is that proof, reusing
// Net3DPhysicsAuthorityTests.cpp's Node/RunNetwork shape (a fresh copy per that
// file's own precedent - "a shared header for one struct costs more than the
// duplication does" - extended with a driving helper and a third peer for the
// mid-flight handoff).
#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <thread>
#include <vector>

#include "assets/GltfAsset.hpp"
#include "net/NetComponents.hpp"
#include "net/NetworkContext.hpp"
#include "net/NetworkSystems.hpp"
#include "physics/PhysicsComponents.hpp"
#include "physics/PhysicsSystem.hpp"
#include "physics/RagdollBuilder.hpp"
#include "scene/Components.hpp"
#include "scene/SceneSerializer.hpp"
#include "scene/TransformUtils.hpp"
#include "scene/World.hpp"
#include "utils/ServiceContainer.hpp"

using namespace aether;

namespace
{
	struct PrefabFixture
	{
		std::filesystem::path dir;

		PrefabFixture()
		      : dir(std::filesystem::temp_directory_path() / "aether_net_ownership_replication_integration_test")
		{
			std::filesystem::remove_all(dir);
			std::filesystem::create_directories(dir);
			aether::app::scene::SetProjectSceneDirectories(dir / "scenes", dir);

			aether::app::scene::SceneDescription prefab;
			prefab.name = "ownership_replication_test_prefab";
			aether::app::scene::EntityRecord record;
			record.entityId = 1;
			record.name = "Crate";
			record.hasTransform = true;
			record.physics = aether::app::scene::PhysicsRecord{
			        .shapeType = PhysicsShapeType::Box,
			        .halfExtents = {0.5f, 0.5f, 0.5f},
			        .motionType = PhysicsMotionType::Dynamic,
			        .gravityFactor = 1.0f,
			};
			prefab.entities.push_back(std::move(record));
			REQUIRE(aether::app::scene::SavePrefabFile("ownership_replication_test_prefab", prefab));
		}

		~PrefabFixture()
		{
			aether::app::scene::ClearProjectSceneDirectories();
			std::error_code ec;
			std::filesystem::remove_all(dir, ec);
		}

		PrefabFixture(const PrefabFixture&) = delete;
		PrefabFixture& operator=(const PrefabFixture&) = delete;
	};

	constexpr const char* kPrefab = "ownership_replication_test_prefab";

	// A small synthetic humanoid skeleton, the same shape RagdollBuilderTests.cpp
	// keeps (bone/joint resolution is name-driven, not asset-specific - see
	// RagdollBuilder.cpp's FindNodeByRole) - duplicated locally rather than shared
	// for the same reason PrefabFixture is duplicated per-file in this test suite.
	aether::assets::GltfAsset MakeHumanoidSkeleton()
	{
		aether::assets::GltfAsset asset;
		auto add = [&](const char* name, std::int32_t parent, glm::vec3 t)
		{
			aether::assets::GltfNode node;
			node.name = name;
			node.parentIndex = parent;
			node.translation = t;
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

	// One peer: world, a real registered PhysicsSystem, and both network systems,
	// driven directly so the test controls the exact interleaving - the same trio
	// Application registers, matching Net3DPhysicsAuthorityTests.cpp's Node exactly.
	struct Node
	{
		ServiceContainer services;
		World world;
		aether::net::NetworkContext context{services};
		aether::net::NetworkReceiveSystem receive{context};
		aether::net::NetworkSendSystem send{context};
		PhysicsSystem* physics = nullptr;

		Node()
		{
			world.SetSceneKind(SceneKind::Scene3D);
			world.SetSceneFeatures(DefaultSceneFeatures(SceneKind::Scene3D));
			auto system = std::make_unique<PhysicsSystem>();
			physics = system.get();
			world.RegisterSystem(std::move(system));
		}

		~Node()
		{
			context.Stop(world);
		}

		Node(const Node&) = delete;
		Node& operator=(const Node&) = delete;

		[[nodiscard]] Entity EntityFor(std::uint32_t netId)
		{
			return context.Session().EntityFor(netId);
		}

		[[nodiscard]] glm::vec3 PositionOfNetId(std::uint32_t netId)
		{
			const Entity entity = EntityFor(netId);
			if (!entity.IsValid())
			{
				return glm::vec3(-777.f);
			}
			const auto* transform = world.TryGet<TransformComponent>(entity);
			return transform != nullptr ? glm::vec3(transform->localToWorld[3]) : glm::vec3(-777.f);
		}

		[[nodiscard]] PhysicsMotionType MotionTypeOfNetId(std::uint32_t netId)
		{
			const Entity entity = EntityFor(netId);
			REQUIRE(entity.IsValid());
			return world.Get<RigidBodyComponent>(entity).motionType;
		}

		[[nodiscard]] aether::net::ConnectionId OwnerOfNetId(std::uint32_t netId)
		{
			const Entity entity = EntityFor(netId);
			REQUIRE(entity.IsValid());
			return world.Get<aether::net::NetworkIdentity>(entity).owner;
		}

		[[nodiscard]] glm::vec3 JoltVelocityOfNetId(std::uint32_t netId)
		{
			const Entity entity = EntityFor(netId);
			REQUIRE(entity.IsValid());
			return physics->GetLinearVelocity(world.Get<RigidBodyComponent>(entity).body);
		}
	};

	// Network only - receive then send on every node, Application's registration
	// order - until `done()` or the deadline. Used while WAITING for a handshake or
	// an ownership decision to land, where advancing physics would only add noise.
	bool RunNetwork(const std::vector<Node*>& nodes, auto done, int maxMs = 3000)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(maxMs);
		while (std::chrono::steady_clock::now() < deadline)
		{
			for (Node* node: nodes)
			{
				node->receive.Update(node->world, 1.f / 60.f);
				node->send.Update(node->world, 1.f / 60.f);
			}
			if (done())
			{
				return true;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		return false;
	}

	// Physics AND network, interleaved, for every node, one simulated frame -
	// Application's own per-frame order (this node's physics integrates, then the
	// network exchanges the result) run on each node in turn. `beforePhysics(tick)`
	// runs on every node before ITS OWN physics step, which is where a "driving"
	// peer writes this tick's velocity - the physics-gun spring pattern - before
	// Jolt integrates it.
	void RunFrames(const std::vector<Node*>& nodes, int frames, auto beforePhysics)
	{
		for (int tick = 0; tick < frames; ++tick)
		{
			for (Node* node: nodes)
			{
				beforePhysics(*node, tick);
				node->physics->Update(node->world, PhysicsSystem::kFixedTimestep);
				node->receive.Update(node->world, PhysicsSystem::kFixedTimestep);
				node->send.Update(node->world, PhysicsSystem::kFixedTimestep);
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
	}

	void RunFrames(const std::vector<Node*>& nodes, int frames)
	{
		RunFrames(nodes, frames, [](Node&, int) {});
	}

	// A host with `clientCount` real connected clients, all send rates uncapped and
	// host relevancy effectively disabled - pacing and culling are not this file's
	// subject, exactly like Net3DPhysicsAuthorityTests.cpp's Session.
	struct Session
	{
		PrefabFixture prefabs;
		Node host;
		std::vector<std::unique_ptr<Node>> clients;

		Session(std::uint16_t port, int clientCount)
		{
			REQUIRE(host.context.StartHost(host.world, port, 8));
			host.context.SetSendRateHz(1'000'000.f);
			host.context.Relevancy().radius = 1e6f;

			std::vector<Node*> all{&host};
			for (int i = 0; i < clientCount; ++i)
			{
				auto client = std::make_unique<Node>();
				REQUIRE(client->context.StartClient(client->world, "127.0.0.1", port));
				client->context.SetSendRateHz(1'000'000.f);
				all.push_back(client.get());
				clients.push_back(std::move(client));
			}
			REQUIRE(RunNetwork(all,
			        [&]
			        {
				        for (const auto& c: clients)
				        {
					        if (!c->context.IsConnected())
					        {
						        return false;
					        }
				        }
				        return true;
			        }));
		}

		[[nodiscard]] std::vector<Node*> AllNodes()
		{
			std::vector<Node*> all{&host};
			for (const auto& c: clients)
			{
				all.push_back(c.get());
			}
			return all;
		}

		[[nodiscard]] aether::net::ConnectionId ConnectionOf(Node& client)
		{
			return client.context.LocalConnectionId();
		}

		// Spawns the physics prefab owned by `owner` and waits for every client to
		// have it.
		[[nodiscard]] std::uint32_t Spawn(glm::vec3 position, aether::net::ConnectionId owner)
		{
			const Entity entity = host.context.SpawnPrefab(host.world, kPrefab, position, owner);
			REQUIRE(entity.IsValid());
			const std::uint32_t netId = host.world.Get<aether::net::NetworkIdentity>(entity).netId;
			REQUIRE(RunNetwork(AllNodes(),
			        [&]
			        {
				        for (const auto& c: clients)
				        {
					        if (!c->EntityFor(netId).IsValid())
					        {
						        return false;
					        }
				        }
				        return true;
			        }));
			return netId;
		}
	};
} // namespace

TEST_CASE("Claiming a prop genuinely inverts simulation authority, and the new owner's driven motion converges on the host")
{
	Session s(24992, 1);
	Node& client = *s.clients.front();
	const aether::net::ConnectionId clientConn = s.ConnectionOf(client);

	// 1. Host spawns it, host-owned (the "free" state - see ValidateOwnershipRequest).
	const std::uint32_t netId = s.Spawn({0.f, 5.f, 0.f}, aether::net::kInvalidConnection);
	CHECK(client.OwnerOfNetId(netId) == aether::net::kInvalidConnection);
	CHECK(client.MotionTypeOfNetId(netId) == PhysicsMotionType::Kinematic); // not the client's - frozen

	// 2. The client claims it. Requested, not Applied - the client sends and waits
	// for the host's OwnershipTransfer broadcast, which is what actually flips the
	// field on either end (see RequestOwnershipTransfer's own doc).
	const Entity clientEntity = client.EntityFor(netId);
	REQUIRE(client.context.RequestOwnershipTransfer(client.world, clientEntity, clientConn)
	        == aether::net::OwnershipTransferOutcome::Requested);
	REQUIRE(RunNetwork(s.AllNodes(),
	        [&] { return s.host.OwnerOfNetId(netId) == clientConn && client.OwnerOfNetId(netId) == clientConn; }));

	// GENUINE inversion, not a flag flip in isolation: SyncSimulationAuthority has
	// to actually run (through the real per-frame receive loop) and flip BOTH
	// bodies' real Jolt motion type in the opposite direction from step 1.
	REQUIRE(RunNetwork(s.AllNodes(),
	        [&]
	        {
		        return client.MotionTypeOfNetId(netId) == PhysicsMotionType::Dynamic
		               && s.host.MotionTypeOfNetId(netId) == PhysicsMotionType::Kinematic;
	        }));

	// 3. The client drives it: the physics-gun spring pattern, a velocity write
	// every physics tick toward a target that HOLDS the prop well above where
	// gravity alone would ever leave it, while sweeping it sideways. Neither half
	// is something the host's own (frozen) simulation could produce on its own -
	// if the freeze had silently failed and the host's copy stayed Dynamic, local
	// gravity would pull it straight down regardless of what this loop asks for.
	constexpr glm::vec3 kBase{0.f, 5.f, 0.f};
	constexpr float kAmplitude = 3.f;
	constexpr float kAngularFreq = 2.f; // rad/s - slow enough for a P-controller to track
	constexpr int kDriveFrames = 150; // 2.5s at 60Hz - several full sweeps
	float simTime = 0.f;
	RunFrames(s.AllNodes(), kDriveFrames,
	        [&](Node& node, int)
	        {
		        if (&node != &client)
		        {
			        return;
		        }
		        const glm::vec3 target = kBase + glm::vec3(kAmplitude * std::sin(kAngularFreq * simTime), 0.f, 0.f);
		        const glm::vec3 current = client.PositionOfNetId(netId);
		        const glm::vec3 velocity = (target - current) * 15.f; // spring gain
		        client.physics->SetLinearVelocity(client.world.Get<RigidBodyComponent>(clientEntity).body, velocity);
		        simTime += PhysicsSystem::kFixedTimestep;
	        });

	const glm::vec3 ownerPos = client.PositionOfNetId(netId);
	const glm::vec3 hostPos = s.host.PositionOfNetId(netId);
	INFO("owner pos = (" << ownerPos.x << ", " << ownerPos.y << ", " << ownerPos.z << ")");
	INFO("host pos  = (" << hostPos.x << ", " << hostPos.y << ", " << hostPos.z << ")");

	// Control: the owner's own drive actually held it aloft and away from spawn X.
	REQUIRE(ownerPos.y > 3.f);
	// The claim: the host - which local gravity alone would have pulled well below
	// y=3 by now - tracked the SAME held-aloft, swept position instead.
	CHECK(hostPos.y > 3.f);
	CHECK(std::abs(hostPos.x - ownerPos.x) < 1.5f);
	CHECK(std::abs(hostPos.y - ownerPos.y) < 1.5f);
}

TEST_CASE("Releasing a prop reverts it to the host, and a second client can then claim it mid-flight without teleporting, dropping, or duplicating it")
{
	Session s(24993, 2);
	Node& client1 = *s.clients[0];
	Node& client2 = *s.clients[1];
	const aether::net::ConnectionId conn1 = s.ConnectionOf(client1);
	const aether::net::ConnectionId conn2 = s.ConnectionOf(client2);

	const std::uint32_t netId = s.Spawn({0.f, 5.f, 0.f}, aether::net::kInvalidConnection);
	const Entity client1Entity = client1.EntityFor(netId);

	// Claim.
	REQUIRE(client1.context.RequestOwnershipTransfer(client1.world, client1Entity, conn1)
	        == aether::net::OwnershipTransferOutcome::Requested);
	REQUIRE(RunNetwork(s.AllNodes(), [&] { return s.host.OwnerOfNetId(netId) == conn1; }));
	REQUIRE(RunNetwork(s.AllNodes(), [&] { return client1.MotionTypeOfNetId(netId) == PhysicsMotionType::Dynamic; }));

	// Throw: an impulse nothing else on this body will ever apply, so a velocity
	// found anywhere downstream can only have come from this exact call.
	//
	// The body must actually exist in Jolt before SetLinearVelocity can do
	// anything - RigidBodyComponent::body stays invalid (PENDING) until the
	// first physics tick bakes it (FlushPendingBodies), and every step above
	// this point ran network-only (RunNetwork), never physics. One warm-up
	// frame first, so the throw lands on a real body instead of silently no-op'ing
	// against one that does not exist yet.
	RunFrames(s.AllNodes(), 1);
	constexpr glm::vec3 kThrowVelocity{6.f, 4.f, 0.f};
	client1.physics->SetLinearVelocity(client1.world.Get<RigidBodyComponent>(client1Entity).body, kThrowVelocity);
	RunFrames(s.AllNodes(), 5); // let at least one Snapshot/VelocitySnapshot carry it out

	// The velocity WIRE PATH: by the time the host applies the release below, has
	// NetReceivedVelocity actually carried the throw across the network? Checked
	// BEFORE release - and independent of it - so a refusal or a redirect bug in
	// the next step cannot be confused with this one.
	const auto* hostReceivedVelocity = s.host.world.TryGet<aether::net::NetReceivedVelocity>(s.host.EntityFor(netId));
	REQUIRE(hostReceivedVelocity != nullptr);
	INFO("host's received velocity = (" << hostReceivedVelocity->linear.x << ", " << hostReceivedVelocity->linear.y
	                                    << ", " << hostReceivedVelocity->linear.z << ")");
	CHECK(hostReceivedVelocity->linear.x == doctest::Approx(kThrowVelocity.x).epsilon(0.2));
	CHECK(hostReceivedVelocity->linear.y == doctest::Approx(kThrowVelocity.y).epsilon(0.2));

	const glm::vec3 positionAtThrow = client1.PositionOfNetId(netId);

	// Release.
	REQUIRE(client1.context.RequestOwnershipTransfer(client1.world, client1Entity, aether::net::kInvalidConnection)
	        == aether::net::OwnershipTransferOutcome::Requested);
	REQUIRE(RunNetwork(s.AllNodes(), [&] { return s.host.OwnerOfNetId(netId) == aether::net::kInvalidConnection; }));
	REQUIRE(RunNetwork(s.AllNodes(),
	        [&]
	        {
		        return s.host.MotionTypeOfNetId(netId) == PhysicsMotionType::Dynamic
		               && client1.MotionTypeOfNetId(netId) == PhysicsMotionType::Kinematic;
	        }));

	// No teleport across the handoff: the host's own position right after reclaim
	// must still be close to where the throw actually happened, not snapped to the
	// prefab's original spawn point or some other stale value.
	const glm::vec3 positionAtReclaim = s.host.PositionOfNetId(netId);
	INFO("position at throw   = (" << positionAtThrow.x << ", " << positionAtThrow.y << ", " << positionAtThrow.z
	                                << ")");
	INFO("position at reclaim = (" << positionAtReclaim.x << ", " << positionAtReclaim.y << ", " << positionAtReclaim.z
	                                << ")");
	CHECK(glm::length(positionAtReclaim - positionAtThrow) < 1.f);

	// FIX VERIFIED HERE: the reclaimed Dynamic body's actual Jolt velocity must
	// carry the throw, not read zero. PushKinematicTargets (PhysicsSystem.cpp)
	// drives a Kinematic body with SetPositionAndRotation - a direct teleport with
	// no Jolt-side velocity implication, unlike Jolt's own MoveKinematic, which
	// would derive one - so while this body was non-owned its real Jolt velocity
	// sat wherever freezing left it, not what the wire said it was doing.
	// ReclaimBody3D (NetworkContext.cpp) now seeds it from NetReceivedVelocity
	// (checked above) the moment the body resumes Dynamic; a regression here
	// reads back at or near zero instead of kThrowVelocity - a thrown crate that
	// stops dead in mid-air the instant its owner changes.
	const glm::vec3 hostVelocityAfterReclaim = s.host.JoltVelocityOfNetId(netId);
	INFO("host Jolt velocity immediately after reclaim = (" << hostVelocityAfterReclaim.x << ", "
	                                                         << hostVelocityAfterReclaim.y << ", "
	                                                         << hostVelocityAfterReclaim.z << ")");
	CHECK(hostVelocityAfterReclaim.x == doctest::Approx(kThrowVelocity.x).epsilon(0.2));
	CHECK(hostVelocityAfterReclaim.y == doctest::Approx(kThrowVelocity.y).epsilon(0.2));

	// Let the host's now-Dynamic body actually fall for a bit under its own gravity
	// before the second claim, so "no teleport" below has a real position to compare against.
	RunFrames(s.AllNodes(), 10);
	const glm::vec3 positionBeforeSecondClaim = s.host.PositionOfNetId(netId);

	// Second client claims the SAME prop mid-flight. Allowed: it is currently free
	// (host-owned) after the release above.
	const Entity client2Entity = client2.EntityFor(netId);
	REQUIRE(client2.context.RequestOwnershipTransfer(client2.world, client2Entity, conn2)
	        == aether::net::OwnershipTransferOutcome::Requested);
	REQUIRE(RunNetwork(s.AllNodes(), [&] { return s.host.OwnerOfNetId(netId) == conn2; }));
	REQUIRE(RunNetwork(s.AllNodes(),
	        [&]
	        {
		        return client2.MotionTypeOfNetId(netId) == PhysicsMotionType::Dynamic
		               && s.host.MotionTypeOfNetId(netId) == PhysicsMotionType::Kinematic
		               && client1.MotionTypeOfNetId(netId) == PhysicsMotionType::Kinematic;
	        }));

	// No teleport on the SECOND handoff either.
	const glm::vec3 positionAfterSecondClaim = client2.PositionOfNetId(netId);
	INFO("position before second claim = (" << positionBeforeSecondClaim.x << ", " << positionBeforeSecondClaim.y
	                                        << ", " << positionBeforeSecondClaim.z << ")");
	INFO("position after second claim  = (" << positionAfterSecondClaim.x << ", " << positionAfterSecondClaim.y
	                                        << ", " << positionAfterSecondClaim.z << ")");
	CHECK(glm::length(positionAfterSecondClaim - positionBeforeSecondClaim) < 1.f);

	// No duplication: every node still resolves this net id to exactly one entity,
	// and the population of replicated identities on each node has not grown.
	for (Node* node: s.AllNodes())
	{
		CHECK(node->EntityFor(netId).IsValid());
	}
	CHECK(client1.OwnerOfNetId(netId) == conn2);
	CHECK(s.host.OwnerOfNetId(netId) == conn2);
	CHECK(client2.OwnerOfNetId(netId) == conn2);
}

TEST_CASE("Claiming a ragdoll's limb transfers the whole body, and every bone converges on the far peer")
{
	Session s(24994, 1);
	Node& client = *s.clients.front();
	const aether::net::ConnectionId clientConn = s.ConnectionOf(client);
	const aether::assets::GltfAsset skeleton = MakeHumanoidSkeleton();

	// A plain replicated entity first (host allocates the net id, both peers get
	// it the normal way), THEN turned into a ragdoll independently on each peer -
	// see RagdollPoseEntry's own comment on why this precondition (SpawnRagdoll
	// runs deterministically, from the same skeleton, on every peer) is what makes
	// bone-index addressing safe without a net id of its own for every bone.
	const std::uint32_t netId = s.Spawn({0.f, 5.f, 0.f}, aether::net::kInvalidConnection);
	const Entity clientRoot = client.EntityFor(netId);
	const Entity hostRoot = s.host.EntityFor(netId);
	REQUIRE(SpawnRagdoll(s.host.world, hostRoot, skeleton));
	REQUIRE(SpawnRagdoll(client.world, clientRoot, skeleton));

	const auto& hostRagdoll = s.host.world.Get<RagdollComponent>(hostRoot);
	const auto& clientRagdoll = client.world.Get<RagdollComponent>(clientRoot);
	REQUIRE(hostRagdoll.bones.size() == clientRagdoll.bones.size());
	REQUIRE(hostRagdoll.bones.size() > 1); // the root plus at least one real limb

	// Claim a LIMB, never the root itself, and confirm the WHOLE body's ownership
	// inverts from a single call naming one arm.
	Entity limb{};
	for (const Entity bone: clientRagdoll.bones)
	{
		if (bone != clientRoot)
		{
			limb = bone;
			break;
		}
	}
	REQUIRE(limb.IsValid());
	REQUIRE(client.world.Has<RagdollBoneComponent>(limb));
	REQUIRE(client.context.RequestOwnershipTransfer(client.world, limb, clientConn)
	        == aether::net::OwnershipTransferOutcome::Requested);
	REQUIRE(RunNetwork(s.AllNodes(), [&] { return s.host.OwnerOfNetId(netId) == clientConn; }));
	REQUIRE(RunNetwork(s.AllNodes(),
	        [&]
	        {
		        return client.MotionTypeOfNetId(netId) == PhysicsMotionType::Dynamic
		               && s.host.MotionTypeOfNetId(netId) == PhysicsMotionType::Kinematic;
	        }));

	// AS A UNIT: every OTHER bone flipped too, on both peers - not just the root
	// bones[0] the claim actually named a net id through.
	for (const Entity bone: clientRagdoll.bones)
	{
		CAPTURE(bone.id);
		CHECK(client.world.Get<RigidBodyComponent>(bone).motionType == PhysicsMotionType::Dynamic);
	}
	for (const Entity bone: hostRagdoll.bones)
	{
		CAPTURE(bone.id);
		CHECK(s.host.world.Get<RigidBodyComponent>(bone).motionType == PhysicsMotionType::Kinematic);
	}

	// Drive the ROOT (the only bone with a net id, so the only one whose
	// position/velocity rides the ordinary Snapshot path) with the same
	// held-aloft, swept pattern as the single-body test - both a position
	// discriminator for the root AND, transitively through the joints, motion
	// every OTHER bone has no independent reason to also show unless
	// RagdollPose is actually landing on it.
	constexpr glm::vec3 kBase{0.f, 5.f, 0.f};
	constexpr float kAmplitude = 2.f;
	constexpr float kAngularFreq = 1.5f;
	constexpr int kDriveFrames = 120;
	float simTime = 0.f;
	RunFrames(s.AllNodes(), kDriveFrames,
	        [&](Node& node, int)
	        {
		        if (&node != &client)
		        {
			        return;
		        }
		        const glm::vec3 target = kBase + glm::vec3(kAmplitude * std::sin(kAngularFreq * simTime), 0.f, 0.f);
		        const auto* transform = client.world.TryGet<TransformComponent>(clientRoot);
		        const glm::vec3 current(transform->localToWorld[3]);
		        const glm::vec3 velocity = (target - current) * 15.f;
		        client.physics->SetLinearVelocity(client.world.Get<RigidBodyComponent>(clientRoot).body, velocity);
		        simTime += PhysicsSystem::kFixedTimestep;
	        });

	// The root converges (the ordinary Snapshot/authority path this sits on top
	// of, already proven elsewhere - checked here only as this test's control).
	const glm::vec3 ownerRootPos(client.world.Get<TransformComponent>(clientRoot).localToWorld[3]);
	const glm::vec3 hostRootPos(s.host.world.Get<TransformComponent>(hostRoot).localToWorld[3]);
	INFO("owner root = (" << ownerRootPos.x << ", " << ownerRootPos.y << ", " << ownerRootPos.z << ")");
	INFO("host root  = (" << hostRootPos.x << ", " << hostRootPos.y << ", " << hostRootPos.z << ")");
	REQUIRE(ownerRootPos.y > 3.f);
	CHECK(hostRootPos.y > 3.f);
	CHECK(glm::length(hostRootPos - ownerRootPos) < 1.5f);

	// Both peers' root sits at the same index in their own bones vector - the
	// assumption BuildRagdollPoseSnapshot/ApplyRagdollPoses' index addressing
	// itself depends on (deterministic construction from the same skeleton), and
	// the assumption the loop below relies on to compare the right bones.
	std::size_t clientRootIndex = clientRagdoll.bones.size();
	std::size_t hostRootIndex = hostRagdoll.bones.size();
	for (std::size_t i = 0; i < clientRagdoll.bones.size(); ++i)
	{
		if (clientRagdoll.bones[i] == clientRoot)
		{
			clientRootIndex = i;
		}
		if (hostRagdoll.bones[i] == hostRoot)
		{
			hostRootIndex = i;
		}
	}
	REQUIRE(clientRootIndex == hostRootIndex);

	// The claim: EVERY OTHER bone (limbs jointed to that swept, held-aloft root)
	// also converged - proof RagdollPose is landing on all of them, not just the
	// root's ordinary Snapshot. A limb frozen at its pre-claim pose (the "gap"
	// this feature exists to close) would show a large, growing gap here instead.
	for (std::size_t i = 0; i < clientRagdoll.bones.size(); ++i)
	{
		if (i == clientRootIndex)
		{
			continue; // the root - already checked above as this test's control
		}
		const Entity clientBone = clientRagdoll.bones[i];
		const Entity hostBone = hostRagdoll.bones[i];
		const glm::vec3 ownerBonePos(client.world.Get<TransformComponent>(clientBone).localToWorld[3]);
		const glm::vec3 hostBonePos(s.host.world.Get<TransformComponent>(hostBone).localToWorld[3]);
		CAPTURE(i);
		INFO("owner bone = (" << ownerBonePos.x << ", " << ownerBonePos.y << ", " << ownerBonePos.z << ")");
		INFO("host bone  = (" << hostBonePos.x << ", " << hostBonePos.y << ", " << hostBonePos.z << ")");
		CHECK(glm::length(hostBonePos - ownerBonePos) < 1.5f);
	}
}
