#include <doctest/doctest.h>

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
	const std::vector<std::byte> bytes = net::EncodeRpc(11, 0xABCDEF01u, 2, args);

	net::ByteReader r{bytes};
	CHECK(static_cast<net::NetMessage>(r.U8()) == net::NetMessage::Rpc);

	const auto msg = net::DecodeRpc(r);
	REQUIRE(msg.has_value());
	CHECK(msg->netId == 11);
	CHECK(msg->scriptTypeHash == 0xABCDEF01u);
	CHECK(msg->methodIndex == 2);
	REQUIRE(msg->args.size() == 3);
	CHECK(msg->args[2] == std::byte{3});
}

TEST_CASE("An RPC claiming more argument bytes than it carries is rejected")
{
	net::ByteWriter w;
	w.U8(static_cast<std::uint8_t>(net::NetMessage::Rpc));
	w.U32(1);
	w.U32(1);
	w.U16(0);
	w.U32(1000); // claims 1000 argument bytes
	const std::vector<std::byte> bytes = w.Take();

	net::ByteReader r{bytes};
	r.U8();
	CHECK_FALSE(net::DecodeRpc(r).has_value());
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

		[[nodiscard]] int FindMethodIndex(const std::string&, const std::string&) const override
		{
			return -1; // encode side; ApplyRpc never calls it
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
	const net::RpcMessage msg{.netId = 1, .scriptTypeHash = net::ScriptTypeHash("Chat"), .methodIndex = 0, .args = {}};
	net::ApplyRpc(world, session, bridge, msg, net::kInvalidConnection, /*localIsHost=*/false);

	REQUIRE(bridge.calls.size() == 1);
	CHECK(bridge.calls[0].entity == entity);
}
