// NetworkContext: the object where the transport, the session and the replicated
// world meet. Everything below it (codec, snapshot, relevancy, RPC, script fields)
// is covered by its own tests against pure functions; this file covers the seams
// those tests cannot reach - session start/stop, the net-id lifecycle across a
// join/leave/re-join cycle, and the ownership rules.
//
// This TU became possible when NetworkContext stopped constructing the CoreCLR
// -backed script bridges itself (see NetworkContext::SetFieldBridge): the context
// now takes them from its wiring site, so EngineTests can link it and pass fakes.

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "io/FileSystem.hpp"
#include "io/FileUtil.hpp"
#include "net/NetComponents.hpp"
#include "net/NetOwnership.hpp"
#include "net/NetRpc.hpp"
#include "net/NetScriptFields.hpp"
#include "net/NetSpawn.hpp"
#include "net/NetworkContext.hpp"
#include "net/RoomCode.hpp"
#include "physics/PhysicsComponents.hpp"
#include "physics2d/Physics2DComponents.hpp"
#include "scene/Components.hpp"
#include "scene/SceneSerializer.hpp"
#include "scene/World.hpp"
#include "utils/ServiceContainer.hpp"

#include "NetTestSupport.hpp"

using namespace aether;
using namespace aether::net::test;

namespace
{
	// Distinct from every other net test's ports (NetLoopbackTests uses 2468x), so a
	// leftover socket from another case can never make one of these fail to bind.
	constexpr std::uint16_t kHostPort = 24701;
	constexpr std::uint16_t kUnreachablePort = 24702; // nothing listens here, deliberately
} // namespace

TEST_CASE("StartHost numbers the scene-placed entities and destroys none of them")
{
	// The load-bearing half of Stop()'s predicate. Stop runs at the TOP of StartHost,
	// and at that moment every scene-placed entity still reads scenePlaced == false
	// (only AssignScenePlacedNetIds, which runs AFTER, sets it). A predicate of
	// `!scenePlaced` alone would therefore delete the entire replicated scene the
	// instant hosting began. This case fails outright if anyone "simplifies" it that
	// way.
	ServiceContainer services;
	World world;
	aether::net::NetworkContext context(services);

	const Entity a = MakeScenePlaced(world, 10);
	const Entity b = MakeScenePlaced(world, 20);
	REQUIRE(CountIdentities(world) == 2);

	REQUIRE(context.StartHost(world, kHostPort, 4));

	CHECK(CountIdentities(world) == 2); // nothing was destroyed
	REQUIRE(world.TryGet<aether::net::NetworkIdentity>(a) != nullptr);
	REQUIRE(world.TryGet<aether::net::NetworkIdentity>(b) != nullptr);
	CHECK(world.TryGet<aether::net::NetworkIdentity>(a)->netId != 0);
	CHECK(world.TryGet<aether::net::NetworkIdentity>(b)->netId != 0);
	CHECK(world.TryGet<aether::net::NetworkIdentity>(a)->scenePlaced);
	CHECK(world.TryGet<aether::net::NetworkIdentity>(b)->scenePlaced);
	// Lower node id gets the lower net id - the determinism both ends depend on.
	CHECK(world.TryGet<aether::net::NetworkIdentity>(a)->netId
	      < world.TryGet<aether::net::NetworkIdentity>(b)->netId);

	context.Stop(world);
}

TEST_CASE("Stop destroys the session's spawned entities and keeps the scene's")
{
	ServiceContainer services;
	World world;
	aether::net::NetworkContext context(services);

	MakeScenePlaced(world, 1);
	MakeScenePlaced(world, 2);
	REQUIRE(context.StartHost(world, kHostPort, 4));

	const Entity spawned = MakeSessionSpawned(world, context, aether::net::kInvalidConnection);
	const std::uint32_t spawnedNetId = world.TryGet<aether::net::NetworkIdentity>(spawned)->netId;
	REQUIRE(CountIdentities(world) == 3);
	REQUIRE(context.Session().EntityFor(spawnedNetId).IsValid());

	context.Stop(world);

	CHECK(CountIdentities(world) == 2);                     // the two scene entities survive
	CHECK_FALSE(world.GetRegistry().valid(World::ToEntt(spawned))); // the spawned one is gone
	CHECK_FALSE(context.Session().EntityFor(spawnedNetId).IsValid()); // and its binding with it
	CHECK_FALSE(context.IsActive());
}

TEST_CASE("A second StartHost keeps the scene-placed entities and renumbers them")
{
	// The OTHER half of the predicate. On the second session the scene entities carry
	// netId != 0 AND scenePlaced == true, so `netId != 0` alone - dropping the
	// `!scenePlaced` term - would destroy them here.
	ServiceContainer services;
	World world;
	aether::net::NetworkContext context(services);

	MakeScenePlaced(world, 5);
	MakeScenePlaced(world, 6);

	REQUIRE(context.StartHost(world, kHostPort, 4));
	REQUIRE(CountIdentities(world) == 2);
	context.Stop(world);
	REQUIRE(CountIdentities(world) == 2);

	REQUIRE(context.StartHost(world, kHostPort, 4));
	CHECK(CountIdentities(world) == 2);
	for (const std::uint32_t netId: ScenePlacedNetIds(world))
	{
		CHECK(netId != 0);
	}

	context.Stop(world);
}

TEST_CASE("Net id lifecycle: allocate, bind, replicate, despawn, unbind")
{
	ServiceContainer services;
	World world;
	aether::net::NetworkContext context(services);

	MakeScenePlaced(world, 1);
	REQUIRE(context.StartHost(world, kHostPort, 4));

	// allocate + bind
	const Entity spawned = MakeSessionSpawned(world, context, 7);
	const std::uint32_t netId = world.TryGet<aether::net::NetworkIdentity>(spawned)->netId;
	CHECK(netId != 0);
	CHECK(context.Session().EntityFor(netId) == spawned);
	CHECK(context.Session().NetIdFor(spawned) == netId);

	// replicate: the per-connection change cache learns about the entity...
	context.Session().AddConnection(7);
	aether::net::SnapshotCache& cache = context.CacheFor(7);
	const std::vector<std::byte> packet = aether::net::BuildSnapshot(world, context.Schema(), context.Catalog(),
	        context.Session(), cache, {spawned});
	CHECK_FALSE(packet.empty());

	// despawn + unbind, and the cache forgets the id so a long session cannot grow
	// one entry per entity that ever existed.
	context.Despawn(world, spawned);
	CHECK_FALSE(context.Session().EntityFor(netId).IsValid());
	CHECK(context.Session().NetIdFor(spawned) == 0);
	CHECK_FALSE(world.GetRegistry().valid(World::ToEntt(spawned)));

	// ...and a re-bind of the same id to a fresh entity resends everything, proving
	// the cache entries really were dropped rather than left behind as "already sent".
	const Entity replacement = world.Create();
	world.Emplace<TransformComponent>(replacement);
	world.Emplace<aether::net::NetworkIdentity>(replacement).netId = netId;
	context.Session().Bind(netId, replacement);
	const std::vector<std::byte> again = aether::net::BuildSnapshot(world, context.Schema(), context.Catalog(),
	        context.Session(), context.CacheFor(7), {replacement});
	CHECK_FALSE(again.empty());

	context.Stop(world);
}

TEST_CASE("Join, leave and re-join does not grow the replicated population")
{
	// The regression this exists for: Stop() once left session-spawned entities in
	// the world. ResetForNewSession then zeroed their net ids, AssignScenePlacedNetIds
	// skipped them (no scene node id), and the next join's replay instantiated a
	// second copy of each - so every cycle doubled the prefab-spawned population.
	ServiceContainer services;
	World world;
	aether::net::NetworkContext context(services);

	constexpr int kScenePlaced = 3;
	for (int i = 0; i < kScenePlaced; ++i)
	{
		MakeScenePlaced(world, static_cast<std::uint64_t>(100 + i));
	}

	std::size_t afterFirstCycle = 0;
	for (int cycle = 0; cycle < 4; ++cycle)
	{
		REQUIRE(context.StartHost(world, kHostPort, 4));
		CHECK(CountIdentities(world) == kScenePlaced);

		// Two players join and each brings a replicated entity.
		MakeSessionSpawned(world, context, 1);
		MakeSessionSpawned(world, context, 2);
		CHECK(CountIdentities(world) == kScenePlaced + 2);

		context.Stop(world);

		const std::size_t remaining = CountIdentities(world);
		CHECK(remaining == kScenePlaced);
		if (cycle == 0)
		{
			afterFirstCycle = remaining;
		}
		else
		{
			// The doubling bug showed up as growth here, not as an absolute count.
			CHECK(remaining == afterFirstCycle);
		}
	}
}

TEST_CASE("A client owns nothing between Connect and the host's Welcome")
{
	// kInvalidConnection is BOTH "this client has no id yet" and "the host owns this
	// entity". Left unguarded, a client owns the entire world for the handshake's
	// duration and its scripts start driving every host-owned entity.
	ServiceContainer services;
	World world;
	aether::net::NetworkContext context(services);

	const Entity hostOwned = world.Create();
	world.Emplace<TransformComponent>(hostOwned);
	world.Emplace<aether::net::NetworkIdentity>(hostOwned).owner = aether::net::kInvalidConnection;

	REQUIRE(context.StartClient(world, "127.0.0.1", kUnreachablePort));
	REQUIRE(context.IsClient());
	REQUIRE(context.Session().LocalConnection() == aether::net::kInvalidConnection);
	REQUIRE_FALSE(context.IsConnected()); // still mid-handshake

	CHECK_FALSE(context.IsOwner(world, hostOwned));
	CHECK_FALSE(context.HasAuthority(world, hostOwned));

	// An entity with no NetworkIdentity is purely local, so it stays this client's
	// even during the window - otherwise a client's own effects and UI would freeze.
	const Entity local = world.Create();
	world.Emplace<TransformComponent>(local);
	CHECK(context.IsOwner(world, local));

	// Welcome lands.
	context.Session().SetLocalConnection(4);
	CHECK_FALSE(context.IsOwner(world, hostOwned)); // still the host's
	world.TryGet<aether::net::NetworkIdentity>(hostOwned)->owner = 4;
	CHECK(context.IsOwner(world, hostOwned)); // now ours
	CHECK(context.HasAuthority(world, hostOwned));

	context.Stop(world);
}

TEST_CASE("A host owns its own entities even though its connection id is invalid")
{
	// The other half of the same guard. A host's LocalConnection is kInvalidConnection
	// PERMANENTLY - that is how it owns its own entities - so an unconditional
	// "invalid local connection means own nothing" guard would strip the host of
	// authority over its entire world.
	ServiceContainer services;
	World world;
	aether::net::NetworkContext context(services);

	const Entity hostOwned = world.Create();
	world.Emplace<TransformComponent>(hostOwned);
	world.Emplace<aether::net::NetworkIdentity>(hostOwned).owner = aether::net::kInvalidConnection;

	const Entity clientOwned = world.Create();
	world.Emplace<TransformComponent>(clientOwned);
	world.Emplace<aether::net::NetworkIdentity>(clientOwned).owner = 3;

	REQUIRE(context.StartHost(world, kHostPort, 4));
	REQUIRE(context.Session().LocalConnection() == aether::net::kInvalidConnection);

	CHECK(context.IsOwner(world, hostOwned));
	CHECK_FALSE(context.IsOwner(world, clientOwned));
	// Authority follows ownership, on the host as much as anywhere: under client
	// authority the host is NOT authoritative for a character a client owns, and a
	// host that answered true here would go on simulating it against the transforms
	// its owner is sending.
	CHECK(context.HasAuthority(world, hostOwned));
	CHECK_FALSE(context.HasAuthority(world, clientOwned));

	context.Stop(world);
}

TEST_CASE("IsOwner is false for a stale handle to a destroyed entity, not a leftover true")
{
	// A destroyed entity's components are gone - entt strips them at destroy() - so
	// the identity == nullptr branch below, correctly "not replicated, so it's mine"
	// for a genuinely local entity, used to misread a dead REPLICATED one the same
	// way and hand back true. The handle used here is deliberately the SAME Entity
	// value the entity held while alive - a script's cached reference outliving the
	// entity behind it, or a Despawn racing a second one - rather than an id nobody
	// ever created; Entity carries no version of its own (see Entity.hpp), so this
	// is the only shape of "stale" that exists.
	ServiceContainer services;
	World world;
	aether::net::NetworkContext context(services);

	REQUIRE(context.StartClient(world, "127.0.0.1", kUnreachablePort));
	context.Session().SetLocalConnection(4);

	const Entity entity = world.Create();
	world.Emplace<TransformComponent>(entity);
	auto& identity = world.Emplace<aether::net::NetworkIdentity>(entity);
	identity.netId = 1;
	identity.owner = 4;
	context.Session().Bind(1, entity);

	REQUIRE(context.IsOwner(world, entity)); // true while it is alive and owned
	REQUIRE(context.HasAuthority(world, entity));

	world.Destroy(entity); // the SAME handle, now stale rather than merely unbound

	CHECK_FALSE(context.IsOwner(world, entity));
	CHECK_FALSE(context.HasAuthority(world, entity));

	context.Stop(world);
}

TEST_CASE("Offline, every entity is local so ownership and authority are both true")
{
	ServiceContainer services;
	World world;
	aether::net::NetworkContext context(services);

	const Entity replicated = world.Create();
	world.Emplace<aether::net::NetworkIdentity>(replicated).owner = 9;

	CHECK_FALSE(context.IsActive());
	CHECK(context.HasAuthority(world, replicated)); // single-player code just works
	CHECK_FALSE(context.IsConnected());
}

TEST_CASE("A context with no script bridges is safe and reports null")
{
	// Headless and CLR-less builds depend on this: the bridges are injected by the
	// wiring site, and a build with no CLR never injects them.
	ServiceContainer services;
	World world;
	aether::net::NetworkContext context(services);

	CHECK(context.FieldBridge() == nullptr);
	CHECK(context.Rpcs() == nullptr);

	REQUIRE(context.StartHost(world, kHostPort, 4));
	CHECK(context.FieldBridge() == nullptr);
	CHECK(context.Rpcs() == nullptr);
	context.Stop(world);
}

TEST_CASE("An injected script bridge is the one the context hands out")
{
	ServiceContainer services;
	aether::net::NetworkContext context(services);

	auto fieldBridge = std::make_unique<FakeFieldBridge>();
	auto rpcBridge = std::make_unique<FakeRpcCounter>();
	const auto* fieldRaw = fieldBridge.get();
	const auto* rpcRaw = rpcBridge.get();

	context.SetFieldBridge(std::move(fieldBridge));
	context.SetRpcBridge(std::move(rpcBridge));

	CHECK(context.FieldBridge() == fieldRaw);
	CHECK(context.Rpcs() == rpcRaw);

	// Clearing is just as valid - a project that tears the CLR down mid-run must not
	// leave a dangling bridge behind.
	context.SetFieldBridge(nullptr);
	context.SetRpcBridge(nullptr);
	CHECK(context.FieldBridge() == nullptr);
	CHECK(context.Rpcs() == nullptr);
}

TEST_CASE("ResetForNewSession clears every net id so the next session can renumber")
{
	World world;
	const Entity a = MakeScenePlaced(world, 1);
	world.TryGet<aether::net::NetworkIdentity>(a)->netId = 42;

	aether::net::NetworkContext::ResetForNewSession(world);

	CHECK(world.TryGet<aether::net::NetworkIdentity>(a)->netId == 0);
}

TEST_CASE("A client cannot despawn an entity it does not own")
{
	// Destroying it locally while the host keeps replicating it desyncs this client
	// permanently: the net id resolves to nothing here and the entity never returns.
	ServiceContainer services;
	World world;
	aether::net::NetworkContext context(services);

	// Built AFTER the session starts: StartClient runs Stop first, and Stop reclaims
	// exactly this shape of entity (a live net id, not scene-placed).
	REQUIRE(context.StartClient(world, "127.0.0.1", kUnreachablePort));
	context.Session().SetLocalConnection(2);

	const Entity hostOwned = world.Create();
	world.Emplace<TransformComponent>(hostOwned);
	auto& identity = world.Emplace<aether::net::NetworkIdentity>(hostOwned);
	identity.netId = 11;
	identity.owner = aether::net::kInvalidConnection;
	context.Session().Bind(11, hostOwned);

	context.Despawn(world, hostOwned);

	CHECK(world.GetRegistry().valid(World::ToEntt(hostOwned)));
	CHECK(context.Session().EntityFor(11) == hostOwned);

	context.Stop(world);
}

TEST_CASE("SpawnPrefab refuses to spawn from a client")
{
	ServiceContainer services;
	World world;
	aether::net::NetworkContext context(services);

	REQUIRE(context.StartClient(world, "127.0.0.1", kUnreachablePort));
	const Entity spawned = context.SpawnPrefab(world, "anything", glm::vec3(0.f), 1);
	CHECK_FALSE(spawned.IsValid());
	CHECK(CountIdentities(world) == 0);

	context.Stop(world);
}

TEST_CASE("SpawnPrefab builds the prefab locally with no session, and still refuses a client")
{
	// Offline parity, which the whole Net API is written to: a game that spawns
	// through the framework must behave the same in single-player, where Despawn
	// already destroyed locally while Spawn silently did nothing.
	//
	// A REAL prefab is mounted rather than a made-up name, because "returned an
	// invalid entity" is what a missing prefab looks like too - without the file
	// this case would pass against a SpawnPrefab that still refused offline.
	namespace fs = std::filesystem;
	if (aether::io::FileSystem::IsInitialized())
	{
		aether::io::FileSystem::Shutdown();
	}
	const fs::path root = fs::temp_directory_path() / "aethercore_net_offline_spawn_test";
	std::error_code ec;
	fs::remove_all(root, ec);

	aether::app::scene::SceneDescription prefab;
	aether::app::scene::EntityRecord record;
	record.name = "Offline Spawned";
	prefab.entities.push_back(record);
	REQUIRE(aether::io::file_util::WriteText(root / "assets" / "prefabs" / "net_offline_probe.prefab.toml",
	        aether::app::scene::WriteToml(prefab))
	                .has_value());

	aether::io::FileSystem::Initialize();
	aether::io::FileSystem::Mount("project", root);

	{
		ServiceContainer services;
		World world;
		aether::net::NetworkContext context(services);
		// No StartHost and no StartClient: role Offline, the single-player state.
		const Entity spawned = context.SpawnPrefab(world, "net_offline_probe", glm::vec3(3.f, 4.f, 0.f), 0);
		CHECK(spawned.IsValid());
		CHECK(world.GetRegistry().valid(World::ToEntt(spawned)));
		// Nothing was allocated and nothing was bound - there is no session to bind in.
		CHECK(context.Session().NetIdFor(spawned) == 0);
		// SpawnPrefab backs Net.Spawn, which NetSessionDirector.SpawnPlayerFor uses to
		// bring up every player - host-online and, via this exact offline branch, the
		// no-session single-player fallback too. A save mid-Play must never bake that
		// player permanently into the scene file (the reported corruption: a duplicate
		// Player + FirstPersonPlayer pair baked into Sandbox.scene.toml), so the spawn
		// must come back scene-transient.
		CHECK(world.Has<SceneTransientComponent>(spawned));
	}

	{
		ServiceContainer services;
		World world;
		aether::net::NetworkContext context(services);
		REQUIRE(context.StartClient(world, "127.0.0.1", kUnreachablePort));
		// The same prefab, now readable, and a client must STILL be refused: only the
		// host may allocate a net id, so a client-built copy would exist nowhere else.
		const Entity spawned = context.SpawnPrefab(world, "net_offline_probe", glm::vec3(0.f), 1);
		CHECK_FALSE(spawned.IsValid());
		context.Stop(world);
	}

	aether::io::FileSystem::Shutdown();
	fs::remove_all(root, ec);
}

TEST_CASE("ReleaseForDespawn announces and unbinds without destroying the entity")
{
	// The half Net.Despawn from a script needs immediately. The destruction is the
	// half that has to wait for the end of the script update, because a script runs
	// inside the runner's walk of ScriptComponent storage.
	ServiceContainer services;
	World world;
	aether::net::NetworkContext context(services);

	MakeScenePlaced(world, 1);
	REQUIRE(context.StartHost(world, kHostPort, 4));
	const Entity spawned = MakeSessionSpawned(world, context, 7);
	const std::uint32_t netId = world.TryGet<aether::net::NetworkIdentity>(spawned)->netId;
	REQUIRE(netId != 0);

	CHECK(context.ReleaseForDespawn(world, spawned));

	// Unbound at once: the id must stop resolving to an entity that is about to die.
	CHECK_FALSE(context.Session().EntityFor(netId).IsValid());
	CHECK(context.Session().NetIdFor(spawned) == 0);
	// ...and still alive, which is the whole difference from Despawn.
	CHECK(world.GetRegistry().valid(World::ToEntt(spawned)));

	context.Stop(world);
}

TEST_CASE("ReleaseForDespawn refuses a client an entity it does not own, and leaves it bound")
{
	ServiceContainer services;
	World world;
	aether::net::NetworkContext context(services);

	REQUIRE(context.StartClient(world, "127.0.0.1", kUnreachablePort));
	context.Session().SetLocalConnection(2);

	const Entity hostOwned = world.Create();
	world.Emplace<TransformComponent>(hostOwned);
	auto& identity = world.Emplace<aether::net::NetworkIdentity>(hostOwned);
	identity.netId = 12;
	identity.owner = aether::net::kInvalidConnection;
	context.Session().Bind(12, hostOwned);

	CHECK_FALSE(context.ReleaseForDespawn(world, hostOwned));
	CHECK(context.Session().EntityFor(12) == hostOwned);
	CHECK(world.GetRegistry().valid(World::ToEntt(hostOwned)));

	context.Stop(world);
}

TEST_CASE("Net.Despawn of a scene-placed entity is refused while a session is live")
{
	// The divergence the refusal prevents: a host despawning a scene-placed entity
	// broadcasts its deletion to every connected client, but the next joiner
	// re-derives it from the scene file - so the peers permanently disagree about
	// whether it exists. Every other scenePlaced-sensitive path (Stop,
	// OnDisconnected, relevancy) already refuses; Despawn was the one that did not.
	ServiceContainer services;
	World world;
	aether::net::NetworkContext context(services);

	const Entity sceneEntity = MakeScenePlaced(world, 7);
	REQUIRE(context.StartHost(world, kHostPort, 4));
	REQUIRE(world.TryGet<aether::net::NetworkIdentity>(sceneEntity)->scenePlaced);
	const std::uint32_t netId = world.TryGet<aether::net::NetworkIdentity>(sceneEntity)->netId;
	REQUIRE(netId != 0);

	// Refused outright: bound, alive, untouched - through both the split entry
	// point and the whole Despawn.
	CHECK_FALSE(context.ReleaseForDespawn(world, sceneEntity));
	CHECK(context.Session().EntityFor(netId) == sceneEntity);
	context.Despawn(world, sceneEntity);
	CHECK(world.GetRegistry().valid(World::ToEntt(sceneEntity)));
	CHECK(context.Session().EntityFor(netId) == sceneEntity);

	// Offline the refusal is lifted: with the session gone there is no peer left
	// to diverge, and single-player Despawn must keep working through every path.
	context.Stop(world);
	context.Despawn(world, sceneEntity);
	CHECK_FALSE(world.GetRegistry().valid(World::ToEntt(sceneEntity)));
}

TEST_CASE("A client's Net.Despawn of a scene-placed entity it owns is refused")
{
	// The mirror of the host-side refusal. A client destroying its own copy of a
	// scene-placed entity is a permanent local delete: the host and every other
	// client keep it, ExceptOwnedBy stops its state being relayed (it is owned
	// here), and nothing can ever rebuild it for this peer - ClientCanRecreate's
	// "a leave here is a permanent delete" hazard with the client holding the
	// trigger.
	ServiceContainer services;
	World world;
	aether::net::NetworkContext context(services);

	REQUIRE(context.StartClient(world, "127.0.0.1", kUnreachablePort));
	context.Session().SetLocalConnection(2);

	const Entity owned = world.Create();
	world.Emplace<TransformComponent>(owned);
	auto& identity = world.Emplace<aether::net::NetworkIdentity>(owned);
	identity.netId = 12;
	identity.owner = 2; // this client's own - the ownership check passes
	identity.scenePlaced = true;
	context.Session().Bind(12, owned);

	CHECK_FALSE(context.ReleaseForDespawn(world, owned));
	CHECK(context.Session().EntityFor(12) == owned);
	CHECK(world.GetRegistry().valid(World::ToEntt(owned)));

	context.Stop(world);
}

TEST_CASE("RequestOwnershipTransfer applies immediately on the host and broadcasts")
{
	ServiceContainer services;
	World world;
	aether::net::NetworkContext context(services);
	REQUIRE(context.StartHost(world, kHostPort, 4));

	const Entity entity = world.Create();
	auto& identity = world.Emplace<aether::net::NetworkIdentity>(entity);
	identity.netId = context.Session().AllocateNetId();
	identity.owner = aether::net::kInvalidConnection;
	context.Session().Bind(identity.netId, entity);

	constexpr aether::net::ConnectionId kNewOwner = 5;
	CHECK(context.RequestOwnershipTransfer(world, entity, kNewOwner) == aether::net::OwnershipTransferOutcome::Applied);
	CHECK(world.TryGet<aether::net::NetworkIdentity>(entity)->owner == kNewOwner);

	context.Stop(world);
}

TEST_CASE("RequestOwnershipTransfer on a client only sends a request - the owner does not change here")
{
	// The broadcast that actually changes it arrives through ApplyOwnershipTransfer,
	// once (and only once) the host answers - see NetworkSystemsTests.cpp for that
	// half, driven through the real inbound packet path.
	ServiceContainer services;
	World world;
	aether::net::NetworkContext context(services);
	REQUIRE(context.StartClient(world, "127.0.0.1", kUnreachablePort));
	context.Session().SetLocalConnection(2);

	const Entity entity = world.Create();
	auto& identity = world.Emplace<aether::net::NetworkIdentity>(entity);
	identity.netId = 12;
	identity.owner = aether::net::kInvalidConnection;
	context.Session().Bind(12, entity);

	CHECK(context.RequestOwnershipTransfer(world, entity, 2) == aether::net::OwnershipTransferOutcome::Requested);
	CHECK(world.TryGet<aether::net::NetworkIdentity>(entity)->owner == aether::net::kInvalidConnection);

	context.Stop(world);
}

TEST_CASE("RequestOwnershipTransfer is a no-op success for an entity with no NetworkIdentity")
{
	// Nothing to send, nothing to change - the entity is already "mine", exactly
	// as IsOwner's own nullptr branch already answers.
	ServiceContainer services;
	World world;
	aether::net::NetworkContext context(services);
	REQUIRE(context.StartHost(world, kHostPort, 4));

	const Entity plain = world.Create();
	world.Emplace<TransformComponent>(plain);

	CHECK(context.RequestOwnershipTransfer(world, plain, 9) == aether::net::OwnershipTransferOutcome::Applied);
	CHECK(world.TryGet<aether::net::NetworkIdentity>(plain) == nullptr);

	context.Stop(world);
}

TEST_CASE("RequestOwnershipTransfer applies immediately with no session at all")
{
	ServiceContainer services;
	World world;
	aether::net::NetworkContext context(services); // Offline: never started

	const Entity entity = world.Create();
	auto& identity = world.Emplace<aether::net::NetworkIdentity>(entity);
	identity.netId = 3; // stale/authored - offline behaviour must not depend on it
	identity.owner = 4;
	context.Session().Bind(3, entity);

	CHECK(context.RequestOwnershipTransfer(world, entity, aether::net::kInvalidConnection)
	      == aether::net::OwnershipTransferOutcome::Applied);
	CHECK(world.TryGet<aether::net::NetworkIdentity>(entity)->owner == aether::net::kInvalidConnection);
}

TEST_CASE("RequestOwnershipTransfer refuses an invalid or already-destroyed entity handle")
{
	ServiceContainer services;
	World world;
	aether::net::NetworkContext context(services);
	REQUIRE(context.StartHost(world, kHostPort, 4));

	CHECK(context.RequestOwnershipTransfer(world, Entity{}, 5) == aether::net::OwnershipTransferOutcome::Refused);

	const Entity entity = world.Create();
	auto& identity = world.Emplace<aether::net::NetworkIdentity>(entity);
	identity.netId = context.Session().AllocateNetId();
	context.Session().Bind(identity.netId, entity);
	world.Destroy(entity);

	CHECK(context.RequestOwnershipTransfer(world, entity, 5) == aether::net::OwnershipTransferOutcome::Refused);

	context.Stop(world);
}

TEST_CASE("A resync request from one connection is honoured at most once per interval")
{
	// ClientReady is one byte a peer may loop at line rate, and the send tick
	// consumes a queued request every paced tick - unthrottled, that pins the host
	// into replaying the whole replicated world at SendRateHz to that one peer (the
	// amplification DoS). The floor is the same interval the periodic full resend
	// already pays, and the genuine handshake - a client that has just arrived -
	// sends exactly one and never notices it.
	ServiceContainer services;
	World world;
	aether::net::NetworkContext context(services);
	REQUIRE(context.StartHost(world, kHostPort, 4));
	context.Session().AddConnection(5);

	context.RequestResync(5);
	CHECK(context.ConsumeResyncRequest(5)); // the one a real arrival sends: honoured

	// The same connection asking again inside the interval changes nothing, however
	// often it asks.
	for (int i = 0; i < 50; ++i)
	{
		context.RequestResync(5);
	}
	CHECK_FALSE(context.ConsumeResyncRequest(5));

	// The floor is per connection: a different peer's request is unaffected.
	context.Session().AddConnection(6);
	context.RequestResync(6);
	CHECK(context.ConsumeResyncRequest(6));

	context.Stop(world);
}

TEST_CASE("Stop cancels a pending traversal host setup before it can fire")
{
	// HostWithCode defers the host-side scene numbering to the next TickTraversal
	// (it has no World of its own). A game that cancels the host attempt in between
	// - the user backs out of hosting the same frame - must not have that deferred
	// block fire afterwards against a session that no longer exists: it would zero
	// every NetworkIdentity's net id and bind scene-placed ids into a cleared
	// NetSession while nothing is listening.
	ServiceContainer services;
	World world;
	aether::net::NetworkContext context(services);

	const Entity sceneEntity = MakeScenePlaced(world, 7);
	REQUIRE(context.HostWithCode(aether::net::NewRoomCode(), 24703, 4));

	context.Stop(world);
	context.TickTraversal(world, 0.f); // the frame after the cancel

	CHECK(ScenePlacedNetIds(world).empty()); // nothing was numbered with no session
	CHECK_FALSE(context.Session().EntityFor(1).IsValid()); // and nothing was bound
}

TEST_CASE("ApplySpawn refuses a prefab name that could leave the prefab folder")
{
	// The path-traversal boundary, exercised through the code that actually calls it
	// rather than only against IsSafePrefabName directly (NetSpawnTests.cpp).
	ServiceContainer services;
	World world;
	aether::net::NetworkContext context(services);
	REQUIRE(context.StartClient(world, "127.0.0.1", kUnreachablePort));

	for (const std::string& hostile: {std::string("../../etc/passwd"), std::string("..\\..\\secrets"),
	             std::string("sub/dir"), std::string("C:\\Windows\\System32\\config"), std::string("")})
	{
		aether::net::SpawnMessage msg;
		msg.netId = 900 + static_cast<std::uint32_t>(hostile.size());
		msg.prefab = hostile;
		context.ApplySpawn(world, msg);
		CHECK_FALSE(context.Session().EntityFor(msg.netId).IsValid());
	}
	CHECK(CountIdentities(world) == 0);

	context.Stop(world);
}

TEST_CASE("ApplySpawn rebinds a net id the host spawned onto a scene-placed entity")
{
	// The SetReplicationReady cross-scene desync: the host's allocator reached an
	// id this client derived for a scene-placed entity of a scene the host never
	// numbered. The host owns allocation, so the binding must move to its spawn -
	// not be silently dropped, which used to leave the spawn uncreated forever
	// while host state mutated the scene entity.
	ServiceContainer services;
	World world;
	aether::net::NetworkContext context(services);
	REQUIRE(context.StartClient(world, "127.0.0.1", kUnreachablePort));

	const Entity sceneEntity = MakeScenePlaced(world, 7);
	auto* identity = world.TryGet<aether::net::NetworkIdentity>(sceneEntity);
	REQUIRE(identity != nullptr);
	identity->netId = 5;
	identity->scenePlaced = true;
	context.Session().Bind(5, sceneEntity);

	// A Spawn naming no prefab for a bound id stays the documented replay no-op.
	aether::net::SpawnMessage replay;
	replay.netId = 5;
	context.ApplySpawn(world, replay);
	CHECK(context.Session().EntityFor(5) == sceneEntity);

	aether::net::SpawnMessage collision;
	collision.netId = 5;
	collision.prefab = "Enemy"; // no asset in this build, but the unbind happens first
	context.ApplySpawn(world, collision);
	CHECK_FALSE(context.Session().EntityFor(5).IsValid());
	CHECK(world.Get<aether::net::NetworkIdentity>(sceneEntity).netId == 0);

	context.Stop(world);
}

TEST_CASE("Stop puts every body it took off local simulation back")
{
	// A client hands a body it has no authority over to the network by making it
	// kinematic (see SyncSimulationAuthority). Scene-placed entities SURVIVE Stop, so
	// one left kinematic is a character that never falls again once the player is
	// back in single-player - and nothing in an offline game would ever put it right.
	ServiceContainer services;
	World world;
	aether::net::NetworkContext context(services);

	const Entity remote = world.Create();
	world.Emplace<TransformComponent>(remote);
	world.Emplace<aether::net::NetworkIdentity>(remote,
	        aether::net::NetworkIdentity{.netId = 7, .owner = aether::net::kInvalidConnection, .scenePlaced = true});
	world.Emplace<RigidBody2DComponent>(remote, RigidBody2DComponent{.bodyType = Body2DType::Dynamic});

	REQUIRE(context.StartClient(world, "127.0.0.1", kUnreachablePort));
	context.Session().SetLocalConnection(4); // the Welcome landed

	context.SyncSimulationAuthority(world);
	REQUIRE(world.TryGet<RigidBody2DComponent>(remote)->bodyType == Body2DType::Kinematic);
	REQUIRE(world.Has<aether::net::NetSimulationOverride>(remote));

	context.Stop(world);

	CHECK(world.TryGet<RigidBody2DComponent>(remote)->bodyType == Body2DType::Dynamic);
	CHECK_FALSE(world.Has<aether::net::NetSimulationOverride>(remote));

	// And offline it stays that way: with no session there is no authority to defer
	// to, so a reconcile must be a no-op rather than hand everything over again.
	context.SyncSimulationAuthority(world);
	CHECK(world.TryGet<RigidBody2DComponent>(remote)->bodyType == Body2DType::Dynamic);
}

TEST_CASE("Hosting after a client session leaves nothing kinematic")
{
	// Stop runs at the TOP of StartHost, which is the only thing that un-does a
	// previous client session's handovers when the same process goes on to host. A
	// host is authoritative over everything, so a body still on the network here is
	// one the host would replicate out without ever simulating it.
	ServiceContainer services;
	World world;
	aether::net::NetworkContext context(services);

	const Entity entity = MakeScenePlaced(world, 11);
	world.Emplace<RigidBody2DComponent>(entity, RigidBody2DComponent{.bodyType = Body2DType::Dynamic});

	REQUIRE(context.StartClient(world, "127.0.0.1", kUnreachablePort));
	context.Session().SetLocalConnection(4);
	// A live net id is what puts the entity in the session; `scenePlaced` is what
	// makes it survive the Stop at the top of StartHost, which is the whole point of
	// this case - a session-spawned one would simply be destroyed there instead.
	auto* identity = world.TryGet<aether::net::NetworkIdentity>(entity);
	identity->netId = 3;
	identity->scenePlaced = true;
	context.SyncSimulationAuthority(world);
	REQUIRE(world.TryGet<RigidBody2DComponent>(entity)->bodyType == Body2DType::Kinematic);

	REQUIRE(context.StartHost(world, kHostPort, 4));

	CHECK(world.TryGet<RigidBody2DComponent>(entity)->bodyType == Body2DType::Dynamic);
	CHECK_FALSE(world.Has<aether::net::NetSimulationOverride>(entity));

	// A host reconciling its OWN entity changes nothing - it owns everything whose
	// owner is kInvalidConnection, which is what StartHost restored this one to.
	context.SyncSimulationAuthority(world);
	CHECK(world.TryGet<RigidBody2DComponent>(entity)->bodyType == Body2DType::Dynamic);
	CHECK_FALSE(world.Has<aether::net::NetSimulationOverride>(entity));

	context.Stop(world);
}

TEST_CASE("Stop puts every 3D body it took off local simulation back")
{
	// The 3D counterpart of "Stop puts every body it took off local simulation
	// back" - same setup, RigidBodyComponent/PhysicsMotionType/
	// NetSimulationOverride3D instead of the 2D equivalents.
	ServiceContainer services;
	World world;
	aether::net::NetworkContext context(services);

	const Entity remote = world.Create();
	world.Emplace<TransformComponent>(remote);
	world.Emplace<aether::net::NetworkIdentity>(remote,
	        aether::net::NetworkIdentity{.netId = 7, .owner = aether::net::kInvalidConnection, .scenePlaced = true});
	world.Emplace<RigidBodyComponent>(remote, RigidBodyComponent{.motionType = PhysicsMotionType::Dynamic});

	REQUIRE(context.StartClient(world, "127.0.0.1", kUnreachablePort));
	context.Session().SetLocalConnection(4);

	context.SyncSimulationAuthority(world);
	REQUIRE(world.TryGet<RigidBodyComponent>(remote)->motionType == PhysicsMotionType::Kinematic);
	REQUIRE(world.Has<aether::net::NetSimulationOverride3D>(remote));

	context.Stop(world);

	CHECK(world.TryGet<RigidBodyComponent>(remote)->motionType == PhysicsMotionType::Dynamic);
	CHECK_FALSE(world.Has<aether::net::NetSimulationOverride3D>(remote));

	context.SyncSimulationAuthority(world);
	CHECK(world.TryGet<RigidBodyComponent>(remote)->motionType == PhysicsMotionType::Dynamic);
}

TEST_CASE("Hosting after a client session leaves no 3D body kinematic")
{
	ServiceContainer services;
	World world;
	aether::net::NetworkContext context(services);

	const Entity entity = MakeScenePlaced(world, 11);
	world.Emplace<RigidBodyComponent>(entity, RigidBodyComponent{.motionType = PhysicsMotionType::Dynamic});

	REQUIRE(context.StartClient(world, "127.0.0.1", kUnreachablePort));
	context.Session().SetLocalConnection(4);
	auto* identity = world.TryGet<aether::net::NetworkIdentity>(entity);
	identity->netId = 3;
	identity->scenePlaced = true;
	context.SyncSimulationAuthority(world);
	REQUIRE(world.TryGet<RigidBodyComponent>(entity)->motionType == PhysicsMotionType::Kinematic);

	REQUIRE(context.StartHost(world, kHostPort, 4));

	CHECK(world.TryGet<RigidBodyComponent>(entity)->motionType == PhysicsMotionType::Dynamic);
	CHECK_FALSE(world.Has<aether::net::NetSimulationOverride3D>(entity));

	context.SyncSimulationAuthority(world);
	CHECK(world.TryGet<RigidBodyComponent>(entity)->motionType == PhysicsMotionType::Dynamic);
	CHECK_FALSE(world.Has<aether::net::NetSimulationOverride3D>(entity));

	context.Stop(world);
}

TEST_CASE("A non-owned ragdoll freezes and thaws every bone together, not just its root")
{
	// The failure mode this guards: freezing only the NetworkIdentity-carrying root
	// would leave its jointed limbs fully Dynamic while the root teleports to
	// wherever the network says every tick - a Hinge/Swing Twist constraint between
	// a Kinematic and a still-Dynamic body is exactly the mixed-authority case a
	// solver was never asked to make sense of.
	ServiceContainer services;
	World world;
	aether::net::NetworkContext context(services);

	const Entity root = world.Create();
	world.Emplace<TransformComponent>(root);
	world.Emplace<aether::net::NetworkIdentity>(root,
	        aether::net::NetworkIdentity{.netId = 7, .owner = aether::net::kInvalidConnection, .scenePlaced = true});
	world.Emplace<RigidBodyComponent>(root, RigidBodyComponent{.motionType = PhysicsMotionType::Dynamic});

	const Entity limbA = world.Create();
	world.Emplace<TransformComponent>(limbA);
	world.Emplace<RigidBodyComponent>(limbA, RigidBodyComponent{.motionType = PhysicsMotionType::Dynamic});

	const Entity limbB = world.Create();
	world.Emplace<TransformComponent>(limbB);
	world.Emplace<RigidBodyComponent>(limbB, RigidBodyComponent{.motionType = PhysicsMotionType::Dynamic});

	world.Emplace<RagdollComponent>(root, RagdollComponent{.bones = {root, limbA, limbB}});

	REQUIRE(context.StartClient(world, "127.0.0.1", kUnreachablePort));
	context.Session().SetLocalConnection(4);

	context.SyncSimulationAuthority(world);
	CHECK(world.TryGet<RigidBodyComponent>(root)->motionType == PhysicsMotionType::Kinematic);
	CHECK(world.TryGet<RigidBodyComponent>(limbA)->motionType == PhysicsMotionType::Kinematic);
	CHECK(world.TryGet<RigidBodyComponent>(limbB)->motionType == PhysicsMotionType::Kinematic);
	CHECK(world.Has<aether::net::NetSimulationOverride3D>(root));
	CHECK(world.Has<aether::net::NetSimulationOverride3D>(limbA));
	CHECK(world.Has<aether::net::NetSimulationOverride3D>(limbB));

	context.Stop(world);
	CHECK(world.TryGet<RigidBodyComponent>(root)->motionType == PhysicsMotionType::Dynamic);
	CHECK(world.TryGet<RigidBodyComponent>(limbA)->motionType == PhysicsMotionType::Dynamic);
	CHECK(world.TryGet<RigidBodyComponent>(limbB)->motionType == PhysicsMotionType::Dynamic);
	CHECK_FALSE(world.Has<aether::net::NetSimulationOverride3D>(root));
	CHECK_FALSE(world.Has<aether::net::NetSimulationOverride3D>(limbA));
	CHECK_FALSE(world.Has<aether::net::NetSimulationOverride3D>(limbB));
}

TEST_CASE("SyncSimulationAuthority wires a Character Controller's locallySimulated to ownership")
{
	ServiceContainer services;
	World world;
	aether::net::NetworkContext context(services);

	const Entity remote = world.Create();
	world.Emplace<TransformComponent>(remote);
	world.Emplace<aether::net::NetworkIdentity>(remote,
	        aether::net::NetworkIdentity{.netId = 7, .owner = aether::net::kInvalidConnection, .scenePlaced = true});
	world.Emplace<CharacterControllerComponent>(remote);
	REQUIRE(world.Get<CharacterControllerComponent>(remote).locallySimulated);

	REQUIRE(context.StartClient(world, "127.0.0.1", kUnreachablePort));
	context.Session().SetLocalConnection(4);

	context.SyncSimulationAuthority(world);
	CHECK_FALSE(world.Get<CharacterControllerComponent>(remote).locallySimulated);

	// Ownership changing hands - the identity is now this client's own - flips it
	// straight back without any restore/marker step, unlike the Rigid Body path.
	world.Get<aether::net::NetworkIdentity>(remote).owner = 4;
	context.SyncSimulationAuthority(world);
	CHECK(world.Get<CharacterControllerComponent>(remote).locallySimulated);

	// And Stop puts it back too, offline having no other authority to defer to.
	world.Get<aether::net::NetworkIdentity>(remote).owner = aether::net::kInvalidConnection;
	context.SyncSimulationAuthority(world);
	REQUIRE_FALSE(world.Get<CharacterControllerComponent>(remote).locallySimulated);
	context.Stop(world);
	CHECK(world.Get<CharacterControllerComponent>(remote).locallySimulated);
}
