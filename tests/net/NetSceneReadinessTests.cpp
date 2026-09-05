// A replicated entity is created into whatever scene the receiving peer happens to be
// standing in, and the host answers a join with a Spawn for every relevant entity in one
// burst. A client that is still on its menu - which is exactly where a game shows
// "connecting..." - therefore built the whole session's cast into the MENU scene and
// destroyed it a frame later on the scene change, permanently: the host remembers per
// connection what it has already sent and never offers any of it again.
//
// NetworkContext::SetReplicationReady is the framework's answer. These cases drive it over
// a REAL loopback connection with both systems running, because what is under test is what
// crosses the wire and what the far end does with it - not a decoder's reaction to a byte
// string. The negative half of every case is asserted the only way it can be: by pumping
// both ends for a fixed span and requiring that the thing under test still has not happened.

#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <thread>
#include <vector>

#include "net/NetComponents.hpp"
#include "net/NetSpawn.hpp"
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
	// One trivial prefab on disk so SpawnPrefab/ApplySpawn genuinely instantiate
	// something at both ends. A local copy of the same fixture NetworkSendSystemTests.cpp
	// keeps, for the same reason it keeps its own: a shared header for one struct costs
	// more than the duplication does.
	struct PrefabFixture
	{
		std::filesystem::path dir;

		PrefabFixture()
		      : dir(std::filesystem::temp_directory_path() / "aether_net_scene_readiness_test")
		{
			std::filesystem::remove_all(dir);
			std::filesystem::create_directories(dir);
			aether::app::scene::SetProjectSceneDirectories(dir / "scenes", dir);

			aether::app::scene::SceneDescription prefab;
			prefab.name = "readiness_test_prefab";
			aether::app::scene::EntityRecord record;
			record.entityId = 1;
			record.name = "Body";
			record.hasTransform = true;
			prefab.entities.push_back(std::move(record));
			REQUIRE(aether::app::scene::SavePrefabFile("readiness_test_prefab", prefab));
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

	constexpr const char* kPrefab = "readiness_test_prefab";

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

	// ENet needs several service calls to complete a handshake or land a packet, so a
	// single Update() is never enough.
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

	// A host and one connected client. `holdClient` is the whole point: a client whose
	// game has said it is NOT standing in the session's scene yet, which is the state a
	// title screen showing "connecting..." is in. Declared BEFORE the connect, exactly as
	// a menu must declare it - the host's join burst is already in flight by the time the
	// Welcome lands.
	struct Pair
	{
		Node host;
		Node client;
		aether::net::NetworkSendSystem send{host.context};
		aether::net::ConnectionId peer = aether::net::kInvalidConnection;

		Pair(std::uint16_t port, bool holdClient, const std::function<void(World&)>& seedHost = {},
		        const std::function<void(World&)>& seedClient = {})
		{
			// Scene-placed entities have to exist before either end starts: the host
			// numbers its own inside StartHost and a client numbers its own when the
			// Welcome lands, and both walk SceneNodeComponent::id.
			if (seedHost)
			{
				seedHost(host.world);
			}
			if (seedClient)
			{
				seedClient(client.world);
			}

			REQUIRE(host.context.StartHost(host.world, port, 4));
			host.context.SetSendRateHz(1'000'000.f); // pacing is not what any of this tests
			host.context.Relevancy().radius = 10'000.f;

			if (holdClient)
			{
				client.context.SetReplicationReady(client.world, false);
			}
			REQUIRE(client.context.StartClient(client.world, "127.0.0.1", port));
			REQUIRE(PumpUntil({&host, &client}, [&] { return client.context.IsConnected(); }));

			for (const aether::net::ConnectionId conn: host.context.Session().Connections())
			{
				peer = conn;
			}
			REQUIRE(peer != aether::net::kInvalidConnection);
		}
	};

	// Runs both ends (and the host's send system) for `ms`, so a message that should NOT
	// arrive has every chance to. A host's packets only leave the box when its own Poll
	// services enet_host, so pumping one end is not enough to prove a negative.
	void PumpFor(Pair& p, int ms)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
		while (std::chrono::steady_clock::now() < deadline)
		{
			p.send.Update(p.host.world, 0.f);
			p.host.receive.Update(p.host.world, 0.f);
			p.client.receive.Update(p.client.world, 0.f);
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}

	// Pumps both ends AND the host's send system until `done`, which is what a resync
	// needs: the request is consumed by a send tick, not by the receive path.
	bool PumpSendUntil(Pair& p, auto done, int maxMs = 2000)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(maxMs);
		while (std::chrono::steady_clock::now() < deadline)
		{
			p.send.Update(p.host.world, 0.f);
			p.host.receive.Update(p.host.world, 0.f);
			p.client.receive.Update(p.client.world, 0.f);
			if (done())
			{
				return true;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		return false;
	}

	glm::vec3 PositionOf(World& world, Entity entity)
	{
		const auto* transform = world.TryGet<TransformComponent>(entity);
		return transform != nullptr ? glm::vec3(transform->localToWorld[3]) : glm::vec3(-999.f);
	}

	void MoveTo(World& world, Entity entity, glm::vec3 position)
	{
		world.TryGet<TransformComponent>(entity)->localToWorld
		        = ComposeTransform(position, {0.f, 0.f, 0.f}, {1.f, 1.f, 1.f});
	}
} // namespace

TEST_CASE("A client that joins from a menu is given the world when it says it has arrived")
{
	// THE DEFECT. The host spawns the joiner's player while that joiner is still on the
	// title screen - which is what NetSessionDirector does the moment a connection
	// appears - and the join burst lands on a peer standing in the wrong scene.
	const PrefabFixture prefabs;
	Pair p(24760, /*holdClient=*/true);

	const Entity hostEntity = p.host.context.SpawnPrefab(p.host.world, kPrefab, {5.f, 1.f, 0.f},
	        aether::net::kInvalidConnection);
	REQUIRE(hostEntity.IsValid());
	const std::uint32_t netId = p.host.world.TryGet<aether::net::NetworkIdentity>(hostEntity)->netId;

	// Held: the Spawn arrives and is refused, so nothing is built into the menu. Pumped
	// generously - this is the assertion the whole fix turns on, and a race that merely
	// had not happened yet would be indistinguishable from a fix.
	PumpFor(p, 400);
	REQUIRE_FALSE(p.client.context.Session().EntityFor(netId).IsValid());
	CHECK(aether::net::test::CountIdentities(p.client.world) == 0);

	// The player reaches the arena and says so. Everything the host sent while it was
	// elsewhere is offered again, because the client is holding none of it.
	p.client.context.SetReplicationReady(p.client.world, true);
	REQUIRE(PumpSendUntil(p, [&] { return p.client.context.Session().EntityFor(netId).IsValid(); }));

	const Entity clientEntity = p.client.context.Session().EntityFor(netId);
	CHECK(p.client.world.GetRegistry().valid(World::ToEntt(clientEntity)));

	// And the STATE follows the spawn, not just the entity. The host's per-connection
	// change-detection cache had already recorded every field as sent to this connection
	// while it was throwing them away, so without the resync the entity would sit at its
	// spawn-message pose until something happened to change again.
	MoveTo(p.host.world, hostEntity, {9.f, 2.f, 0.f});
	REQUIRE(PumpSendUntil(p,
	        [&] { return glm::distance(PositionOf(p.client.world, clientEntity), glm::vec3(9.f, 2.f, 0.f)) < 0.01f; }));
}

TEST_CASE("A client already standing in the scene is replicated to exactly as before")
{
	// The control for the case above, and the one behaviour that must not change: a game
	// that never mentions readiness is ready, joins in one round trip, and adds nothing to
	// the handshake.
	const PrefabFixture prefabs;
	Pair p(24761, /*holdClient=*/false);

	CHECK(p.client.context.IsReplicationReady());

	// Nothing was asked for. A ClientReady is sent ONLY by a client that actually held,
	// so the join replay in OnConnected is still the whole of the join and no extra
	// message and no extra full-state send happen for an ordinary client.
	CHECK_FALSE(p.host.context.ConsumeResyncRequest(p.peer));

	const Entity hostEntity = p.host.context.SpawnPrefab(p.host.world, kPrefab, {5.f, 0.f, 0.f},
	        aether::net::kInvalidConnection);
	REQUIRE(hostEntity.IsValid());
	const std::uint32_t netId = p.host.world.TryGet<aether::net::NetworkIdentity>(hostEntity)->netId;

	REQUIRE(PumpUntil({&p.host, &p.client}, [&] { return p.client.context.Session().EntityFor(netId).IsValid(); }));
	CHECK_FALSE(p.host.context.ConsumeResyncRequest(p.peer));
}

TEST_CASE("A client that changes scene between joining and arriving still agrees about scene-placed ids")
{
	// Scene-placed entities are numbered by walking the scene's node ids from a shared
	// starting counter, with no exchange - which is only deterministic if both ends run
	// the derivation over the SAME scene. A client that was welcomed on its menu derived
	// the menu's ids; by the time it arrives it is standing somewhere else entirely.
	//
	// The menu entity here is what makes that visible: it consumes an id, so a derivation
	// that is not restarted numbers the arena's entity one higher than the host does and
	// the two peers disagree about an entity neither will ever send a Spawn for.
	Pair p(24762, /*holdClient=*/true,
	        [](World& world) { aether::net::test::MakeScenePlaced(world, 7); },  // the host's arena
	        [](World& world) { aether::net::test::MakeScenePlaced(world, 1); }); // the client's menu

	const std::vector<std::uint32_t> hostIds = aether::net::test::ScenePlacedNetIds(p.host.world);
	REQUIRE(hostIds.size() == 1);
	const std::uint32_t sceneNetId = hostIds.front();
	const Entity hostScene = p.host.context.Session().EntityFor(sceneNetId);
	REQUIRE(hostScene.IsValid());

	// While held, state naming that id must not land on the menu entity that happens to
	// carry it. Nothing this peer is holding belongs to the session.
	const Entity menuEntity = p.client.context.Session().EntityFor(sceneNetId);
	REQUIRE(menuEntity.IsValid()); // derived locally on the Welcome, in the wrong scene
	MoveTo(p.host.world, hostScene, {40.f, 0.f, 0.f});
	PumpFor(p, 300);
	CHECK(glm::distance(PositionOf(p.client.world, menuEntity), glm::vec3(0.f)) < 0.01f);

	// The scene change: the menu goes, the arena arrives.
	aether::ecs::DestroyHierarchy(p.client.world, menuEntity);
	const Entity arenaEntity = aether::net::test::MakeScenePlaced(p.client.world, 7);
	p.client.context.SetReplicationReady(p.client.world, true);

	// Same entity, same number, derived independently on both machines.
	CHECK(p.client.world.TryGet<aether::net::NetworkIdentity>(arenaEntity)->netId == sceneNetId);
	CHECK(p.client.context.Session().EntityFor(sceneNetId) == arenaEntity);
	// Nothing was instantiated across the wire for it: a scene-placed entity has no spawn
	// prefab, and the resync must not have mistaken it for something it could rebuild.
	CHECK(aether::net::test::CountIdentities(p.client.world) == 1);

	// And now the host's state does land on it.
	REQUIRE(PumpSendUntil(p,
	        [&] { return glm::distance(PositionOf(p.client.world, arenaEntity), glm::vec3(40.f, 0.f, 0.f)) < 0.01f; }));
}

TEST_CASE("A client holding replication ignores Despawn and Relevancy, like Snapshot")
{
	// The gate Snapshot and ScriptFields have always had, extended to the two kinds
	// that DESTROY. A held client still carries the bindings its Welcome derived -
	// against whatever scene it was standing in - so a despawn or a leave naming
	// one of those ids destroys an entity in a world the session does not own (a
	// menu). Refusing is lossless: the host replays everything this client missed
	// the moment it says it has arrived.
	const auto seed = [](World& world)
	{
		aether::net::test::MakeScenePlaced(world, 7);
		aether::net::test::MakeScenePlaced(world, 8);
	};

	Pair held(24768, /*holdClient=*/true, seed, seed);
	const std::vector<std::uint32_t> hostIds = aether::net::test::ScenePlacedNetIds(held.host.world);
	REQUIRE(hostIds.size() == 2);

	// A despawn for one id and a relevancy leave for the other, straight from the
	// host the way its own systems would send them.
	held.host.context.Transport().Send(held.peer, aether::net::kChannelReliable, true,
	        aether::net::EncodeDespawn(hostIds.front()));
	held.host.context.Transport().Send(held.peer, aether::net::kChannelReliable, true,
	        aether::net::EncodeRelevancyLeave(hostIds.back()));
	PumpFor(held, 300);

	// Both entities the client derived are still there, and still bound.
	CHECK(aether::net::test::CountIdentities(held.client.world) == 2);
	CHECK(held.client.context.Session().EntityFor(hostIds.front()).IsValid());
	CHECK(held.client.context.Session().EntityFor(hostIds.back()).IsValid());

	// Positive control: the very same bytes on a client that IS standing in the
	// session's scene destroy both - one as a despawn, one as a leave.
	Pair ready(24769, /*holdClient=*/false, seed, seed);
	const std::vector<std::uint32_t> readyIds = aether::net::test::ScenePlacedNetIds(ready.host.world);
	REQUIRE(readyIds.size() == 2);
	ready.host.context.Transport().Send(ready.peer, aether::net::kChannelReliable, true,
	        aether::net::EncodeDespawn(readyIds.front()));
	ready.host.context.Transport().Send(ready.peer, aether::net::kChannelReliable, true,
	        aether::net::EncodeRelevancyLeave(readyIds.back()));
	PumpFor(ready, 300);

	CHECK(aether::net::test::CountIdentities(ready.client.world) == 0);
}

TEST_CASE("A client that gives up before arriving leaves nothing behind on either peer")
{
	// The other end of the same story: a player who starts a join, waits on the menu and
	// changes their mind. Nothing was queued anywhere while they waited, so there is
	// nothing to leak - and the host's request to resend, if one was ever made, goes with
	// the connection rather than waiting for whoever inherits that id.
	const PrefabFixture prefabs;

	SUBCASE("a held client that stops is holding nothing to apply to a later world")
	{
		Pair p(24763, /*holdClient=*/true);
		REQUIRE(p.host.context.SpawnPrefab(p.host.world, kPrefab, {5.f, 0.f, 0.f}, aether::net::kInvalidConnection)
		                .IsValid());
		PumpFor(p, 300);
		// The precondition for the claim: it was refused on arrival rather than parked
		// somewhere, so "nothing leaked" is about a peer that was never holding anything.
		REQUIRE(aether::net::test::CountIdentities(p.client.world) == 0);

		p.client.context.Stop(p.client.world);
		CHECK(aether::net::test::CountIdentities(p.client.world) == 0);
		CHECK(p.client.context.Session().Bindings().empty());

		// Becoming ready with no session does not resurrect anything: there is nobody to
		// ask and nothing to apply.
		p.client.context.SetReplicationReady(p.client.world, true);
		CHECK(aether::net::test::CountIdentities(p.client.world) == 0);
	}

	SUBCASE("a resync request dies with the connection that asked for it")
	{
		// Recorded directly rather than by sending one, because what is under test is
		// the LIFETIME of a request nobody got round to acting on - and a request the
		// send system consumed is one this could never have caught leaking. Connection
		// ids are handed out from a counter the transport resets on a Stop/StartHost
		// cycle, so a request outliving its connection would be inherited by whoever
		// gets that id next.
		Pair p(24764, /*holdClient=*/true);
		p.host.context.RequestResync(p.peer);

		p.client.context.Stop(p.client.world);
		REQUIRE(PumpUntil({&p.host, &p.client}, [&] { return p.host.context.Session().Connections().empty(); }, 4000));
		CHECK_FALSE(p.host.context.ConsumeResyncRequest(p.peer));
	}

	SUBCASE("ending the session drops every outstanding request")
	{
		Node host;
		REQUIRE(host.context.StartHost(host.world, 24765, 4));
		host.context.Session().AddConnection(5);
		host.context.RequestResync(5);
		CHECK(host.context.ConsumeResyncRequest(5)); // it really was recorded

		host.context.RequestResync(5);
		host.context.Stop(host.world);
		CHECK_FALSE(host.context.ConsumeResyncRequest(5));
	}
}

TEST_CASE("Only a live connection can ask the host to resend the world")
{
	// The inbound trust boundary for the new kind. A ClientReady is a request for a full
	// state send, so a peer the session does not have - one being refused over the player
	// cap, which holds a link long enough to be told why - must not be able to queue work
	// against it, and a client must not honour one at all.
	Node host;
	REQUIRE(host.context.StartHost(host.world, 24766, 4));
	const std::vector<std::byte> packet = aether::net::EncodeClientReady();

	host.receive.OnData(host.world, 9, packet);
	CHECK_FALSE(host.context.ConsumeResyncRequest(9));

	host.context.Session().AddConnection(9);
	host.receive.OnData(host.world, 9, packet);
	CHECK(host.context.ConsumeResyncRequest(9));

	// The same bytes on a client change nothing. The connection is put into the client's
	// session by hand purely to ISOLATE the role gate: a client never records one, so
	// without this the membership check above would refuse the packet first and "the role
	// gate works" would be asserted by a test that never reached it.
	Node client;
	REQUIRE(client.context.StartClient(client.world, "127.0.0.1", 24767));
	client.context.Session().AddConnection(9);
	client.receive.OnData(client.world, 9, packet);
	CHECK_FALSE(client.context.ConsumeResyncRequest(9));
}
