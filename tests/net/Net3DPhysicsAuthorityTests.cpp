// 3D physics simulation authority, over a REAL two-peer connection. NetworkContextTests.cpp
// already pins the SyncSimulationAuthority/RestoreSimulationAuthority state transitions
// (motion type flips, marker components, ragdoll-as-a-unit grouping) by calling the function
// directly against one World. What none of those prove is the actual claim the feature exists
// for: that a REMOTE peer's own Jolt simulation of a non-owned Dynamic body stops fighting the
// network and instead tracks what the OWNER's PhysicsSystem actually computed. This file is
// the two-peer version of NetClientAuthorityTests.cpp, with a real PhysicsSystem registered on
// every node's World instead of a bare TransformComponent.
//
// The discriminator: gravity here is purely vertical, so a non-owned body that stayed Dynamic
// and simply free-fell from rest would show ZERO horizontal drift - two peers integrating the
// same deterministic fall from the same rest state look identical whether or not the fix
// exists. The owner is given a horizontal kick nothing else replicates. If the host's rendered
// X ends up near the owner's real (moving) X rather than near the spawn X, the only thing that
// could have put it there is the kinematic-target push driven by an inbound snapshot.
#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <thread>
#include <vector>

#include "net/NetComponents.hpp"
#include "net/NetworkContext.hpp"
#include "net/NetworkSystems.hpp"
#include "physics/PhysicsComponents.hpp"
#include "physics/PhysicsSystem.hpp"
#include "scene/Components.hpp"
#include "scene/SceneSerializer.hpp"
#include "scene/TransformUtils.hpp"
#include "scene/World.hpp"
#include "utils/ServiceContainer.hpp"

using namespace aether;

namespace
{
	// A scratch prefab directory, the same shape as NetClientAuthorityTests.cpp's
	// PrefabFixture, but the one entity ALSO carries a PhysicsRecord - a Dynamic box - so
	// InstantiatePrefab gives both peers a real RigidBodyComponent + ColliderComponent
	// through the same apply path a hand-authored scene uses. A distinct temp directory
	// and prefab name keep this from colliding with that file's own fixture.
	struct PrefabFixture
	{
		std::filesystem::path dir;

		PrefabFixture()
		      : dir(std::filesystem::temp_directory_path() / "aether_net_3d_physics_authority_test")
		{
			std::filesystem::remove_all(dir);
			std::filesystem::create_directories(dir);
			aether::app::scene::SetProjectSceneDirectories(dir / "scenes", dir);

			aether::app::scene::SceneDescription prefab;
			prefab.name = "physics_authority_test_prefab";
			aether::app::scene::EntityRecord record;
			record.entityId = 1;
			record.name = "Box";
			record.hasTransform = true;
			record.physics = aether::app::scene::PhysicsRecord{
			        .shapeType = PhysicsShapeType::Box,
			        .halfExtents = {0.5f, 0.5f, 0.5f},
			        .motionType = PhysicsMotionType::Dynamic,
			        .gravityFactor = 1.0f,
			};
			prefab.entities.push_back(std::move(record));
			REQUIRE(aether::app::scene::SavePrefabFile("physics_authority_test_prefab", prefab));
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

	constexpr const char* kPrefab = "physics_authority_test_prefab";

	// One peer: world, a real registered PhysicsSystem, and both network systems - the
	// same trio Application registers, driven directly so the test controls the exact
	// interleaving instead of going through World::UpdateSystems.
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
	};

	// Runs receive then send on every node - Application's registration order - until
	// `done()` or the deadline. ENet needs several service calls to complete a handshake
	// or land a packet, so a single pass is never enough.
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

	// A host with one real connected client, both peers' send rate uncapped and host
	// relevancy effectively disabled - pacing and culling are not this file's subject.
	struct Session
	{
		PrefabFixture prefabs;
		Node host;
		Node client;

		explicit Session(std::uint16_t port)
		{
			REQUIRE(host.context.StartHost(host.world, port, 8));
			host.context.SetSendRateHz(1'000'000.f);
			host.context.Relevancy().radius = 1e6f;

			REQUIRE(client.context.StartClient(client.world, "127.0.0.1", port));
			client.context.SetSendRateHz(1'000'000.f);

			REQUIRE(RunNetwork({&host, &client}, [&] { return client.context.IsConnected(); }));
		}

		[[nodiscard]] aether::net::ConnectionId ClientConnection()
		{
			const auto connections = host.context.Session().Connections();
			REQUIRE(connections.size() == 1);
			return connections.front();
		}

		// Spawns the physics prefab owned by `owner` and waits for the client to have it.
		[[nodiscard]] std::uint32_t Spawn(glm::vec3 position, aether::net::ConnectionId owner)
		{
			const Entity entity = host.context.SpawnPrefab(host.world, kPrefab, position, owner);
			REQUIRE(entity.IsValid());
			const std::uint32_t netId = host.world.Get<aether::net::NetworkIdentity>(entity).netId;
			REQUIRE(RunNetwork({&host, &client}, [&] { return client.EntityFor(netId).IsValid(); }));
			return netId;
		}
	};
} // namespace

TEST_CASE("A non-owned 3D dynamic body tracks its owner's real simulation instead of free-falling independently")
{
	Session s(24790);
	const aether::net::ConnectionId owner = s.ClientConnection();
	const std::uint32_t netId = s.Spawn({0.f, 5.f, 0.f}, owner);

	// The owner's kick. Gravity alone puts a body nowhere but straight down, so any
	// horizontal drift measured on the HOST later can only be explained by the network,
	// not by two peers coincidentally integrating the same deterministic fall from rest.
	// Set before this world's physics has ever stepped once, so FlushPendingBodies bakes
	// it into the very first Jolt body it creates for this entity (see the field's own
	// comment: applied once, at creation).
	const Entity clientEntity = s.client.EntityFor(netId);
	s.client.world.Get<RigidBodyComponent>(clientEntity).initialVelocity = {10.f, 2.f, 0.f};

	// Physics and network, interleaved for both peers, for 1.5 simulated seconds -
	// Application's own order (physics integrates this frame, then the network exchanges
	// its result) run on each node in turn.
	for (int i = 0; i < 90; ++i)
	{
		s.client.physics->Update(s.client.world, PhysicsSystem::kFixedTimestep);
		s.client.send.Update(s.client.world, PhysicsSystem::kFixedTimestep);

		s.host.receive.Update(s.host.world, PhysicsSystem::kFixedTimestep);
		s.host.physics->Update(s.host.world, PhysicsSystem::kFixedTimestep);
		s.host.send.Update(s.host.world, PhysicsSystem::kFixedTimestep);

		s.client.receive.Update(s.client.world, PhysicsSystem::kFixedTimestep);
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}

	// The mechanism: the owner still integrates its own body normally, and the
	// non-owner's copy has been forced Kinematic - the same state
	// NetworkContextTests.cpp pins by calling SyncSimulationAuthority directly, now
	// reached through the real per-frame network loop instead of a direct call.
	CHECK(s.client.MotionTypeOfNetId(netId) == PhysicsMotionType::Dynamic);
	CHECK(s.host.MotionTypeOfNetId(netId) == PhysicsMotionType::Kinematic);

	// The observable claim. First, the control: the owner's kick actually moved it.
	const float ownerX = s.client.PositionOfNetId(netId).x;
	INFO("owner x = " << ownerX);
	REQUIRE(ownerX > 5.f);

	// Then the convergence: the host's copy - which local gravity alone could never move
	// sideways - ended up near the owner's real, moving X rather than near the spawn X
	// (0) a still-independent Dynamic simulation would have shown.
	const float hostX = s.host.PositionOfNetId(netId).x;
	INFO("host x = " << hostX);
	CHECK(hostX > 3.f);
	CHECK(std::abs(hostX - ownerX) < 3.f);
}

TEST_CASE("The owner's dynamic body is never frozen, single-player parity for the 3D physics authority path")
{
	// Offline has no session, so SyncSimulationAuthority's very first check
	// (!IsConnected()) returns before it can touch anything - the same single-player
	// parity NetClientAuthorityTests.cpp pins for the transform-only path, exercised here
	// with a real falling body so a regression that started freezing offline physics
	// would show up as "does not fall" rather than only as a state flag.
	ServiceContainer services;
	World world;
	world.SetSceneKind(SceneKind::Scene3D);
	world.SetSceneFeatures(DefaultSceneFeatures(SceneKind::Scene3D));
	auto system = std::make_unique<PhysicsSystem>();
	PhysicsSystem* physics = system.get();
	world.RegisterSystem(std::move(system));
	aether::net::NetworkContext context(services);
	aether::net::NetworkReceiveSystem receive(context);

	const Entity entity = world.Create();
	world.Emplace<TransformComponent>(entity).localToWorld
	        = ComposeTransform({0.f, 5.f, 0.f}, {0.f, 0.f, 0.f}, {1.f, 1.f, 1.f});
	world.Emplace<RigidBodyComponent>(entity, RigidBodyComponent{.motionType = PhysicsMotionType::Dynamic});
	world.Emplace<ColliderComponent>(entity,
	        ColliderComponent{.shape = PhysicsShapeType::Box, .halfExtents = {0.5f, 0.5f, 0.5f}});
	// A leftover owner from an ended session - the one shape of this that could silently
	// stop a solo player's own body being simulated (see NetClientAuthorityTests.cpp's
	// matching case for the transform-only path).
	world.Emplace<aether::net::NetworkIdentity>(entity, aether::net::NetworkIdentity{.netId = 5, .owner = 9});

	for (int i = 0; i < 60; ++i)
	{
		physics->Update(world, PhysicsSystem::kFixedTimestep);
		receive.Update(world, PhysicsSystem::kFixedTimestep);
	}

	CHECK(world.Get<RigidBodyComponent>(entity).motionType == PhysicsMotionType::Dynamic);
	CHECK(world.Get<TransformComponent>(entity).localToWorld[3].y < 4.5f); // it fell
}
