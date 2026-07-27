#include <doctest/doctest.h>

#include <map>
#include <utility>
#include <vector>

#include "net/NetSpawn.hpp"

#include "net/NetComponents.hpp"
#include "net/NetSession.hpp"
#include "scene/Components.hpp"
#include "scene/World.hpp"

using namespace aether;

TEST_CASE("A spawn message round-trips")
{
	const std::vector<std::byte> bytes = net::EncodeSpawn(42, 3, "player", {1.f, 2.f, 3.f});

	net::ByteReader r{bytes};
	CHECK(static_cast<net::NetMessage>(r.U8()) == net::NetMessage::Spawn);

	const auto msg = net::DecodeSpawn(r);
	REQUIRE(msg.has_value());
	CHECK(msg->netId == 42);
	CHECK(msg->owner == 3);
	CHECK(msg->prefab == "player");
	CHECK(msg->position.y == doctest::Approx(2.f));
}

TEST_CASE("A despawn message round-trips")
{
	const std::vector<std::byte> bytes = net::EncodeDespawn(7);

	net::ByteReader r{bytes};
	CHECK(static_cast<net::NetMessage>(r.U8()) == net::NetMessage::Despawn);

	const auto netId = net::DecodeDespawn(r);
	REQUIRE(netId.has_value());
	CHECK(*netId == 7);
}

TEST_CASE("A truncated spawn message decodes to nothing rather than garbage")
{
	net::ByteWriter w;
	w.U8(static_cast<std::uint8_t>(net::NetMessage::Spawn));
	w.U32(1); // netId only; the rest is missing
	const std::vector<std::byte> bytes = w.Take();

	net::ByteReader r{bytes};
	CHECK(static_cast<net::NetMessage>(r.U8()) == net::NetMessage::Spawn);
	CHECK_FALSE(net::DecodeSpawn(r).has_value());
}

TEST_CASE("Scene-placed net ids follow the persisted node id, not creation order")
{
	World world;
	net::NetSession session;

	// Emplace in scrambled order (30, 10, 20) so a bug that sorted by creation
	// order or by raw ECS iteration would still pass a naive test.
	const Entity e30 = world.Create();
	world.Emplace<net::NetworkIdentity>(e30);
	world.Emplace<SceneNodeComponent>(e30, SceneNodeComponent{.id = 30});

	const Entity e10 = world.Create();
	world.Emplace<net::NetworkIdentity>(e10);
	world.Emplace<SceneNodeComponent>(e10, SceneNodeComponent{.id = 10});

	const Entity e20 = world.Create();
	world.Emplace<net::NetworkIdentity>(e20);
	world.Emplace<SceneNodeComponent>(e20, SceneNodeComponent{.id = 20});

	net::AssignScenePlacedNetIds(world, session);

	const std::uint32_t id10 = world.Get<net::NetworkIdentity>(e10).netId;
	const std::uint32_t id20 = world.Get<net::NetworkIdentity>(e20).netId;
	const std::uint32_t id30 = world.Get<net::NetworkIdentity>(e30).netId;

	CHECK(id10 != 0);
	CHECK(id20 != 0);
	CHECK(id30 != 0);
	CHECK(id10 < id20);
	CHECK(id20 < id30);
}

TEST_CASE("Host and client derive the same scene-placed ids from the same scene")
{
	// The invariant this whole mechanism rests on is CROSS-MACHINE agreement: two
	// processes loading the same scene file must land on the same nodeId -> netId
	// map with no handshake. Testing ordering within one world does not prove that -
	// the two ends create their entities in whatever order their own scene apply
	// produced, so the ids must not depend on it.
	const std::vector<std::uint64_t> nodeIds{4001, 12, 900, 7, 65535};

	const auto derive = [&](const std::vector<std::uint64_t>& creationOrder)
	{
		World world;
		net::NetSession session;
		std::vector<std::pair<std::uint64_t, Entity>> entities;
		for (const std::uint64_t nodeId: creationOrder)
		{
			const Entity e = world.Create();
			world.Emplace<net::NetworkIdentity>(e);
			world.Emplace<SceneNodeComponent>(e, SceneNodeComponent{.id = nodeId});
			entities.emplace_back(nodeId, e);
		}
		net::AssignScenePlacedNetIds(world, session);

		std::map<std::uint64_t, std::uint32_t> byNode;
		for (const auto& [nodeId, entity]: entities)
		{
			byNode[nodeId] = world.Get<net::NetworkIdentity>(entity).netId;
		}
		return byNode;
	};

	// "Host": the file's own order. "Client": reversed, standing in for any other
	// creation order the same file could produce on another machine.
	const std::map<std::uint64_t, std::uint32_t> host = derive(nodeIds);
	std::vector<std::uint64_t> reversed(nodeIds.rbegin(), nodeIds.rend());
	const std::map<std::uint64_t, std::uint32_t> client = derive(reversed);

	CHECK(host == client);
	for (const auto& [nodeId, netId]: host)
	{
		CHECK(netId != 0);
	}
}

TEST_CASE("Scene-placed net id assignment skips entities with no persisted node id")
{
	World world;
	net::NetSession session;

	const Entity prefabSpawned = world.Create();
	world.Emplace<net::NetworkIdentity>(prefabSpawned);
	world.Emplace<SceneNodeComponent>(prefabSpawned, SceneNodeComponent{.id = 0});

	net::AssignScenePlacedNetIds(world, session);

	CHECK(world.Get<net::NetworkIdentity>(prefabSpawned).netId == 0);
}

// The framework's only path-traversal defence. A prefab name arrives from a remote
// peer and is handed to a file loader, so this is the check standing between a
// hostile Spawn and an arbitrary read off the host's disk. It lived file-local in a
// CLR-linked TU with no coverage until the spawn validation moved here.
TEST_CASE("IsSafePrefabName accepts a bare prefab name and rejects anything path-like")
{
	CHECK(net::IsSafePrefabName("player"));
	CHECK(net::IsSafePrefabName("enemy_grunt_v2"));
	CHECK(net::IsSafePrefabName("Weapon-Rifle.01"));

	CHECK_FALSE(net::IsSafePrefabName(""));                   // names nothing
	CHECK_FALSE(net::IsSafePrefabName("../../etc/passwd"));   // the canonical escape
	CHECK_FALSE(net::IsSafePrefabName(".."));                 // the parent alone
	CHECK_FALSE(net::IsSafePrefabName("a/b"));                // any subdirectory
	CHECK_FALSE(net::IsSafePrefabName(R"(a\b)"));             // ...on either separator
	CHECK_FALSE(net::IsSafePrefabName(R"(C:\x)"));            // an absolute Windows path
	CHECK_FALSE(net::IsSafePrefabName("C:x"));                // a drive-relative one
	CHECK_FALSE(net::IsSafePrefabName("/etc/passwd"));        // an absolute POSIX path
	CHECK_FALSE(net::IsSafePrefabName(R"(..\..\windows\system32)"));
	CHECK_FALSE(net::IsSafePrefabName(R"(\\server\share)"));  // a UNC path
}
