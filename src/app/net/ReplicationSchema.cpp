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
		return schema;
	}
} // namespace aether::net
