#include <doctest/doctest.h>

#include "net/NetComponents.hpp"
#include "net/NetSnapshot.hpp"
#include "net/NetSession.hpp"
#include "scene/Components.hpp"
#include "scene/TransformUtils.hpp"
#include "scene/World.hpp"
#include "scene/reflection/Reflection.hpp"

using namespace aether;

// NOTE: TransformComponent stores only `localToWorld` (glm::mat4); "position" is a
// reflected field decomposed from/composed into that matrix (see
// CoreComponents.reflect.cpp and scene/TransformUtils.hpp), not a direct struct
// member. The tests below drive the matrix directly with the same TRS helpers the
// reflection getter/setter use, rather than assigning a `.position` member.

namespace
{
	// A host world and a client world, with one replicated entity bound to the same
	// net id on both sides - the minimal setup replication actually operates on.
	struct TwoWorlds
	{
		World host;
		World client;
		net::NetSession hostSession;
		net::NetSession clientSession;
		Entity hostEntity;
		Entity clientEntity;
		net::ReplicationSchema schema = net::BuildReplicationSchema(reflect::ComponentTypes());

		TwoWorlds()
		{
			hostEntity = host.Create();
			host.Emplace<TransformComponent>(hostEntity);
			host.Emplace<net::NetworkIdentity>(hostEntity).netId = 1;
			hostSession.Bind(1, hostEntity);

			clientEntity = client.Create();
			client.Emplace<TransformComponent>(clientEntity);
			client.Emplace<net::NetworkIdentity>(clientEntity).netId = 1;
			clientSession.Bind(1, clientEntity);
		}
	};
} // namespace

TEST_CASE("A changed replicated field reaches the client world")
{
	TwoWorlds tw;
	net::SnapshotCache cache;

	tw.host.TryGet<TransformComponent>(tw.hostEntity)->localToWorld =
	        ComposeTransform({5.f, 6.f, 7.f}, {0.f, 0.f, 0.f}, {1.f, 1.f, 1.f});

	const std::vector<std::byte> packet = net::BuildSnapshot(
	        tw.host, tw.schema, reflect::ComponentTypes(), tw.hostSession, cache, {tw.hostEntity});
	REQUIRE_FALSE(packet.empty());

	net::ApplySnapshot(tw.client, tw.schema, reflect::ComponentTypes(), tw.clientSession, packet);

	const auto* t = tw.client.TryGet<TransformComponent>(tw.clientEntity);
	REQUIRE(t != nullptr);
	const glm::vec3 pos = glm::vec3(t->localToWorld[3]);
	CHECK(pos.x == doctest::Approx(5.f));
	CHECK(pos.z == doctest::Approx(7.f));
}

TEST_CASE("An unchanged field produces no snapshot at all")
{
	TwoWorlds tw;
	net::SnapshotCache cache;

	tw.host.TryGet<TransformComponent>(tw.hostEntity)->localToWorld =
	        ComposeTransform({1.f, 0.f, 0.f}, {0.f, 0.f, 0.f}, {1.f, 1.f, 1.f});

	// First build sends everything; the second has nothing to say.
	const std::vector<std::byte> first = net::BuildSnapshot(
	        tw.host, tw.schema, reflect::ComponentTypes(), tw.hostSession, cache, {tw.hostEntity});
	CHECK_FALSE(first.empty());

	const std::vector<std::byte> second = net::BuildSnapshot(
	        tw.host, tw.schema, reflect::ComponentTypes(), tw.hostSession, cache, {tw.hostEntity});
	CHECK(second.empty());
}

TEST_CASE("Only entities in the relevant set are included")
{
	TwoWorlds tw;
	net::SnapshotCache cache;
	tw.host.TryGet<TransformComponent>(tw.hostEntity)->localToWorld =
	        ComposeTransform({9.f, 9.f, 9.f}, {0.f, 0.f, 0.f}, {1.f, 1.f, 1.f});

	// Relevant set is empty, so nothing is sent even though the field changed.
	const std::vector<std::byte> packet = net::BuildSnapshot(
	        tw.host, tw.schema, reflect::ComponentTypes(), tw.hostSession, cache, {});
	CHECK(packet.empty());
}

TEST_CASE("A snapshot naming an unknown net id is ignored, not applied blindly")
{
	TwoWorlds tw;

	net::ByteWriter w;
	w.U16(1);        // one field
	w.U32(9999);     // net id nobody has
	w.U16(0);
	w.U16(0);
	w.F32(1.f);
	const std::vector<std::byte> hostile = w.Take();

	// Must not crash, and must leave the client world untouched.
	net::ApplySnapshot(tw.client, tw.schema, reflect::ComponentTypes(), tw.clientSession, hostile);

	const auto* t = tw.client.TryGet<TransformComponent>(tw.clientEntity);
	REQUIRE(t != nullptr);
	CHECK(glm::vec3(t->localToWorld[3]).x == doctest::Approx(0.f));
}

TEST_CASE("A truncated snapshot is rejected without applying a partial field")
{
	TwoWorlds tw;

	net::ByteWriter w;
	w.U16(1);
	w.U32(1);
	w.U16(0);
	// component index written, field index and value missing
	const std::vector<std::byte> truncated = w.Take();

	net::ApplySnapshot(tw.client, tw.schema, reflect::ComponentTypes(), tw.clientSession, truncated);

	const auto* t = tw.client.TryGet<TransformComponent>(tw.clientEntity);
	REQUIRE(t != nullptr);
	CHECK(glm::vec3(t->localToWorld[3]).x == doctest::Approx(0.f));
}
