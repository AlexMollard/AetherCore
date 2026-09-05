#include "net/ReplicationSchema.hpp"

namespace aether::net
{
	ReplicationSchema BuildReplicationSchema(const std::vector<reflect::ComponentType>& catalog)
	{
		ReplicationSchema schema;
		for (std::size_t c = 0; c < catalog.size(); ++c)
		{
			const reflect::ComponentType& type = catalog[c];
			for (std::size_t f = 0; f < type.fields.size(); ++f)
			{
				const reflect::FieldDesc& field = type.fields[f];
				// A field can be marked replicated but hold a type replication cannot
				// carry (an entity ref means nothing on another machine). Drop it here
				// rather than emit a field the codec would silently skip.
				if (!field.meta.replicated || !IsReplicableFieldType(field.type))
				{
					continue;
				}
				schema.fields.push_back(ReplicatedField{
				        .componentIndex = static_cast<std::uint16_t>(c),
				        .fieldIndex = static_cast<std::uint16_t>(f),
				        .type = field.type,
				});
				schema.fieldKeys.insert(PackFieldKey(static_cast<std::uint16_t>(c), static_cast<std::uint16_t>(f)));
			}
		}

	// FNV-1a over every replicated field's component name, field name, packed
	// (component, field) indices and type, in schema order. ApplySnapshot resolves
	// a packet's indices against the RECEIVER's catalog, so the hash has to cover
	// everything that resolution depends on: names catch a same-shaped component
	// swapped into another's slot (identical field lists hash equal without
	// them), indices catch an insertion or reorder, and the type tag catches a
	// retyped field - any of which would otherwise write garbage into unrelated
	// components. Same parameters as ScriptTypeHash, which solves the same
	// problem for script type names.
	std::uint32_t hash = 0x811c9dc5u;
	for (const ReplicatedField& f: schema.fields)
	{
		const reflect::ComponentType& type = catalog[f.componentIndex];
		const reflect::FieldDesc& field = type.fields[f.fieldIndex];
		for (const char ch: type.name)
		{
			hash = (hash ^ static_cast<std::uint32_t>(static_cast<unsigned char>(ch))) * 0x01000193u;
		}
		for (const char ch: field.name)
		{
			hash = (hash ^ static_cast<std::uint32_t>(static_cast<unsigned char>(ch))) * 0x01000193u;
		}
		hash = (hash ^ PackFieldKey(f.componentIndex, f.fieldIndex)) * 0x01000193u;
		hash = (hash ^ static_cast<std::uint32_t>(f.type)) * 0x01000193u;
	}
	schema.hash = hash;
	return schema;
	}
} // namespace aether::net
