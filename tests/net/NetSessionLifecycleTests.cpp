// Session lifecycle: who is allowed in, who is allowed to name themselves, and how a
// link ENDS.
//
// The three subjects here share one root. Under client authority the owner of an
// entity is authoritative for it, so a display name - replicated state like any other
// - is the owner's to write and nobody else's; a peer that writes it on somebody
// else's player is writing a value that player's owner overwrites on its next send,
// and whether it survives is a race. That race is what "some players' names do not
// show" was.
//
// The ending half is the same rule from the other direction: a link that ends because
// somebody DECIDED it should (a refusal, a host quitting) is a different fact from one
// that ends because the network broke, and only the deliberate ones carry a reason. A
// game cannot write a sane reconnect policy without being able to tell them apart, so
// the distinction is pinned here rather than left to whatever ENet happens to report.

#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "net/NetComponents.hpp"
#include "net/NetSpawn.hpp"
#include "net/NetworkContext.hpp"
#include "net/NetworkSystems.hpp"
#include "scene/Components.hpp"
#include "scene/SceneSerializer.hpp"
#include "scene/World.hpp"
#include "utils/ServiceContainer.hpp"

#include "NetTestSupport.hpp"

using namespace aether;
using namespace aether::net::test;

namespace
{
	// Distinct from every other net test's ports, so a socket left over from another
	// case can never make one of these fail to bind.
	constexpr std::uint16_t kFullPort = 24781;
	constexpr std::uint16_t kNamePort = 24782;
	constexpr std::uint16_t kClosePort = 24783;
	constexpr std::uint16_t kDropPort = 24784;

	// A scratch prefab directory, so SpawnPrefab/ApplySpawn genuinely instantiate
	// something at both ends: a Spawn message is the only thing that carries ownership
	// across the wire, and ownership is the whole subject.
	struct PrefabFixture
	{
		std::filesystem::path dir;

		PrefabFixture()
		      : dir(std::filesystem::temp_directory_path() / "aether_net_session_lifecycle_test")
		{
			std::filesystem::remove_all(dir);
			std::filesystem::create_directories(dir);
			aether::app::scene::SetProjectSceneDirectories(dir / "scenes", dir);

			aether::app::scene::SceneDescription prefab;
			prefab.name = "lifecycle_test_prefab";
			aether::app::scene::EntityRecord record;
			record.entityId = 1;
			record.name = "Body";
			record.hasTransform = true;
			prefab.entities.push_back(std::move(record));
			REQUIRE(aether::app::scene::SavePrefabFile("lifecycle_test_prefab", prefab));
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

	constexpr const char* kPrefab = "lifecycle_test_prefab";

	// One peer: its own world, context and both systems, in the order Application
	// registers them.
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

		// A player prefab carries a NetPlayer; the scratch prefab above does not, and
		// ApplySnapshot writes into a component that already exists rather than creating
		// one. Emplacing it at both ends is what the real prefab does for us.
		void GiveNetPlayer(std::uint32_t netId, const std::string& name = {})
		{
			const Entity entity = context.Session().EntityFor(netId);
			REQUIRE(entity.IsValid());
			world.EmplaceOrReplace<aether::net::NetPlayer>(entity, aether::net::NetPlayer{.displayName = name});
		}

		[[nodiscard]] std::string NameOfNetId(std::uint32_t netId)
		{
			const Entity entity = context.Session().EntityFor(netId);
			if (!entity.IsValid())
			{
				return "<no entity>";
			}
			const auto* player = world.TryGet<aether::net::NetPlayer>(entity);
			return player != nullptr ? player->displayName : std::string{"<no component>"};
		}
	};

	// Runs one whole frame on every node - receive then send - until `done()` or the
	// deadline. ENet needs several service calls to complete a handshake or land a
	// packet, so a single pass is never enough.
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

	// An entity that looks like a spawned player: a live net id owned by `owner`, and a
	// NetPlayer already carrying `name`.
	Entity MakeNamedPlayer(World& world, aether::net::NetworkContext& context, aether::net::ConnectionId owner,
	        const std::string& name)
	{
		const Entity entity = MakeSessionSpawned(world, context, owner);
		world.EmplaceOrReplace<aether::net::NetPlayer>(entity, aether::net::NetPlayer{.displayName = name});
		return entity;
	}
} // namespace

// ── Names: resolution ───────────────────────────────────────────────────────────

TEST_CASE("A name nobody ahead of this player is using is adopted unchanged")
{
	ServiceContainer services;
	World world;
	aether::net::NetworkContext context(services);
	REQUIRE(context.StartHost(world, kNamePort, 4));

	const Entity host = MakeNamedPlayer(world, context, aether::net::kInvalidConnection, "Alice");
	const Entity other = MakeSessionSpawned(world, context, 2);

	CHECK(context.ResolveDisplayName(world, other, "Bob") == "Bob");
	CHECK(context.ResolveDisplayName(world, host, "Alice") == "Alice");

	context.Stop(world);
}

TEST_CASE("Two players choosing the same name stay distinguishable")
{
	// The suffix is what keeps a transcript readable: "Alice: hi" from two different
	// Alices is indistinguishable, and a chat log is the whole point of this testbed.
	ServiceContainer services;
	World world;
	aether::net::NetworkContext context(services);
	REQUIRE(context.StartHost(world, kNamePort, 4));

	MakeNamedPlayer(world, context, aether::net::kInvalidConnection, "Alice"); // the host
	const Entity second = MakeSessionSpawned(world, context, 1);

	CHECK(context.ResolveDisplayName(world, second, "Alice") == "Alice (2)");

	// A third Alice keeps counting rather than colliding with the second.
	world.EmplaceOrReplace<aether::net::NetPlayer>(second, aether::net::NetPlayer{.displayName = "Alice (2)"});
	const Entity third = MakeSessionSpawned(world, context, 2);
	CHECK(context.ResolveDisplayName(world, third, "Alice") == "Alice (3)");

	context.Stop(world);
}

TEST_CASE("A player never steps around one that joined after it")
{
	// The asymmetry is the convergence argument, not a detail. If a collision made
	// BOTH players move, two peers picking the same name in the same frame would both
	// become "Alice (2)" and collide again on the next frame, forever. Only the later
	// one moves, so the sequence settles.
	ServiceContainer services;
	World world;
	aether::net::NetworkContext context(services);
	REQUIRE(context.StartHost(world, kNamePort, 4));

	const Entity host = MakeSessionSpawned(world, context, aether::net::kInvalidConnection);
	MakeNamedPlayer(world, context, 3, "Alice"); // a client that got there first in wall-clock terms

	// The host is connection 0, so it is ahead of every client whatever the timing.
	CHECK(context.ResolveDisplayName(world, host, "Alice") == "Alice");

	context.Stop(world);
}

TEST_CASE("A blank or unprintable name resolves to something showable")
{
	ServiceContainer services;
	World world;
	aether::net::NetworkContext context(services);
	REQUIRE(context.StartHost(world, kNamePort, 4));

	const Entity player = MakeSessionSpawned(world, context, aether::net::kInvalidConnection);

	CHECK(context.ResolveDisplayName(world, player, "") == "Player");
	CHECK(context.ResolveDisplayName(world, player, "   ") == "Player");
	// The font pipeline bakes ASCII only, so anything else is dropped rather than
	// rendered as a box - and a name that was ONLY unprintable falls back like a blank.
	CHECK(context.ResolveDisplayName(world, player, "\x01\x02") == "Player");
	CHECK(context.ResolveDisplayName(world, player, "  Bob  ") == "Bob");

	context.Stop(world);
}

// ── Names: authority ────────────────────────────────────────────────────────────

TEST_CASE("Only the owner may claim a player's name")
{
	// The architectural half of the "some names do not show" bug. Under client
	// authority a name written by a peer that does not own the entity is overwritten by
	// the owner's next send, so it is refused outright rather than allowed to race.
	ServiceContainer services;
	World world;
	aether::net::NetworkContext context(services);
	REQUIRE(context.StartHost(world, kNamePort, 4));

	const Entity mine = MakeNamedPlayer(world, context, aether::net::kInvalidConnection, "");
	const Entity theirs = MakeNamedPlayer(world, context, 7, "");

	CHECK(context.ClaimPlayerName(world, mine, "Alice") == "Alice");
	CHECK(world.TryGet<aether::net::NetPlayer>(mine)->displayName == "Alice");

	// Refused, and - the part that matters - nothing was written.
	CHECK(context.ClaimPlayerName(world, theirs, "Bob").empty());
	CHECK(world.TryGet<aether::net::NetPlayer>(theirs)->displayName.empty());

	context.Stop(world);
}

TEST_CASE("Claiming a name adds the component when the entity has none")
{
	ServiceContainer services;
	World world;
	aether::net::NetworkContext context(services);

	// Offline: every entity is this peer's, which is what makes single-player code
	// need no role test.
	const Entity player = world.Create();
	REQUIRE(world.TryGet<aether::net::NetPlayer>(player) == nullptr);

	CHECK(context.ClaimPlayerName(world, player, "Solo") == "Solo");
	REQUIRE(world.TryGet<aether::net::NetPlayer>(player) != nullptr);
	CHECK(world.TryGet<aether::net::NetPlayer>(player)->displayName == "Solo");
}

// ── Names: over the wire ────────────────────────────────────────────────────────

TEST_CASE("The owner's own name reaches the host and is not argued with")
{
	// The reported bug, end to end. The name is authored on the machine that owns the
	// player and travels outward like any other replicated field; the host reads it and
	// never writes it back.
	PrefabFixture prefabs;
	Node host;
	Node client;

	REQUIRE(host.context.StartHost(host.world, kNamePort, 4));
	host.context.SetSendRateHz(1'000'000.f);
	host.context.Relevancy().radius = 1e6f;
	REQUIRE(client.context.StartClient(client.world, "127.0.0.1", kNamePort));
	client.context.SetSendRateHz(1'000'000.f);

	const std::vector<Node*> all{&host, &client};
	REQUIRE(Run(all, [&] { return client.context.IsConnected(); }));
	const std::vector<aether::net::ConnectionId> connections = host.context.Session().Connections();
	REQUIRE(connections.size() == 1);

	// A player owned by the client, spawned by the host as every replicated entity is.
	const Entity spawned = host.context.SpawnPrefab(host.world, kPrefab, {0.f, 0.f, 0.f}, connections.front());
	REQUIRE(spawned.IsValid());
	const std::uint32_t netId = host.world.TryGet<aether::net::NetworkIdentity>(spawned)->netId;
	REQUIRE(Run(all, [&] { return client.context.Session().EntityFor(netId).IsValid(); }));

	host.GiveNetPlayer(netId);
	client.GiveNetPlayer(netId);

	// The OWNER names itself. Nothing on the host writes this field.
	const Entity ownedHere = client.context.Session().EntityFor(netId);
	CHECK(client.context.ClaimPlayerName(client.world, ownedHere, "Bob") == "Bob");

	REQUIRE(Run(all, [&] { return host.NameOfNetId(netId) == "Bob"; }));

	// And it stays. The host relays a client's state to everyone EXCEPT that client,
	// so there is no return trip that could put the empty prefab default back.
	Run(all, kNever, 300);
	CHECK(host.NameOfNetId(netId) == "Bob");
	CHECK(client.NameOfNetId(netId) == "Bob");
}

// ── Endings: refusal ────────────────────────────────────────────────────────────

TEST_CASE("A connection past the player cap is refused with a reason, not dropped")
{
	// "Server full" has to be something the joiner can SAY. A socket-level refusal is
	// indistinguishable from a crashed host, a firewall or a typo'd address, and a
	// player who cannot tell those apart retries the wrong one.
	Node host;
	Node first;
	Node second;

	REQUIRE(host.context.StartHost(host.world, kFullPort, 1));
	CHECK(host.context.MaxConnections() == 1);
	host.context.SetSendRateHz(1'000'000.f);

	REQUIRE(first.context.StartClient(first.world, "127.0.0.1", kFullPort));
	const std::vector<Node*> withFirst{&host, &first};
	REQUIRE(Run(withFirst, [&] { return first.context.IsConnected(); }));
	CHECK(host.context.IsFull());

	REQUIRE(second.context.StartClient(second.world, "127.0.0.1", kFullPort));
	const std::vector<Node*> all{&host, &first, &second};
	REQUIRE(Run(all, [&] { return !second.context.DisconnectReason().empty(); }));

	CHECK(second.context.DisconnectReason() == std::string(aether::net::kReasonServerFull));
	CHECK_FALSE(second.context.IsConnected());
	CHECK_FALSE(second.context.IsActive());

	// The refusal is not a disconnect for anybody else: the one legitimate client is
	// still in the session, and the refused peer was never admitted to it.
	CHECK(host.context.Session().Connections().size() == 1);
	CHECK(first.context.IsConnected());
}

// ── Endings: deliberate versus accidental ───────────────────────────────────────

TEST_CASE("A host closing the session tells its clients so, deliberately")
{
	Node host;
	Node client;

	REQUIRE(host.context.StartHost(host.world, kClosePort, 4));
	host.context.SetSendRateHz(1'000'000.f);
	REQUIRE(client.context.StartClient(client.world, "127.0.0.1", kClosePort));

	const std::vector<Node*> all{&host, &client};
	REQUIRE(Run(all, [&] { return client.context.IsConnected(); }));

	host.context.Stop(host.world);

	// Only the client is left to pump; the host's transport is gone.
	const std::vector<Node*> justClient{&client};
	REQUIRE(Run(justClient, [&] { return !client.context.DisconnectReason().empty(); }));

	CHECK(client.context.DisconnectReason() == std::string(aether::net::kReasonHostClosed));
	CHECK_FALSE(client.context.IsActive());
}

TEST_CASE("A link that dies without a word leaves no reason behind")
{
	// The other half of the pair, and the one a reconnect policy hangs on. The
	// transport is dropped underneath the framework - no goodbye, exactly as a crash or
	// a pulled cable looks - and the client must end up disconnected with NOTHING to
	// show for it, because "no reason" is precisely what makes a retry the right
	// response.
	Node host;
	Node client;

	REQUIRE(host.context.StartHost(host.world, kDropPort, 4));
	host.context.SetSendRateHz(1'000'000.f);
	REQUIRE(client.context.StartClient(client.world, "127.0.0.1", kDropPort));

	const std::vector<Node*> all{&host, &client};
	REQUIRE(Run(all, [&] { return client.context.IsConnected(); }));

	// Straight at the transport, bypassing NetworkContext::Stop and therefore its
	// goodbye broadcast: the host is simply gone.
	host.context.Transport().Disconnect();

	// Then the event ENet eventually reports for a peer that stopped answering. It is
	// delivered through the handler rather than by waiting for the real timeout,
	// because how long ENet takes to give up is ENet's business and sitting on the
	// suite for five seconds to re-measure it buys nothing - what is under test is what
	// the framework does with the event, and OnDisconnected is a documented seam for
	// exactly this.
	client.receive.OnDisconnected(client.world, aether::net::kInvalidConnection);

	CHECK_FALSE(client.context.IsActive());
	CHECK(client.context.DisconnectReason().empty());
}

TEST_CASE("Starting a new session clears the reason the last one ended with")
{
	// Or the next Title screen apologises for a session the player already left, and -
	// worse - a reconnect policy reading a stale reason would refuse to retry a drop it
	// should have retried.
	Node client;
	client.context.SetDisconnectReason(std::string(aether::net::kReasonServerFull));
	REQUIRE_FALSE(client.context.DisconnectReason().empty());

	// A connect ATTEMPT is enough: it does not have to succeed for the previous
	// session's verdict to stop applying.
	REQUIRE(client.context.StartClient(client.world, "127.0.0.1", kDropPort));
	CHECK(client.context.DisconnectReason().empty());
	client.context.Stop(client.world);

	client.context.SetDisconnectReason(std::string(aether::net::kReasonHostClosed));
	REQUIRE(client.context.StartHost(client.world, kDropPort, 4));
	CHECK(client.context.DisconnectReason().empty());
	client.context.Stop(client.world);
}
