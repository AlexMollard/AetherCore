// NetworkSendSystem: relevancy leave/re-entry (see the design note on
// NetworkContext::ApplyRelevancyLeave) and per-connection send pacing. Unlike
// NetworkSystemsTests.cpp - which drives NetworkReceiveSystem::OnData directly with
// hand-built bytes - these cases run BOTH systems over a real loopback connection,
// because what is under test here is what actually crosses the wire and what a real
// client does with it, not just a decoder's reaction to a byte string.

#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <thread>
#include <vector>

#include "net/NetComponents.hpp"
#include "net/NetSnapshot.hpp"
#include "net/NetworkContext.hpp"
#include "net/NetworkSystems.hpp"
#include "scene/Components.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/SceneSerializer.hpp"
#include "scene/TransformUtils.hpp"
#include "scene/World.hpp"
#include "utils/ServiceContainer.hpp"

#include "NetTestSupport.hpp"

using namespace aether;

namespace
{
	// Points the prefab loader at a scratch directory holding one trivial prefab, so
	// SpawnPrefab/ApplySpawn genuinely instantiate something on both ends. A local
	// copy of NetworkSystemsTests.cpp's PrefabFixture - small enough that sharing it
	// through NetTestSupport.hpp would cost more (a header both files then depend on
	// for one struct) than it saves.
	struct PrefabFixture
	{
		std::filesystem::path dir;

		PrefabFixture()
		      : dir(std::filesystem::temp_directory_path() / "aether_net_send_system_test")
		{
			std::filesystem::remove_all(dir);
			std::filesystem::create_directories(dir);
			aether::app::scene::SetProjectSceneDirectories(dir / "scenes", dir);

			aether::app::scene::SceneDescription prefab;
			prefab.name = "send_system_test_prefab";
			aether::app::scene::EntityRecord record;
			record.entityId = 1;
			record.name = "Body";
			record.hasTransform = true;
			prefab.entities.push_back(std::move(record));
			REQUIRE(aether::app::scene::SavePrefabFile("send_system_test_prefab", prefab));
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

	constexpr const char* kPrefab = "send_system_test_prefab";

	// One end of a connection: its own world, context and receive system. Stop()
	// on teardown mirrors every other net test fixture.
	struct Node
	{
		ServiceContainer services;
		World world;
		aether::net::NetworkContext context{services};
		aether::net::NetworkReceiveSystem receive{context};

		~Node()
		{
			context.Stop(world);
		}

		Node() = default;
		Node(const Node&) = delete;
		Node& operator=(const Node&) = delete;
	};

	// Pumps every given node's receive system until `done()` or the deadline. ENet
	// needs several service calls to complete a handshake or land a packet, so a
	// single Update() is never enough - mirrors NetLoopbackTests.cpp's PumpUntil,
	// generalised past two fixed endpoints since one case here needs three.
	bool PumpUntil(std::initializer_list<Node*> nodes, auto done, int maxMs = 2000)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(maxMs);
		while (std::chrono::steady_clock::now() < deadline)
		{
			for (Node* node: nodes)
			{
				node->receive.Update(node->world, 0.f);
			}
			if (done())
			{
				return true;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		return false;
	}

	// A host and one real, connected client, plus the host's send system. The rate
	// is left at its 20Hz default - callers that need it uncapped (most of these
	// cases: what is under test is the relevancy transition, not the pacing) call
	// SetSendRateHz themselves.
	struct Pair
	{
		Node host;
		Node client;
		aether::net::NetworkSendSystem send{host.context};
		aether::net::ConnectionId hostSawPeer = aether::net::kInvalidConnection;

		// `seed` runs on BOTH worlds before either end starts, which is the only
		// window in which a scene-placed entity can be set up: the host numbers its
		// scene entities inside StartHost and the client numbers its own the moment
		// the Welcome lands, and both derivations walk SceneNodeComponent::id. An
		// entity created after either point is simply not part of the scene as far as
		// the deterministic id derivation is concerned.
		explicit Pair(std::uint16_t port, const std::function<void(World&)>& seed = {})
		{
			if (seed)
			{
				seed(host.world);
				seed(client.world);
			}
			REQUIRE(host.context.StartHost(host.world, port, 4));
			REQUIRE(client.context.StartClient(client.world, "127.0.0.1", port));

			REQUIRE(PumpUntil({&host, &client}, [&] { return client.context.IsConnected(); }));

			for (const aether::net::ConnectionId conn: host.context.Session().Connections())
			{
				hostSawPeer = conn;
			}
			REQUIRE(hostSawPeer != aether::net::kInvalidConnection);
		}
	};

	glm::vec3 PositionOf(World& world, Entity entity)
	{
		const auto* transform = world.TryGet<TransformComponent>(entity);
		return transform != nullptr ? glm::vec3(transform->localToWorld[3]) : glm::vec3(-999.f);
	}

	void MoveTo(World& world, Entity entity, glm::vec3 position)
	{
		world.TryGet<TransformComponent>(entity)->localToWorld = ComposeTransform(position, {0.f, 0.f, 0.f}, {1.f, 1.f, 1.f});
	}

	void MoveWithRotation(World& world, Entity entity, glm::vec3 position, glm::vec3 euler)
	{
		world.TryGet<TransformComponent>(entity)->localToWorld = ComposeTransform(position, euler, {1.f, 1.f, 1.f});
	}

	glm::vec3 EulerOf(World& world, Entity entity)
	{
		const auto* transform = world.TryGet<TransformComponent>(entity);
		if (transform == nullptr)
		{
			return glm::vec3(-999.f);
		}
		glm::vec3 position;
		glm::vec3 euler;
		glm::vec3 scale;
		DecomposeTRS(transform->localToWorld, position, euler, scale);
		return euler;
	}
} // namespace

TEST_CASE("An entity that leaves a connection's relevancy radius is despawned client-side")
{
	const PrefabFixture prefabs;
	Pair p(24730);
	p.host.context.Relevancy().radius = 50.f;
	p.host.context.SetSendRateHz(1'000'000.f); // pacing is not what this case tests

	const Entity hostEntity = p.host.context.SpawnPrefab(p.host.world, kPrefab, {5.f, 0.f, 0.f},
	        aether::net::kInvalidConnection);
	REQUIRE(hostEntity.IsValid());
	const std::uint32_t netId = p.host.world.TryGet<aether::net::NetworkIdentity>(hostEntity)->netId;

	REQUIRE(PumpUntil({&p.host, &p.client}, [&] { return p.client.context.Session().EntityFor(netId).IsValid(); }));
	const Entity clientEntity = p.client.context.Session().EntityFor(netId);

	// Tick 1: relevant (distance 5 < radius 50). Seeds the baseline; no message yet.
	p.send.Update(p.host.world, 0.f);

	// The entity walks out of range.
	MoveTo(p.host.world, hostEntity, {5000.f, 0.f, 0.f});

	// Tick 2: no longer relevant, still alive - a leave, not a despawn.
	p.send.Update(p.host.world, 0.f);

	REQUIRE(PumpUntil({&p.host, &p.client}, [&] { return !p.client.context.Session().EntityFor(netId).IsValid(); }, 3000));
	CHECK_FALSE(p.client.world.GetRegistry().valid(World::ToEntt(clientEntity)));
	// The entity is still alive on the host - a leave is not a despawn.
	CHECK(p.host.world.GetRegistry().valid(World::ToEntt(hostEntity)));
}

TEST_CASE("An entity the client cannot recreate is never relevancy-despawned")
{
	// THE case this file was missing, and the one defect it let through. A relevancy
	// leave is ecs::DestroyHierarchy client-side, and the re-entry that is supposed to
	// undo it is an EncodeSpawn naming `spawnPrefab` - which is EMPTY for a
	// scene-placed entity by design, so ApplySpawn refuses it. Culling one is
	// therefore not "stop caring for now", it is a permanent delete that nothing in
	// the framework can reverse: not re-entry, not the join replay, only a full scene
	// reload. At the default 60-unit radius that is every replicated scene entity a
	// player walks away from.
	//
	// Three entities, one radius change, so the exemption is measured against a live
	// control rather than asserted on its own:
	//   - scene-placed        exempt (nothing can recreate it)
	//   - bound, no prefab    exempt for the same reason, without the flag
	//   - prefab-spawned      NOT exempt: this is the control, and it must still go
	const PrefabFixture prefabs;
	Pair p(24735, [](World& world) { aether::net::test::MakeScenePlaced(world, 1); });
	p.host.context.Relevancy().radius = 50.f;
	p.host.context.SetSendRateHz(1'000'000.f);

	// The scene-placed entity, numbered identically on both ends off the scene node id.
	const std::vector<std::uint32_t> sceneIds = aether::net::test::ScenePlacedNetIds(p.host.world);
	REQUIRE(sceneIds.size() == 1);
	const std::uint32_t sceneNetId = sceneIds.front();
	const Entity hostScene = p.host.context.Session().EntityFor(sceneNetId);
	REQUIRE(hostScene.IsValid());
	REQUIRE(PumpUntil({&p.host, &p.client}, [&] { return p.client.context.IsConnected(); }));
	const Entity clientScene = p.client.context.Session().EntityFor(sceneNetId);
	REQUIRE(clientScene.IsValid()); // derived locally, never spawned across the wire

	// A replicated entity with no prefab name and no scenePlaced flag: the other half
	// of the same predicate. Nothing in the framework produces one today, which is
	// exactly why it needs pinning - the flag is only set by AssignScenePlacedNetIds,
	// so "not flagged" must not be read as "recreatable". Bound by hand on both ends
	// because no spawn message could establish it.
	const std::uint32_t prefablessNetId = p.host.context.Session().AllocateNetId();
	{
		const Entity onHost = p.host.world.Create();
		p.host.world.Emplace<TransformComponent>(onHost);
		p.host.world.Emplace<aether::net::NetworkIdentity>(onHost,
		        aether::net::NetworkIdentity{.netId = prefablessNetId, .owner = aether::net::kInvalidConnection});
		p.host.context.Session().Bind(prefablessNetId, onHost);

		const Entity onClient = p.client.world.Create();
		p.client.world.Emplace<TransformComponent>(onClient);
		p.client.world.Emplace<aether::net::NetworkIdentity>(onClient,
		        aether::net::NetworkIdentity{.netId = prefablessNetId, .owner = aether::net::kInvalidConnection});
		p.client.context.Session().Bind(prefablessNetId, onClient);
	}
	const Entity hostPrefabless = p.host.context.Session().EntityFor(prefablessNetId);

	// The control: an ordinary prefab-spawned entity, which the client CAN rebuild.
	const Entity hostSpawned = p.host.context.SpawnPrefab(p.host.world, kPrefab, {6.f, 0.f, 0.f},
	        aether::net::kInvalidConnection);
	REQUIRE(hostSpawned.IsValid());
	const std::uint32_t spawnedNetId = p.host.world.TryGet<aether::net::NetworkIdentity>(hostSpawned)->netId;
	REQUIRE(PumpUntil({&p.host, &p.client}, [&] { return p.client.context.Session().EntityFor(spawnedNetId).IsValid(); }));

	// All three inside the radius, so all three enter this connection's relevant set.
	MoveTo(p.host.world, hostScene, {5.f, 0.f, 0.f});
	MoveTo(p.host.world, hostPrefabless, {7.f, 0.f, 0.f});
	p.send.Update(p.host.world, 0.f); // tick 1: seeds the baseline with all three

	// All three walk out together.
	MoveTo(p.host.world, hostScene, {5000.f, 0.f, 0.f});
	MoveTo(p.host.world, hostPrefabless, {5000.f, 0.f, 0.f});
	MoveTo(p.host.world, hostSpawned, {5000.f, 0.f, 0.f});
	p.send.Update(p.host.world, 0.f); // tick 2: all three leave the relevant set

	// The control leaving is what proves the leave path ran at all this tick - without
	// it, "the scene entity survived" would pass just as well against a system that
	// sent nothing whatsoever.
	REQUIRE(PumpUntil({&p.host, &p.client}, [&] { return !p.client.context.Session().EntityFor(spawnedNetId).IsValid(); },
	        3000));

	// Give any wrongly-sent leave for the other two every chance to land. Both ends
	// have to keep pumping: a host's sends only leave the box when its own Poll()
	// services enet_host, so a queued-but-unflushed leave would otherwise let this
	// case pass for the wrong reason.
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
	while (std::chrono::steady_clock::now() < deadline)
	{
		p.host.receive.Update(p.host.world, 0.f);
		p.client.receive.Update(p.client.world, 0.f);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}

	CHECK(p.client.context.Session().EntityFor(sceneNetId) == clientScene);
	CHECK(p.client.world.GetRegistry().valid(World::ToEntt(clientScene)));
	CHECK(p.client.context.Session().EntityFor(prefablessNetId).IsValid());
	// And the host kept all three - a leave was never a host-side destroy either way.
	CHECK(p.host.world.GetRegistry().valid(World::ToEntt(hostScene)));
	CHECK(p.host.world.GetRegistry().valid(World::ToEntt(hostSpawned)));
}

TEST_CASE("An entity outside a joiner's radius is not replayed to it, so it cannot freeze there")
{
	// The join replay used to be unfiltered while the send system diffed only the
	// relevant set, so an entity out of range AT JOIN existed on the client and was in
	// no connection's "previous" set - it could never reach the leave loop, and sat
	// frozen at its join-time pose for the rest of the session. Filtering the replay
	// through the same relevancy call the send tick makes is what closes that: what a
	// connection has is exactly what relevancy admitted, with no third state.
	//
	// Cannot use Pair: the entity has to exist BEFORE the client connects, which is
	// the whole point.
	constexpr std::uint16_t kPort = 24736;
	const PrefabFixture prefabs;

	Node host;
	Node client;
	REQUIRE(host.context.StartHost(host.world, kPort, 4));
	aether::net::NetworkSendSystem send(host.context);
	host.context.Relevancy().radius = 50.f;
	host.context.SetSendRateHz(1'000'000.f);

	// Far outside the radius, and spawned while nobody is connected - so the ONLY way
	// it could reach the joiner is the replay.
	const Entity hostEntity = host.context.SpawnPrefab(host.world, kPrefab, {5000.f, 0.f, 0.f},
	        aether::net::kInvalidConnection);
	REQUIRE(hostEntity.IsValid());
	const std::uint32_t netId = host.world.TryGet<aether::net::NetworkIdentity>(hostEntity)->netId;

	REQUIRE(client.context.StartClient(client.world, "127.0.0.1", kPort));
	REQUIRE(PumpUntil({&host, &client}, [&] { return client.context.IsConnected(); }));

	// Several send ticks with the entity still out of range, then a settling window:
	// whatever the replay and the ticks are going to send has been sent by now.
	for (int i = 0; i < 3; ++i)
	{
		send.Update(host.world, 0.f);
	}
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
	while (std::chrono::steady_clock::now() < deadline)
	{
		host.receive.Update(host.world, 0.f);
		client.receive.Update(client.world, 0.f);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}

	// Not present at all is the only non-frozen answer available: the client either
	// has an entity relevancy keeps updated, or it does not have it.
	CHECK_FALSE(client.context.Session().EntityFor(netId).IsValid());
	CHECK(aether::net::test::CountIdentities(client.world) == 0);

	// And withholding it is only safe because re-entry genuinely restores it. It comes
	// into range somewhere it has never been announced from.
	MoveTo(host.world, hostEntity, {12.f, 3.f, -4.f});
	send.Update(host.world, 0.f);

	REQUIRE(PumpUntil({&host, &client}, [&] { return client.context.Session().EntityFor(netId).IsValid(); }, 3000));
	const glm::vec3 pos = PositionOf(client.world, client.context.Session().EntityFor(netId));
	CHECK(pos.x == doctest::Approx(12.f));
	CHECK(pos.y == doctest::Approx(3.f));
	CHECK(pos.z == doctest::Approx(-4.f));
}

TEST_CASE("A disconnected connection is dropped from both of the send system's per-connection maps")
{
	// PruneDisconnected had no coverage at all - deleting it outright kept the suite
	// green. Both maps are keyed by ConnectionId and outlive any one connection, so a
	// leak here is not just memory: NetworkSubsystem's id counter resets across a
	// Stop/StartHost cycle, and a reused id inheriting a stale relevant set would make
	// the new connection's first tick diff against the OLD connection's world.
	const PrefabFixture prefabs;
	Pair p(24737);
	p.host.context.SetSendRateHz(1'000'000.f);

	const Entity hostEntity = p.host.context.SpawnPrefab(p.host.world, kPrefab, {0.f, 0.f, 0.f},
	        aether::net::kInvalidConnection);
	REQUIRE(hostEntity.IsValid());

	p.send.Update(p.host.world, 0.f);

	// Positive control: both maps are genuinely populated, so the checks after the
	// disconnect are measuring a removal and not an entry that was never made.
	REQUIRE(p.send.PacedConnectionCount() == 1);
	REQUIRE(p.send.TrackedRelevancyCount() == 1);

	p.client.context.Stop(p.client.world);
	REQUIRE(PumpUntil({&p.host}, [&] { return p.host.context.Session().Connections().empty(); }, 3000));

	p.send.Update(p.host.world, 0.f);

	CHECK(p.send.PacedConnectionCount() == 0);
	CHECK(p.send.TrackedRelevancyCount() == 0);
}

TEST_CASE("An entity that re-enters relevancy is restored with its current state, not its state when it left")
{
	// Position ALONE proves nothing here, and this case used to assert only that: the
	// position rides the re-entry Spawn packet, which is sent whether or not
	// cache.Forget was ever called - so the whole "the cache is cleared on leave" half
	// could be deleted and this case would stay green. What actually needs pinning is
	// a replicated field the Spawn does NOT carry, held UNCHANGED across the away
	// window: the host cached its value on tick 1, the client destroyed and rebuilt
	// the entity from the prefab (which knows nothing of it), and only a forgotten
	// cache makes the snapshot resend a value that never changed. Rotation is that
	// field - AE_FIELD_CUSTOM_REP("euler") on TransformComponent.
	constexpr float kEulerY = 0.75f;

	const PrefabFixture prefabs;
	Pair p(24731);
	p.host.context.Relevancy().radius = 50.f;
	p.host.context.SetSendRateHz(1'000'000.f);

	const Entity hostEntity = p.host.context.SpawnPrefab(p.host.world, kPrefab, {5.f, 0.f, 0.f},
	        aether::net::kInvalidConnection);
	REQUIRE(hostEntity.IsValid());
	const std::uint32_t netId = p.host.world.TryGet<aether::net::NetworkIdentity>(hostEntity)->netId;

	REQUIRE(PumpUntil({&p.host, &p.client}, [&] { return p.client.context.Session().EntityFor(netId).IsValid(); }));

	// Set before tick 1 and never touched again, so the host's cache records it as
	// sent and no later change can cause a resend.
	MoveWithRotation(p.host.world, hostEntity, {5.f, 0.f, 0.f}, {0.f, kEulerY, 0.f});
	p.send.Update(p.host.world, 0.f); // tick 1: relevant, seeds the baseline

	MoveWithRotation(p.host.world, hostEntity, {5000.f, 0.f, 0.f}, {0.f, kEulerY, 0.f});
	p.send.Update(p.host.world, 0.f); // tick 2: leaves

	REQUIRE(PumpUntil({&p.host, &p.client}, [&] { return !p.client.context.Session().EntityFor(netId).IsValid(); }, 3000));

	// It comes back somewhere it has never been - not the spawn position (5,0,0)
	// and not the far-away position it left from (5000,0,0) - still rotated exactly
	// as it was before it left.
	MoveWithRotation(p.host.world, hostEntity, {42.f, 7.f, -3.f}, {0.f, kEulerY, 0.f});
	p.send.Update(p.host.world, 0.f); // tick 3: re-enters

	REQUIRE(PumpUntil({&p.host, &p.client}, [&] { return p.client.context.Session().EntityFor(netId).IsValid(); }, 3000));

	const Entity clientEntity = p.client.context.Session().EntityFor(netId);
	const glm::vec3 pos = PositionOf(p.client.world, clientEntity);
	CHECK(pos.x == doctest::Approx(42.f));
	CHECK(pos.y == doctest::Approx(7.f));
	CHECK(pos.z == doctest::Approx(-3.f));

	// The half the Spawn cannot deliver. Pumped for rather than read immediately: the
	// Spawn and the resync snapshot are separate packets, and only the first is what
	// PumpUntil above waited on.
	bool rotated = false;
	PumpUntil({&p.host, &p.client},
	        [&]
	        {
		        rotated = std::abs(EulerOf(p.client.world, clientEntity).y - kEulerY) < 1e-3f;
		        return rotated;
	        },
	        3000);
	CHECK(rotated);
}

TEST_CASE("Leaving relevancy forgets the connection's cache, so re-entry is not silently skipped")
{
	// Pins the second half of Fix 3: a leave clears SnapshotCache for that netId on
	// that connection, or a re-entry at the SAME value the cache last recorded would
	// read as "unchanged" and BuildSnapshot would send nothing - even though this
	// connection currently has no representation of the entity at all (its Spawn is a
	// separate, always-sent packet, but every FUTURE incremental Snapshot would go
	// silent until the value changed again).
	const PrefabFixture prefabs;
	Pair p(24732);
	p.host.context.SetSendRateHz(1'000'000.f);

	// The entity itself never moves - only the radius changes - so its cached value
	// on re-entry is bit-for-bit identical to what tick 1 already recorded. Nothing
	// distinguishes "forgotten" from "not forgotten" here except the cache itself.
	const Entity hostEntity = p.host.context.SpawnPrefab(p.host.world, kPrefab, {10.f, 0.f, 0.f},
	        aether::net::kInvalidConnection);
	REQUIRE(hostEntity.IsValid());
	const std::uint32_t netId = p.host.world.TryGet<aether::net::NetworkIdentity>(hostEntity)->netId;

	REQUIRE(PumpUntil({&p.host, &p.client}, [&] { return p.client.context.Session().EntityFor(netId).IsValid(); }));

	p.host.context.Relevancy().radius = 50.f;
	p.send.Update(p.host.world, 0.f); // tick 1: relevant at distance 10; cache records (10,0,0)

	p.host.context.Relevancy().radius = 1.f;
	p.send.Update(p.host.world, 0.f); // tick 2: leaves (distance 10 > radius 1)

	REQUIRE(PumpUntil({&p.host, &p.client}, [&] { return !p.client.context.Session().EntityFor(netId).IsValid(); }, 3000));

	// Ask the cache directly, at the SAME unchanged value: forgotten reads as
	// "changed" (nothing recorded); not forgotten reads as "unchanged" and the
	// packet is empty.
	const std::vector<std::byte> afterLeave = aether::net::BuildSnapshot(p.host.world, p.host.context.Schema(),
	        p.host.context.Catalog(), p.host.context.Session(), p.host.context.CacheFor(p.hostSawPeer), {hostEntity});
	CHECK_FALSE(afterLeave.empty());
}

TEST_CASE("A connection's own entity never leaves relevancy, however far it moves")
{
	// The rule this pins already lives in RelevantFor (see NetRelevancyTests.cpp's
	// "A connection always receives the entity it owns, however distant"); this case
	// exists to prove Fix 3's leave detection does not accidentally reintroduce
	// distance for an owned entity through its OWN diffing, since RelevantFor's
	// exemption is exactly what UpdateRelevancyMembership's "current" set is built
	// from.
	//
	// TWO owned entities, not one: ViewerPosition sees from "the first owned entity
	// with a transform", so a connection that owns only the entity under test would
	// make its own position double as the viewer's - distance-to-self is always
	// zero, which passes trivially whether or not the exemption exists at all and
	// proves nothing. With two, whichever one ViewerPosition happens to pick as the
	// anchor, the OTHER gets a genuine non-zero-distance check that only survives
	// because of the exemption.
	const PrefabFixture prefabs;
	Pair p(24733);
	p.host.context.Relevancy().radius = 10.f;
	p.host.context.SetSendRateHz(1'000'000.f);

	const Entity anchor = p.host.context.SpawnPrefab(p.host.world, kPrefab, {0.f, 0.f, 0.f}, p.hostSawPeer);
	const Entity hostEntity = p.host.context.SpawnPrefab(p.host.world, kPrefab, {0.f, 0.f, 0.f}, p.hostSawPeer);
	REQUIRE(anchor.IsValid());
	REQUIRE(hostEntity.IsValid());
	const std::uint32_t anchorNetId = p.host.world.TryGet<aether::net::NetworkIdentity>(anchor)->netId;
	const std::uint32_t netId = p.host.world.TryGet<aether::net::NetworkIdentity>(hostEntity)->netId;

	REQUIRE(PumpUntil({&p.host, &p.client},
	        [&]
	        {
		        return p.client.context.Session().EntityFor(anchorNetId).IsValid()
		               && p.client.context.Session().EntityFor(netId).IsValid();
	        }));

	p.send.Update(p.host.world, 0.f); // tick 1: seeds the baseline

	// Far outside the radius - the ownership exemption must keep it relevant anyway.
	// The anchor stays put, so whichever of the two ViewerPosition happens to pick,
	// at least one of these two checks is a real distance test.
	MoveTo(p.host.world, hostEntity, {9999.f, 0.f, 0.f});
	p.send.Update(p.host.world, 0.f); // tick 2

	// Give a (wrongly sent) leave message every chance to actually reach the wire and
	// be applied: sends only queue on the host until ITS OWN Poll() (inside its
	// receive.Update) services enet_host and flushes them, so both ends have to keep
	// pumping across the window - not just the client - or a leave that really was
	// queued here would sit unsent and this case would pass for the wrong reason.
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
	while (std::chrono::steady_clock::now() < deadline)
	{
		p.host.receive.Update(p.host.world, 0.f);
		p.client.receive.Update(p.client.world, 0.f);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}

	CHECK(p.client.context.Session().EntityFor(netId).IsValid());
	CHECK(p.client.context.Session().EntityFor(anchorNetId).IsValid());
}

TEST_CASE("A newly joined connection's first send is not blocked by another connection's pacing window")
{
	// The regression a single, host-wide send deadline produces: once one
	// connection's tick fires, the deadline it sets governs the WHOLE update, so a
	// second connection joining a moment later waits out the first one's remaining
	// window before it is serviced even once - see the comment on
	// m_nextSendTimeByConnection. Left at the real 20Hz default deliberately: what is
	// under test IS the pacing.
	constexpr std::uint16_t kPort = 24734;
	const PrefabFixture prefabs;

	Node host;
	Node clientA;
	Node clientB;
	REQUIRE(host.context.StartHost(host.world, kPort, 4));
	aether::net::NetworkSendSystem send(host.context);
	// A generous window (2Hz, not the real 20Hz) so B's connect handshake below -
	// itself real wall-clock work with no upper bound this test controls - is
	// guaranteed to land inside it. What is under test is that B's OWN deadline does
	// not inherit A's, not the exact 20Hz cadence (a separate, timing-free concern).
	host.context.SetSendRateHz(2.f);

	REQUIRE(clientA.context.StartClient(clientA.world, "127.0.0.1", kPort));
	REQUIRE(PumpUntil({&host, &clientA}, [&] { return clientA.context.IsConnected(); }));

	const Entity hostEntity = host.context.SpawnPrefab(host.world, kPrefab, {0.f, 0.f, 0.f},
	        aether::net::kInvalidConnection);
	REQUIRE(hostEntity.IsValid());
	const std::uint32_t netId = host.world.TryGet<aether::net::NetworkIdentity>(hostEntity)->netId;
	REQUIRE(PumpUntil({&host, &clientA}, [&] { return clientA.context.Session().EntityFor(netId).IsValid(); }));

	// A's first tick: fires immediately (its deadline starts at 0), and sets its OWN
	// next-send deadline roughly 50ms out.
	send.Update(host.world, 0.f);

	// B connects right behind it - well inside A's just-set window.
	REQUIRE(clientB.context.StartClient(clientB.world, "127.0.0.1", kPort));
	REQUIRE(PumpUntil({&host, &clientA, &clientB}, [&] { return clientB.context.IsConnected(); }));
	REQUIRE(PumpUntil({&host, &clientA, &clientB}, [&] { return clientB.context.Session().EntityFor(netId).IsValid(); }));

	// One synchronous call, no sleep: against the old single shared deadline this
	// would return immediately and send nothing to anyone, including B, which has
	// never been serviced at all. Per-connection, B's own deadline still reads its
	// zero-value default, so it is serviced on this very call.
	MoveTo(host.world, hostEntity, {1.f, 2.f, 3.f});
	send.Update(host.world, 0.f);

	bool moved = false;
	PumpUntil({&host, &clientB},
	        [&]
	        {
		        const Entity e = clientB.context.Session().EntityFor(netId);
		        if (!e.IsValid())
		        {
			        return false;
		        }
		        const glm::vec3 pos = PositionOf(clientB.world, e);
		        moved = pos.x > 0.5f;
		        return moved;
	        },
	        500);
	CHECK(moved);
}

TEST_CASE("A host entity destroyed outside Net.Despawn is still despawned on the client")
{
	// THE GHOST. ReleaseForDespawn was the only Despawn broadcaster, so a host entity
	// destroyed by any other route - a scene load, a script's Entity.Destroy, a bare
	// DestroyHierarchy, as here - told nobody: PruneDeadBindings quietly dropped the
	// binding and every client kept a frozen replica until it disconnected. The sweep
	// is the one point all of those routes funnel through, so the despawn belongs
	// there; Net.Despawn itself already unbinds before the entity dies and never
	// reaches it.
	const PrefabFixture prefabs;
	Pair p(24738);

	const Entity hostEntity = p.host.context.SpawnPrefab(p.host.world, kPrefab, {3.f, 0.f, 0.f},
	        aether::net::kInvalidConnection);
	REQUIRE(hostEntity.IsValid());
	const std::uint32_t netId = p.host.world.TryGet<aether::net::NetworkIdentity>(hostEntity)->netId;

	REQUIRE(PumpUntil({&p.host, &p.client}, [&] { return p.client.context.Session().EntityFor(netId).IsValid(); }));
	const Entity clientEntity = p.client.context.Session().EntityFor(netId);
	REQUIRE(p.client.world.GetRegistry().valid(World::ToEntt(clientEntity)));

	// Destruction by a route Net.Despawn never sees. The host's receive Update is
	// what sweeps the dead binding, and pumping both ends is what delivers whatever
	// it broadcasts to the real client on the other side of the socket.
	ecs::DestroyHierarchy(p.host.world, hostEntity);

	REQUIRE(PumpUntil({&p.host, &p.client},
	        [&] { return !p.client.world.GetRegistry().valid(World::ToEntt(clientEntity)); }));
	CHECK_FALSE(p.client.context.Session().EntityFor(netId).IsValid());
	CHECK(aether::net::test::CountIdentities(p.client.world) == 0);
}

TEST_CASE("A client periodically resends its full state to the host reliably")
{
	// The upload path's own failure model: diffs go unreliably and BuildSnapshot
	// records every value as sent, so dropping the LAST update before an owned
	// entity goes idle desyncs the host's copy of it forever - the host's
	// per-connection loop already bounds exactly that with a periodic full resend,
	// and a client's upload is the same machinery pointed the other way. Observable
	// on the wire as a Snapshot arriving on the RELIABLE channel once per interval
	// while the entity sits perfectly still: between resyncs a still entity sends
	// nothing at all, and nothing else this peer emits is a reliable Snapshot.
	constexpr std::uint16_t kPort = 24740;
	const PrefabFixture prefabs;

	Node host;
	Node client;
	REQUIRE(host.context.StartHost(host.world, kPort, 4));
	REQUIRE(client.context.StartClient(client.world, "127.0.0.1", kPort));
	REQUIRE(PumpUntil({&host, &client}, [&] { return client.context.IsConnected(); }));

	aether::net::ConnectionId peer = aether::net::kInvalidConnection;
	for (const aether::net::ConnectionId conn: host.context.Session().Connections())
	{
		peer = conn;
	}
	REQUIRE(peer != aether::net::kInvalidConnection);

	// The client's own entity: host-spawned (so both ends bind the same id), owned
	// by the joiner, and then never touched again.
	const Entity player = host.context.SpawnPrefab(host.world, kPrefab, {0.f, 0.f, 0.f}, peer);
	REQUIRE(player.IsValid());
	const std::uint32_t netId = host.world.TryGet<aether::net::NetworkIdentity>(player)->netId;
	REQUIRE(PumpUntil({&host, &client}, [&] { return client.context.Session().EntityFor(netId).IsValid(); }));

	aether::net::NetworkSendSystem clientSend(client.context);
	client.context.SetSendRateHz(1'000'000.f); // pacing is not what this case tests

	// Two reliable Snapshots - the first send (the cache starts empty) and the first
	// periodic resend a second later - inside a window that cannot contain a third
	// interval by accident.
	int reliableSnapshots = 0;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1300);
	while (std::chrono::steady_clock::now() < deadline && reliableSnapshots < 2)
	{
		clientSend.Update(client.world, 0.f);
		host.receive.Update(host.world, 0.f);
		client.receive.Update(client.world, 0.f);
		for (const aether::net::NetEvent& event: host.context.Transport().Events())
		{
			if (event.kind == aether::net::NetEvent::Kind::Data && event.channel == aether::net::kChannelReliable
			        && !event.data.empty()
			        && event.data[0] == std::byte{static_cast<std::uint8_t>(aether::net::NetMessage::Snapshot)})
			{
				++reliableSnapshots;
			}
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	CHECK(reliableSnapshots >= 2);
}
