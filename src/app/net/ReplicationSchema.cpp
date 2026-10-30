#include "net/ReplicationSchema.hpp"

#include <algorithm>
#include <numeric>

namespace aether::net
{
	ReplicationSchema BuildReplicationSchema(const std::vector<reflect::ComponentType>& catalog)
	{
		ReplicationSchema schema;
		std::vector<std::size_t> order(catalog.size());
		std::iota(order.begin(), order.end(), 0);
		std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
			return catalog[a].name < catalog[b].name;
		});
		for (const std::size_t c: order)
		{
			const reflect::ComponentType& type = catalog[c];
			const bool hasReplicatedField = std::any_of(type.fields.begin(), type.fields.end(), [](const reflect::FieldDesc& field) {
				return field.meta.replicated && IsReplicableFieldType(field.type);
			});
			if (!hasReplicatedField)
			{
				continue;
			}
			const auto wireIndex = static_cast<std::uint16_t>(schema.wireToCatalog.size());
			schema.wireToCatalog.push_back(static_cast<std::uint16_t>(c));
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
				        .wireComponentIndex = wireIndex,
				        .fieldIndex = static_cast<std::uint16_t>(f),
				        .type = field.type,
				});
				schema.fieldKeys.insert(PackFieldKey(wireIndex, static_cast<std::uint16_t>(f)));
			}
		}

	// Hash the canonical wire indices, names and types. Local catalog positions
	// differ across link orders and must not participate in the wire identity.
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
		hash = (hash ^ PackFieldKey(f.wireComponentIndex, f.fieldIndex)) * 0x01000193u;
		hash = (hash ^ static_cast<std::uint32_t>(f.type)) * 0x01000193u;
	}
	schema.hash = hash;
	return schema;
	}
} // namespace aether::net
