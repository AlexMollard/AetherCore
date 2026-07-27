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
	FakeFieldBridge* AttachScriptTarget(Endpoint& endpoint, std::uint32_t netId)
	{
		const Entity entity = endpoint.world.Create();
		endpoint.world.Emplace<aether::net::NetworkIdentity>(entity, aether::net::NetworkIdentity{.netId = netId});
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

// ── Role gating ─────────────────────────────────────────────────────────────────

TEST_CASE("A client-sent Snapshot is dropped on the host")
{
	// Only the host is authoritative. If this gate goes, any connected client can
	// rewrite every replicated field on the host - position, health, anything in the
	// schema - by sending one unsolicited packet on an unreliable channel.
	const std::vector<std::byte> body = SnapshotMoving(1, {1.f, 2.f, 3.f});
	const std::vector<std::byte> packet = aether::net::NetworkContext::Frame(aether::net::NetMessage::Snapshot, body);

	{
		Endpoint host;
		host.BecomeHost();
		const Entity entity = host.Replicate(1, {9.f, 9.f, 9.f});

		host.receive.OnData(host.world, kPeer, packet);

		const glm::vec3 after = host.PositionOf(entity);
		CHECK(after.x == doctest::Approx(9.f));
		CHECK(after.y == doctest::Approx(9.f));
		CHECK(after.z == doctest::Approx(9.f));
	}

	// Positive control: the very same bytes DO apply on a client, so the case above
	// proves a role gate and not a malformed packet.
	{
		Endpoint client;
		client.BecomeClient();
		const Entity entity = client.Replicate(1, {9.f, 9.f, 9.f});

		client.receive.OnData(client.world, kPeer, packet);

		const glm::vec3 after = client.PositionOf(entity);
		CHECK(after.x == doctest::Approx(1.f));
		CHECK(after.y == doctest::Approx(2.f));
		CHECK(after.z == doctest::Approx(3.f));
	}
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

TEST_CASE("A ScriptFields packet inbound on the host is dropped")
{
	// Script fields are arbitrary gameplay state - the packet writes straight into a
	// script's [Replicated] properties. On the host this must never come from a peer.
	// The bridge write counter is the assertion: a dropped packet is one the bridge
	// was never asked to apply.
	const ScriptFieldExchange exchange(1);
	const std::vector<std::byte> packet = aether::net::NetworkContext::Frame(aether::net::NetMessage::ScriptFields,
	        exchange.body);

	{
		Endpoint host;
		host.BecomeHost();
		FakeFieldBridge* bridge = AttachScriptTarget(host, 1);

		host.receive.OnData(host.world, kPeer, packet);

		CHECK(bridge->Writes() == 0);
	}

	{
		Endpoint client;
		client.BecomeClient();
		FakeFieldBridge* bridge = AttachScriptTarget(client, 1);

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
