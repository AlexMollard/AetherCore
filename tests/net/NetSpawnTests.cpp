#include <doctest/doctest.h>

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
