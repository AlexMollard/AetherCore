// The client->host input path (NetInput.hpp) and the owned-entity reconciliation it
// makes safe (NetworkReceiveSystem::ResolveTransforms).
//
// These two belong in one file because they are one bug. State replication is
// host-authoritative and one-way, so before this existed a client's input never
// reached the host, the host simulated that player as an unattended body, and its
// authoritative answer for it was "still on the spawn marker". Turning reconciliation
// on without the input path would therefore have made things WORSE - every client
// would have been dragged back to its spawn point and nobody could move at all. The
// order matters, and the cases below are written in it.

#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <thread>
#include <vector>

#include "net/NetComponents.hpp"
#include "net/NetInput.hpp"
#include "net/NetRpc.hpp"
#include "net/NetSession.hpp"
#include "net/NetworkContext.hpp"
#include "net/NetworkSystems.hpp"
#include "physics2d/Physics2DComponents.hpp"
#include "physics2d/Physics2DSystem.hpp"
#include "scene/Components.hpp"
#include "scene/SceneSerializer.hpp"
#include "scene/TransformUtils.hpp"
#include "scene/World.hpp"
#include "utils/ServiceContainer.hpp"

using namespace aether;

namespace
{
	constexpr net::ConnectionId kOwner = 3;
	constexpr net::ConnectionId kStranger = 4;

	std::vector<std::byte> Payload(std::string_view text)
	{
		std::vector<std::byte> out;
		out.reserve(text.size());
		for (const char c: text)
		{
			out.push_back(static_cast<std::byte>(c));
		}
		return out;
	}

	std::string Text(std::span<const std::byte> bytes)
	{
		std::string out;
		for (const std::byte b: bytes)
		{
			out.push_back(static_cast<char>(b));
		}
		return out;
	}

	// A bridge that both COUNTS invocations and acts on them: each call slides the
	// target entity one unit along +X, and records the payload verbatim.
	//
	// Acting matters. A counter alone proves the dispatch was reached; what the bug
	// was about is whether a client's input changes the HOST'S authoritative state,
	// and only a bridge that writes the world can show that.
	class MovingBridge final : public net::RpcBridge
	{
	public:
		explicit MovingBridge(World& world)
		      : m_world(world)
		{
		}

		[[nodiscard]] int Invocations() const
		{
			return m_invocations;
		}

		[[nodiscard]] const std::string& LastPayload() const
		{
			return m_lastPayload;
		}

		[[nodiscard]] net::RpcMethod FindMethod(const std::string&, const std::string&) const override
		{
			return net::RpcMethod{.index = 0, .target = net::NetRpcTarget::Server};
		}

		void Invoke(Entity entity, std::uint32_t, std::uint16_t, std::span<const std::byte> args) const override
		{
			++m_invocations;
			m_lastPayload = Text(args);
			if (auto* transform = m_world.TryGet<TransformComponent>(entity))
			{
				transform->localToWorld[3].x += 1.f;
			}
		}

	private:
		World& m_world;
		mutable int m_invocations = 0;
		mutable std::string m_lastPayload;
	};

	// A replicated, scripted entity bound to `netId` and owned by `owner`.
	Entity MakeInputTarget(World& world, net::NetSession& session, std::uint32_t netId, net::ConnectionId owner)
	{
		const Entity entity = world.Create();
		world.Emplace<TransformComponent>(entity);
		world.Emplace<ScriptComponent>(entity).scripts.push_back(ScriptEntry{.path = "Mover"});
		world.Emplace<net::NetworkIdentity>(entity, net::NetworkIdentity{.netId = netId, .owner = owner});
		session.Bind(netId, entity);
		return entity;
	}

	float XOf(World& world, Entity entity)
	{
		const auto* transform = world.TryGet<TransformComponent>(entity);
		return transform != nullptr ? transform->localToWorld[3].x : -999.f;
	}
} // namespace

// ── Wire format ─────────────────────────────────────────────────────────────────

TEST_CASE("An input message round-trips every field")
{
	const std::vector<std::byte> packet = net::EncodeInput(7, 0xC0FFEEu, 9, 41, Payload("R101"));
	REQUIRE(packet.size() > 1);
	CHECK(static_cast<std::uint8_t>(packet[0]) == static_cast<std::uint8_t>(net::NetMessage::Input));

	net::ByteReader reader{std::span<const std::byte>(packet).subspan(1)};
	const std::optional<net::InputMessage> msg = net::DecodeInput(reader);
	REQUIRE(msg.has_value());
	CHECK(msg->netId == 7);
	CHECK(msg->scriptTypeHash == 0xC0FFEEu);
	CHECK(msg->methodIndex == 9);
	CHECK(msg->sequence == 41);
	CHECK(Text(msg->payload) == "R101");
}

TEST_CASE("A malformed input message is dropped rather than half-decoded")
{
	SUBCASE("truncated")
	{
		const std::vector<std::byte> packet = net::EncodeInput(7, 1, 0, 1, Payload("R101"));
		const std::span<const std::byte> body = std::span<const std::byte>(packet).subspan(1);
		for (std::size_t cut = 0; cut + 1 < body.size(); ++cut)
		{
			net::ByteReader reader{body.subspan(0, cut)};
			CHECK_FALSE(net::DecodeInput(reader).has_value());
		}
	}

	SUBCASE("net id 0 addresses nothing")
	{
		const std::vector<std::byte> packet = net::EncodeInput(0, 1, 0, 1, Payload("R101"));
		net::ByteReader reader{std::span<const std::byte>(packet).subspan(1)};
		CHECK_FALSE(net::DecodeInput(reader).has_value());
	}

	SUBCASE("a hostile payload length over-reads nothing")
	{
		net::ByteWriter w;
		w.U32(7);           // netId
		w.U32(1);           // typeHash
		w.U16(0);           // methodIndex
		w.U32(1);           // sequence
		w.U32(0xFFFFFFFFu); // payload length, with four bytes actually present
		w.U32(0);
		const std::vector<std::byte> body = w.Take();
		net::ByteReader reader{body};
		CHECK_FALSE(net::DecodeInput(reader).has_value());
	}
}

// ── Send-side pacing ────────────────────────────────────────────────────────────

TEST_CASE("A changed input payload is sent at once and an unchanged one is paced")
{
	net::InputSendPacer pacer;

	// First submission of a stream always goes: the host has nothing for this entity.
	const net::InputSend first = pacer.Prepare(1, Payload("N000"), 0.f, 30.f);
	CHECK(first.send);
	CHECK(first.sequence == 1);

	// Same payload, well inside the repeat window: throttled. This is the bandwidth
	// case - a stationary player at 300 fps is not 300 packets a second.
	CHECK_FALSE(pacer.Prepare(1, Payload("N000"), 0.001f, 30.f).send);
	CHECK_FALSE(pacer.Prepare(1, Payload("N000"), 0.020f, 30.f).send);

	// A DIFFERENT payload jumps the queue. This is what makes an edge survive: a jump
	// press is held for exactly one frame, and a pure rate limiter would sample the
	// stream and could miss it entirely.
	const net::InputSend pressed = pacer.Prepare(1, Payload("N100"), 0.021f, 30.f);
	CHECK(pressed.send);
	CHECK(pressed.sequence == 2);

	// And the unchanged repeat still goes out once the window elapses, so a lost
	// "stopped moving" packet on an unreliable channel cannot leave the host running
	// the character forever.
	CHECK_FALSE(pacer.Prepare(1, Payload("N100"), 0.030f, 30.f).send);
	const net::InputSend repeat = pacer.Prepare(1, Payload("N100"), 0.100f, 30.f);
	CHECK(repeat.send);
	CHECK(repeat.sequence == 3);
}

TEST_CASE("Each entity's input stream is paced and numbered independently")
{
	net::InputSendPacer pacer;
	CHECK(pacer.Prepare(1, Payload("N000"), 0.f, 30.f).sequence == 1);
	// A second entity is a separate stream: its first submission is not throttled by
	// the first entity's window, and its sequence starts from its own zero.
	const net::InputSend other = pacer.Prepare(2, Payload("N000"), 0.001f, 30.f);
	CHECK(other.send);
	CHECK(other.sequence == 1);

	// Forgetting one stream (a despawn) leaves the other alone.
	pacer.Forget(1);
	CHECK(pacer.Prepare(1, Payload("N000"), 0.002f, 30.f).sequence == 1);
	CHECK_FALSE(pacer.Prepare(2, Payload("N000"), 0.002f, 30.f).send);
}

// ── Routing ─────────────────────────────────────────────────────────────────────

TEST_CASE("Input routing follows authority, and always predicts locally")
{
	World world;
	net::NetSession session;
	const Entity owned = MakeInputTarget(world, session, 1, kOwner);
	const Entity someoneElses = MakeInputTarget(world, session, 2, kStranger);

	SUBCASE("offline runs it here and sends nothing")
	{
		const net::InputRoute route = net::RouteInput(world, session, owned);
		CHECK(route.allowed);
		CHECK(route.invokeLocally);
		CHECK_FALSE(route.send);
	}

	SUBCASE("the host is the server, so its own input needs no round trip")
	{
		session.SetRole(net::NetRole::Host);
		const net::InputRoute route = net::RouteInput(world, session, someoneElses);
		CHECK(route.allowed);
		CHECK(route.invokeLocally);
		CHECK_FALSE(route.send);
	}

	SUBCASE("a client predicts AND sends for what it owns")
	{
		session.SetRole(net::NetRole::Client);
		session.SetLocalConnection(kOwner);
		const net::InputRoute route = net::RouteInput(world, session, owned);
		CHECK(route.allowed);
		CHECK(route.invokeLocally); // the prediction half - without it the owner lags a round trip
		CHECK(route.send);
	}

	SUBCASE("a client is refused for an entity it does not own")
	{
		session.SetRole(net::NetRole::Client);
		session.SetLocalConnection(kOwner);
		const net::InputRoute route = net::RouteInput(world, session, someoneElses);
		CHECK_FALSE(route.allowed);
		CHECK_FALSE(route.invokeLocally);
		CHECK_FALSE(route.send);
	}

	SUBCASE("a client before its Welcome owns nothing")
	{
		// LocalConnection is kInvalidConnection until the Welcome lands, which is the
		// same value every host-owned entity's owner defaults to - unguarded, a client
		// briefly owns the entire world.
		session.SetRole(net::NetRole::Client);
		const Entity hostOwned = MakeInputTarget(world, session, 3, net::kInvalidConnection);
		CHECK_FALSE(net::RouteInput(world, session, hostOwned).allowed);
	}

	SUBCASE("an unreplicated entity is predicted locally and sent nowhere")
	{
		session.SetRole(net::NetRole::Client);
		session.SetLocalConnection(kOwner);
		const Entity local = world.Create();
		world.Emplace<TransformComponent>(local);
		const net::InputRoute route = net::RouteInput(world, session, local);
		CHECK(route.allowed);
		CHECK(route.invokeLocally);
		CHECK_FALSE(route.send);
	}
}

// ── Staleness ───────────────────────────────────────────────────────────────────

TEST_CASE("The host applies only strictly newer input for an entity")
{
	net::InputSequenceGate gate;
	CHECK(gate.Accept(1, 5));
	CHECK_FALSE(gate.Accept(1, 5)); // duplicate
	CHECK_FALSE(gate.Accept(1, 4)); // reordered: arrived late, already superseded
	CHECK(gate.Accept(1, 6));
	// A different entity has its own high-water mark.
	CHECK(gate.Accept(2, 1));

	gate.Forget(1);
	CHECK(gate.Accept(1, 1));
}

// ── The gates on the receive side ───────────────────────────────────────────────

TEST_CASE("A client's input drives the entity it owns, and nothing else")
{
	World world;
	net::NetSession session;
	session.SetRole(net::NetRole::Host);
	net::InputSequenceGate gate;
	const MovingBridge bridge(world);

	const Entity mine = MakeInputTarget(world, session, 1, kOwner);
	const Entity theirs = MakeInputTarget(world, session, 2, kStranger);

	net::InputMessage msg;
	msg.netId = 1;
	msg.scriptTypeHash = net::ScriptTypeHash("Mover");
	msg.sequence = 1;
	msg.payload = Payload("R000");

	// The owner is obeyed.
	net::ApplyInput(world, session, bridge, msg, kOwner, gate);
	CHECK(bridge.Invocations() == 1);
	CHECK(bridge.LastPayload() == "R000");
	CHECK(XOf(world, mine) == doctest::Approx(1.f));

	// Somebody else asking to drive the same character is refused. THIS is the gate
	// that stops one player walking another player around the arena.
	msg.sequence = 2;
	net::ApplyInput(world, session, bridge, msg, kStranger, gate);
	CHECK(bridge.Invocations() == 1);
	CHECK(XOf(world, mine) == doctest::Approx(1.f));

	// And the owner is not locked out by the refused packet. The ownership check runs
	// BEFORE the sequence gate precisely so a stranger cannot burn a sequence number
	// on somebody else's stream: were the order reversed, that one refused packet
	// would have recorded sequence 2 and silently frozen this player's input until
	// its own counter climbed past it.
	net::ApplyInput(world, session, bridge, msg, kOwner, gate);
	CHECK(bridge.Invocations() == 2);
	CHECK(XOf(world, mine) == doctest::Approx(2.f));

	CHECK(XOf(world, theirs) == doctest::Approx(0.f));
}

TEST_CASE("A reordered input message is dropped rather than rewinding the character")
{
	World world;
	net::NetSession session;
	session.SetRole(net::NetRole::Host);
	net::InputSequenceGate gate;
	const MovingBridge bridge(world);
	const Entity entity = MakeInputTarget(world, session, 1, kOwner);

	net::InputMessage msg;
	msg.netId = 1;
	msg.scriptTypeHash = net::ScriptTypeHash("Mover");
	msg.payload = Payload("R000");

	msg.sequence = 2;
	net::ApplyInput(world, session, bridge, msg, kOwner, gate);
	msg.sequence = 1; // the older packet, delivered late by an unreliable channel
	net::ApplyInput(world, session, bridge, msg, kOwner, gate);

	CHECK(bridge.Invocations() == 1);
	CHECK(XOf(world, entity) == doctest::Approx(1.f));
}

TEST_CASE("Input naming an unknown entity or an unscripted one is dropped")
{
	World world;
	net::NetSession session;
	session.SetRole(net::NetRole::Host);
	net::InputSequenceGate gate;
	const MovingBridge bridge(world);

	net::InputMessage msg;
	msg.netId = 99; // never bound
	msg.scriptTypeHash = net::ScriptTypeHash("Mover");
	msg.sequence = 1;
	net::ApplyInput(world, session, bridge, msg, kOwner, gate);
	CHECK(bridge.Invocations() == 0);

	// Bound, owned, but carrying no script that hashes to the named type.
	const Entity entity = world.Create();
	world.Emplace<TransformComponent>(entity);
	world.Emplace<net::NetworkIdentity>(entity, net::NetworkIdentity{.netId = 5, .owner = kOwner});
	session.Bind(5, entity);
	msg.netId = 5;
	net::ApplyInput(world, session, bridge, msg, kOwner, gate);
	CHECK(bridge.Invocations() == 0);
}

// ── Over a real connection ──────────────────────────────────────────────────────

namespace
{
	// One trivial prefab, so SpawnPrefab/ApplySpawn genuinely instantiate on both
	// ends. Same shape as the fixtures in NetworkSystemsTests.cpp and
	// NetworkSendSystemTests.cpp, with its own scratch directory.
	struct PrefabFixture
	{
		std::filesystem::path dir;

		PrefabFixture()
		      : dir(std::filesystem::temp_directory_path() / "aether_net_input_test")
		{
			std::filesystem::remove_all(dir);
			std::filesystem::create_directories(dir);
			aether::app::scene::SetProjectSceneDirectories(dir / "scenes", dir);

			aether::app::scene::SceneDescription prefab;
			prefab.name = "net_input_test_prefab";
			aether::app::scene::EntityRecord record;
			record.entityId = 1;
			record.name = "Body";
			record.hasTransform = true;
			prefab.entities.push_back(std::move(record));
			REQUIRE(aether::app::scene::SavePrefabFile("net_input_test_prefab", prefab));
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

	constexpr const char* kPrefab = "net_input_test_prefab";

	struct Node
	{
		ServiceContainer services;
		World world;
		net::NetworkContext context{services};
		net::NetworkReceiveSystem receive{context};

		~Node()
		{
			context.Stop(world);
		}

		Node() = default;
		Node(const Node&) = delete;
		Node& operator=(const Node&) = delete;
	};

	bool PumpUntil(std::initializer_list<Node*> nodes, auto done, float dt = 0.f, int maxMs = 2000)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(maxMs);
		while (std::chrono::steady_clock::now() < deadline)
		{
			for (Node* node: nodes)
			{
				node->receive.Update(node->world, dt);
			}
			if (done())
			{
				return true;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		return false;
	}

	struct Pair
	{
		Node host;
		Node client;
		net::NetworkSendSystem send{host.context};
		net::ConnectionId hostSawPeer = net::kInvalidConnection;

		explicit Pair(std::uint16_t port)
		{
			REQUIRE(host.context.StartHost(host.world, port, 4));
			REQUIRE(client.context.StartClient(client.world, "127.0.0.1", port));
			REQUIRE(PumpUntil({&host, &client}, [&] { return client.context.IsConnected(); }));
			for (const net::ConnectionId conn: host.context.Session().Connections())
			{
				hostSawPeer = conn;
			}
			REQUIRE(hostSawPeer != net::kInvalidConnection);
		}
	};

	void MoveTo(World& world, Entity entity, glm::vec3 position)
	{
		world.TryGet<TransformComponent>(entity)->localToWorld
		        = ComposeTransform(position, {0.f, 0.f, 0.f}, {1.f, 1.f, 1.f});
	}
} // namespace

TEST_CASE("A client's input reaches the host and moves the entity in its authoritative state")
{
	// The headline case, and the one the four-player run proved was missing. Built
	// exactly as the Net.SendInput export builds it - route, pace, encode, send on
	// kChannelInput unreliably - because what is under test is the whole path, not a
	// decoder in isolation.
	const PrefabFixture prefabs;
	Pair p(24761);

	const Entity hostEntity = p.host.context.SpawnPrefab(p.host.world, kPrefab, {0.f, 0.f, 0.f}, p.hostSawPeer);
	REQUIRE(hostEntity.IsValid());
	const std::uint32_t netId = p.host.world.TryGet<net::NetworkIdentity>(hostEntity)->netId;
	p.host.world.Emplace<ScriptComponent>(hostEntity).scripts.push_back(ScriptEntry{.path = "Mover"});

	auto bridge = std::make_unique<MovingBridge>(p.host.world);
	auto* raw = bridge.get();
	p.host.context.SetRpcBridge(std::move(bridge));

	REQUIRE(PumpUntil({&p.host, &p.client}, [&] { return p.client.context.Session().EntityFor(netId).IsValid(); }));
	const Entity clientEntity = p.client.context.Session().EntityFor(netId);
	p.client.world.Emplace<ScriptComponent>(clientEntity).scripts.push_back(ScriptEntry{.path = "Mover"});

	const net::InputRoute route = net::RouteInput(p.client.world, p.client.context.Session(), clientEntity);
	REQUIRE(route.allowed);
	REQUIRE(route.send);
	const net::InputSend paced = p.client.context.InputPacer().Prepare(netId, Payload("R010"),
	        p.client.context.Now(), p.client.context.InputSendRateHz());
	REQUIRE(paced.send);
	p.client.context.Transport().Send(net::kInvalidConnection, net::kChannelInput, /*reliable=*/false,
	        net::EncodeInput(netId, net::ScriptTypeHash("Mover"), 0, paced.sequence, Payload("R010")));

	REQUIRE(PumpUntil({&p.host, &p.client}, [&] { return raw->Invocations() > 0; }));
	CHECK(raw->LastPayload() == "R010");
	// The host's own copy moved. Before this path existed the host had no input at
	// all for a client's player and held it on its spawn marker indefinitely, which
	// is exactly what every other peer then rendered.
	CHECK(XOf(p.host.world, hostEntity) == doctest::Approx(1.f));
}

TEST_CASE("An input message inbound on a client is dropped")
{
	// Input travels client-to-host and nowhere else, so unlike an RPC there is no
	// target byte for a sender to pick a direction with - the role gate is the whole
	// of the check. A client accepting one would let any peer drive this machine's
	// simulation directly.
	const PrefabFixture prefabs;
	Pair p(24762);

	const Entity hostEntity = p.host.context.SpawnPrefab(p.host.world, kPrefab, {0.f, 0.f, 0.f}, p.hostSawPeer);
	const std::uint32_t netId = p.host.world.TryGet<net::NetworkIdentity>(hostEntity)->netId;
	REQUIRE(PumpUntil({&p.host, &p.client}, [&] { return p.client.context.Session().EntityFor(netId).IsValid(); }));

	const Entity clientEntity = p.client.context.Session().EntityFor(netId);
	p.client.world.Emplace<ScriptComponent>(clientEntity).scripts.push_back(ScriptEntry{.path = "Mover"});

	auto bridge = std::make_unique<MovingBridge>(p.client.world);
	auto* raw = bridge.get();
	p.client.context.SetRpcBridge(std::move(bridge));

	// Sent AS THE ENTITY'S OWNER, so the ownership gate cannot be what refuses it -
	// the role gate has to be. Addressed any other way this case passes with the role
	// gate deleted, which is exactly what it did when it was first written.
	p.client.receive.OnData(p.client.world, p.hostSawPeer,
	        net::EncodeInput(netId, net::ScriptTypeHash("Mover"), 0, 1, Payload("R010")));

	CHECK(raw->Invocations() == 0);
	CHECK(XOf(p.client.world, clientEntity) == doctest::Approx(0.f));
}

// ── Reconciliation ──────────────────────────────────────────────────────────────

namespace
{
	// Sets the client's world up to actually SIMULATE, which is the whole point: a
	// locally-owned body stays Dynamic on a client (it is the predicted one), and
	// Dynamic is exactly the body type Physics2DSystem writes the transform back for.
	Physics2DSystem* GiveClientPhysics(Node& node)
	{
		node.world.SetSceneKind(SceneKind::Scene2D);
		node.world.SetSceneFeatures(DefaultSceneFeatures(SceneKind::Scene2D));
		auto system = std::make_unique<Physics2DSystem>();
		auto* raw = system.get();
		node.world.RegisterSystem(std::move(system));
		return raw;
	}

	void GiveBody(World& world, Entity entity)
	{
		world.Emplace<RigidBody2DComponent>(entity,
		        RigidBody2DComponent{.bodyType = Body2DType::Dynamic, .gravityScale = 0.f, .allowSleeping = false});
		world.Emplace<Collider2DComponent>(entity, Collider2DComponent{.size = {1.f, 1.f}});
	}
} // namespace

TEST_CASE("An owned entity's correction survives the physics write-back")
{
	// THE reconciliation bug, in one case. The owner branch of ResolveTransforms was
	// computing the right eased position every single tick and writing it to the
	// TransformComponent - and Physics2DSystem::SyncTransforms then overwrote it from
	// the Box2D pose later in the same frame, because a locally-owned body is
	// deliberately left Dynamic. Correction was running and had no observable effect
	// at all, which is how a fourteen-unit error outlived a snapDistance of four for
	// thirty seconds.
	const PrefabFixture prefabs;
	Pair p(24763);
	p.host.context.SetSendRateHz(1'000'000.f); // pacing is not what this case tests

	Physics2DSystem* physics = GiveClientPhysics(p.client);

	const Entity hostEntity = p.host.context.SpawnPrefab(p.host.world, kPrefab, {0.f, 0.f, 0.f}, p.hostSawPeer);
	REQUIRE(hostEntity.IsValid());
	const std::uint32_t netId = p.host.world.TryGet<net::NetworkIdentity>(hostEntity)->netId;

	// A second, host-owned entity: it is both the "remote entities still interpolate"
	// control AND the tripwire that says the snapshot has landed, since the owned
	// entity's own correction is what the test is trying to observe.
	const Entity hostRemote = p.host.context.SpawnPrefab(p.host.world, kPrefab, {0.f, 0.f, 0.f},
	        net::kInvalidConnection);
	const std::uint32_t remoteNetId = p.host.world.TryGet<net::NetworkIdentity>(hostRemote)->netId;

	REQUIRE(PumpUntil({&p.host, &p.client},
	        [&] {
		        return p.client.context.Session().EntityFor(netId).IsValid()
		               && p.client.context.Session().EntityFor(remoteNetId).IsValid();
	        }));
	const Entity owned = p.client.context.Session().EntityFor(netId);
	const Entity remote = p.client.context.Session().EntityFor(remoteNetId);

	p.client.world.Emplace<net::NetworkTransform>(owned,
	        net::NetworkTransform{.interpolationDelaySeconds = 0.f, .correctionRate = 10.f, .snapDistance = 4.f});
	p.client.world.Emplace<net::NetworkTransform>(remote,
	        net::NetworkTransform{.interpolationDelaySeconds = 0.f, .correctionRate = 10.f, .snapDistance = 4.f});
	GiveBody(p.client.world, owned);

	// The client predicts itself forward to x = 1 and lets physics build the body
	// there, so the pre-correction body pose really is 1.
	MoveTo(p.client.world, owned, {1.f, 0.f, 0.f});
	physics->Update(p.client.world, 1.f / 60.f);
	REQUIRE(XOf(p.client.world, owned) == doctest::Approx(1.f).epsilon(0.01));

	// The host's authoritative answer is 2 for the owned entity, and 3 for the remote
	// one. Small enough to ease rather than snap.
	MoveTo(p.host.world, hostEntity, {2.f, 0.f, 0.f});
	MoveTo(p.host.world, hostRemote, {3.f, 0.f, 0.f});
	p.send.Update(p.host.world, 0.f);

	// dt = 0 while pumping: the ease is frame-rate independent, so nothing moves, and
	// the remote entity arriving at 3 is the signal that the owned entity's
	// authoritative position has been recorded too - both rode the same snapshot.
	REQUIRE(PumpUntil({&p.host, &p.client},
	        [&] { return XOf(p.client.world, remote) == doctest::Approx(3.f).epsilon(0.01); }));
	CHECK(XOf(p.client.world, owned) == doctest::Approx(1.f).epsilon(0.01));

	// One tick with a real delta: the owned entity eases PART of the way, and the
	// rotation/scale channels are left alone.
	p.client.receive.Update(p.client.world, 0.1f);
	const float eased = XOf(p.client.world, owned);
	CHECK(eased > 1.05f);
	CHECK(eased < 1.95f);

	// And now the part that was actually broken: the physics step must not undo it.
	physics->Update(p.client.world, 1.f / 60.f);
	CHECK(XOf(p.client.world, owned) == doctest::Approx(eased).epsilon(0.02));
}

TEST_CASE("An owned entity beyond the snap distance cuts instead of easing")
{
	const PrefabFixture prefabs;
	Pair p(24764);
	p.host.context.SetSendRateHz(1'000'000.f);

	Physics2DSystem* physics = GiveClientPhysics(p.client);

	const Entity hostEntity = p.host.context.SpawnPrefab(p.host.world, kPrefab, {0.f, 0.f, 0.f}, p.hostSawPeer);
	const std::uint32_t netId = p.host.world.TryGet<net::NetworkIdentity>(hostEntity)->netId;
	REQUIRE(PumpUntil({&p.host, &p.client}, [&] { return p.client.context.Session().EntityFor(netId).IsValid(); }));
	const Entity owned = p.client.context.Session().EntityFor(netId);

	p.client.world.Emplace<net::NetworkTransform>(owned,
	        net::NetworkTransform{.interpolationDelaySeconds = 0.f, .correctionRate = 10.f, .snapDistance = 4.f});
	GiveBody(p.client.world, owned);
	physics->Update(p.client.world, 1.f / 60.f);

	// 14 units apart - the error the four-player run measured and watched persist.
	MoveTo(p.host.world, hostEntity, {14.f, 0.f, 0.f});
	p.send.Update(p.host.world, 0.f);

	// dt = 0 deliberately: a snap is not an ease, so it must land on the very tick the
	// snapshot arrives and owe nothing to the frame delta.
	REQUIRE(PumpUntil({&p.host, &p.client},
	        [&] { return XOf(p.client.world, owned) == doctest::Approx(14.f).epsilon(0.01); }));

	physics->Update(p.client.world, 1.f / 60.f);
	CHECK(XOf(p.client.world, owned) == doctest::Approx(14.f).epsilon(0.02));
}

TEST_CASE("A non-owned entity still interpolates through its buffer, and is never teleported")
{
	// The non-regression half. A remote entity is rendered in the PAST, from the
	// interpolation buffer, and its body is handed to Kinematic - neither of which the
	// owned-entity correction path may touch.
	const PrefabFixture prefabs;
	Pair p(24765);
	p.host.context.SetSendRateHz(1'000'000.f);

	GiveClientPhysics(p.client);

	const Entity hostEntity = p.host.context.SpawnPrefab(p.host.world, kPrefab, {0.f, 0.f, 0.f},
	        net::kInvalidConnection);
	const std::uint32_t netId = p.host.world.TryGet<net::NetworkIdentity>(hostEntity)->netId;
	REQUIRE(PumpUntil({&p.host, &p.client}, [&] { return p.client.context.Session().EntityFor(netId).IsValid(); }));
	const Entity remote = p.client.context.Session().EntityFor(netId);

	// A whole second of interpolation delay, so "rendered in the past" is unmissable.
	p.client.world.Emplace<net::NetworkTransform>(remote,
	        net::NetworkTransform{.interpolationDelaySeconds = 1.f, .correctionRate = 10.f, .snapDistance = 4.f});
	GiveBody(p.client.world, remote);

	// Two authoritative samples, milliseconds apart.
	MoveTo(p.host.world, hostEntity, {10.f, 0.f, 0.f});
	p.send.Update(p.host.world, 0.f);
	REQUIRE(PumpUntil({&p.host, &p.client},
	        [&] { return XOf(p.client.world, remote) == doctest::Approx(10.f).epsilon(0.01); }, 1.f / 60.f));

	MoveTo(p.host.world, hostEntity, {50.f, 0.f, 0.f});
	p.send.Update(p.host.world, 0.f);

	// Long enough for the second packet to land many times over. The entity must stay
	// at 10: a render time a whole second in the past is still older than the first
	// sample, so the buffer holds it there. An owned-entity correction leaking onto
	// this path would have snapped it straight to 50 - the error is ten times
	// snapDistance - and a lost interpolation delay would have walked it there.
	PumpUntil({&p.host, &p.client}, [] { return false; }, 1.f / 60.f, 300);
	CHECK(XOf(p.client.world, remote) == doctest::Approx(10.f).epsilon(0.01));

	// And its body was taken off local simulation rather than corrected into place.
	CHECK(p.client.world.Get<RigidBody2DComponent>(remote).bodyType == Body2DType::Kinematic);
	CHECK(p.client.world.Has<net::NetSimulationOverride>(remote));
}
