#include <doctest/doctest.h>

#include <chrono>
#include <optional>
#include <span>
#include <thread>

#include "net/NetComponents.hpp"
#include "net/NetSession.hpp"
#include "net/NetSnapshot.hpp"
#include "net/NetSpawn.hpp"
#include "net/NetworkSubsystem.hpp"
#include "net/ReplicationSchema.hpp"
#include "scene/Components.hpp"
#include "scene/TransformUtils.hpp"
#include "scene/World.hpp"
#include "scene/reflection/Reflection.hpp"

using namespace aether;

// NOTE: TransformComponent stores only `localToWorld` (glm::mat4); "position" is a
// reflected view onto column 3 composed/decomposed by ComposeTransform/DecomposeTRS
// (see tests/net/NetSnapshotTests.cpp), not a direct struct member.

namespace
{
	// Pump both ends until `done` or the deadline. ENet needs several service calls
	// to complete a handshake, so a single Poll() is never enough.
	bool PumpUntil(net::NetworkSubsystem& a, net::NetworkSubsystem& b, auto done, int maxMs = 2000)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(maxMs);
		while (std::chrono::steady_clock::now() < deadline)
		{
			a.Poll();
			b.Poll();
			if (done())
			{
				return true;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		return false;
	}

	// Mirrors NetworkContext::Frame (src/app/net/NetworkContext.cpp), which this test
	// deliberately avoids depending on - constructing a NetworkContext pulls in the
	// CoreCLR-backed script field/RPC bridges, which EngineTests keeps out. Snapshot
	// and script-field payloads carry no NetMessage byte of their own (Spawn/Despawn
	// already do, from their own encoders), so the sender has to add it.
	std::vector<std::byte> Frame(net::NetMessage kind, std::span<const std::byte> payload)
	{
		net::ByteWriter w;
		w.U8(static_cast<std::uint8_t>(kind));
		w.Bytes(payload);
		return w.Take();
	}

	// True once `events` contains a Data message of NetMessage `kind` on `channel`, in
	// which case `outPayload` holds everything after the leading NetMessage byte -
	// exactly what NetworkReceiveSystem::OnData hands its decoders.
	bool FindMessage(std::span<const net::NetEvent> events, int channel, net::NetMessage kind,
	        std::vector<std::byte>& outPayload)
	{
		for (const net::NetEvent& e: events)
		{
			if (e.kind != net::NetEvent::Kind::Data || e.channel != channel || e.data.empty())
			{
				continue;
			}
			if (static_cast<net::NetMessage>(static_cast<std::uint8_t>(e.data[0])) != kind)
			{
				continue;
			}
			outPayload.assign(e.data.begin() + 1, e.data.end());
			return true;
		}
		return false;
	}
} // namespace

TEST_CASE("A client connects to a host over loopback and exchanges a payload")
{
	net::NetworkSubsystem host;
	net::NetworkSubsystem client;

	REQUIRE(host.Host(24681, 4));
	CHECK(host.Role() == net::NetRole::Host);

	REQUIRE(client.Connect("127.0.0.1", 24681));

	net::ConnectionId hostSawPeer = 0;
	const bool connected = PumpUntil(host, client,
	        [&]
	        {
		        for (const net::NetEvent& e: host.Events())
		        {
			        if (e.kind == net::NetEvent::Kind::Connected)
			        {
				        hostSawPeer = e.peer;
			        }
		        }
		        return hostSawPeer != 0;
	        });
	REQUIRE(connected);

	const std::string payload = "hello";
	host.Send(hostSawPeer, net::kChannelReliable, true,
	        std::as_bytes(std::span<const char>{payload.data(), payload.size()}));

	std::string received;
	const bool gotData = PumpUntil(host, client,
	        [&]
	        {
		        for (const net::NetEvent& e: client.Events())
		        {
			        if (e.kind == net::NetEvent::Kind::Data)
			        {
				        received.assign(reinterpret_cast<const char*>(e.data.data()), e.data.size());
			        }
		        }
		        return !received.empty();
	        });
	REQUIRE(gotData);
	CHECK(received == "hello");

	client.Disconnect();
	host.Disconnect();
	CHECK(host.Role() == net::NetRole::Offline);
}

TEST_CASE("Two subsystems can coexist and outlive each other")
{
	// This is the scenario the shared ENet refcount (EnetInit.hpp) exists for: an
	// editor ControlServer and a game NetworkSubsystem sharing one process, where
	// either can tear down while the other is still active. It will NOT fail against
	// a broken (non-shared, or unrefcounted) implementation on Windows, because the
	// underlying WSAStartup/timeBeginPeriod primitives tolerate redundant init/deinit
	// - so passing here is not proof the refcount is genuine. It pins the intended
	// usage and would catch a regression on a platform where double-deinit is fatal.
	net::NetworkSubsystem host;
	net::NetworkSubsystem client;

	REQUIRE(host.Host(24682, 4));
	REQUIRE(client.Connect("127.0.0.1", 24682));

	net::ConnectionId hostSawPeer = 0;
	const bool connected = PumpUntil(host, client,
	        [&]
	        {
		        for (const net::NetEvent& e: host.Events())
		        {
			        if (e.kind == net::NetEvent::Kind::Connected)
			        {
				        hostSawPeer = e.peer;
			        }
		        }
		        return hostSawPeer != 0;
	        });
	REQUIRE(connected);

	// The client tears down and releases its ENet reference while the host is
	// still active - the host must keep working afterwards.
	client.Disconnect();

	const std::string payload = "still alive";
	host.Send(hostSawPeer, net::kChannelReliable, true,
	        std::as_bytes(std::span<const char>{payload.data(), payload.size()}));
	host.Poll();
	CHECK(host.Role() == net::NetRole::Host);

	host.Disconnect();
	CHECK(host.Role() == net::NetRole::Offline);
}

TEST_CASE("A send on an invalid channel is a no-op")
{
	net::NetworkSubsystem host;
	net::NetworkSubsystem client;

	REQUIRE(host.Host(24683, 4));
	REQUIRE(client.Connect("127.0.0.1", 24683));

	net::ConnectionId hostSawPeer = 0;
	const bool connected = PumpUntil(host, client,
	        [&]
	        {
		        for (const net::NetEvent& e: host.Events())
		        {
			        if (e.kind == net::NetEvent::Kind::Connected)
			        {
				        hostSawPeer = e.peer;
			        }
		        }
		        return hostSawPeer != 0;
	        });
	REQUIRE(connected);

	const std::string payload = "should not arrive";
	host.Send(hostSawPeer, -1, true, std::as_bytes(std::span<const char>{payload.data(), payload.size()}));
	host.Send(hostSawPeer, net::kChannelCount, true, std::as_bytes(std::span<const char>{payload.data(), payload.size()}));

	bool gotData = false;
	PumpUntil(host, client,
	        [&]
	        {
		        for (const net::NetEvent& e: client.Events())
		        {
			        if (e.kind == net::NetEvent::Kind::Data)
			        {
				        gotData = true;
			        }
		        }
		        return false;
	        },
	        200);
	CHECK_FALSE(gotData);

	client.Disconnect();
	host.Disconnect();
}

// This is the one test in the suite that drives the whole replication stack over a
// real socket: NetworkSubsystem, NetSession, ReplicationSchema, BuildSnapshot /
// ApplySnapshot, and Encode/DecodeSpawn / Encode/DecodeDespawn, composed exactly as
// NetworkReceiveSystem/NetworkSendSystem drive them (see src/app/net/NetworkSystems.cpp)
// - minus the two System classes themselves and NetworkContext, which cannot be
// instantiated here without pulling CoreCLR into EngineTests. Every other net/* test
// exercises one of these layers with a fake or no transport at all; this one is the
// only place two of them talk over 127.0.0.1.
TEST_CASE("Replication composes end to end over a loopback connection")
{
	net::NetworkSubsystem host;
	net::NetworkSubsystem client;

	REQUIRE(host.Host(24684, 4));
	REQUIRE(client.Connect("127.0.0.1", 24684));

	// ── 1. A client connects and the host records the connection ──────────────
	net::ConnectionId hostSawPeer = 0;
	const bool connected = PumpUntil(host, client,
	        [&]
	        {
		        for (const net::NetEvent& e: host.Events())
		        {
			        if (e.kind == net::NetEvent::Kind::Connected)
			        {
				        hostSawPeer = e.peer;
			        }
		        }
		        return hostSawPeer != 0;
	        });
	REQUIRE(connected);

	net::NetSession hostSession;
	net::NetSession clientSession;
	hostSession.SetRole(net::NetRole::Host);
	clientSession.SetRole(net::NetRole::Client);

	// Mirrors NetworkReceiveSystem::OnConnected's host-side bookkeeping.
	hostSession.AddConnection(hostSawPeer);
	REQUIRE(hostSession.Connections().size() == 1);
	CHECK(hostSession.Connections().front() == hostSawPeer);

	World hostWorld;
	World clientWorld;
	const std::vector<reflect::ComponentType>& catalog = reflect::ComponentTypes();
	const net::ReplicationSchema schema = net::BuildReplicationSchema(catalog);
	net::SnapshotCache cache;

	// ── 2. The host spawns a networked entity; the client binds the same net id ─
	const std::uint32_t netId = hostSession.AllocateNetId();
	const Entity hostEntity = hostWorld.Create();
	hostWorld.Emplace<TransformComponent>(hostEntity).localToWorld =
	        ComposeTransform({1.f, 2.f, 3.f}, {0.f, 0.f, 0.f}, {1.f, 1.f, 1.f});
	net::NetworkIdentity& hostIdentity = hostWorld.Emplace<net::NetworkIdentity>(hostEntity);
	hostIdentity.netId = netId;
	hostIdentity.owner = net::kInvalidConnection; // host-owned
	hostSession.Bind(netId, hostEntity);

	// Broadcast, reliable - exactly NetworkContext::SpawnPrefab's send.
	host.Broadcast(net::kChannelReliable, true,
	        net::EncodeSpawn(netId, hostIdentity.owner, "networked_entity", glm::vec3{1.f, 2.f, 3.f}));

	std::optional<net::SpawnMessage> receivedSpawn;
	const bool gotSpawn = PumpUntil(host, client,
	        [&]
	        {
		        std::vector<std::byte> payload;
		        if (!receivedSpawn.has_value()
		                && FindMessage(client.Events(), net::kChannelReliable, net::NetMessage::Spawn, payload))
		        {
			        net::ByteReader reader{payload};
			        receivedSpawn = net::DecodeSpawn(reader);
		        }
		        return receivedSpawn.has_value();
	        });
	REQUIRE(gotSpawn);
	REQUIRE(receivedSpawn.has_value());
	CHECK(receivedSpawn->netId == netId);
	CHECK(receivedSpawn->owner == net::kInvalidConnection);

	// The client-side equivalent of NetworkContext::ApplySpawn's bookkeeping, minus
	// the prefab-file instantiation (out of scope here - see NetSpawnTests.cpp and the
	// scene-authoring tests for that half).
	const Entity clientEntity = clientWorld.Create();
	clientWorld.Emplace<TransformComponent>(clientEntity);
	net::NetworkIdentity& clientIdentity = clientWorld.Emplace<net::NetworkIdentity>(clientEntity);
	clientIdentity.netId = receivedSpawn->netId;
	clientIdentity.owner = receivedSpawn->owner;
	clientSession.Bind(receivedSpawn->netId, clientEntity);

	CHECK(clientSession.NetIdFor(clientEntity) == netId);
	CHECK(clientSession.EntityFor(netId) == clientEntity);

	// ── 3. Moving the entity on the host changes the client's copy after a
	//        build/send/receive/apply round ─────────────────────────────────────
	hostWorld.TryGet<TransformComponent>(hostEntity)->localToWorld =
	        ComposeTransform({10.f, 20.f, 30.f}, {0.f, 0.f, 0.f}, {1.f, 1.f, 1.f});

	const std::vector<std::byte> moveSnapshot =
	        net::BuildSnapshot(hostWorld, schema, catalog, hostSession, cache, {hostEntity});
	REQUIRE_FALSE(moveSnapshot.empty());
	// Unreliable, on the snapshot channel - exactly NetworkSendSystem::Update's send.
	host.Broadcast(net::kChannelSnapshot, false, Frame(net::NetMessage::Snapshot, moveSnapshot));

	bool appliedMove = false;
	const bool gotMoveSnapshot = PumpUntil(host, client,
	        [&]
	        {
		        std::vector<std::byte> payload;
		        if (!appliedMove
		                && FindMessage(client.Events(), net::kChannelSnapshot, net::NetMessage::Snapshot, payload))
		        {
			        net::ApplySnapshot(clientWorld, schema, catalog, clientSession, payload);
			        appliedMove = true;
		        }
		        return appliedMove;
	        });
	REQUIRE(gotMoveSnapshot);

	const auto* movedTransform = clientWorld.TryGet<TransformComponent>(clientEntity);
	REQUIRE(movedTransform != nullptr);
	const glm::vec3 movedPos = glm::vec3(movedTransform->localToWorld[3]);
	CHECK(movedPos.x == doctest::Approx(10.f));
	CHECK(movedPos.y == doctest::Approx(20.f));
	CHECK(movedPos.z == doctest::Approx(30.f));

	// ── 4. An unchanged entity produces no snapshot at all, so nothing is sent ──
	const std::vector<std::byte> unchangedSnapshot =
	        net::BuildSnapshot(hostWorld, schema, catalog, hostSession, cache, {hostEntity});
	CHECK(unchangedSnapshot.empty());
	// BuildSnapshot returning empty is already covered without a socket in
	// NetSnapshotTests.cpp ("An unchanged field produces no snapshot at all"); what
	// only this test can prove is that the send system's `if (!snapshot.empty())`
	// guard actually keeps a wire quiet. Mirror that guard - do not broadcast the
	// empty packet - and then watch the wire for the same window a real packet
	// would have arrived in.
	std::size_t snapshotsSeenWhileUnchanged = 0;
	PumpUntil(
	        host, client,
	        [&]
	        {
		        for (const net::NetEvent& e: client.Events())
		        {
			        if (e.kind == net::NetEvent::Kind::Data && e.channel == net::kChannelSnapshot)
			        {
				        ++snapshotsSeenWhileUnchanged;
			        }
		        }
		        return false; // run out the whole window; nothing should ever arrive
	        },
	        250);
	CHECK(snapshotsSeenWhileUnchanged == 0);

	// ── 5. Despawning on the host removes the entity from the client ───────────
	hostSession.Unbind(netId);
	hostWorld.Destroy(hostEntity);
	host.Broadcast(net::kChannelReliable, true, net::EncodeDespawn(netId));

	std::optional<std::uint32_t> receivedDespawn;
	const bool gotDespawn = PumpUntil(host, client,
	        [&]
	        {
		        std::vector<std::byte> payload;
		        if (!receivedDespawn.has_value()
		                && FindMessage(client.Events(), net::kChannelReliable, net::NetMessage::Despawn, payload))
		        {
			        net::ByteReader reader{payload};
			        receivedDespawn = net::DecodeDespawn(reader);
		        }
		        return receivedDespawn.has_value();
	        });
	REQUIRE(gotDespawn);
	REQUIRE(receivedDespawn.has_value());
	CHECK(*receivedDespawn == netId);

	// The client-side equivalent of NetworkContext::ApplyDespawn.
	const Entity toDestroy = clientSession.EntityFor(*receivedDespawn);
	REQUIRE(toDestroy.IsValid());
	clientSession.Unbind(*receivedDespawn);
	clientWorld.Destroy(toDestroy);

	CHECK_FALSE(clientSession.EntityFor(netId).IsValid());
	CHECK(clientWorld.TryGet<TransformComponent>(toDestroy) == nullptr);

	// ── 6. Disconnecting the client is observed by the host ────────────────────
	client.Disconnect();

	net::ConnectionId hostSawDisconnect = 0;
	const bool disconnected = PumpUntil(host, client,
	        [&]
	        {
		        for (const net::NetEvent& e: host.Events())
		        {
			        if (e.kind == net::NetEvent::Kind::Disconnected)
			        {
				        hostSawDisconnect = e.peer;
			        }
		        }
		        return hostSawDisconnect != 0;
	        });
	REQUIRE(disconnected);
	CHECK(hostSawDisconnect == hostSawPeer);

	// Mirrors NetworkReceiveSystem::OnDisconnected's host-side bookkeeping.
	hostSession.RemoveConnection(hostSawDisconnect);
	CHECK(hostSession.Connections().empty());

	host.Disconnect();
	CHECK(host.Role() == net::NetRole::Offline);
}
