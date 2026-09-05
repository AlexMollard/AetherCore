#include <doctest/doctest.h>

#include <algorithm>

#include "net/ReplicationSchema.hpp"
#include "scene/reflection/Reflection.hpp"

using namespace aether;

TEST_CASE("The schema contains only fields marked replicated")
{
	const net::ReplicationSchema schema = net::BuildReplicationSchema(reflect::ComponentTypes());

	REQUIRE_FALSE(schema.fields.empty()); // TransformComponent's position is marked

	for (const net::ReplicatedField& f: schema.fields)
	{
		const reflect::ComponentType& ct = reflect::ComponentTypes()[f.componentIndex];
		REQUIRE(f.fieldIndex < ct.fields.size());
		const reflect::FieldDesc& fd = ct.fields[f.fieldIndex];
		CHECK(fd.meta.replicated);
		CHECK(fd.type == f.type);
		CHECK(net::IsReplicableFieldType(fd.type));
	}
}

TEST_CASE("Transform position and euler are replicated")
{
	const net::ReplicationSchema schema = net::BuildReplicationSchema(reflect::ComponentTypes());

	auto hasField = [&](std::string_view component, std::string_view field)
	{
		return std::any_of(schema.fields.begin(), schema.fields.end(),
		        [&](const net::ReplicatedField& f)
		        {
			        const reflect::ComponentType& ct = reflect::ComponentTypes()[f.componentIndex];
			        return ct.name == component && ct.fields[f.fieldIndex].name == field;
		        });
	};

	CHECK(hasField("Transform", "position"));
	CHECK(hasField("Transform", "euler"));
}

TEST_CASE("A replicable-type check keeps EntityRef and List out of the schema")
{
	// Guards the rule rather than the current data: if someone marks an EntityRef
	// replicated, the schema must drop it rather than emit an unserializable field.
	CHECK_FALSE(net::IsReplicableFieldType(reflect::FieldType::EntityRef));
	CHECK_FALSE(net::IsReplicableFieldType(reflect::FieldType::List));
	CHECK(net::IsReplicableFieldType(reflect::FieldType::Vec3));
}

TEST_CASE("The schema hash is deterministic and distinguishes catalogs")
{
	// Snapshot packets carry this hash so a peer whose catalog differs refuses
	// them instead of decoding field indices against the wrong table. It is pure
	// FNV-1a over field positions and types - nothing run-specific - so the same
	// catalog must hash equal on every build and every run.
	CHECK(net::BuildReplicationSchema(reflect::ComponentTypes()).hash
	        == net::BuildReplicationSchema(reflect::ComponentTypes()).hash);

	reflect::ComponentType ct;
	ct.name = "SyntheticA";
	reflect::FieldDesc f;
	f.name = "a";
	f.type = reflect::FieldType::Float;
	f.meta.replicated = true;
	ct.fields.push_back(f);

	const net::ReplicationSchema one = net::BuildReplicationSchema({ct});
	CHECK(one.fields.size() == 1);

	// The minimal version skew: one replicated field added before an existing
	// one shifts every position after it.
	reflect::ComponentType shifted = ct;
	reflect::FieldDesc extra = f;
	extra.name = "b";
	shifted.fields.insert(shifted.fields.begin(), extra);
	CHECK(net::BuildReplicationSchema({shifted}).hash != one.hash);

	// A retyped field at the same position: same indices, different FieldType.
	reflect::ComponentType retyped = ct;
	retyped.fields[0].type = reflect::FieldType::String;
	CHECK(net::BuildReplicationSchema({retyped}).hash != one.hash);

	// Component order is registration order, which is fixed per binary but not
	// per source - two link orders are two different catalogs and must not agree.
	reflect::ComponentType ctB = ct;
	ctB.name = "SyntheticB";
	CHECK(net::BuildReplicationSchema({ct, ctB}).hash != net::BuildReplicationSchema({ctB, ct}).hash);
}
