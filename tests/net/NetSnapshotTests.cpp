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

	// A field's position in the component catalog, looked up by display name.
	struct FieldIndices
	{
		std::uint16_t componentIndex = 0;
		std::uint16_t fieldIndex = 0;
		reflect::FieldType type = reflect::FieldType::Float;
	};

	// Component registration order across `.reflect.cpp` translation units is
	// unspecified by the standard - catalog[0][0] happens to be
	// SpriteRendererComponent::texture (a String) in this build, not the float field a
	// hardcoded [0][0] would assume. Tests must target a field by name, not by raw
	// index, so they keep pinning the field they mean to test regardless of link order.
	FieldIndices RequireCatalogField(
	        const std::vector<reflect::ComponentType>& catalog, std::string_view componentName, std::string_view fieldName)
	{
		for (std::size_t c = 0; c < catalog.size(); ++c)
		{
			if (catalog[c].name != componentName)
			{
				continue;
			}
			for (std::size_t f = 0; f < catalog[c].fields.size(); ++f)
			{
				if (catalog[c].fields[f].name == fieldName)
				{
					return FieldIndices{
					        .componentIndex = static_cast<std::uint16_t>(c),
					        .fieldIndex = static_cast<std::uint16_t>(f),
					        .type = catalog[c].fields[f].type,
					};
				}
			}
		}
		REQUIRE_MESSAGE(false, "field not found in catalog: ", componentName, ".", fieldName);
		return {};
	}
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
	const FieldIndices position = RequireCatalogField(reflect::ComponentTypes(), "Transform", "position");

	net::ByteWriter w;
	w.U16(1);        // one field
	w.U32(9999);     // net id nobody has
	w.U16(position.componentIndex);
	w.U16(position.fieldIndex);
	net::WriteFieldValue(w, reflect::MakeValue(glm::vec3{1.f, 1.f, 1.f}));
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
	const FieldIndices position = RequireCatalogField(reflect::ComponentTypes(), "Transform", "position");

	net::ByteWriter w;
	w.U16(1);
	w.U32(1);
	w.U16(position.componentIndex);
	// field index and value missing
	const std::vector<std::byte> truncated = w.Take();

	net::ApplySnapshot(tw.client, tw.schema, reflect::ComponentTypes(), tw.clientSession, truncated);

	const auto* t = tw.client.TryGet<TransformComponent>(tw.clientEntity);
	REQUIRE(t != nullptr);
	CHECK(glm::vec3(t->localToWorld[3]).x == doctest::Approx(0.f));
}

TEST_CASE("A field skipped for an unknown net id does not desync the cursor for the next field")
{
	TwoWorlds tw;
	const FieldIndices position = RequireCatalogField(reflect::ComponentTypes(), "Transform", "position");

	net::ByteWriter w;
	w.U16(2); // two fields
	// Field 1: a valid, replicated field, but nobody is bound to this net id - skipped.
	w.U32(9999);
	w.U16(position.componentIndex);
	w.U16(position.fieldIndex);
	net::WriteFieldValue(w, reflect::MakeValue(glm::vec3{-1.f, -1.f, -1.f}));
	// Field 2: the same field, the real net id, a distinctive value that must still land.
	// Against a broken implementation that skips without consuming field 1's value bytes,
	// this decodes as garbage and the assertions below fail.
	w.U32(1);
	w.U16(position.componentIndex);
	w.U16(position.fieldIndex);
	net::WriteFieldValue(w, reflect::MakeValue(glm::vec3{11.f, 22.f, 33.f}));
	const std::vector<std::byte> packet = w.Take();

	net::ApplySnapshot(tw.client, tw.schema, reflect::ComponentTypes(), tw.clientSession, packet);

	const auto* t = tw.client.TryGet<TransformComponent>(tw.clientEntity);
	REQUIRE(t != nullptr);
	const glm::vec3 pos = glm::vec3(t->localToWorld[3]);
	CHECK(pos.x == doctest::Approx(11.f));
	CHECK(pos.y == doctest::Approx(22.f));
	CHECK(pos.z == doctest::Approx(33.f));
}

TEST_CASE("A field skipped because the entity lacks the component does not desync the cursor for the next field")
{
	TwoWorlds tw;
	const FieldIndices position = RequireCatalogField(reflect::ComponentTypes(), "Transform", "position");

	// A second client entity, bound to a real net id, but with no TransformComponent -
	// the net id resolves, the component lookup does not.
	const Entity bare = tw.client.Create();
	tw.client.Emplace<net::NetworkIdentity>(bare).netId = 2;
	tw.clientSession.Bind(2, bare);

	net::ByteWriter w;
	w.U16(2);
	// Field 1: a real net id, but that entity has no Transform - skipped.
	w.U32(2);
	w.U16(position.componentIndex);
	w.U16(position.fieldIndex);
	net::WriteFieldValue(w, reflect::MakeValue(glm::vec3{-1.f, -1.f, -1.f}));
	// Field 2: the entity that actually has Transform, a distinctive value that must
	// still land. A broken skip-without-consuming implementation desyncs the cursor here.
	w.U32(1);
	w.U16(position.componentIndex);
	w.U16(position.fieldIndex);
	net::WriteFieldValue(w, reflect::MakeValue(glm::vec3{44.f, 55.f, 66.f}));
	const std::vector<std::byte> packet = w.Take();

	net::ApplySnapshot(tw.client, tw.schema, reflect::ComponentTypes(), tw.clientSession, packet);

	const auto* t = tw.client.TryGet<TransformComponent>(tw.clientEntity);
	REQUIRE(t != nullptr);
	const glm::vec3 pos = glm::vec3(t->localToWorld[3]);
	CHECK(pos.x == doctest::Approx(44.f));
	CHECK(pos.y == doctest::Approx(55.f));
	CHECK(pos.z == doctest::Approx(66.f));
}

TEST_CASE("A snapshot naming a non-replicated field is rejected, not applied")
{
	TwoWorlds tw;
	// "scale" is reflected (AE_FIELD_CUSTOM) but deliberately NOT marked AE_FIELD_REP -
	// it must not be reachable through the schema, even with valid catalog indices, a
	// valid net id, and an entity that actually has the component.
	const FieldIndices scale = RequireCatalogField(reflect::ComponentTypes(), "Transform", "scale");
	REQUIRE(scale.type == reflect::FieldType::Vec3);

	net::ByteWriter w;
	w.U16(1);
	w.U32(1);
	w.U16(scale.componentIndex);
	w.U16(scale.fieldIndex);
	net::WriteFieldValue(w, reflect::MakeValue(glm::vec3{2.f, 2.f, 2.f}));
	const std::vector<std::byte> packet = w.Take();

	net::ApplySnapshot(tw.client, tw.schema, reflect::ComponentTypes(), tw.clientSession, packet);

	const auto* t = tw.client.TryGet<TransformComponent>(tw.clientEntity);
	REQUIRE(t != nullptr);
	glm::vec3 p{};
	glm::vec3 e{};
	glm::vec3 s{};
	DecomposeTRS(t->localToWorld, p, e, s);
	CHECK(s.x == doctest::Approx(1.f));
	CHECK(s.y == doctest::Approx(1.f));
	CHECK(s.z == doctest::Approx(1.f));
}

TEST_CASE("A field skipped for not being replicated does not desync the cursor for the next field")
{
	TwoWorlds tw;
	const FieldIndices scale = RequireCatalogField(reflect::ComponentTypes(), "Transform", "scale");
	const FieldIndices position = RequireCatalogField(reflect::ComponentTypes(), "Transform", "position");

	net::ByteWriter w;
	w.U16(2);
	// Field 1: in range, entity has the component, but "scale" is not in the schema -
	// skipped.
	w.U32(1);
	w.U16(scale.componentIndex);
	w.U16(scale.fieldIndex);
	net::WriteFieldValue(w, reflect::MakeValue(glm::vec3{9.f, 9.f, 9.f}));
	// Field 2: a replicated field, a distinctive value that must still land.
	w.U32(1);
	w.U16(position.componentIndex);
	w.U16(position.fieldIndex);
	net::WriteFieldValue(w, reflect::MakeValue(glm::vec3{77.f, 88.f, 99.f}));
	const std::vector<std::byte> packet = w.Take();

	net::ApplySnapshot(tw.client, tw.schema, reflect::ComponentTypes(), tw.clientSession, packet);

	const auto* t = tw.client.TryGet<TransformComponent>(tw.clientEntity);
	REQUIRE(t != nullptr);
	const glm::vec3 pos = glm::vec3(t->localToWorld[3]);
	CHECK(pos.x == doctest::Approx(77.f));
	CHECK(pos.y == doctest::Approx(88.f));
	CHECK(pos.z == doctest::Approx(99.f));
}
