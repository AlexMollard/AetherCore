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
#include <thread>
#include <vector>

#include "net/NetComponents.hpp"
#include "net/NetSnapshot.hpp"
#include "net/NetworkContext.hpp"
#include "net/NetworkSystems.hpp"
#include "scene/Components.hpp"
#include "scene/SceneSerializer.hpp"
#include "scene/TransformUtils.hpp"
#include "scene/World.hpp"
#include "utils/ServiceContainer.hpp"

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

		explicit Pair(std::uint16_t port)
		{
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

TEST_CASE("An entity that re-enters relevancy is restored with its current state, not its state when it left")
{
	const PrefabFixture prefabs;
	Pair p(24731);
	p.host.context.Relevancy().radius = 50.f;
	p.host.context.SetSendRateHz(1'000'000.f);

	const Entity hostEntity = p.host.context.SpawnPrefab(p.host.world, kPrefab, {5.f, 0.f, 0.f},
	        aether::net::kInvalidConnection);
	REQUIRE(hostEntity.IsValid());
	const std::uint32_t netId = p.host.world.TryGet<aether::net::NetworkIdentity>(hostEntity)->netId;

	REQUIRE(PumpUntil({&p.host, &p.client}, [&] { return p.client.context.Session().EntityFor(netId).IsValid(); }));

	p.send.Update(p.host.world, 0.f); // tick 1: relevant, seeds the baseline

	MoveTo(p.host.world, hostEntity, {5000.f, 0.f, 0.f});
	p.send.Update(p.host.world, 0.f); // tick 2: leaves

	REQUIRE(PumpUntil({&p.host, &p.client}, [&] { return !p.client.context.Session().EntityFor(netId).IsValid(); }, 3000));

	// It comes back somewhere it has never been - not the spawn position (5,0,0)
	// and not the far-away position it left from (5000,0,0).
	MoveTo(p.host.world, hostEntity, {42.f, 7.f, -3.f});
	p.send.Update(p.host.world, 0.f); // tick 3: re-enters

	REQUIRE(PumpUntil({&p.host, &p.client}, [&] { return p.client.context.Session().EntityFor(netId).IsValid(); }, 3000));

	const Entity clientEntity = p.client.context.Session().EntityFor(netId);
	const glm::vec3 pos = PositionOf(p.client.world, clientEntity);
	CHECK(pos.x == doctest::Approx(42.f));
	CHECK(pos.y == doctest::Approx(7.f));
	CHECK(pos.z == doctest::Approx(-3.f));
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
