#include <doctest/doctest.h>

#include "net/NetSession.hpp"
#include "scene/World.hpp"

using namespace aether;

TEST_CASE("NetSession allocates monotonic ids and maps them both ways")
{
	net::NetSession session;
	World w;

	const Entity a = w.Create();
	const Entity b = w.Create();

	const std::uint32_t idA = session.AllocateNetId();
	const std::uint32_t idB = session.AllocateNetId();
	CHECK(idA != 0);
	CHECK(idB != idA);

	session.Bind(idA, a);
	session.Bind(idB, b);

	CHECK(session.EntityFor(idA) == a);
	CHECK(session.EntityFor(idB) == b);
	CHECK(session.NetIdFor(a) == idA);
	CHECK(session.NetIdFor(b) == idB);
}

TEST_CASE("Unbinding removes both directions")
{
	net::NetSession session;
	World w;
	const Entity e = w.Create();

	const std::uint32_t id = session.AllocateNetId();
	session.Bind(id, e);
	session.Unbind(id);

	CHECK_FALSE(session.EntityFor(id).IsValid());
	CHECK(session.NetIdFor(e) == 0);
}

TEST_CASE("An unknown net id resolves to an invalid entity rather than a stale one")
{
	net::NetSession session;
	CHECK_FALSE(session.EntityFor(12345).IsValid());
	CHECK(session.NetIdFor(Entity{}) == 0);
}

TEST_CASE("Connections are tracked and cleared with the session")
{
	net::NetSession session;
	session.SetRole(net::NetRole::Host);
	session.AddConnection(1);
	session.AddConnection(2);
	CHECK(session.Connections().size() == 2);

	session.RemoveConnection(1);
	CHECK(session.Connections().size() == 1);

	session.Clear();
	CHECK(session.Connections().empty());
	CHECK(session.Role() == net::NetRole::Offline);
}
