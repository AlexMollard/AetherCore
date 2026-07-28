// A HUD that wants to show a ping needs two things the framework did not have: a
// reading of what a link costs, and a way for that reading to reach the peers that
// cannot measure it. Only the machine at one end of a link can time it, so a client's
// ping is unmeasurable from anywhere else and has to travel like any other piece of
// state that peer owns.
//
// These cases drive it over a REAL loopback connection with both ends' send and receive
// systems running, because what is under test is what crosses the wire. ENet's own
// round-trip estimate is what is being surfaced - no probe packet is sent by anything
// here - so the assertions are about WHOSE number ends up WHERE, not about a specific
// millisecond count, which is a property of the machine and not of the code.

#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <thread>
#include <utility>

#include "net/NetComponents.hpp"
#include "net/NetworkContext.hpp"
#include "net/NetworkSystems.hpp"
#include "scene/Components.hpp"
#include "scene/World.hpp"
#include "utils/ServiceContainer.hpp"

#include "NetTestSupport.hpp"

using namespace aether;

namespace
{
	// Loopback is not seconds away from itself. ENet seeds a fresh peer at 500 ms and
	// converges downward from there, so this is a ceiling on "a plausible reading",
	// not a claim about the link's real speed.
	constexpr std::uint32_t kSaneCeilingMs = 2000;

	struct PingNode
	{
		ServiceContainer services;
		World world;
		aether::net::NetworkContext context{services};
		aether::net::NetworkReceiveSystem receive{context};
		aether::net::NetworkSendSystem send{context};

		~PingNode()
		{
			context.Stop(world);
		}

		PingNode() = default;
		PingNode(const PingNode&) = delete;
		PingNode& operator=(const PingNode&) = delete;
	};

	// Both ends fully pumped - receive AND send on each. Unlike the readiness fixture
	// this needs the CLIENT's send system too: the client is the peer that owns its own
	// player, so it is the only one that can stamp and upload that player's ping.
	void Pump(PingNode& host, PingNode& client)
	{
		host.send.Update(host.world, 0.f);
		client.send.Update(client.world, 0.f);
		host.receive.Update(host.world, 0.f);
		client.receive.Update(client.world, 0.f);
	}

	bool PumpUntil(PingNode& host, PingNode& client, auto done, int maxMs = 3000)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(maxMs);
		while (std::chrono::steady_clock::now() < deadline)
		{
			Pump(host, client);
			if (done())
			{
				return true;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		return false;
	}

	// A host with one connected client, both running both systems. No prefab on disk:
	// the entity under test is built by hand at both ends and bound to the same net id,
	// which is all replication needs and is what MakeSessionSpawned already does.
	struct PingPair
	{
		PingNode host;
		PingNode client;
		aether::net::ConnectionId peer = aether::net::kInvalidConnection;

		explicit PingPair(std::uint16_t port)
		{
			REQUIRE(host.context.StartHost(host.world, port, 4));
			host.context.SetSendRateHz(1'000'000.f); // pacing is not what any of this tests
			host.context.Relevancy().radius = 10'000.f;
			REQUIRE(client.context.StartClient(client.world, "127.0.0.1", port));
			client.context.SetSendRateHz(1'000'000.f);
			REQUIRE(PumpUntil(host, client, [&] { return client.context.IsConnected(); }));

			for (const aether::net::ConnectionId conn: host.context.Session().Connections())
			{
				peer = conn;
			}
			REQUIRE(peer != aether::net::kInvalidConnection);
		}
	};

	// The same replicated entity at both ends, owned by `owner`, carrying a NetPlayer.
	// A snapshot only ever WRITES a component that already exists on the receiver, so
	// both copies have to carry one - which is what authoring `net_player` on a player
	// prefab does in a real game.
	std::pair<Entity, Entity> MakeMirroredPlayer(PingPair& p, aether::net::ConnectionId owner)
	{
		const Entity onHost = aether::net::test::MakeSessionSpawned(p.host.world, p.host.context, owner);
		const std::uint32_t netId = p.host.world.TryGet<aether::net::NetworkIdentity>(onHost)->netId;
		p.host.world.Emplace<aether::net::NetPlayer>(onHost);

		const Entity onClient = p.client.world.Create();
		p.client.world.Emplace<TransformComponent>(onClient);
		aether::net::NetworkIdentity identity{};
		identity.netId = netId;
		identity.owner = owner;
		identity.scenePlaced = false;
		p.client.world.EmplaceOrReplace<aether::net::NetworkIdentity>(onClient, identity);
		p.client.world.Emplace<aether::net::NetPlayer>(onClient);
		p.client.context.Session().Bind(netId, onClient);
		return {onHost, onClient};
	}

	std::uint32_t PingOf(World& world, Entity entity)
	{
		const auto* player = world.TryGet<aether::net::NetPlayer>(entity);
		return player != nullptr ? player->pingMs : 0u;
	}
} // namespace

TEST_CASE("An unnetworked peer reports no round-trip time at all")
{
	ServiceContainer services;
	aether::net::NetworkContext context{services};

	// Not an error state and not a missing value: with no link there is no latency, and
	// a HUD asking this offline must be given a number it can print.
	CHECK(context.LocalRoundTripMs() == 0);
	CHECK(context.RoundTripMs(aether::net::kInvalidConnection) == 0);
	CHECK(context.RoundTripMs(0) == 0);
}

TEST_CASE("A host has no round trip to itself, but does to each connection")
{
	PingPair p(24790);

	// The host is not one of its own peers. Reporting anything else here would put a
	// latency figure on the one player who has none.
	CHECK(p.host.context.LocalRoundTripMs() == 0);

	const std::uint32_t toClient = p.host.context.RoundTripMs(p.peer);
	CHECK(toClient > 0);
	CHECK(toClient < kSaneCeilingMs);

	// A connection id nobody is using is not a link, and must not be answered with
	// whatever the last live peer happened to read.
	CHECK(p.host.context.RoundTripMs(p.peer + 100) == 0);
}

TEST_CASE("A connected client measures its own link to the host")
{
	PingPair p(24791);

	const std::uint32_t ping = p.client.context.LocalRoundTripMs();
	CHECK(ping > 0);
	CHECK(ping < kSaneCeilingMs);

	// A client addresses its single link the way the transport does everywhere else.
	CHECK(p.client.context.RoundTripMs(aether::net::kInvalidConnection) == ping);
}

TEST_CASE("A client's ping is stamped on the player it owns and replicated to the host")
{
	PingPair p(24792);

	// Owned by the client, so the CLIENT is the peer that both measures and uploads it -
	// the host cannot measure a link from the far end and must not try.
	const auto [onHost, onClient] = MakeMirroredPlayer(p, p.client.context.LocalConnectionId());

	REQUIRE(PumpUntil(p.host, p.client, [&] { return PingOf(p.host.world, onHost) > 0; }));

	const std::uint32_t stamped = PingOf(p.client.world, onClient);
	CHECK(stamped > 0);
	CHECK(stamped < kSaneCeilingMs);
	// The host is holding the client's OWN reading, not one of its own making.
	CHECK(PingOf(p.host.world, onHost) == stamped);
}

TEST_CASE("The host's own player carries no ping, and the client is told that too")
{
	PingPair p(24793);

	const auto [onHost, onClient] = MakeMirroredPlayer(p, p.host.context.LocalConnectionId());

	// Give it every chance to be stamped with something. A host writing its own transport
	// reading here would put a phantom latency on the one peer that has none, and the
	// client would faithfully render it.
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
	while (std::chrono::steady_clock::now() < deadline)
	{
		Pump(p.host, p.client);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}

	CHECK(PingOf(p.host.world, onHost) == 0);
	CHECK(PingOf(p.client.world, onClient) == 0);
}

TEST_CASE("A peer does not stamp its own ping onto somebody else's player")
{
	PingPair p(24794);

	// A THIRD player, owned by a connection neither end of this pair is. The client has
	// a live link and a non-zero reading of its own, so a stamp that skipped the
	// ownership check would write this peer's latency onto a stranger's player - and
	// then upload it over the value that player's real owner is sending. That is the
	// display-name defect in a different disguise, and it fails silently.
	constexpr aether::net::ConnectionId stranger = 7;
	REQUIRE(stranger != p.host.context.LocalConnectionId());
	REQUIRE(stranger != p.client.context.LocalConnectionId());
	const auto [onHost, onClient] = MakeMirroredPlayer(p, stranger);
	REQUIRE(p.client.context.LocalRoundTripMs() > 0);

	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
	while (std::chrono::steady_clock::now() < deadline)
	{
		Pump(p.host, p.client);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}

	CHECK(PingOf(p.client.world, onClient) == 0);
	CHECK(PingOf(p.host.world, onHost) == 0);
}
