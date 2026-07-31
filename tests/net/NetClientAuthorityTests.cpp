// Client authority, end to end. The framework used to replicate state one way -
// host to client - and a client's own character reached nobody. Now the OWNER of an
// entity simulates it and replicates the result, and the host relays.
//
// These cases run over a real ENet loopback with both systems on every node, in the
// order Application registers them, because what is under test is what actually
// crosses the wire and what the peer at the far end does with it. Two of them read
// the wire directly rather than the far end's transform: the send-side ownership
// filter and the receive-side ownership guarantee both protect the same value, so a
// test that only watched the transform would pass with either one deleted.
//
// The inbound ownership GATE - the security boundary, where a peer names entities no
// well-behaved sender ever would - is driven through OnData in NetworkSystemsTests.cpp.

#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "net/NetComponents.hpp"
#include "net/NetScriptFields.hpp"
#include "net/NetSnapshot.hpp"
#include "net/NetSpawn.hpp"
#include "net/NetworkContext.hpp"
#include "net/NetworkSystems.hpp"
#include "scene/Components.hpp"
#include "scene/SceneSerializer.hpp"
#include "scene/TransformUtils.hpp"
#include "scene/World.hpp"
#include "scene/reflection/Reflection.hpp"
#include "utils/ServiceContainer.hpp"

#include "NetTestSupport.hpp"

using namespace aether;
using namespace aether::net::test;

namespace
{
	// A scratch prefab directory so SpawnPrefab/ApplySpawn genuinely instantiate
	// something on both ends. Ownership is the whole subject here and a Spawn message
	// is the only thing that carries it across the wire, so these cases cannot use the
	// hand-bound entities the pure-decoder tests use.
	struct PrefabFixture
	{
		std::filesystem::path dir;

		PrefabFixture()
		      : dir(std::filesystem::temp_directory_path() / "aether_net_client_authority_test")
		{
			std::filesystem::remove_all(dir);
			std::filesystem::create_directories(dir);
			aether::app::scene::SetProjectSceneDirectories(dir / "scenes", dir);

			aether::app::scene::SceneDescription prefab;
			prefab.name = "authority_test_prefab";
			aether::app::scene::EntityRecord record;
			record.entityId = 1;
			record.name = "Body";
			record.hasTransform = true;
			prefab.entities.push_back(std::move(record));
			REQUIRE(aether::app::scene::SavePrefabFile("authority_test_prefab", prefab));
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

	constexpr const char* kPrefab = "authority_test_prefab";
	constexpr float kSentinel = -777.f;

	// One peer: its own world, context, and BOTH systems. The send system on a client
	// is what makes this fixture different from every other one in this directory -
	// under client authority a client sends too, so a fixture that only gave the host
	// one could not observe the change at all.
	struct Node
	{
		ServiceContainer services;
		World world;
		aether::net::NetworkContext context{services};
		aether::net::NetworkReceiveSystem receive{context};
		aether::net::NetworkSendSystem send{context};

		~Node()
		{
			context.Stop(world);
		}

		Node() = default;
		Node(const Node&) = delete;
		Node& operator=(const Node&) = delete;

		[[nodiscard]] glm::vec3 PositionOfNetId(std::uint32_t netId)
		{
			const Entity entity = context.Session().EntityFor(netId);
			if (!entity.IsValid())
			{
				return glm::vec3(kSentinel);
			}
			const auto* transform = world.TryGet<TransformComponent>(entity);
			return transform != nullptr ? glm::vec3(transform->localToWorld[3]) : glm::vec3(kSentinel);
		}

		void MoveNetId(std::uint32_t netId, glm::vec3 position)
		{
			const Entity entity = context.Session().EntityFor(netId);
			REQUIRE(entity.IsValid());
			world.TryGet<TransformComponent>(entity)->localToWorld
			        = ComposeTransform(position, {0.f, 0.f, 0.f}, {1.f, 1.f, 1.f});
		}

		void SetOwnerOfNetId(std::uint32_t netId, aether::net::ConnectionId owner)
		{
			const Entity entity = context.Session().EntityFor(netId);
			REQUIRE(entity.IsValid());
			world.TryGet<aether::net::NetworkIdentity>(entity)->owner = owner;
		}
	};

	// Runs one whole frame on every node - receive then send, the order Application
	// registers them - until `done()` or the deadline. ENet needs several service
	// calls to complete a handshake or land a packet, so a single pass is never
	// enough. Takes a vector rather than an initializer_list because the Session
	// fixture builds its node list at runtime.
	bool Run(const std::vector<Node*>& nodes, auto done, int maxMs = 3000)
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

	// Never satisfied, so Run drives the whole window - for the cases whose claim is
	// "and it STILL did not happen".
	constexpr auto kNever = [] { return false; };

	// Decodes a captured snapshot body against `netIds` and reports where each one
	// ended up, or kSentinel for a net id the packet never mentioned. Applied with
	// TrustAll, so what this measures is purely WHAT THE SENDER PUT ON THE WIRE - the
	// receive-side ownership gate cannot mask a send-side filter that is not working.
	std::vector<float> ProbeSnapshot(std::span<const std::byte> body, const std::vector<std::uint32_t>& netIds)
	{
		World probe;
		aether::net::NetSession session;
		const aether::net::ReplicationSchema schema = aether::net::BuildReplicationSchema(reflect::ComponentTypes());

		std::vector<Entity> entities;
		for (const std::uint32_t netId: netIds)
		{
			const Entity entity = probe.Create();
			probe.Emplace<TransformComponent>(entity).localToWorld
			        = ComposeTransform({kSentinel, 0.f, 0.f}, {0.f, 0.f, 0.f}, {1.f, 1.f, 1.f});
			session.Bind(netId, entity);
			entities.push_back(entity);
		}

		aether::net::ApplySnapshot(probe, schema, reflect::ComponentTypes(), session, body,
		        aether::net::StateWriteGate::TrustAll());

		std::vector<float> out;
		for (const Entity entity: entities)
		{
			out.push_back(probe.TryGet<TransformComponent>(entity)->localToWorld[3].x);
		}
		return out;
	}

	// The first Snapshot sitting in `node`'s freshly polled inbound events, or empty.
	// Poll() clears the event vector, so this must be the only thing servicing that
	// transport for as long as the caller is watching it.
	std::vector<std::byte> PollForSnapshot(Node& node)
	{
		node.context.Transport().Poll();
		for (const aether::net::NetEvent& event: node.context.Transport().Events())
		{
			if (event.kind != aether::net::NetEvent::Kind::Data || event.data.empty())
			{
				continue;
			}
			if (static_cast<aether::net::NetMessage>(static_cast<std::uint8_t>(event.data[0]))
			        != aether::net::NetMessage::Snapshot)
			{
				continue;
			}
			return std::vector<std::byte>(event.data.begin() + 1, event.data.end());
		}
		return {};
	}

	ScriptPropertyValue BoolValue(bool v)
	{
		ScriptPropertyValue out;
		out.type = ScriptPropertyValue::Type::Bool;
		out.i64 = v ? 1 : 0;
		return out;
	}

	// Gives the entity already bound to `netId` a "Facing" script and the fake bridge
	// that will be asked to read and write its one replicated bool. The bridge is
	// owned by the context, so the raw pointer stays valid for the node's lifetime.
	FakeFieldBridge* AttachFacingBridge(Node& node, std::uint32_t netId)
	{
		const Entity entity = node.context.Session().EntityFor(netId);
		REQUIRE(entity.IsValid());
		node.world.EmplaceOrReplace<ScriptComponent>(entity).scripts.push_back(ScriptEntry{.path = "Facing"});

		auto bridge = std::make_unique<FakeFieldBridge>();
		bridge->Declare("Facing",
		        {aether::net::ScriptPropertyDesc{.index = 0, .type = ScriptPropertyValue::Type::Bool}});
		auto* raw = bridge.get();
		node.context.SetFieldBridge(std::move(bridge));
		return raw;
	}

	// A host with `clientCount` real connected clients, every peer's send rate
	// uncapped (pacing is not what any of these cases test).
	struct Session
	{
		PrefabFixture prefabs;
		Node host;
		std::vector<std::unique_ptr<Node>> clients;
		std::vector<aether::net::ConnectionId> connections;

		explicit Session(std::uint16_t port, std::size_t clientCount = 1)
		{
			REQUIRE(host.context.StartHost(host.world, port, 8));
			host.context.SetSendRateHz(1'000'000.f);
			// Relevancy is somebody else's subject (NetworkSendSystemTests) and its
			// default 60-unit radius would despawn an entity these cases deliberately
			// walk a long way, turning an interpolation assertion into a missing-entity
			// one. Effectively disabled so what is measured here is only ownership.
			host.context.Relevancy().radius = 1e6f;

			for (std::size_t i = 0; i < clientCount; ++i)
			{
				auto node = std::make_unique<Node>();
				REQUIRE(node->context.StartClient(node->world, "127.0.0.1", port));
				node->context.SetSendRateHz(1'000'000.f);
				clients.push_back(std::move(node));
			}
			REQUIRE(Run(All(), [&] { return AllConnected(); }));

			connections = host.context.Session().Connections();
			REQUIRE(connections.size() == clientCount);
		}

		[[nodiscard]] std::vector<Node*> All()
		{
			std::vector<Node*> all{&host};
			for (auto& client: clients)
			{
				all.push_back(client.get());
			}
			return all;
		}

		[[nodiscard]] bool AllConnected() const
		{
			for (const auto& client: clients)
			{
				if (!client->context.IsConnected())
				{
					return false;
				}
			}
			return true;
		}

		// Spawns a prefab owned by `owner` and waits for every client to have it.
		[[nodiscard]] std::uint32_t Spawn(glm::vec3 position, aether::net::ConnectionId owner)
		{
			const Entity entity = host.context.SpawnPrefab(host.world, kPrefab, position, owner);
			REQUIRE(entity.IsValid());
			const std::uint32_t netId = host.world.TryGet<aether::net::NetworkIdentity>(entity)->netId;
			REQUIRE(Run(All(),
			        [&]
			        {
				        for (const auto& client: clients)
				        {
					        if (!client->context.Session().EntityFor(netId).IsValid())
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

// ── Sending ─────────────────────────────────────────────────────────────────────

TEST_CASE("A client puts the entity it owns on the wire, and nothing else")
{
	// The headline change, measured at the wire rather than at the far end. Before
	// this, NetworkSendSystem::Update returned early off the host and a client's own
	// character was invisible to every other peer for the whole session.
	//
	// Read off the host's raw inbound events and decoded with an ungated apply, so
	// the receive-side ownership gate - which would refuse the second entity anyway -
	// cannot stand in for a send filter that is not working.
	Session s(24760);
	Node& client = *s.clients.front();
	const std::uint32_t mine = s.Spawn({6.f, 0.f, 0.f}, s.connections.front());
	const std::uint32_t theirs = s.Spawn({4.f, 0.f, 0.f}, aether::net::kInvalidConnection);

	// Settle, so the client's change-detection cache holds a baseline for everything
	// it is going to talk about and the capture below is a diff of the moves made
	// after it rather than of the spawn.
	Run(s.All(), kNever, 300);

	client.MoveNetId(mine, {12.5f, 0.f, 0.f});
	client.MoveNetId(theirs, {99.f, 0.f, 0.f});

	// Only the client sends from here, and only the host's transport is serviced, so
	// nothing else can put a snapshot on this link or consume one.
	std::vector<std::byte> captured;
	for (int i = 0; i < 400 && captured.empty(); ++i)
	{
		client.send.Update(client.world, 1.f / 60.f);
		client.context.Transport().Poll();
		captured = PollForSnapshot(s.host);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	REQUIRE_FALSE(captured.empty());

	const std::vector<float> probed = ProbeSnapshot(captured, {mine, theirs});
	CHECK(probed[0] == doctest::Approx(12.5f)); // it owns this one, so it sent it
	CHECK(probed[1] == doctest::Approx(kSentinel)); // it does not own this one, so it did not
}

TEST_CASE("The host never puts an entity's state on the wire back to its owner")
{
	// Zero correction, enforced at the SENDER. Nobody is told where their own
	// character is, so there is no stale value for a receiver to be dragged toward -
	// which is the artifact that made the old model feel sluggish in the first place.
	//
	// Two runs over the same machinery, differing only in who owns the entity, so
	// "no snapshot" is measured against a live control rather than asserted on its
	// own: an assertion that nothing arrived passes just as well against a link that
	// was never working.
	Session s(24761);
	Node& client = *s.clients.front();
	const std::uint32_t netId = s.Spawn({6.f, 0.f, 0.f}, s.connections.front());
	Run(s.All(), kNever, 300);

	// Owned by the client: the host moves it and must stay silent about it.
	s.host.MoveNetId(netId, {-50.f, 0.f, 0.f});
	std::vector<std::byte> leaked;
	for (int i = 0; i < 200 && leaked.empty(); ++i)
	{
		// Re-asserted every pass because the client is still uploading its own value,
		// which lands on the host's transform between sends. Without this the host
		// would have nothing to disagree about and the case would pass vacuously.
		s.host.MoveNetId(netId, {-50.f, 0.f, 0.f});
		s.host.send.Update(s.host.world, 1.f / 60.f);
		s.host.receive.Update(s.host.world, 1.f / 60.f);
		client.send.Update(client.world, 1.f / 60.f);
		leaked = PollForSnapshot(client);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	CHECK(leaked.empty());

	// The control: the same entity, now host-owned, and the same loop DOES deliver.
	s.host.SetOwnerOfNetId(netId, aether::net::kInvalidConnection);
	std::vector<std::byte> delivered;
	for (int i = 0; i < 400 && delivered.empty(); ++i)
	{
		s.host.MoveNetId(netId, {-50.f, 0.f, 0.f});
		s.host.send.Update(s.host.world, 1.f / 60.f);
		s.host.receive.Update(s.host.world, 1.f / 60.f);
		delivered = PollForSnapshot(client);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	REQUIRE_FALSE(delivered.empty());
	CHECK(ProbeSnapshot(delivered, {netId})[0] == doctest::Approx(-50.f));
}

// ── Relaying ────────────────────────────────────────────────────────────────────

TEST_CASE("The host relays one client's state to another client")
{
	// The relay is not code of its own: a client's state lands on the host's
	// transform through the ownership-gated apply, and the host's next diff carries
	// it onward like any other change. This is the case that proves the two halves
	// join up, and it is the whole point of the exercise - a client's movement
	// reaching the other players.
	Session s(24762, 2);
	Node& mover = *s.clients[0];
	Node& watcher = *s.clients[1];
	const std::uint32_t netId = s.Spawn({6.f, 0.f, 0.f}, s.connections[0]);

	mover.MoveNetId(netId, {14.25f, 1.f, 0.f});
	REQUIRE(Run(s.All(), [&] { return watcher.PositionOfNetId(netId).x > 14.f; }));

	CHECK(s.host.PositionOfNetId(netId).x == doctest::Approx(14.25f));
	CHECK(watcher.PositionOfNetId(netId).x == doctest::Approx(14.25f));
	// And the mover still holds exactly what it simulated - the round trip did not
	// tug it anywhere.
	CHECK(mover.PositionOfNetId(netId).x == doctest::Approx(14.25f));
}

TEST_CASE("The host's own movement still reaches a client")
{
	// The direction that already worked, kept honest. Every filter added on the send
	// side is one more thing that could silently stop replicating the host's world.
	Session s(24763);
	Node& client = *s.clients.front();
	const std::uint32_t netId = s.Spawn({1.f, 0.f, 0.f}, aether::net::kInvalidConnection);

	s.host.MoveNetId(netId, {7.65f, 0.f, 0.f});
	REQUIRE(Run(s.All(), [&] { return client.PositionOfNetId(netId).x > 7.f; }));

	CHECK(client.PositionOfNetId(netId).x == doctest::Approx(7.65f));
}

// ── Correction and smoothing ────────────────────────────────────────────────────

TEST_CASE("An owned entity is not corrected even by a host that insists")
{
	// The receiver-side half of the zero-correction guarantee, and the stronger one:
	// a snapshot naming the owner's own entity is manufactured and delivered anyway -
	// an older host build, or a hostile one - and the local simulation must survive it
	// untouched. No easing, no snapping, not a millimetre.
	Session s(24764);
	Node& client = *s.clients.front();
	const std::uint32_t mine = s.Spawn({6.f, 0.f, 0.f}, s.connections.front());
	client.MoveNetId(mine, {20.f, 0.f, 0.f});
	Run(s.All(), kNever, 200);
	REQUIRE(client.PositionOfNetId(mine).x == doctest::Approx(20.f));

	// Make the host insist, through the real path rather than a hand-delivered
	// packet: its record is flipped to "I own this" so its send filter stops
	// excluding the entity, while the CLIENT's record still says the entity is its
	// own. The host therefore transmits a contradicting position every tick, and the
	// only thing that can stop it landing is the receiver's zero-correction rule.
	s.host.SetOwnerOfNetId(mine, aether::net::kInvalidConnection);
	for (int i = 0; i < 300; ++i)
	{
		s.host.MoveNetId(mine, {0.f, 0.f, 0.f});
		s.host.send.Update(s.host.world, 1.f / 60.f);
		s.host.receive.Update(s.host.world, 1.f / 60.f);
		client.receive.Update(client.world, 1.f / 60.f);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}

	CHECK(client.PositionOfNetId(mine).x == doctest::Approx(20.f));

	// The control: the same stream of packets DOES move the entity once this client
	// is no longer its owner, so the case above is a decision and not a dead link.
	// A DIFFERENT value, because the host's change-detection cache already recorded
	// the one above as sent and a diff never repeats an unchanged field.
	client.SetOwnerOfNetId(mine, aether::net::kInvalidConnection);
	for (int i = 0; i < 400 && client.PositionOfNetId(mine).x > -4.f; ++i)
	{
		s.host.MoveNetId(mine, {-5.f, 0.f, 0.f});
		s.host.send.Update(s.host.world, 1.f / 60.f);
		s.host.receive.Update(s.host.world, 1.f / 60.f);
		client.receive.Update(client.world, 1.f / 60.f);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	CHECK(client.PositionOfNetId(mine).x == doctest::Approx(-5.f));
}

TEST_CASE("A non-owned entity is still interpolated, and an owned one is not")
{
	// Interpolation is what makes OTHER players smooth between snapshots, and the
	// move to client authority must not have taken it out along with the correction
	// machinery. A remote entity is rendered slightly in the past, so while it is
	// moving the client shows it BEHIND the value that has already arrived; once it
	// stops, the render time catches up and the two agree.
	//
	// The entity this client owns, in the same world and the same frames, is the
	// control: it must show exactly what this peer simulated, with no lag at all.
	Session s(24765);
	Node& client = *s.clients.front();
	const std::uint32_t theirs = s.Spawn({0.f, 0.f, 0.f}, aether::net::kInvalidConnection);
	const std::uint32_t mine = s.Spawn({6.f, 0.f, 0.f}, s.connections.front());

	// Smoothing is opt-in per entity: without a NetworkTransform an arriving snapshot
	// is applied raw. Both get one so the difference below is about ownership.
	client.world.Emplace<aether::net::NetworkTransform>(client.context.Session().EntityFor(theirs));
	client.world.Emplace<aether::net::NetworkTransform>(client.context.Session().EntityFor(mine));

	client.MoveNetId(mine, {20.f, 0.f, 0.f});
	Run(s.All(), kNever, 100);

	// Walk the host's entity 0 -> 100 over ~400 ms, well past the 0.1 s render delay,
	// so the client's buffer holds a run of samples to interpolate between.
	const auto start = std::chrono::steady_clock::now();
	for (int step = 1; step <= 100; ++step)
	{
		s.host.MoveNetId(theirs, {static_cast<float>(step), 0.f, 0.f});
		Run(s.All(), kNever, 4);
	}
	const auto elapsed = std::chrono::steady_clock::now() - start;
	REQUIRE(elapsed > std::chrono::milliseconds(250));

	const float lagging = client.PositionOfNetId(theirs).x;
	CHECK(lagging > 0.f);   // it is moving
	CHECK(lagging < 100.f); // and it is behind, because it is rendered in the past
	// The control, in the very same frames: no lag on what this peer owns.
	CHECK(client.PositionOfNetId(mine).x == doctest::Approx(20.f));

	// Once the motion stops, render time catches up and the remote copy arrives.
	//
	// This wait needs a much larger budget than the 3 s default, and the reason is not
	// slowness for its own sake. NetworkSendSystem paces snapshots off the WALL CLOCK
	// (see its `(void) dt` note), so the walk above builds an interpolation backlog whose
	// size depends on how much real time those 100 steps took - which in turn depends on
	// machine load. Draining that backlog at the render delay therefore takes longer on a
	// loaded machine, and 3 s was marginal: this case failed roughly one full-suite run in
	// six while passing every time it ran alone. The budget only costs wall time when the
	// case genuinely fails, so it is set well clear of the margin rather than near it.
	const bool settled = Run(s.All(), [&] { return client.PositionOfNetId(theirs).x > 99.9f; }, 30000);
	// Reported only on failure, and worth having: "client x" one step behind "host x" is a
	// backlog still draining, which reads very differently from a value stuck at the start.
	INFO("walk elapsed ms = " << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
	INFO("host x = " << s.host.PositionOfNetId(theirs).x);
	INFO("client x = " << client.PositionOfNetId(theirs).x);
	INFO("lagging x during walk = " << lagging);
	REQUIRE(settled);
}

// ── Single-player ───────────────────────────────────────────────────────────────

TEST_CASE("Offline, neither system does anything and everything is owned")
{
	// Single-player must be untouched by all of the above. There is no session, so
	// there is nothing to gate, nothing to send, and no other authority to defer to -
	// including for an entity whose NetworkIdentity names an owner left over from a
	// session that has ended, which is the one shape of this that could silently stop
	// a solo player's character being simulated.
	ServiceContainer services;
	World world;
	aether::net::NetworkContext context(services);
	aether::net::NetworkReceiveSystem receive(context);
	aether::net::NetworkSendSystem send(context);

	const Entity entity = world.Create();
	world.Emplace<TransformComponent>(entity).localToWorld
	        = ComposeTransform({3.f, 4.f, 0.f}, {0.f, 0.f, 0.f}, {1.f, 1.f, 1.f});
	world.Emplace<aether::net::NetworkIdentity>(entity, aether::net::NetworkIdentity{.netId = 5, .owner = 9});

	receive.Update(world, 1.f / 60.f);
	send.Update(world, 1.f / 60.f);

	CHECK(context.IsOwner(world, entity));
	CHECK(context.HasAuthority(world, entity));
	CHECK(send.PacedConnectionCount() == 0); // not even a send deadline was scheduled
	CHECK(world.TryGet<TransformComponent>(entity)->localToWorld[3].x == doctest::Approx(3.f));
}

// ── Replicated script fields (the route a facing flag travels) ───────────────────

TEST_CASE("A replicated script field set by a client reaches the host and the other clients")
{
	// Whisper decides facing on the owner and carries it as a [Replicated] bool
	// beside the animation state, so what has to hold in the framework is that a
	// CLIENT-owned entity's replicated script fields go upstream and get relayed -
	// which they could not do at all while state was one-way, and which is why remote
	// players used to moonwalk.
	//
	// A bool specifically, because that is what a facing flag is, and seeded TRUE
	// because the value that matters is not the type's default: with false, a packet
	// that never arrived and a field that was never written are indistinguishable.
	Session s(24766, 2);
	Node& mover = *s.clients[0];
	Node& watcher = *s.clients[1];
	const std::uint32_t netId = s.Spawn({6.f, 0.f, 0.f}, s.connections[0]);

	FakeFieldBridge* hostBridge = AttachFacingBridge(s.host, netId);
	FakeFieldBridge* moverBridge = AttachFacingBridge(mover, netId);
	FakeFieldBridge* watcherBridge = AttachFacingBridge(watcher, netId);

	const Entity hostEntity = s.host.context.Session().EntityFor(netId);
	const Entity moverEntity = mover.context.Session().EntityFor(netId);
	const Entity watcherEntity = watcher.context.Session().EntityFor(netId);
	moverBridge->Seed(moverEntity.id, 0, 0, BoolValue(true));

	REQUIRE(Run(s.All(),
	        [&]
	        {
		        const ScriptPropertyValue* onWatcher = watcherBridge->Peek(watcherEntity.id, 0, 0);
		        return onWatcher != nullptr && onWatcher->i64 != 0;
	        }));

	const ScriptPropertyValue* onHost = hostBridge->Peek(hostEntity.id, 0, 0);
	REQUIRE(onHost != nullptr);
	CHECK(onHost->i64 == 1);
	const ScriptPropertyValue* onWatcher = watcherBridge->Peek(watcherEntity.id, 0, 0);
	REQUIRE(onWatcher != nullptr);
	CHECK(onWatcher->i64 == 1);
}
