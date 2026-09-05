#include <doctest/doctest.h>

#include <cmath>
#include <limits>

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

	net::ApplySnapshot(tw.client, tw.schema, reflect::ComponentTypes(), tw.clientSession, packet, net::StateWriteGate::TrustAll());

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
	w.U32(tw.schema.hash); // header: schema identity, then the field count
	w.U32(1);        // one field
	w.U32(9999);     // net id nobody has
	w.U16(position.componentIndex);
	w.U16(position.fieldIndex);
	net::WriteFieldValue(w, reflect::MakeValue(glm::vec3{1.f, 1.f, 1.f}));
	const std::vector<std::byte> hostile = w.Take();

	// Must not crash, and must leave the client world untouched.
	net::ApplySnapshot(tw.client, tw.schema, reflect::ComponentTypes(), tw.clientSession, hostile, net::StateWriteGate::TrustAll());

	const auto* t = tw.client.TryGet<TransformComponent>(tw.clientEntity);
	REQUIRE(t != nullptr);
	CHECK(glm::vec3(t->localToWorld[3]).x == doctest::Approx(0.f));
}

TEST_CASE("A truncated snapshot is rejected without applying a partial field")
{
	TwoWorlds tw;
	const FieldIndices position = RequireCatalogField(reflect::ComponentTypes(), "Transform", "position");

	net::ByteWriter w;
	w.U32(tw.schema.hash); // header: schema identity, then the field count
	w.U32(1);
	w.U32(1);
	w.U16(position.componentIndex);
	// field index and value missing
	const std::vector<std::byte> truncated = w.Take();

	net::ApplySnapshot(tw.client, tw.schema, reflect::ComponentTypes(), tw.clientSession, truncated, net::StateWriteGate::TrustAll());

	const auto* t = tw.client.TryGet<TransformComponent>(tw.clientEntity);
	REQUIRE(t != nullptr);
	CHECK(glm::vec3(t->localToWorld[3]).x == doctest::Approx(0.f));
}

TEST_CASE("A field skipped for an unknown net id does not desync the cursor for the next field")
{
	TwoWorlds tw;
	const FieldIndices position = RequireCatalogField(reflect::ComponentTypes(), "Transform", "position");

	net::ByteWriter w;
	w.U32(tw.schema.hash); // header: schema identity, then the field count
	w.U32(2); // two fields
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

	net::ApplySnapshot(tw.client, tw.schema, reflect::ComponentTypes(), tw.clientSession, packet, net::StateWriteGate::TrustAll());

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
	w.U32(tw.schema.hash); // header: schema identity, then the field count
	w.U32(2);
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

	net::ApplySnapshot(tw.client, tw.schema, reflect::ComponentTypes(), tw.clientSession, packet, net::StateWriteGate::TrustAll());

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
	w.U32(tw.schema.hash); // header: schema identity, then the field count
	w.U32(1);
	w.U32(1);
	w.U16(scale.componentIndex);
	w.U16(scale.fieldIndex);
	net::WriteFieldValue(w, reflect::MakeValue(glm::vec3{2.f, 2.f, 2.f}));
	const std::vector<std::byte> packet = w.Take();

	net::ApplySnapshot(tw.client, tw.schema, reflect::ComponentTypes(), tw.clientSession, packet, net::StateWriteGate::TrustAll());

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
	w.U32(tw.schema.hash); // header: schema identity, then the field count
	w.U32(2);
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

	net::ApplySnapshot(tw.client, tw.schema, reflect::ComponentTypes(), tw.clientSession, packet, net::StateWriteGate::TrustAll());

	const auto* t = tw.client.TryGet<TransformComponent>(tw.clientEntity);
	REQUIRE(t != nullptr);
	const glm::vec3 pos = glm::vec3(t->localToWorld[3]);
	CHECK(pos.x == doctest::Approx(77.f));
	CHECK(pos.y == doctest::Approx(88.f));
	CHECK(pos.z == doctest::Approx(99.f));
}

// The headline hostile case: a peer writes the NaN bit pattern for a Float/Vec
// field of an entity the gate lets it write. The value must never reach a
// component (interpolation and Physics2D never recover from NaN), and refusing
// it must not take the rest of the packet down: field 2 lands.
TEST_CASE("A non-finite field value from the wire is dropped, and the packet continues past it")
{
	TwoWorlds tw;
	const FieldIndices position = RequireCatalogField(reflect::ComponentTypes(), "Transform", "position");
	const FieldIndices euler = RequireCatalogField(reflect::ComponentTypes(), "Transform", "euler");
	REQUIRE(euler.type == reflect::FieldType::Vec3);

	const float nan = std::numeric_limits<float>::quiet_NaN();
	net::ByteWriter w;
	w.U32(tw.schema.hash);
	w.U32(2);
	w.U32(1);
	w.U16(position.componentIndex);
	w.U16(position.fieldIndex);
	net::WriteFieldValue(w, reflect::MakeValue(glm::vec3{nan, nan, nan}));
	w.U32(1);
	w.U16(euler.componentIndex);
	w.U16(euler.fieldIndex);
	net::WriteFieldValue(w, reflect::MakeValue(glm::vec3{20.f, 10.f, 5.f}));
	const std::vector<std::byte> packet = w.Take();

	net::ApplySnapshot(tw.client, tw.schema, reflect::ComponentTypes(), tw.clientSession, packet, net::StateWriteGate::TrustAll());

	const auto* t = tw.client.TryGet<TransformComponent>(tw.clientEntity);
	REQUIRE(t != nullptr);
	const glm::vec3 pos = glm::vec3(t->localToWorld[3]);
	CHECK(std::isfinite(pos.x));
	CHECK(pos.x == doctest::Approx(0.f)); // the field was refused, not applied

	glm::vec3 p{};
	glm::vec3 e{};
	glm::vec3 s{};
	DecomposeTRS(t->localToWorld, p, e, s);
	CHECK(e.x == doctest::Approx(20.f).epsilon(1e-3)); // ...and the field AFTER it landed
	CHECK(e.y == doctest::Approx(10.f).epsilon(1e-3));
}

// The sender side of the same coin, through the real builder: a locally-broken
// simulation (division by zero, whatever) produces NaN in its own transform. The
// writer must refuse to put it on the wire, and the refusal must not eat the
// entity's other fields or any other entity's.
TEST_CASE("A non-finite value never leaves the sender, and does not block other fields")
{
	TwoWorlds tw;
	net::SnapshotCache cache;

	// A second entity pair, so one packet carries a poisoned field and a good one.
	const Entity hostB = tw.host.Create();
	tw.host.Emplace<TransformComponent>(hostB);
	tw.host.Emplace<net::NetworkIdentity>(hostB).netId = 2;
	tw.hostSession.Bind(2, hostB);
	const Entity clientB = tw.client.Create();
	tw.client.Emplace<TransformComponent>(clientB);
	tw.client.Emplace<net::NetworkIdentity>(clientB).netId = 2;
	tw.clientSession.Bind(2, clientB);


	const float nan = std::numeric_limits<float>::quiet_NaN();
	tw.host.TryGet<TransformComponent>(tw.hostEntity)->localToWorld =
	        ComposeTransform({nan, nan, nan}, {0.f, 0.f, 0.f}, {1.f, 1.f, 1.f});
	tw.host.TryGet<TransformComponent>(hostB)->localToWorld =
	        ComposeTransform({5.f, 6.f, 7.f}, {0.f, 0.f, 0.f}, {1.f, 1.f, 1.f});

	const std::vector<std::byte> packet = net::BuildSnapshot(
	        tw.host, tw.schema, reflect::ComponentTypes(), tw.hostSession, cache, {tw.hostEntity, hostB});
	REQUIRE_FALSE(packet.empty());

	net::ApplySnapshot(tw.client, tw.schema, reflect::ComponentTypes(), tw.clientSession, packet, net::StateWriteGate::TrustAll());

	const auto* a = tw.client.TryGet<TransformComponent>(tw.clientEntity);
	REQUIRE(a != nullptr);
	const glm::vec3 posA = glm::vec3(a->localToWorld[3]);
	CHECK(std::isfinite(posA.x)); // before the writer guard this arrived as NaN
	CHECK(std::isfinite(posA.y));

	const auto* b = tw.client.TryGet<TransformComponent>(clientB);
	REQUIRE(b != nullptr);
	const glm::vec3 posB = glm::vec3(b->localToWorld[3]);
	CHECK(posB.x == doctest::Approx(5.f)); // the poisoned field did not eat B's
	CHECK(posB.z == doctest::Approx(7.f));
}

// The writer/reader string asymmetry: the writer used to emit any length while
// the reader refused strings past 64 KiB, so one oversized replicated string
// (a Net.SetPlayerName with no cap behind it) made every receiver drop the
// whole packet carrying it - permanently, since the cache had recorded it.
TEST_CASE("A replicated string past the reader's cap is never put on the wire")
{
	TwoWorlds tw;
	net::SnapshotCache cache;

	const Entity hostB = tw.host.Create();
	tw.host.Emplace<TransformComponent>(hostB);
	tw.host.Emplace<net::NetworkIdentity>(hostB).netId = 2;
	tw.hostSession.Bind(2, hostB);
	const Entity clientB = tw.client.Create();
	tw.client.Emplace<TransformComponent>(clientB);
	tw.client.Emplace<net::NetworkIdentity>(clientB).netId = 2;
	tw.clientSession.Bind(2, clientB);

	// 64 KiB is the wire cap (kMaxStringBytes); one byte past it is unsendable.
	tw.host.Emplace<net::NetPlayer>(tw.hostEntity).displayName = std::string(64u * 1024u + 1, 'x');
	tw.client.Emplace<net::NetPlayer>(tw.clientEntity); // the receiver that would carry the name
	tw.host.TryGet<TransformComponent>(hostB)->localToWorld =
	        ComposeTransform({8.f, 9.f, 10.f}, {0.f, 0.f, 0.f}, {1.f, 1.f, 1.f});

	const std::vector<std::byte> packet = net::BuildSnapshot(
	        tw.host, tw.schema, reflect::ComponentTypes(), tw.hostSession, cache, {tw.hostEntity, hostB});
	REQUIRE_FALSE(packet.empty()); // B's position is in there even though the name is not

	net::ApplySnapshot(tw.client, tw.schema, reflect::ComponentTypes(), tw.clientSession, packet, net::StateWriteGate::TrustAll());

	// Before the writer guard the oversized name killed the packet at that byte
	// and B's position never arrived.
	const auto* b = tw.client.TryGet<TransformComponent>(clientB);
	REQUIRE(b != nullptr);
	const glm::vec3 posB = glm::vec3(b->localToWorld[3]);
	CHECK(posB.x == doctest::Approx(8.f));
	CHECK(posB.z == doctest::Approx(10.f));

	// And the poisoned value did not occupy the field's cache slot: when the name
	// becomes legal again it must actually be sent and land.
	tw.host.TryGet<net::NetPlayer>(tw.hostEntity)->displayName = "recovered";
	const std::vector<std::byte> again = net::BuildSnapshot(
	        tw.host, tw.schema, reflect::ComponentTypes(), tw.hostSession, cache, {tw.hostEntity, hostB});
	REQUIRE_FALSE(again.empty());
	net::ApplySnapshot(tw.client, tw.schema, reflect::ComponentTypes(), tw.clientSession, again, net::StateWriteGate::TrustAll());
	const auto* player = tw.client.TryGet<net::NetPlayer>(tw.clientEntity);
	REQUIRE(player != nullptr);
	CHECK(player->displayName == "recovered");
}

TEST_CASE("A snapshot from a peer with a different schema hash is refused whole")
{
	// Under catalog skew (version-mismatched binaries, a mod registering a
	// component early) the sender's (component, field) indices name different
	// fields here, and the value cursor desyncs into garbage writes. The hash in
	// the header must refuse the packet before any of it is decoded.
	TwoWorlds tw;
	const FieldIndices position = RequireCatalogField(reflect::ComponentTypes(), "Transform", "position");

	net::ByteWriter w;
	w.U32(tw.schema.hash ^ 0xDEADBEEFu); // some other binary's schema
	w.U32(1);
	w.U32(1);
	w.U16(position.componentIndex);
	w.U16(position.fieldIndex);
	net::WriteFieldValue(w, reflect::MakeValue(glm::vec3{1.f, 2.f, 3.f}));
	const std::vector<std::byte> hostile = w.Take();

	net::ApplySnapshot(tw.client, tw.schema, reflect::ComponentTypes(), tw.clientSession, hostile, net::StateWriteGate::TrustAll());

	const auto* t = tw.client.TryGet<TransformComponent>(tw.clientEntity);
	REQUIRE(t != nullptr);
	CHECK(glm::vec3(t->localToWorld[3]).x == doctest::Approx(0.f)); // nothing applied
}
