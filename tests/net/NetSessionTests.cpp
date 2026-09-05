#include <doctest/doctest.h>

#include "net/NetSession.hpp"
#include "scene/World.hpp"

using namespace aether;

namespace aether::net
{
	// The friend NetSession names: stages the private allocator counters for the
	// exhaustion cases, which the public API would need ~4.3 billion calls to
	// reach. Test-only by design - a public "set the counter" would be a footgun.
	struct NetSessionTestPeer
	{
		static void SetNextNetId(NetSession& session, std::uint32_t value)
		{
			session.m_nextNetId = value;
		}
	};
} // namespace aether::net

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

TEST_CASE("Scene-placed and spawned ids come from disjoint spaces")
{
	// The SetReplicationReady cross-scene fix: a client that re-derives scene ids
	// from 1 in a bigger scene must never hold an id the host's spawn counter is
	// about to hand out, and ResetBindings must not rewind the spawn counter - ids
	// are never reused within a session.
	net::NetSession session;

	CHECK(session.AllocateSceneNetId() == 1);
	CHECK(session.AllocateSceneNetId() == 2);
	const std::uint32_t spawnId = session.AllocateNetId();
	CHECK(spawnId >= net::kSpawnNetIdBase);

	session.ResetBindings();
	CHECK(session.AllocateSceneNetId() == 1); // deterministic re-derivation
	CHECK(session.AllocateNetId() == spawnId + 1); // spawn counter survives
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

TEST_CASE("The spawn id space refuses to wrap")
{
	// The mirror of AllocateSceneNetId's exhaustion guard, for the spawn half: past
	// UINT32_MAX the counter would wrap into 0 - which every decoder reads as "no
	// id" - and then walk up through the scene-placed space the split exists to
	// keep disjoint. The staged counter stands in for the ~4.3 billion allocations
	// the genuine article would take.
	net::NetSession session;
	aether::net::NetSessionTestPeer::SetNextNetId(session, 0xFFFF'FFFFu);

	CHECK(session.AllocateNetId() == 0xFFFF'FFFFu); // the last real id is still handed out
	CHECK(session.AllocateNetId() == 0u); // past the ceiling: refuse, do not wrap
	CHECK(session.AllocateNetId() == 0u); // and never "recover" into the scene-placed space
}

TEST_CASE("Rebinding an id or an entity replaces the old pair rather than stranding it")
{
	// Bind is public API; a second binding for an id that already names another
	// entity (or an entity that already carries another id) used to leave the
	// loser's reverse entry live - NetIdFor then answered with an id EntityFor
	// resolved to somebody else.
	net::NetSession session;
	World w;
	const Entity a = w.Create();
	const Entity b = w.Create();

	// Same id, new entity: `a` must stop carrying net id 1.
	session.Bind(1, a);
	session.Bind(1, b);
	CHECK(session.EntityFor(1) == b);
	CHECK(session.NetIdFor(a) == 0u);

	// Same entity, new id: id 1 must stop resolving to `b`.
	session.Bind(2, b);
	CHECK(session.EntityFor(1).IsValid() == false);
	CHECK(session.NetIdFor(b) == 2u);

	// Rebinding the identical pair is a no-op, not an erase of itself.
	session.Bind(2, b);
	CHECK(session.EntityFor(2) == b);
	CHECK(session.NetIdFor(b) == 2u);
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
