#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

#include "net/NetComponents.hpp"
#include "net/NetRpc.hpp"
#include "net/NetSession.hpp"
#include "scene/Components.hpp"
#include "scene/World.hpp"

using namespace aether;

TEST_CASE("An RPC round-trips with its argument blob intact")
{
	const std::vector<std::byte> args{std::byte{1}, std::byte{2}, std::byte{3}};
	const std::vector<std::byte> bytes = net::EncodeRpc(11, 0xABCDEF01u, 2, net::NetRpcTarget::Server, args);

	net::ByteReader r{bytes};
	CHECK(static_cast<net::NetMessage>(r.U8()) == net::NetMessage::Rpc);

	const auto msg = net::DecodeRpc(r);
	REQUIRE(msg.has_value());
	CHECK(msg->netId == 11);
	CHECK(msg->scriptTypeHash == 0xABCDEF01u);
	CHECK(msg->methodIndex == 2);
	CHECK(msg->target == net::NetRpcTarget::Server);
	REQUIRE(msg->args.size() == 3);
	CHECK(msg->args[2] == std::byte{3});
}

TEST_CASE("Every RPC target survives the wire")
{
	// The target is what the receiver checks direction against, so a value that did
	// not survive encode/decode would silently become someone else's direction.
	for (const net::NetRpcTarget target:
	        {net::NetRpcTarget::Server, net::NetRpcTarget::Client, net::NetRpcTarget::Multicast})
	{
		const std::vector<std::byte> bytes = net::EncodeRpc(4, 0x1234u, 1, target, {});
		net::ByteReader r{bytes};
		REQUIRE(static_cast<net::NetMessage>(r.U8()) == net::NetMessage::Rpc);
		const auto msg = net::DecodeRpc(r);
		REQUIRE(msg.has_value());
		CHECK(msg->target == target);
	}
}

TEST_CASE("An RPC carrying an out-of-range target byte is dropped, not defaulted")
{
	// Coercing an unknown target to Server would let a peer choose which direction
	// gate its packet is measured against just by writing a byte we do not know.
	for (const std::uint8_t target: {std::uint8_t{3}, std::uint8_t{4}, std::uint8_t{99}, std::uint8_t{255}})
	{
		net::ByteWriter w;
		w.U8(static_cast<std::uint8_t>(net::NetMessage::Rpc));
		w.U32(1);
		w.U32(1);
		w.U16(0);
		w.U8(target);
		w.U32(0);
		const std::vector<std::byte> bytes = w.Take();

		net::ByteReader r{bytes};
		r.U8();
		CHECK_FALSE(net::DecodeRpc(r).has_value());
	}

	// Positive control: the same packet with a target we DO know decodes, so the
	// rejections above are the target check and not a malformed frame.
	net::ByteWriter w;
	w.U8(static_cast<std::uint8_t>(net::NetMessage::Rpc));
	w.U32(1);
	w.U32(1);
	w.U16(0);
	w.U8(static_cast<std::uint8_t>(net::NetRpcTarget::Multicast));
	w.U32(0);
	const std::vector<std::byte> bytes = w.Take();
	net::ByteReader r{bytes};
	r.U8();
	CHECK(net::DecodeRpc(r).has_value());
}

TEST_CASE("An RPC claiming more argument bytes than it carries is rejected")
{
	net::ByteWriter w;
	w.U8(static_cast<std::uint8_t>(net::NetMessage::Rpc));
	w.U32(1);
	w.U32(1);
	w.U16(0);
	w.U8(static_cast<std::uint8_t>(net::NetRpcTarget::Server));
	w.U32(1000); // claims 1000 argument bytes
	const std::vector<std::byte> bytes = w.Take();

	net::ByteReader r{bytes};
	r.U8();
	CHECK_FALSE(net::DecodeRpc(r).has_value());
}

TEST_CASE("An RPC truncated before its target byte is rejected")
{
	// The target read must be bounds-checked like every other field: a packet that
	// ends mid-record must fail the reader rather than yield target 0.
	const std::vector<std::byte> full = net::EncodeRpc(1, 0x99u, 0, net::NetRpcTarget::Multicast, {});
	for (std::size_t len = 1; len < full.size(); ++len)
	{
		net::ByteReader r{std::span<const std::byte>(full.data(), len)};
		r.U8();
		CHECK_FALSE(net::DecodeRpc(r).has_value());
	}
}

namespace
{
	// An RpcBridge with no CLR behind it, recording every call it receives - what
	// makes ApplyRpc's resolution logic testable without a loaded script assembly.
	class FakeRpcBridge final : public net::RpcBridge
	{
	public:
		struct Call
		{
			Entity entity;
			std::uint32_t scriptIndex = 0;
			std::uint16_t methodIndex = 0;
			std::vector<std::byte> args;
		};

		[[nodiscard]] net::RpcMethod FindMethod(const std::string&, const std::string&) const override
		{
			return net::RpcMethod{}; // encode side; ApplyRpc never calls it
		}

		void Invoke(Entity entity, std::uint32_t scriptIndex, std::uint16_t methodIndex,
		        std::span<const std::byte> args) const override
		{
			calls.push_back(Call{entity, scriptIndex, methodIndex, std::vector<std::byte>(args.begin(), args.end())});
		}

		mutable std::vector<Call> calls;
	};

	// The connection id used for "the client that owns the entity" throughout. The
	// host's own entities own themselves with kInvalidConnection, so any nonzero id
	// is distinct from host ownership.
	constexpr net::ConnectionId kOwner = 7;
	constexpr net::ConnectionId kOtherClient = 9;

	// A replicated entity with one script, bound to `netId` and owned by `owner`.
	Entity MakeReplicated(World& world, net::NetSession& session, std::uint32_t netId, net::ConnectionId owner,
	        const char* scriptType = "Chat")
	{
		const Entity entity = world.Create();
		world.Emplace<ScriptComponent>(entity).scripts.push_back(ScriptEntry{.path = scriptType});
		world.Emplace<net::NetworkIdentity>(entity, net::NetworkIdentity{.netId = netId, .owner = owner});
		session.Bind(netId, entity);
		return entity;
	}
} // namespace

TEST_CASE("ApplyRpc invokes the bridge when the net id and script type hash resolve")
{
	World world;
	net::NetSession session;
	const Entity entity = MakeReplicated(world, session, 1, kOwner);

	FakeRpcBridge bridge;
	const net::RpcMessage msg{
	        .netId = 1, .scriptTypeHash = net::ScriptTypeHash("Chat"), .methodIndex = 3, .args = {std::byte{9}}};
	net::ApplyRpc(world, session, bridge, msg, kOwner, /*localIsHost=*/true);

	REQUIRE(bridge.calls.size() == 1);
	CHECK(bridge.calls[0].entity == entity);
	CHECK(bridge.calls[0].scriptIndex == 0);
	CHECK(bridge.calls[0].methodIndex == 3);
	REQUIRE(bridge.calls[0].args.size() == 1);
	CHECK(bridge.calls[0].args[0] == std::byte{9});
}

TEST_CASE("ApplyRpc drops a call whose net id is unknown")
{
	World world;
	net::NetSession session;
	FakeRpcBridge bridge;
	const net::RpcMessage msg{.netId = 42, .scriptTypeHash = 0, .methodIndex = 0, .args = {}};
	net::ApplyRpc(world, session, bridge, msg, kOwner, /*localIsHost=*/true);
	CHECK(bridge.calls.empty());
}

TEST_CASE("ApplyRpc drops a call whose script type hash matches no script on the entity")
{
	World world;
	net::NetSession session;
	MakeReplicated(world, session, 7, kOwner);

	FakeRpcBridge bridge;
	const net::RpcMessage msg{.netId = 7, .scriptTypeHash = 0xDEADBEEFu, .methodIndex = 0, .args = {}};
	net::ApplyRpc(world, session, bridge, msg, kOwner, /*localIsHost=*/true);
	CHECK(bridge.calls.empty());
}

TEST_CASE("ApplyRpc drops a call for an entity with no ScriptComponent")
{
	World world;
	net::NetSession session;
	const Entity entity = world.Create();
	world.Emplace<net::NetworkIdentity>(entity, net::NetworkIdentity{.netId = 3, .owner = kOwner});
	session.Bind(3, entity);

	FakeRpcBridge bridge;
	const net::RpcMessage msg{.netId = 3, .scriptTypeHash = net::ScriptTypeHash("Chat"), .methodIndex = 0, .args = {}};
	net::ApplyRpc(world, session, bridge, msg, kOwner, /*localIsHost=*/true);
	CHECK(bridge.calls.empty());
}

// ── Sender gate ─────────────────────────────────────────────────────────────
// RPC is the only channel by which a client can affect host state, so the host
// must refuse a call aimed at an entity the sender does not own. Without this the
// framework's largest attack surface is wide open: any connected client could
// invoke any [NetRpc] method on any replicated entity.

TEST_CASE("A client may invoke an RPC on the entity it owns")
{
	World world;
	net::NetSession session;
	const Entity entity = MakeReplicated(world, session, 1, kOwner);

	FakeRpcBridge bridge;
	const net::RpcMessage msg{.netId = 1, .scriptTypeHash = net::ScriptTypeHash("Chat"), .methodIndex = 0, .args = {}};
	net::ApplyRpc(world, session, bridge, msg, kOwner, /*localIsHost=*/true);

	REQUIRE(bridge.calls.size() == 1);
	CHECK(bridge.calls[0].entity == entity);
}

TEST_CASE("The host rejects a client RPC aimed at an entity owned by someone else")
{
	World world;
	net::NetSession session;
	MakeReplicated(world, session, 1, kOwner);

	FakeRpcBridge bridge;
	const net::RpcMessage msg{.netId = 1, .scriptTypeHash = net::ScriptTypeHash("Chat"), .methodIndex = 0, .args = {}};
	net::ApplyRpc(world, session, bridge, msg, kOtherClient, /*localIsHost=*/true);

	CHECK(bridge.calls.empty());
}

TEST_CASE("The host rejects a client RPC aimed at one of its own entities")
{
	World world;
	net::NetSession session;
	// A host-owned entity: owner stays kInvalidConnection, which no client is ever
	// assigned (Welcome hands out ids from 1).
	MakeReplicated(world, session, 1, net::kInvalidConnection);

	FakeRpcBridge bridge;
	const net::RpcMessage msg{.netId = 1, .scriptTypeHash = net::ScriptTypeHash("Chat"), .methodIndex = 0, .args = {}};
	net::ApplyRpc(world, session, bridge, msg, kOwner, /*localIsHost=*/true);

	CHECK(bridge.calls.empty());
}

TEST_CASE("The host rejects a client RPC aimed at an entity with no NetworkIdentity")
{
	World world;
	net::NetSession session;
	// Bound but unreplicated - nothing a peer says addresses it.
	const Entity entity = world.Create();
	world.Emplace<ScriptComponent>(entity).scripts.push_back(ScriptEntry{.path = "Chat"});
	session.Bind(1, entity);

	FakeRpcBridge bridge;
	const net::RpcMessage msg{.netId = 1, .scriptTypeHash = net::ScriptTypeHash("Chat"), .methodIndex = 0, .args = {}};
	net::ApplyRpc(world, session, bridge, msg, kOwner, /*localIsHost=*/true);

	CHECK(bridge.calls.empty());
}

TEST_CASE("A host-originated RPC applied on a client is not owner-checked")
{
	World world;
	net::NetSession session;
	// The client owns nothing here (the entity belongs to another player), yet a
	// host-sent call must still apply: the host is authoritative over everything.
	const Entity entity = MakeReplicated(world, session, 1, kOtherClient);

	FakeRpcBridge bridge;
	const net::RpcMessage msg{.netId = 1,
	        .scriptTypeHash = net::ScriptTypeHash("Chat"),
	        .methodIndex = 0,
	        .target = net::NetRpcTarget::Multicast,
	        .args = {}};
	net::ApplyRpc(world, session, bridge, msg, net::kInvalidConnection, /*localIsHost=*/false);

	REQUIRE(bridge.calls.size() == 1);
	CHECK(bridge.calls[0].entity == entity);
}

// ── Direction gate ──────────────────────────────────────────────────────────
// Server travels client-to-host; Client and Multicast travel host-to-client. Each
// role accepts exactly one direction. This is what makes "only the host originates
// a multicast" true on the wire rather than only by convention: a client can put
// any target byte it likes in a packet, and the host drops everything but Server.

TEST_CASE("The host drops an inbound RPC whose target is not Server")
{
	// A client trying to originate a host-to-client call. Both spellings must be
	// refused, including the one that would have made the host broadcast on its
	// behalf.
	for (const net::NetRpcTarget target: {net::NetRpcTarget::Client, net::NetRpcTarget::Multicast})
	{
		World world;
		net::NetSession session;
		MakeReplicated(world, session, 1, kOwner);

		FakeRpcBridge bridge;
		const net::RpcMessage msg{.netId = 1,
		        .scriptTypeHash = net::ScriptTypeHash("Chat"),
		        .methodIndex = 0,
		        .target = target,
		        .args = {}};
		net::ApplyRpc(world, session, bridge, msg, kOwner, /*localIsHost=*/true);

		CHECK(bridge.calls.empty());
	}

	// Positive control: the identical call with target Server, from the same owning
	// sender, DOES apply - so the drops above are the direction gate and nothing else.
	World world;
	net::NetSession session;
	MakeReplicated(world, session, 1, kOwner);

	FakeRpcBridge bridge;
	const net::RpcMessage msg{.netId = 1,
	        .scriptTypeHash = net::ScriptTypeHash("Chat"),
	        .methodIndex = 0,
	        .target = net::NetRpcTarget::Server,
	        .args = {}};
	net::ApplyRpc(world, session, bridge, msg, kOwner, /*localIsHost=*/true);
	CHECK(bridge.calls.size() == 1);
}

TEST_CASE("A client drops an inbound RPC whose target is Server")
{
	// The other half. A Server call is one a client sends, never one it receives;
	// accepting it would let a peer run server-authoritative logic on a client.
	World world;
	net::NetSession session;
	MakeReplicated(world, session, 1, kOtherClient);

	FakeRpcBridge bridge;
	const net::RpcMessage msg{.netId = 1,
	        .scriptTypeHash = net::ScriptTypeHash("Chat"),
	        .methodIndex = 0,
	        .target = net::NetRpcTarget::Server,
	        .args = {}};
	net::ApplyRpc(world, session, bridge, msg, net::kInvalidConnection, /*localIsHost=*/false);
	CHECK(bridge.calls.empty());

	// Positive control: the same packet with a host-to-client target applies.
	const net::RpcMessage allowed{.netId = 1,
	        .scriptTypeHash = net::ScriptTypeHash("Chat"),
	        .methodIndex = 0,
	        .target = net::NetRpcTarget::Client,
	        .args = {}};
	net::ApplyRpc(world, session, bridge, allowed, net::kInvalidConnection, /*localIsHost=*/false);
	CHECK(bridge.calls.size() == 1);
}

// ── Outbound routing ────────────────────────────────────────────────────────
// RouteRpc decides, from role + declared target + ownership alone, who a call goes
// to. Everything the send path does is a consequence of what it returns, so these
// cases are the authority model itself rather than a proxy for it.

namespace
{
	// A session in `role` with `connections` live, plus a replicated entity owned by
	// `owner` bound to net id 1. The entity is what every route below is aimed at.
	struct RouteFixture
	{
		World world;
		net::NetSession session;
		Entity entity;

		RouteFixture(net::NetRole role, net::ConnectionId owner, const std::vector<net::ConnectionId>& connections)
		{
			session.SetRole(role);
			for (const net::ConnectionId connection: connections)
			{
				session.AddConnection(connection);
			}
			entity = MakeReplicated(world, session, 1, owner);
		}
	};

	bool Reaches(const net::RpcRoute& route, net::ConnectionId connection)
	{
		return std::find(route.recipients.begin(), route.recipients.end(), connection) != route.recipients.end();
	}
} // namespace

TEST_CASE("A client may not originate a Client or Multicast call")
{
	// THE authority rule. If this goes, any client can make the host relay whatever
	// it likes to every other player - the exact spoof the host-authoritative model
	// exists to prevent. A client that wants everyone to hear something sends a
	// Server call and lets the host decide.
	RouteFixture fixture(net::NetRole::Client, kOwner, {});

	for (const net::NetRpcTarget target: {net::NetRpcTarget::Client, net::NetRpcTarget::Multicast})
	{
		const net::RpcRoute route = net::RouteRpc(fixture.world, fixture.session, target, fixture.entity);
		CHECK_FALSE(route.allowed);
		CHECK_FALSE(route.invokeLocally);
		CHECK(route.recipients.empty());
	}

	// Positive control: the same client, same entity, Server target - allowed, and
	// aimed at the host rather than run locally.
	const net::RpcRoute route = net::RouteRpc(fixture.world, fixture.session, net::NetRpcTarget::Server,
	        fixture.entity);
	CHECK(route.allowed);
	CHECK_FALSE(route.invokeLocally);
	REQUIRE(route.recipients.size() == 1);
	CHECK(route.recipients[0] == net::kInvalidConnection); // "the host"
}

TEST_CASE("A client's Server call needs a replicated entity")
{
	// An unbound entity has no name on the wire, so the call would arrive addressed
	// to nothing. Refused rather than run locally: a server-authoritative RPC that
	// silently becomes a client-local one reports success for state nobody else has.
	World world;
	net::NetSession session;
	session.SetRole(net::NetRole::Client);
	const Entity entity = world.Create();
	world.Emplace<ScriptComponent>(entity).scripts.push_back(ScriptEntry{.path = "Chat"});

	const net::RpcRoute route = net::RouteRpc(world, session, net::NetRpcTarget::Server, entity);
	CHECK_FALSE(route.allowed);
	CHECK_FALSE(route.invokeLocally);
}

TEST_CASE("A host's Server call runs locally - the host is the server")
{
	RouteFixture fixture(net::NetRole::Host, kOwner, {kOwner, kOtherClient});

	const net::RpcRoute route = net::RouteRpc(fixture.world, fixture.session, net::NetRpcTarget::Server,
	        fixture.entity);
	CHECK(route.allowed);
	CHECK(route.invokeLocally);
	CHECK(route.recipients.empty()); // nothing goes on the wire
}

TEST_CASE("A Client call reaches the owning connection and nobody else")
{
	// The whole point of the Client target. A call that leaked to every connection
	// would be a Multicast wearing the wrong name, and would hand every player the
	// contents of one player's private call.
	RouteFixture fixture(net::NetRole::Host, kOwner, {kOwner, kOtherClient, kOtherClient + 1});

	const net::RpcRoute route = net::RouteRpc(fixture.world, fixture.session, net::NetRpcTarget::Client,
	        fixture.entity);
	CHECK(route.allowed);
	CHECK_FALSE(route.invokeLocally); // a remote client owns it, so the host is not a recipient
	REQUIRE(route.recipients.size() == 1);
	CHECK(route.recipients[0] == kOwner);
	CHECK_FALSE(Reaches(route, kOtherClient));
	CHECK_FALSE(Reaches(route, kOtherClient + 1));
}

TEST_CASE("A Client call on a host-owned entity runs on the host")
{
	// The host IS the owning client of its own entity, so "run on the owner" means
	// run here. The alternative - dropping it - would make an ownership change
	// silently stop delivering a call that was arriving fine a moment earlier.
	RouteFixture fixture(net::NetRole::Host, net::kInvalidConnection, {kOwner, kOtherClient});

	const net::RpcRoute route = net::RouteRpc(fixture.world, fixture.session, net::NetRpcTarget::Client,
	        fixture.entity);
	CHECK(route.allowed);
	CHECK(route.invokeLocally);
	CHECK(route.recipients.empty());
}

TEST_CASE("A Client call whose owner has already left reaches nobody")
{
	// Allowed, but with nothing to deliver to - and emphatically NOT fanned out to
	// the connections that are still here.
	RouteFixture fixture(net::NetRole::Host, kOwner, {kOtherClient});

	const net::RpcRoute route = net::RouteRpc(fixture.world, fixture.session, net::NetRpcTarget::Client,
	        fixture.entity);
	CHECK(route.allowed);
	CHECK_FALSE(route.invokeLocally);
	CHECK(route.recipients.empty());
}

TEST_CASE("A Multicast reaches every connected client")
{
	RouteFixture fixture(net::NetRole::Host, kOwner, {kOwner, kOtherClient, kOtherClient + 1});

	const net::RpcRoute route = net::RouteRpc(fixture.world, fixture.session, net::NetRpcTarget::Multicast,
	        fixture.entity);
	CHECK(route.allowed);
	REQUIRE(route.recipients.size() == 3);
	CHECK(Reaches(route, kOwner));
	CHECK(Reaches(route, kOtherClient));
	CHECK(Reaches(route, kOtherClient + 1));
}

TEST_CASE("A Multicast also runs on the host")
{
	// PINNED DECISION. UE5 runs a multicast on the server too, and the motivating
	// case - a chat line the host prefixes and broadcasts - is wrong without it: the
	// host would be the only participant that never sees what it just said. Changing
	// this is a behaviour change for every project, so it must break a test.
	RouteFixture fixture(net::NetRole::Host, kOwner, {kOwner});

	const net::RpcRoute route = net::RouteRpc(fixture.world, fixture.session, net::NetRpcTarget::Multicast,
	        fixture.entity);
	CHECK(route.allowed);
	CHECK(route.invokeLocally);
}

TEST_CASE("A Multicast with no connections still runs on the host")
{
	// The single-client-drops-out case, and the degenerate one a host hits before
	// anybody joins. Nothing to send, but the host still hears itself.
	RouteFixture fixture(net::NetRole::Host, net::kInvalidConnection, {});

	const net::RpcRoute route = net::RouteRpc(fixture.world, fixture.session, net::NetRpcTarget::Multicast,
	        fixture.entity);
	CHECK(route.allowed);
	CHECK(route.invokeLocally);
	CHECK(route.recipients.empty());
}

TEST_CASE("A host-to-client call needs a replicated entity")
{
	// The packet names the entity by net id; without one there is nothing to address.
	// Running it locally instead would report success for a call no client ever saw.
	World world;
	net::NetSession session;
	session.SetRole(net::NetRole::Host);
	session.AddConnection(kOwner);
	const Entity entity = world.Create();
	world.Emplace<ScriptComponent>(entity).scripts.push_back(ScriptEntry{.path = "Chat"});
	world.Emplace<net::NetworkIdentity>(entity, net::NetworkIdentity{.netId = 0, .owner = kOwner});

	for (const net::NetRpcTarget target: {net::NetRpcTarget::Client, net::NetRpcTarget::Multicast})
	{
		const net::RpcRoute route = net::RouteRpc(world, session, target, entity);
		CHECK_FALSE(route.allowed);
		CHECK_FALSE(route.invokeLocally);
	}
}

TEST_CASE("Offline every target runs locally")
{
	// A single-player build has no session at all, so project code written for
	// multiplayer must run unchanged - including a multicast, which addresses only
	// this process because this process is the whole world.
	World world;
	net::NetSession session; // role Offline
	const Entity entity = world.Create();
	world.Emplace<ScriptComponent>(entity).scripts.push_back(ScriptEntry{.path = "Chat"});

	for (const net::NetRpcTarget target:
	        {net::NetRpcTarget::Server, net::NetRpcTarget::Client, net::NetRpcTarget::Multicast})
	{
		const net::RpcRoute route = net::RouteRpc(world, session, target, entity);
		CHECK(route.allowed);
		CHECK(route.invokeLocally);
		CHECK(route.recipients.empty());
	}
}

TEST_CASE("An explicit call-site target that contradicts the declaration is a mismatch")
{
	// The Net.CallServer guard, which used to live inline in NetRpcExports.cpp - a
	// CLR-linked TU EngineTests cannot link - and which RouteRpc never sees, so the
	// refusal had no coverage anywhere. Extracted as a predicate precisely so it can
	// have some.

	// Net.Call passes -1: the declaration is the only opinion, whatever it declares.
	for (const net::NetRpcTarget declared:
	        {net::NetRpcTarget::Server, net::NetRpcTarget::Client, net::NetRpcTarget::Multicast})
	{
		CAPTURE(static_cast<std::uint32_t>(declared));
		CHECK_FALSE(net::RpcTargetMismatch(-1, declared));
	}

	// Net.CallServer passes Server: it agrees only with a method that declares Server.
	constexpr auto kServer = static_cast<std::int32_t>(net::NetRpcTarget::Server);
	CHECK_FALSE(net::RpcTargetMismatch(kServer, net::NetRpcTarget::Server));
	CHECK(net::RpcTargetMismatch(kServer, net::NetRpcTarget::Client));
	CHECK(net::RpcTargetMismatch(kServer, net::NetRpcTarget::Multicast));

	// Every other explicit spelling agrees with exactly its own declaration. Written
	// as a matrix rather than as the two rows Net.CallServer happens to use today, so
	// a second explicit helper cannot arrive uncovered.
	for (const net::NetRpcTarget expected:
	        {net::NetRpcTarget::Server, net::NetRpcTarget::Client, net::NetRpcTarget::Multicast})
	{
		for (const net::NetRpcTarget declared:
		        {net::NetRpcTarget::Server, net::NetRpcTarget::Client, net::NetRpcTarget::Multicast})
		{
			CAPTURE(static_cast<std::uint32_t>(expected));
			CAPTURE(static_cast<std::uint32_t>(declared));
			CHECK(net::RpcTargetMismatch(static_cast<std::int32_t>(expected), declared) == (expected != declared));
		}
	}

	// A value no enumerator has - an assembly built against a newer NetRpcTarget than
	// this binary knows. It matches nothing, so it is refused rather than coerced.
	CHECK(net::RpcTargetMismatch(99, net::NetRpcTarget::Server));
}
