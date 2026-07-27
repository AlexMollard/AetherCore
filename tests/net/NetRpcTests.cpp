#include <doctest/doctest.h>

#include <vector>

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

		void Invoke(Entity entity, std::uint32_t scriptIndex, std::uint16_t methodIndex,
		        std::span<const std::byte> args) const override
		{
			calls.push_back(Call{entity, scriptIndex, methodIndex, std::vector<std::byte>(args.begin(), args.end())});
		}

		mutable std::vector<Call> calls;
	};
} // namespace

TEST_CASE("ApplyRpc invokes the bridge when the net id and script type hash resolve")
{
	World world;
	net::NetSession session;
	const Entity entity = world.Create();
	world.Emplace<ScriptComponent>(entity).scripts.push_back(ScriptEntry{.path = "Chat"});
	session.Bind(1, entity);

	FakeRpcBridge bridge;
	const net::RpcMessage msg{
	        .netId = 1, .scriptTypeHash = net::ScriptTypeHash("Chat"), .methodIndex = 3, .args = {std::byte{9}}};
	net::ApplyRpc(world, session, bridge, msg);

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
	net::ApplyRpc(world, session, bridge, msg);
	CHECK(bridge.calls.empty());
}

TEST_CASE("ApplyRpc drops a call whose script type hash matches no script on the entity")
{
	World world;
	net::NetSession session;
	const Entity entity = world.Create();
	world.Emplace<ScriptComponent>(entity).scripts.push_back(ScriptEntry{.path = "Chat"});
	session.Bind(7, entity);

	FakeRpcBridge bridge;
	const net::RpcMessage msg{.netId = 7, .scriptTypeHash = 0xDEADBEEFu, .methodIndex = 0, .args = {}};
	net::ApplyRpc(world, session, bridge, msg);
	CHECK(bridge.calls.empty());
}

TEST_CASE("ApplyRpc drops a call for an entity with no ScriptComponent")
{
	World world;
	net::NetSession session;
	const Entity entity = world.Create();
	session.Bind(3, entity);

	FakeRpcBridge bridge;
	const net::RpcMessage msg{.netId = 3, .scriptTypeHash = net::ScriptTypeHash("Chat"), .methodIndex = 0, .args = {}};
	net::ApplyRpc(world, session, bridge, msg);
	CHECK(bridge.calls.empty());
}
