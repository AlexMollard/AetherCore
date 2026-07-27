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

TEST_CASE("Transform position and rotation are replicated")
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
