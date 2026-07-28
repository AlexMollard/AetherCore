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
#include <memory>
#include <string>
#include <vector>

#include "net/NetComponents.hpp"
#include "net/NetRpc.hpp"
#include "net/NetScriptFields.hpp"
#include "net/NetSpawn.hpp"
#include "net/NetworkContext.hpp"
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
