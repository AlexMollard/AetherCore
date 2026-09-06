// NetMessage::OwnershipRequest/OwnershipTransfer: the wire kinds that let ownership
// of an already-spawned entity change hands after Spawn - a physics gun grabbing a
// prop it does not yet own, or dropping one it does. NetFramingTests.cpp pins the
// framing convention (self-framing, contiguous enum numbering); this file pins the
// three things that actually break if the feature regresses: the payload survives a
// round trip, ValidateOwnershipRequest enforces "the host decides; a client may only
// ask for itself, and only for what is currently free or already its own", and a
// live two-peer exchange over real loopback sockets lands the same answer on both
// ends - including a hostile request naming a third connection, which the host must
// refuse rather than honour.

#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <thread>
#include <vector>

#include "net/NetComponents.hpp"
#include "net/NetOwnership.hpp"
#include "net/NetSerialize.hpp"
#include "net/NetworkContext.hpp"
#include "net/NetworkSystems.hpp"
#include "scene/World.hpp"
#include "utils/ServiceContainer.hpp"

using namespace aether;

namespace
{
	net::NetMessage KindOf(std::span<const std::byte> packet)
	{
		return static_cast<net::NetMessage>(static_cast<std::uint8_t>(packet[0]));
	}

	// A live host+client pair over real loopback sockets - the same shape
	// NetSceneReadinessTests.cpp's Node/PumpUntil use, kept local rather than
	// shared: a struct this small costs less to duplicate than to factor out.
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

	// ENet needs several service calls to complete a handshake or land a packet, so
	// a single Update() is never enough.
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

	// A bound entity on `node`, carrying a NetworkIdentity owned by `owner` - a
	// scene-placed-style prop both peers already agree exists, with no Spawn round
	// trip needed for what these tests are actually about (the ownership messages).
	Entity Bind(Node& node, std::uint32_t netId, net::ConnectionId owner)
	{
		const Entity entity = node.world.Create();
		node.world.Emplace<net::NetworkIdentity>(entity, net::NetworkIdentity{.netId = netId, .owner = owner});
		node.context.Session().Bind(netId, entity);
		return entity;
	}
} // namespace

TEST_CASE("EncodeOwnershipRequest/DecodeOwnershipRequest round-trips netId and newOwner")
{
	const std::vector<std::byte> bytes = net::EncodeOwnershipRequest(42, 7);

	net::ByteReader r{bytes};
	CHECK(KindOf(bytes) == net::NetMessage::OwnershipRequest);
	r.U8(); // consume the leading kind byte DecodeOwnershipRequest does not re-read

	const auto msg = net::DecodeOwnershipRequest(r);
	REQUIRE(msg.has_value());
	CHECK(msg->netId == 42);
	CHECK(msg->newOwner == 7);
}

TEST_CASE("EncodeOwnershipTransfer/DecodeOwnershipTransfer round-trips netId and newOwner")
{
	const std::vector<std::byte> bytes = net::EncodeOwnershipTransfer(99, net::kInvalidConnection);

	net::ByteReader r{bytes};
	CHECK(KindOf(bytes) == net::NetMessage::OwnershipTransfer);
	r.U8();

	const auto msg = net::DecodeOwnershipTransfer(r);
	REQUIRE(msg.has_value());
	CHECK(msg->netId == 99);
	CHECK(msg->newOwner == net::kInvalidConnection);
}

TEST_CASE("A truncated ownership packet decodes to nothing rather than to garbage")
{
	SUBCASE("request")
	{
		net::ByteWriter w;
		w.U32(1); // netId only; newOwner is missing
		net::ByteReader r{w.View()};
		CHECK_FALSE(net::DecodeOwnershipRequest(r).has_value());
	}
	SUBCASE("transfer")
	{
		net::ByteWriter w;
		w.U32(1);
		net::ByteReader r{w.View()};
		CHECK_FALSE(net::DecodeOwnershipTransfer(r).has_value());
	}
	SUBCASE("a net id of 0 is never valid, even with a well-formed newOwner")
	{
		net::ByteWriter w;
		w.U32(0);
		w.U32(7);
		net::ByteReader r{w.View()};
		CHECK_FALSE(net::DecodeOwnershipRequest(r).has_value());
		net::ByteReader r2{w.View()};
		CHECK_FALSE(net::DecodeOwnershipTransfer(r2).has_value());
	}
}

TEST_CASE("ValidateOwnershipRequest: claiming a free or already-own entity is allowed")
{
	constexpr net::ConnectionId kSender = 5;
	CHECK(net::ValidateOwnershipRequest(net::kInvalidConnection, kSender, kSender) == net::OwnershipTransferRefusal::None);
	// Idempotent - a physics gun re-asserting its grip every frame must not be refused.
	CHECK(net::ValidateOwnershipRequest(kSender, kSender, kSender) == net::OwnershipTransferRefusal::None);
}

TEST_CASE("ValidateOwnershipRequest: claiming another connected peer's entity is refused")
{
	constexpr net::ConnectionId kSender = 5;
	constexpr net::ConnectionId kOtherOwner = 7;
	CHECK(net::ValidateOwnershipRequest(kOtherOwner, kSender, kSender) == net::OwnershipTransferRefusal::NotFreeOrOwn);
}

TEST_CASE("ValidateOwnershipRequest: releasing is allowed only for the entity's current owner")
{
	constexpr net::ConnectionId kSender = 5;
	constexpr net::ConnectionId kOtherOwner = 7;
	CHECK(net::ValidateOwnershipRequest(kSender, kSender, net::kInvalidConnection) == net::OwnershipTransferRefusal::None);
	CHECK(net::ValidateOwnershipRequest(kOtherOwner, kSender, net::kInvalidConnection)
	      == net::OwnershipTransferRefusal::NotCurrentOwner);
}

TEST_CASE("ValidateOwnershipRequest: naming a third connection as newOwner is refused")
{
	// The anti-hijack rule: a client may name only itself or the host, never
	// reassign an entity - owned by anybody, or by nobody - to some OTHER
	// connection it does not speak for.
	constexpr net::ConnectionId kSender = 5;
	constexpr net::ConnectionId kThirdParty = 9;
	CHECK(net::ValidateOwnershipRequest(net::kInvalidConnection, kSender, kThirdParty)
	      == net::OwnershipTransferRefusal::TargetNotSelfOrHost);
	CHECK(net::ValidateOwnershipRequest(kSender, kSender, kThirdParty) == net::OwnershipTransferRefusal::TargetNotSelfOrHost);
}

TEST_CASE("A client's claim and release round-trip over real loopback and land on both peers")
{
	Node host;
	Node client;

	REQUIRE(host.context.StartHost(host.world, 24795, 4));
	REQUIRE(client.context.StartClient(client.world, "127.0.0.1", 24795));
	REQUIRE(PumpUntil({&host, &client}, [&] { return client.context.IsConnected(); }));

	net::ConnectionId peer = net::kInvalidConnection;
	for (const net::ConnectionId conn: host.context.Session().Connections())
	{
		peer = conn;
	}
	REQUIRE(peer != net::kInvalidConnection);

	constexpr std::uint32_t kNetId = 777;
	const Entity hostEntity = Bind(host, kNetId, net::kInvalidConnection);
	const Entity clientEntity = Bind(client, kNetId, net::kInvalidConnection);

	// Claim: currently free (host-owned), so the host grants it and the broadcast
	// answers back to the very client that asked.
	REQUIRE(client.context.RequestOwnershipTransfer(client.world, clientEntity, client.context.LocalConnectionId())
	        == net::OwnershipTransferOutcome::Requested);
	REQUIRE(PumpUntil({&host, &client},
	        [&]
	        {
		        return host.world.TryGet<net::NetworkIdentity>(hostEntity)->owner == peer
		               && client.world.TryGet<net::NetworkIdentity>(clientEntity)->owner == peer;
	        }));

	// Release: only the entity's actual current owner (the client, now) may hand
	// it back to the host.
	REQUIRE(client.context.RequestOwnershipTransfer(client.world, clientEntity, net::kInvalidConnection)
	        == net::OwnershipTransferOutcome::Requested);
	REQUIRE(PumpUntil({&host, &client},
	        [&]
	        {
		        return host.world.TryGet<net::NetworkIdentity>(hostEntity)->owner == net::kInvalidConnection
		               && client.world.TryGet<net::NetworkIdentity>(clientEntity)->owner == net::kInvalidConnection;
	        }));
}

TEST_CASE("A host's own transfer needs no round trip and still reaches the client")
{
	// "Host always wins": the host's own call is never validated against
	// ValidateOwnershipRequest - it applies synchronously and only THEN goes out
	// over the wire, unlike a client's request which waits for the host's answer.
	Node host;
	Node client;

	REQUIRE(host.context.StartHost(host.world, 24796, 4));
	REQUIRE(client.context.StartClient(client.world, "127.0.0.1", 24796));
	REQUIRE(PumpUntil({&host, &client}, [&] { return client.context.IsConnected(); }));

	net::ConnectionId peer = net::kInvalidConnection;
	for (const net::ConnectionId conn: host.context.Session().Connections())
	{
		peer = conn;
	}
	REQUIRE(peer != net::kInvalidConnection);

	constexpr std::uint32_t kNetId = 778;
	const Entity hostEntity = Bind(host, kNetId, net::kInvalidConnection);
	const Entity clientEntity = Bind(client, kNetId, net::kInvalidConnection);

	REQUIRE(host.context.RequestOwnershipTransfer(host.world, hostEntity, peer) == net::OwnershipTransferOutcome::Applied);
	// Applied immediately, before a single packet has even been sent.
	CHECK(host.world.TryGet<net::NetworkIdentity>(hostEntity)->owner == peer);

	REQUIRE(PumpUntil({&host, &client},
	        [&] { return client.world.TryGet<net::NetworkIdentity>(clientEntity)->owner == peer; }));
}

TEST_CASE("A hijack attempt naming a third connection is refused over real loopback")
{
	// RequestOwnershipTransfer's own API cannot construct this request - it always
	// names the caller's own connection or the host - so the hostile packet is
	// crafted directly, exactly what a modified client could send.
	Node host;
	Node client;

	REQUIRE(host.context.StartHost(host.world, 24798, 4));
	REQUIRE(client.context.StartClient(client.world, "127.0.0.1", 24798));
	REQUIRE(PumpUntil({&host, &client}, [&] { return client.context.IsConnected(); }));

	net::ConnectionId peer = net::kInvalidConnection;
	for (const net::ConnectionId conn: host.context.Session().Connections())
	{
		peer = conn;
	}
	REQUIRE(peer != net::kInvalidConnection);

	constexpr std::uint32_t kNetId = 779;
	const Entity hostEntity = Bind(host, kNetId, net::kInvalidConnection);
	Bind(client, kNetId, net::kInvalidConnection);

	client.context.Transport().Send(net::kInvalidConnection, net::kChannelReliable, true,
	        net::EncodeOwnershipRequest(kNetId, peer + 1000));

	// Pumped generously for a fixed span rather than until a condition: a refusal
	// is proved by the thing that would have happened had it worked, not
	// happening in the time it would have taken.
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
	while (std::chrono::steady_clock::now() < deadline)
	{
		host.receive.Update(host.world, 0.f);
		client.receive.Update(client.world, 0.f);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}

	CHECK(host.world.TryGet<net::NetworkIdentity>(hostEntity)->owner == net::kInvalidConnection);
}
