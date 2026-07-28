// NetworkReceiveSystem::OnData is the framework's inbound trust boundary: every
// byte a remote peer can send arrives here, and the ONLY thing standing between a
// client's packet and the host's world is the role gate at the top of each case.
// Before this file that gate had no coverage at all.
//
// Every "dropped" case below is asserted twice: once that the packet changed
// nothing on the role that must reject it, and once that the SAME bytes do land on
// the role that must accept them. A drop test with no positive control passes just
// as well against a packet that was malformed all along.

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "io/FileUtil.hpp"
#include "net/NetComponents.hpp"
#include "net/NetRpc.hpp"
#include "net/NetScriptFields.hpp"
#include "net/NetSerialize.hpp"
#include "net/NetSnapshot.hpp"
#include "net/NetSpawn.hpp"
#include "net/NetworkContext.hpp"
#include "net/NetworkSystems.hpp"
#include "physics2d/Physics2DComponents.hpp"
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
	// Distinct from every other net test's ports.
	constexpr std::uint16_t kHostPort = 24711;
	constexpr std::uint16_t kUnreachablePort = 24712; // nothing listens here, deliberately

	constexpr aether::net::ConnectionId kPeer = 3;

	// A live context in a given role, plus the receive system driving it. StartClient
	// aims at a port nothing listens on: ENet reports Client the moment the connect is
	// queued, which is exactly the pre-Welcome state these tests care about.
	struct Endpoint
	{
		ServiceContainer services;
		World world;
		aether::net::NetworkContext context{services};
		aether::net::NetworkReceiveSystem receive{context};

		~Endpoint()
		{
			context.Stop(world);
		}

		Endpoint(const Endpoint&) = delete;
		Endpoint& operator=(const Endpoint&) = delete;

		Endpoint() = default;

		void BecomeHost()
		{
			REQUIRE(context.StartHost(world, kHostPort, 4));
		}

		void BecomeClient()
		{
			REQUIRE(context.StartClient(world, "127.0.0.1", kUnreachablePort));
		}

		// A replicated entity at `position`, bound to `netId`.
		Entity Replicate(std::uint32_t netId, glm::vec3 position, aether::net::ConnectionId owner
		        = aether::net::kInvalidConnection)
		{
			const Entity entity = world.Create();
			world.Emplace<TransformComponent>(entity).localToWorld
			        = ComposeTransform(position, {0.f, 0.f, 0.f}, {1.f, 1.f, 1.f});
			world.Emplace<aether::net::NetworkIdentity>(entity,
			        aether::net::NetworkIdentity{.netId = netId, .owner = owner});
			context.Session().Bind(netId, entity);
			return entity;
		}

		[[nodiscard]] glm::vec3 PositionOf(Entity entity) const
		{
			const auto* transform = world.TryGet<TransformComponent>(entity);
			return transform != nullptr ? glm::vec3(transform->localToWorld[3]) : glm::vec3(-999.f);
		}
	};

	// A snapshot body (no NetMessage byte - snapshots are the WRAPPED half of the
	// framing convention) that moves `netId` to `position`. Built from a throwaway
	// world exactly as a host's NetworkSendSystem would, so these are real bytes and
	// not a hand-rolled approximation of them.
	std::vector<std::byte> SnapshotMoving(std::uint32_t netId, glm::vec3 position)
	{
		World sender;
		aether::net::NetSession session;
		aether::net::SnapshotCache cache;
		const aether::net::ReplicationSchema schema = aether::net::BuildReplicationSchema(reflect::ComponentTypes());

		const Entity entity = sender.Create();
		sender.Emplace<TransformComponent>(entity).localToWorld
		        = ComposeTransform(position, {0.f, 0.f, 0.f}, {1.f, 1.f, 1.f});
		sender.Emplace<aether::net::NetworkIdentity>(entity, aether::net::NetworkIdentity{.netId = netId});
		session.Bind(netId, entity);

		std::vector<std::byte> body = aether::net::BuildSnapshot(sender, schema, reflect::ComponentTypes(), session,
		        cache, {entity});
		REQUIRE_FALSE(body.empty());
		return body;
	}

	ScriptPropertyValue IntValue(std::int64_t v)
	{
		ScriptPropertyValue out;
		out.type = ScriptPropertyValue::Type::Int;
		out.i64 = v;
		return out;
	}

	// A replicated entity carrying one script, on both a sender and a receiver, plus
	// the script-field packet the sender would emit for it.
	struct ScriptFieldExchange
	{
		std::vector<std::byte> body;

		explicit ScriptFieldExchange(std::uint32_t netId)
		{
			World sender;
			aether::net::NetSession session;
			aether::net::SnapshotCache cache;
			FakeFieldBridge bridge;
			bridge.Declare("Health", {aether::net::ScriptPropertyDesc{
			                                 .index = 0, .type = ScriptPropertyValue::Type::Int}});

			const Entity entity = sender.Create();
			sender.Emplace<aether::net::NetworkIdentity>(entity, aether::net::NetworkIdentity{.netId = netId});
			sender.Emplace<ScriptComponent>(entity).scripts.push_back(ScriptEntry{.path = "Health"});
			session.Bind(netId, entity);
			bridge.Seed(entity.id, 0, 0, IntValue(77));

			body = aether::net::BuildScriptFieldPacket(sender, session, cache, bridge, {entity});
			REQUIRE_FALSE(body.empty());
		}
	};

	// Gives `endpoint` a scripted entity on `netId` and the fake bridge that will be
	// asked to write it. The bridge is owned by the context, so the raw pointer stays
	// valid for the life of the endpoint.
	FakeFieldBridge* AttachScriptTarget(Endpoint& endpoint, std::uint32_t netId,
	        aether::net::ConnectionId owner = aether::net::kInvalidConnection)
	{
		const Entity entity = endpoint.world.Create();
		endpoint.world.Emplace<aether::net::NetworkIdentity>(entity,
		        aether::net::NetworkIdentity{.netId = netId, .owner = owner});
		endpoint.world.Emplace<ScriptComponent>(entity).scripts.push_back(ScriptEntry{.path = "Health"});
		endpoint.context.Session().Bind(netId, entity);

		auto bridge = std::make_unique<FakeFieldBridge>();
		bridge->Declare("Health", {aether::net::ScriptPropertyDesc{
		                                  .index = 0, .type = ScriptPropertyValue::Type::Int}});
		auto* raw = bridge.get();
		endpoint.context.SetFieldBridge(std::move(bridge));
		return raw;
	}
} // namespace

// ── Inbound gating: role for the one-way kinds, OWNERSHIP for state ─────────────

TEST_CASE("An inbound Snapshot writes only the entities the sender owns")
{
	// THE security boundary of client authority. State replication is two-way now -
	// a client uploads what it owns and the host relays it - so the old "a client
	// never sends state" role gate is gone, and this ownership gate is the ONLY thing
	// left between a connected client and every replicated field in the host's world.
	// Lose it and any peer can teleport any other player, or rewrite any replicated
	// component on any entity in the schema, with one unsolicited packet.
	//
	// Three senders, one packet, so "refused" is measured against a live control
	// rather than asserted on its own:
	//   - a client naming an entity ANOTHER client owns   refused
	//   - a client naming a HOST-owned entity             refused
	//   - the actual owner naming its own entity          applied
	constexpr aether::net::ConnectionId kOther = 7;
	const std::vector<std::byte> body = SnapshotMoving(1, {1.f, 2.f, 3.f});
	const std::vector<std::byte> packet = aether::net::NetworkContext::Frame(aether::net::NetMessage::Snapshot, body);

	// A client claiming an entity that belongs to a different client. This is the
	// case a role gate could never have caught, because the sender IS a legitimate
	// client sending a legitimate kind on the right channel.
	{
		Endpoint host;
		host.BecomeHost();
		const Entity entity = host.Replicate(1, {9.f, 9.f, 9.f}, kOther);

		host.receive.OnData(host.world, kPeer, packet);

		const glm::vec3 after = host.PositionOf(entity);
		CHECK(after.x == doctest::Approx(9.f));
		CHECK(after.y == doctest::Approx(9.f));
		CHECK(after.z == doctest::Approx(9.f));
	}

	// A client claiming a host-owned entity - the level geometry, the game-state
	// entity, anything the session itself decides.
	{
		Endpoint host;
		host.BecomeHost();
		const Entity entity = host.Replicate(1, {9.f, 9.f, 9.f}, aether::net::kInvalidConnection);

		host.receive.OnData(host.world, kPeer, packet);

		CHECK(host.PositionOf(entity).x == doctest::Approx(9.f));
	}

	// The owner sending the SAME bytes, which must land - otherwise the two cases
	// above prove only that the packet was malformed all along, and a client's own
	// character would never move on the host either.
	{
		Endpoint host;
		host.BecomeHost();
		const Entity entity = host.Replicate(1, {9.f, 9.f, 9.f}, kPeer);

		host.receive.OnData(host.world, kPeer, packet);

		const glm::vec3 after = host.PositionOf(entity);
		CHECK(after.x == doctest::Approx(1.f));
		CHECK(after.y == doctest::Approx(2.f));
		CHECK(after.z == doctest::Approx(3.f));
	}

	// A client applying the host's snapshot is ungated: there is one link and the
	// host is the session's authority, so there is no second candidate to tell it
	// apart from. The owner of the entity here is nobody this client knows.
	{
		Endpoint client;
		client.BecomeClient();
		const Entity entity = client.Replicate(1, {9.f, 9.f, 9.f}, kOther);

		client.receive.OnData(client.world, kPeer, packet);

		CHECK(client.PositionOf(entity).x == doctest::Approx(1.f));
	}
}

TEST_CASE("An inbound Snapshot naming an unreplicated entity is refused on the host")
{
	// The gate resolves ownership through NetworkIdentity, so an entity bound to a
	// net id but carrying no identity at all has no owner to match and must be
	// refused rather than written by default. Nothing in the framework produces one,
	// which is exactly why it needs pinning: "no identity" must never read as
	// "unclaimed, therefore yours".
	const std::vector<std::byte> packet = aether::net::NetworkContext::Frame(aether::net::NetMessage::Snapshot,
	        SnapshotMoving(1, {1.f, 2.f, 3.f}));

	Endpoint host;
	host.BecomeHost();
	const Entity entity = host.world.Create();
	host.world.Emplace<TransformComponent>(entity).localToWorld
	        = ComposeTransform({9.f, 9.f, 9.f}, {0.f, 0.f, 0.f}, {1.f, 1.f, 1.f});
	host.context.Session().Bind(1, entity);

	host.receive.OnData(host.world, kPeer, packet);

	CHECK(host.PositionOf(entity).x == doctest::Approx(9.f));
}

TEST_CASE("A Welcome is ignored on the host")
{
	// A client cannot assign the host an id. Accepting one would give the host a
	// connection id, and every host-owned entity (owner == kInvalidConnection) would
	// stop being the host's own.
	const std::vector<std::byte> packet = aether::net::NetworkContext::EncodeWelcome(5);

	{
		Endpoint host;
		host.BecomeHost();
		host.receive.OnData(host.world, kPeer, packet);
		CHECK(host.context.Session().LocalConnection() == aether::net::kInvalidConnection);
		CHECK(host.context.IsHost());
	}

	{
		Endpoint client;
		client.BecomeClient();
		MakeScenePlaced(client.world, 1);
		REQUIRE(client.context.Session().LocalConnection() == aether::net::kInvalidConnection);

		client.receive.OnData(client.world, kPeer, packet);

		CHECK(client.context.Session().LocalConnection() == 5);
		CHECK(client.context.IsConnected());
		// Joining is also what numbers the scene-placed entities on this end.
		const std::vector<std::uint32_t> ids = ScenePlacedNetIds(client.world);
		REQUIRE(ids.size() == 1);
		CHECK(ids[0] != 0);
	}
}

TEST_CASE("A second Welcome mid-session is dropped")
{
	// Re-running the join would zero every already-spawned entity's netId via
	// ResetForNewSession while their session bindings survived, and the fresh
	// AssignScenePlacedNetIds would then bind none of them.
	Endpoint client;
	client.BecomeClient();
	client.receive.OnData(client.world, kPeer, aether::net::NetworkContext::EncodeWelcome(5));
	REQUIRE(client.context.Session().LocalConnection() == 5);

	const Entity spawned = MakeSessionSpawned(client.world, client.context, 5);
	const std::uint32_t netId = client.world.TryGet<aether::net::NetworkIdentity>(spawned)->netId;

	client.receive.OnData(client.world, kPeer, aether::net::NetworkContext::EncodeWelcome(6));

	CHECK(client.context.Session().LocalConnection() == 5); // unchanged
	CHECK(client.world.TryGet<aether::net::NetworkIdentity>(spawned)->netId == netId); // not zeroed
	CHECK(client.context.Session().EntityFor(netId) == spawned); // binding intact
}

TEST_CASE("A Welcome assigning the invalid connection id is rejected")
{
	Endpoint client;
	client.BecomeClient();
	client.receive.OnData(client.world, kPeer, aether::net::NetworkContext::EncodeWelcome(aether::net::kInvalidConnection));
	CHECK(client.context.Session().LocalConnection() == aether::net::kInvalidConnection);
	CHECK_FALSE(client.context.IsConnected());
}

TEST_CASE("A Despawn inbound on the host is dropped")
{
	// Despawn is a destroy primitive. Ungated, any client could delete any replicated
	// entity on the host - and the host would then broadcast nothing, so the deletion
	// would silently diverge every other peer too.
	const std::vector<std::byte> packet = aether::net::EncodeDespawn(5);

	{
		Endpoint host;
		host.BecomeHost();
		const Entity entity = host.Replicate(5, {0.f, 0.f, 0.f});

		host.receive.OnData(host.world, kPeer, packet);

		CHECK(host.world.GetRegistry().valid(World::ToEntt(entity)));
		CHECK(host.context.Session().EntityFor(5) == entity);
	}

	{
		Endpoint client;
		client.BecomeClient();
		const Entity entity = client.Replicate(5, {0.f, 0.f, 0.f});

		client.receive.OnData(client.world, kPeer, packet);

		CHECK_FALSE(client.world.GetRegistry().valid(World::ToEntt(entity)));
		CHECK_FALSE(client.context.Session().EntityFor(5).IsValid());
	}
}

TEST_CASE("A Relevancy leave message inbound on the host is dropped")
{
	// Only the host decides relevancy. Ungated, a client could tell itself (or a
	// hostile peer could tell another client) to forget an entity that is still
	// perfectly relevant - exactly the same trust boundary as Despawn, just for a
	// weaker claim.
	const std::vector<std::byte> packet = aether::net::EncodeRelevancyLeave(5);

	{
		Endpoint host;
		host.BecomeHost();
		const Entity entity = host.Replicate(5, {0.f, 0.f, 0.f});

		host.receive.OnData(host.world, kPeer, packet);

		CHECK(host.world.GetRegistry().valid(World::ToEntt(entity)));
		CHECK(host.context.Session().EntityFor(5) == entity);
	}

	// Positive control: the same bytes DO remove the entity on a client, so the drop
	// above is the role gate and not a malformed packet.
	{
		Endpoint client;
		client.BecomeClient();
		const Entity entity = client.Replicate(5, {0.f, 0.f, 0.f});

		client.receive.OnData(client.world, kPeer, packet);

		CHECK_FALSE(client.world.GetRegistry().valid(World::ToEntt(entity)));
		CHECK_FALSE(client.context.Session().EntityFor(5).IsValid());
	}
}

TEST_CASE("An inbound ScriptFields packet writes only the entities the sender owns")
{
	// The same ownership gate as the component snapshot, and it has to be: a
	// [Replicated] script property is gameplay state exactly as much as a transform
	// is. A client that could not move another player's body but could rewrite that
	// player's replicated script fields would not be gated at all.
	//
	// The bridge write counter is the assertion: a refused field is one the bridge
	// was never asked to apply.
	constexpr aether::net::ConnectionId kOther = 7;
	const ScriptFieldExchange exchange(1);
	const std::vector<std::byte> packet = aether::net::NetworkContext::Frame(aether::net::NetMessage::ScriptFields,
	        exchange.body);

	{
		Endpoint host;
		host.BecomeHost();
		FakeFieldBridge* bridge = AttachScriptTarget(host, 1, kOther);

		host.receive.OnData(host.world, kPeer, packet);

		CHECK(bridge->Writes() == 0);
	}

	{
		Endpoint host;
		host.BecomeHost();
		FakeFieldBridge* bridge = AttachScriptTarget(host, 1, aether::net::kInvalidConnection);

		host.receive.OnData(host.world, kPeer, packet);

		CHECK(bridge->Writes() == 0);
	}

	// The owner's own submission, which must land - this is how a client's animation
	// and facing reach everybody else.
	{
		Endpoint host;
		host.BecomeHost();
		FakeFieldBridge* bridge = AttachScriptTarget(host, 1, kPeer);

		host.receive.OnData(host.world, kPeer, packet);

		CHECK(bridge->Writes() == 1);
		const ScriptPropertyValue* value = bridge->Peek(host.context.Session().EntityFor(1).id, 0, 0);
		REQUIRE(value != nullptr);
		CHECK(value->i64 == 77);
	}

	{
		Endpoint client;
		client.BecomeClient();
		FakeFieldBridge* bridge = AttachScriptTarget(client, 1, kOther);

		client.receive.OnData(client.world, kPeer, packet);

		CHECK(bridge->Writes() == 1);
		const ScriptPropertyValue* value = bridge->Peek(client.context.Session().EntityFor(1).id, 0, 0);
		REQUIRE(value != nullptr);
		CHECK(value->i64 == 77);
	}
}

TEST_CASE("A ScriptFields packet with no bridge injected is a no-op, not a crash")
{
	// The CLR-less build. Both bridges stay null and every consumer must tolerate it.
	const ScriptFieldExchange exchange(1);
	Endpoint client;
	client.BecomeClient();
	client.Replicate(1, {0.f, 0.f, 0.f});
	REQUIRE(client.context.FieldBridge() == nullptr);

	client.receive.OnData(client.world, kPeer,
	        aether::net::NetworkContext::Frame(aether::net::NetMessage::ScriptFields, exchange.body));

	CHECK(client.context.Session().EntityFor(1).IsValid());
}

TEST_CASE("An RPC is dispatched only for the connection that owns the target")
{
	// RPC is the ONE inbound channel a client is allowed to use, so its gate is the
	// ownership check rather than the role check. Driven through OnData here (rather
	// than ApplyRpc directly, as NetRpcTests does) because OnData is what decides
	// which sender and which localIsHost flag reach it.
	Endpoint host;
	host.BecomeHost();

	auto bridge = std::make_unique<FakeRpcCounter>();
	auto* raw = bridge.get();
	host.context.SetRpcBridge(std::move(bridge));

	const Entity entity = host.world.Create();
	host.world.Emplace<ScriptComponent>(entity).scripts.push_back(ScriptEntry{.path = "Chat"});
	host.world.Emplace<aether::net::NetworkIdentity>(entity,
	        aether::net::NetworkIdentity{.netId = 8, .owner = kPeer});
	host.context.Session().Bind(8, entity);

	const std::vector<std::byte> packet = aether::net::EncodeRpc(8, aether::net::ScriptTypeHash("Chat"), 0,
	        aether::net::NetRpcTarget::Server, {});

	// A different connection asking to run a method on someone else's entity.
	host.receive.OnData(host.world, kPeer + 1, packet);
	CHECK(raw->Invocations() == 0);

	// The owner asking is allowed.
	host.receive.OnData(host.world, kPeer, packet);
	CHECK(raw->Invocations() == 1);
}

namespace
{
	// Gives `endpoint` a scripted, replicated entity on `netId` owned by `owner`, plus
	// the counting bridge that would be asked to dispatch a call aimed at it. The
	// bridge is owned by the context, so the raw pointer lives as long as the endpoint.
	FakeRpcCounter* AttachRpcTarget(Endpoint& endpoint, std::uint32_t netId, aether::net::ConnectionId owner)
	{
		const Entity entity = endpoint.world.Create();
		endpoint.world.Emplace<ScriptComponent>(entity).scripts.push_back(ScriptEntry{.path = "Chat"});
		endpoint.world.Emplace<aether::net::NetworkIdentity>(entity,
		        aether::net::NetworkIdentity{.netId = netId, .owner = owner});
		endpoint.context.Session().Bind(netId, entity);

		auto bridge = std::make_unique<FakeRpcCounter>();
		auto* raw = bridge.get();
		endpoint.context.SetRpcBridge(std::move(bridge));
		return raw;
	}
} // namespace

TEST_CASE("A host-to-client RPC inbound on the host is dropped")
{
	// The direction gate, driven through the real packet path. A client putting
	// Multicast in the target byte is trying to make the host relay for it; a client
	// putting Client there is trying to run host-authoritative code on a peer. Both
	// are refused BEFORE the ownership check, so even the legitimate owner of the
	// entity cannot get one through.
	for (const aether::net::NetRpcTarget target:
	        {aether::net::NetRpcTarget::Client, aether::net::NetRpcTarget::Multicast})
	{
		const std::vector<std::byte> packet = aether::net::EncodeRpc(8, aether::net::ScriptTypeHash("Chat"), 0,
		        target, {});

		{
			Endpoint host;
			host.BecomeHost();
			FakeRpcCounter* bridge = AttachRpcTarget(host, 8, kPeer);

			host.receive.OnData(host.world, kPeer, packet); // from the owner, no less
			CHECK(bridge->Invocations() == 0);
		}

		// Positive control: the very same bytes DO dispatch on a client, so the drop
		// above is the direction gate and not a malformed packet.
		{
			Endpoint client;
			client.BecomeClient();
			FakeRpcCounter* bridge = AttachRpcTarget(client, 8, kPeer);

			client.receive.OnData(client.world, kPeer, packet);
			CHECK(bridge->Invocations() == 1);
		}
	}
}

TEST_CASE("On the host the direction gate and the ownership gate each refuse on their own")
{
	// The case above changes TWO things at once between its drop and its positive
	// control - the target byte AND the role - so it proves the bytes are well-formed
	// but not that, on the host, with ownership already satisfied, the target byte
	// alone decides. That left the two gates only ever agreeing, and a build with
	// either one deleted still passed every host-side direction case.
	//
	// One fixture, one entity, one owner; exactly one variable moves per row:
	//   owner  + Server  -> dispatched   (both gates pass: the positive control)
	//   owner  + Client  -> refused      (ownership passes, direction refuses)
	//   other  + Server  -> refused      (direction passes, ownership refuses)
	Endpoint host;
	host.BecomeHost();
	FakeRpcCounter* bridge = AttachRpcTarget(host, 8, kPeer);

	const std::vector<std::byte> serverCall = aether::net::EncodeRpc(8, aether::net::ScriptTypeHash("Chat"), 0,
	        aether::net::NetRpcTarget::Server, {});
	const std::vector<std::byte> clientCall = aether::net::EncodeRpc(8, aether::net::ScriptTypeHash("Chat"), 0,
	        aether::net::NetRpcTarget::Client, {});

	host.receive.OnData(host.world, kPeer, serverCall);
	CHECK(bridge->Invocations() == 1);

	// Same sender, same entity, same owner - only the declared direction differs.
	host.receive.OnData(host.world, kPeer, clientCall);
	CHECK(bridge->Invocations() == 1); // unchanged: the direction gate alone refused

	// Same direction as the call that worked - only the sender differs.
	host.receive.OnData(host.world, kPeer + 1, serverCall);
	CHECK(bridge->Invocations() == 1); // unchanged: the ownership gate alone refused
}

TEST_CASE("A Server-target RPC inbound on a client is dropped")
{
	// The other direction. A Server call is one a client sends, never one it
	// receives - accepting it would run server-authoritative logic on a client.
	const std::vector<std::byte> packet = aether::net::EncodeRpc(8, aether::net::ScriptTypeHash("Chat"), 0,
	        aether::net::NetRpcTarget::Server, {});

	{
		Endpoint client;
		client.BecomeClient();
		FakeRpcCounter* bridge = AttachRpcTarget(client, 8, kPeer);

		client.receive.OnData(client.world, kPeer, packet);
		CHECK(bridge->Invocations() == 0);
	}

	// Positive control: the same bytes on the host, from the owner, do dispatch.
	{
		Endpoint host;
		host.BecomeHost();
		FakeRpcCounter* bridge = AttachRpcTarget(host, 8, kPeer);

		host.receive.OnData(host.world, kPeer, packet);
		CHECK(bridge->Invocations() == 1);
	}
}

TEST_CASE("An RPC with an unrecognised target byte is dropped on both roles")
{
	// The decode-side half: an out-of-range target is not coerced to Server, so it
	// cannot be used to pick which direction gate the packet is measured against.
	aether::net::ByteWriter w;
	w.U8(static_cast<std::uint8_t>(aether::net::NetMessage::Rpc));
	w.U32(8);
	w.U32(aether::net::ScriptTypeHash("Chat"));
	w.U16(0);
	w.U8(200); // no such target
	w.U32(0);
	const std::vector<std::byte> packet = w.Take();

	{
		Endpoint host;
		host.BecomeHost();
		FakeRpcCounter* bridge = AttachRpcTarget(host, 8, kPeer);
		host.receive.OnData(host.world, kPeer, packet);
		CHECK(bridge->Invocations() == 0);
	}

	{
		Endpoint client;
		client.BecomeClient();
		FakeRpcCounter* bridge = AttachRpcTarget(client, 8, kPeer);
		client.receive.OnData(client.world, kPeer, packet);
		CHECK(bridge->Invocations() == 0);
	}
}

// ── Malformed input ─────────────────────────────────────────────────────────────

TEST_CASE("An unknown leading byte is discarded without touching anything")
{
	Endpoint client;
	client.BecomeClient();
	client.receive.OnData(client.world, kPeer, aether::net::NetworkContext::EncodeWelcome(5));
	const Entity entity = client.Replicate(1, {4.f, 5.f, 6.f});

	// 7 used to be in this list and is now NetMessage::Relevancy, so the case kept
	// passing while no longer meaning anything: the packet decoded as a valid
	// relevancy leave for an unbound net id, which is a no-op for entirely different
	// reasons. Derived from the enum's own bound instead, so the first genuinely
	// unassigned value is always the one tested.
	constexpr auto kFirstUnassigned = static_cast<std::uint8_t>(aether::net::kNetMessageMax + 1);
	for (const std::uint8_t leading: {std::uint8_t{0}, kFirstUnassigned, std::uint8_t{99}, std::uint8_t{255}})
	{
		aether::net::ByteWriter w;
		w.U8(leading);
		w.U32(0xDEADBEEFu);
		w.U32(0xDEADBEEFu);
		const std::vector<std::byte> packet = w.Take();
		client.receive.OnData(client.world, kPeer, packet);
	}

	CHECK(client.context.Session().LocalConnection() == 5);
	CHECK(client.context.Session().EntityFor(1) == entity);
	const glm::vec3 after = client.PositionOf(entity);
	CHECK(after.x == doctest::Approx(4.f));
	CHECK(after.z == doctest::Approx(6.f));
}

TEST_CASE("An empty packet and a lone kind byte are safe on both roles")
{
	// Every decoder past the switch reads from a zero-length payload here. They are
	// bounds-checked and report failure rather than throwing - this pins that.
	//
	// Built from the enum's own bound rather than written out: the hand-written
	// {1..6} silently stopped covering the newest kind the moment Relevancy was
	// added, so DecodeRelevancyLeave on an empty payload was never fed anything.
	std::vector<std::uint8_t> kinds;
	for (std::uint8_t kind = 1; kind <= aether::net::kNetMessageMax; ++kind)
	{
		kinds.push_back(kind);
	}

	{
		Endpoint host;
		host.BecomeHost();
		const Entity entity = host.Replicate(1, {1.f, 1.f, 1.f});

		host.receive.OnData(host.world, kPeer, std::span<const std::byte>{});
		for (const std::uint8_t kind: kinds)
		{
			const std::byte lone{kind};
			host.receive.OnData(host.world, kPeer, std::span<const std::byte>(&lone, 1));
		}

		CHECK(host.context.Session().EntityFor(1) == entity);
		CHECK(host.context.Session().LocalConnection() == aether::net::kInvalidConnection);
		CHECK(CountIdentities(host.world) == 1);
	}

	{
		Endpoint client;
		client.BecomeClient();
		const Entity entity = client.Replicate(1, {1.f, 1.f, 1.f});

		client.receive.OnData(client.world, kPeer, std::span<const std::byte>{});
		for (const std::uint8_t kind: kinds)
		{
			const std::byte lone{kind};
			client.receive.OnData(client.world, kPeer, std::span<const std::byte>(&lone, 1));
		}

		// Nothing was applied, nothing was destroyed, and no id was handed out.
		CHECK(client.context.Session().EntityFor(1) == entity);
		CHECK(client.context.Session().LocalConnection() == aether::net::kInvalidConnection);
		CHECK(CountIdentities(client.world) == 1);
	}
}

TEST_CASE("A truncated Spawn is dropped rather than half-applied")
{
	Endpoint client;
	client.BecomeClient();

	// A well-formed Spawn, cut short at every length below its full size.
	const std::vector<std::byte> full = aether::net::EncodeSpawn(12, 0, "net_test_prefab", {1.f, 2.f, 3.f});
	for (std::size_t len = 1; len < full.size(); ++len)
	{
		client.receive.OnData(client.world, kPeer, std::span<const std::byte>(full.data(), len));
		CHECK_FALSE(client.context.Session().EntityFor(12).IsValid());
	}
	CHECK(CountIdentities(client.world) == 0);
}

// ── Spawn replication and the join/leave/re-join cycle ───────────────────────────

namespace
{
	// Points the prefab loader at a scratch directory holding one trivial prefab, so
	// ApplySpawn genuinely instantiates something. Without a real asset, "the host
	// dropped the Spawn" and "the client could not find the prefab" look identical.
	struct PrefabFixture
	{
		std::filesystem::path dir;

		PrefabFixture()
		      : dir(std::filesystem::temp_directory_path() / "aether_net_spawn_test")
		{
			std::filesystem::remove_all(dir);
			std::filesystem::create_directories(dir);
			aether::app::scene::SetProjectSceneDirectories(dir / "scenes", dir);

			aether::app::scene::SceneDescription prefab;
			prefab.name = "net_test_prefab";
			aether::app::scene::EntityRecord record;
			record.entityId = 1;
			record.name = "Body";
			record.hasTransform = true;
			prefab.entities.push_back(std::move(record));
			REQUIRE(aether::app::scene::SavePrefabFile("net_test_prefab", prefab));
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
} // namespace

TEST_CASE("A Spawn inbound on the host is dropped")
{
	const PrefabFixture prefabs;
	const std::vector<std::byte> packet = aether::net::EncodeSpawn(60, 0, "net_test_prefab", {1.f, 2.f, 3.f});

	{
		Endpoint host;
		host.BecomeHost();

		host.receive.OnData(host.world, kPeer, packet);

		CHECK_FALSE(host.context.Session().EntityFor(60).IsValid());
		CHECK(CountIdentities(host.world) == 0);
	}

	// Positive control: the same bytes DO create the entity on a client, so the drop
	// above is the role gate and not a missing asset.
	{
		Endpoint client;
		client.BecomeClient();

		client.receive.OnData(client.world, kPeer, packet);

		REQUIRE(client.context.Session().EntityFor(60).IsValid());
		CHECK(CountIdentities(client.world) == 1);
		const auto* identity = client.world.TryGet<aether::net::NetworkIdentity>(
		        client.context.Session().EntityFor(60));
		REQUIRE(identity != nullptr);
		CHECK(identity->netId == 60);
		CHECK_FALSE(identity->scenePlaced); // session-owned, so Stop() must reclaim it
	}
}

TEST_CASE("Join, leave and re-join replays spawns without growing the world")
{
	// The regression this file exists for. Stop() once left session-spawned entities
	// behind; ResetForNewSession then zeroed their net ids, AssignScenePlacedNetIds
	// skipped them (no scene node id), and the next join's replay instantiated a
	// second copy of every one - so each cycle doubled the spawned population.
	//
	// Unlike the hand-built version in NetworkContextTests, this drives the real
	// packet path: Welcome, two Spawns, then the disconnect handler.
	const PrefabFixture prefabs;

	ServiceContainer services;
	World world;
	aether::net::NetworkContext context(services);
	aether::net::NetworkReceiveSystem receive(context);

	constexpr std::size_t kScenePlaced = 2;
	MakeScenePlaced(world, 1);
	MakeScenePlaced(world, 2);

	std::size_t peakOfFirstCycle = 0;
	for (int cycle = 0; cycle < 3; ++cycle)
	{
		REQUIRE(context.StartClient(world, "127.0.0.1", kUnreachablePort));
		receive.OnData(world, kPeer, aether::net::NetworkContext::EncodeWelcome(4));
		REQUIRE(context.Session().LocalConnection() == 4);

		// The host's replay: the two scene-placed entities (no-ops, both ends already
		// have them) plus two prefab-spawned ones.
		for (const std::uint32_t netId: ScenePlacedNetIds(world))
		{
			receive.OnData(world, kPeer, aether::net::EncodeSpawn(netId, 0, "", {0.f, 0.f, 0.f}));
		}
		receive.OnData(world, kPeer, aether::net::EncodeSpawn(500, 4, "net_test_prefab", {1.f, 0.f, 0.f}));
		receive.OnData(world, kPeer, aether::net::EncodeSpawn(501, 9, "net_test_prefab", {2.f, 0.f, 0.f}));

		const std::size_t peak = CountIdentities(world);
		CHECK(peak == kScenePlaced + 2);
		if (cycle == 0)
		{
			peakOfFirstCycle = peak;
		}
		else
		{
			CHECK(peak == peakOfFirstCycle); // no doubling
		}

		// The link to the host drops: the client tears the session down.
		receive.OnDisconnected(world, kPeer);

		CHECK_FALSE(context.IsActive());
		CHECK(CountIdentities(world) == kScenePlaced); // only the scene survives
	}
}

TEST_CASE("A host despawns everything a leaving connection owned, and nothing else")
{
	Endpoint host;
	host.BecomeHost();

	const Entity leaverA = MakeSessionSpawned(host.world, host.context, kPeer);
	const Entity leaverB = MakeSessionSpawned(host.world, host.context, kPeer);
	const Entity stayer = MakeSessionSpawned(host.world, host.context, kPeer + 1);
	host.context.Session().AddConnection(kPeer);
	host.context.Session().AddConnection(kPeer + 1);
	REQUIRE(CountIdentities(host.world) == 3);

	host.receive.OnDisconnected(host.world, kPeer);

	CHECK(CountIdentities(host.world) == 1);
	CHECK_FALSE(host.world.GetRegistry().valid(World::ToEntt(leaverA)));
	CHECK_FALSE(host.world.GetRegistry().valid(World::ToEntt(leaverB)));
	CHECK(host.world.GetRegistry().valid(World::ToEntt(stayer)));
	CHECK(host.context.Session().Connections().size() == 1);
	CHECK(host.context.Session().Connections().front() == kPeer + 1);
	// The host is still hosting - only the peer left.
	CHECK(host.context.IsHost());
}

TEST_CASE("A host releases a scene-placed entity's ownership on disconnect instead of destroying it")
{
	// Unreachable today - nothing in the framework assigns ownership of a
	// scene-placed entity to a connection - but latent the moment something does.
	// Stop() already treats scenePlaced as sacrosanct when the WHOLE session ends;
	// OnDisconnected must make the same call for a single connection leaving while
	// the session continues, or a scene-placed entity handed to a connection would
	// be destroyed permanently on the host the moment that connection dropped.
	Endpoint host;
	host.BecomeHost();

	const Entity scenePlaced = MakeScenePlaced(host.world, 1);
	auto* identity = host.world.TryGet<aether::net::NetworkIdentity>(scenePlaced);
	identity->netId = host.context.Session().AllocateNetId();
	identity->owner = kPeer;
	// MakeScenePlaced leaves scenePlaced false - only AssignScenePlacedNetIds sets
	// it, and this test bypasses that to hand ownership to a connection directly.
	identity->scenePlaced = true;
	host.context.Session().Bind(identity->netId, scenePlaced);
	const std::uint32_t scenePlacedNetId = identity->netId;

	const Entity sessionSpawned = MakeSessionSpawned(host.world, host.context, kPeer);
	host.context.Session().AddConnection(kPeer);
	REQUIRE(CountIdentities(host.world) == 2);

	host.receive.OnDisconnected(host.world, kPeer);

	// The scene-placed entity survives, its net id and binding intact, with
	// ownership released rather than assigned to nobody-in-particular by omission.
	REQUIRE(host.world.GetRegistry().valid(World::ToEntt(scenePlaced)));
	const auto* afterIdentity = host.world.TryGet<aether::net::NetworkIdentity>(scenePlaced);
	REQUIRE(afterIdentity != nullptr);
	CHECK(afterIdentity->owner == aether::net::kInvalidConnection);
	CHECK(afterIdentity->netId == scenePlacedNetId);
	CHECK(afterIdentity->scenePlaced);
	CHECK(host.context.Session().EntityFor(scenePlacedNetId) == scenePlaced);

	// The session-spawned one is despawned exactly as before this fix.
	CHECK_FALSE(host.world.GetRegistry().valid(World::ToEntt(sessionSpawned)));
	CHECK(CountIdentities(host.world) == 1);
}

TEST_CASE("A host ignores a connect event for the invalid connection id")
{
	Endpoint host;
	host.BecomeHost();
	host.receive.OnConnected(host.world, aether::net::kInvalidConnection);
	CHECK(host.context.Session().Connections().empty());
}

// ── Simulation authority ────────────────────────────────────────────────────
//
// The bug these cover: a client received a remote character's transform every
// tick and then overwrote it, in the SAME frame, with Physics2DSystem's own
// integration of a body it had no authority over - so a remotely owned character
// never appeared to move at all. Everything below drives NetworkReceiveSystem's
// real Update, because the point is that the reconcile actually happens on the
// frame the packets land, not merely that a function exists.
//
// No Physics2DSystem is registered in these worlds, so RebuildBody2D finds
// nothing and the observable effect is exactly the authored state change - which
// is the whole of the decision. Whether Box2D honours a rebuilt Kinematic body is
// Physics2DSystem's own contract (PushKinematicTargets / SyncTransforms), covered
// by tests/physics2d.

namespace
{
	// A replicated entity with a 2D body - the shape of every networked character,
	// and the only shape the local simulation fights the network over.
	Entity ReplicateWithBody(Endpoint& endpoint, std::uint32_t netId, aether::net::ConnectionId owner,
	        Body2DType type = Body2DType::Dynamic)
	{
		const Entity entity = endpoint.Replicate(netId, {0.f, 0.f, 0.f}, owner);
		endpoint.world.Emplace<RigidBody2DComponent>(entity, RigidBody2DComponent{.bodyType = type});
		return entity;
	}

	[[nodiscard]] Body2DType BodyTypeOf(World& world, Entity entity)
	{
		const auto* rigid = world.TryGet<RigidBody2DComponent>(entity);
		REQUIRE(rigid != nullptr);
		return rigid->bodyType;
	}
} // namespace

TEST_CASE("A client stops locally simulating a body it has no authority over")
{
	Endpoint client;
	client.BecomeClient();
	client.context.Session().SetLocalConnection(4); // the Welcome landed

	const Entity remote = ReplicateWithBody(client, 1, aether::net::kInvalidConnection); // host-owned

	REQUIRE(BodyTypeOf(client.world, remote) == Body2DType::Dynamic);

	client.receive.Update(client.world, 1.f / 60.f);

	// Kinematic is transform-driven in this engine, which is precisely what a
	// replicated pose needs: Physics2DSystem pushes the ECS transform into Box2D
	// instead of integrating over it, and its dynamic write-back skips the body.
	CHECK(BodyTypeOf(client.world, remote) == Body2DType::Kinematic);
	REQUIRE(client.world.Has<aether::net::NetSimulationOverride>(remote));
	CHECK(client.world.Get<aether::net::NetSimulationOverride>(remote).authoredBodyType == Body2DType::Dynamic);
}

TEST_CASE("A client keeps simulating the entity it owns, which is locally predicted")
{
	// The owned entity is the one this client's scripts drive and the receive system
	// eases toward the host's answer. Take its body off local simulation and the
	// player's own character stops responding to input entirely - a strictly worse
	// bug than the one being fixed.
	Endpoint client;
	client.BecomeClient();
	client.context.Session().SetLocalConnection(4);

	const Entity owned = ReplicateWithBody(client, 1, 4);
	const Entity remote = ReplicateWithBody(client, 2, aether::net::kInvalidConnection);

	client.receive.Update(client.world, 1.f / 60.f);

	CHECK(BodyTypeOf(client.world, owned) == Body2DType::Dynamic);
	CHECK_FALSE(client.world.Has<aether::net::NetSimulationOverride>(owned));
	// The positive control: the same tick DID hand over the entity it does not own,
	// so "owned stayed dynamic" is a decision and not a no-op pass.
	CHECK(BodyTypeOf(client.world, remote) == Body2DType::Kinematic);
}

TEST_CASE("A host stops simulating a body a client owns, and keeps simulating its own")
{
	// The reverse of what host authority did, and the whole point of the switch: the
	// owner of an entity simulates it, so the host must take a client-owned body off
	// local simulation exactly as a client does with the host's. Left Dynamic, the
	// host's Box2D integration would fight the transforms arriving from that body's
	// owner every frame, and whichever wrote last would win.
	Endpoint host;
	host.BecomeHost();

	const Entity hostOwned = ReplicateWithBody(host, 1, aether::net::kInvalidConnection);
	const Entity clientOwned = ReplicateWithBody(host, 2, kPeer);

	host.receive.Update(host.world, 1.f / 60.f);
	// Driven a second time straight into the reconcile, bypassing the receive
	// system's own call: without this the case only proves what the CALL does, and a
	// rule lost from SyncSimulationAuthority itself - the one that has to hold for
	// every other caller - would go unnoticed.
	host.context.SyncSimulationAuthority(host.world);

	CHECK(BodyTypeOf(host.world, clientOwned) == Body2DType::Kinematic);
	CHECK(host.world.Has<aether::net::NetSimulationOverride>(clientOwned));
	// The negative control: the host's OWN body is untouched, so "handed over" is a
	// decision about ownership and not a blanket handover of everything replicated.
	CHECK(BodyTypeOf(host.world, hostOwned) == Body2DType::Dynamic);
	CHECK_FALSE(host.world.Has<aether::net::NetSimulationOverride>(hostOwned));
}

TEST_CASE("Nothing is handed over before the host's Welcome arrives")
{
	// Pre-Welcome a client has no connection id, so IsOwner answers false for
	// everything - including the character it is about to be given. Acting on that
	// would hand over its own entity and then rebuild the body a second time to give
	// it straight back.
	Endpoint client;
	client.BecomeClient();
	REQUIRE(client.context.Session().LocalConnection() == aether::net::kInvalidConnection);

	const Entity soonToBeOwned = ReplicateWithBody(client, 1, 4);

	client.receive.Update(client.world, 1.f / 60.f);

	CHECK(BodyTypeOf(client.world, soonToBeOwned) == Body2DType::Dynamic);
	CHECK_FALSE(client.world.Has<aether::net::NetSimulationOverride>(soonToBeOwned));

	// And once the Welcome lands it is recognised as this client's own.
	client.context.Session().SetLocalConnection(4);
	client.receive.Update(client.world, 1.f / 60.f);
	CHECK(BodyTypeOf(client.world, soonToBeOwned) == Body2DType::Dynamic);
}

TEST_CASE("The handover happens once, not on every tick")
{
	// The reconcile runs every frame, so it must be a decision about divergence and
	// not an unconditional write. Re-running it would re-capture the CURRENT body
	// type as the authored one, and the recorded original would decay to Kinematic -
	// at which point nothing can ever be restored. The recorded type is where that
	// damage is visible; what it costs is covered by the Stop cases in
	// NetworkContextTests.cpp.
	Endpoint client;
	client.BecomeClient();
	client.context.Session().SetLocalConnection(4);

	const Entity remote = ReplicateWithBody(client, 1, aether::net::kInvalidConnection);

	for (int tick = 0; tick < 5; ++tick)
	{
		client.receive.Update(client.world, 1.f / 60.f);
	}

	CHECK(BodyTypeOf(client.world, remote) == Body2DType::Kinematic);
	REQUIRE(client.world.Has<aether::net::NetSimulationOverride>(remote));
	CHECK(client.world.Get<aether::net::NetSimulationOverride>(remote).authoredBodyType == Body2DType::Dynamic);
}

TEST_CASE("A client that gains ownership of an entity gets its body back")
{
	// Ownership is not fixed for an entity's lifetime: a re-sent Spawn can name a new
	// owner, and a host releases a leaver's scene-placed entities. The entity that
	// becomes this client's must start simulating again or it can never be driven.
	Endpoint client;
	client.BecomeClient();
	client.context.Session().SetLocalConnection(4);

	const Entity entity = ReplicateWithBody(client, 1, aether::net::kInvalidConnection);
	client.receive.Update(client.world, 1.f / 60.f);
	REQUIRE(BodyTypeOf(client.world, entity) == Body2DType::Kinematic);

	client.world.TryGet<aether::net::NetworkIdentity>(entity)->owner = 4;
	client.receive.Update(client.world, 1.f / 60.f);

	CHECK(BodyTypeOf(client.world, entity) == Body2DType::Dynamic);
	CHECK_FALSE(client.world.Has<aether::net::NetSimulationOverride>(entity));
}

TEST_CASE("A remote body that was never dynamic is left exactly as authored")
{
	// Static and Kinematic bodies are already transform-driven - Physics2DSystem's
	// write-back only ever touches Dynamic ones - so there is nothing to take over,
	// and marking one would mean a pointless body rebuild plus a restore that has to
	// remember a type that never changed.
	Endpoint client;
	client.BecomeClient();
	client.context.Session().SetLocalConnection(4);

	const Entity platform = ReplicateWithBody(client, 1, aether::net::kInvalidConnection, Body2DType::Static);
	const Entity lift = ReplicateWithBody(client, 2, aether::net::kInvalidConnection, Body2DType::Kinematic);

	client.receive.Update(client.world, 1.f / 60.f);

	CHECK(BodyTypeOf(client.world, platform) == Body2DType::Static);
	CHECK(BodyTypeOf(client.world, lift) == Body2DType::Kinematic);
	CHECK_FALSE(client.world.Has<aether::net::NetSimulationOverride>(platform));
	CHECK_FALSE(client.world.Has<aether::net::NetSimulationOverride>(lift));
}
