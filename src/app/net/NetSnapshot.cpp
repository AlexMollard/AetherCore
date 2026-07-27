#include "net/NetSnapshot.hpp"

#include <unordered_set>

#include "net/NetComponents.hpp"
#include "scene/World.hpp"

namespace aether::net
{
	namespace
	{
		// FieldValue has no operator==; compare only the members the field's type uses.
		bool SameValue(const reflect::FieldValue& a, const reflect::FieldValue& b)
		{
			using reflect::FieldType;
			if (a.type != b.type)
			{
				return false;
			}
			switch (a.type)
			{
			case FieldType::Float:
			case FieldType::Int:
			case FieldType::UInt:
				return a.num == b.num;
			case FieldType::Bool:
				return a.boolean == b.boolean;
			case FieldType::Vec2:
			case FieldType::Vec3:
			case FieldType::Vec4:
			case FieldType::Color3:
			case FieldType::Color4:
				return a.vec == b.vec;
			case FieldType::Enum:
				return a.enumValue == b.enumValue;
			case FieldType::String:
				return a.str == b.str;
			case FieldType::EntityRef:
			case FieldType::List:
				return true; // never replicated
			}
			return false;
		}

		// Packs a (componentIndex, fieldIndex) pair into one key for the replicated-field
		// lookup set. Both indices are std::uint16_t, so they fit side by side in 32 bits
		// with no collisions.
		std::uint32_t PackFieldKey(std::uint16_t componentIndex, std::uint16_t fieldIndex)
		{
			return (static_cast<std::uint32_t>(componentIndex) << 16) | fieldIndex;
		}
	} // namespace

	bool SnapshotCache::Changed(const FieldKey& key, const reflect::FieldValue& value)
	{
		const auto it = m_last.find(key);
		if (it != m_last.end() && SameValue(it->second, value))
		{
			return false;
		}
		m_last[key] = value;
		return true;
	}

	void SnapshotCache::Forget(std::uint32_t netId)
	{
		for (auto it = m_last.begin(); it != m_last.end();)
		{
			it = it->first.netId == netId ? m_last.erase(it) : std::next(it);
		}
	}

	void SnapshotCache::Clear()
	{
		m_last.clear();
	}

	std::vector<std::byte> BuildSnapshot(World& world, const ReplicationSchema& schema,
	        const std::vector<reflect::ComponentType>& catalog, NetSession& session, SnapshotCache& cache,
	        const std::vector<Entity>& relevant)
	{
		ByteWriter body;
		std::uint16_t count = 0;

		for (const Entity entity: relevant)
		{
			const std::uint32_t netId = session.NetIdFor(entity);
			if (netId == 0)
			{
				continue;
			}
			for (const ReplicatedField& field: schema.fields)
			{
				const reflect::ComponentType& type = catalog[field.componentIndex];
				const void* component = type.tryGetRawConst(world, entity);
				if (component == nullptr)
				{
					continue;
				}
				const reflect::FieldValue value = type.fields[field.fieldIndex].get(component);
				const FieldKey key{.netId = netId, .componentIndex = field.componentIndex, .fieldIndex = field.fieldIndex};
				if (!cache.Changed(key, value))
				{
					continue;
				}
				body.U32(netId);
				body.U16(field.componentIndex);
				body.U16(field.fieldIndex);
				WriteFieldValue(body, value);
				++count;
			}
		}

		if (count == 0)
		{
			return {};
		}

		ByteWriter packet;
		packet.U16(count);
		packet.Bytes(body.View());
		return packet.Take();
	}

	void ApplySnapshot(World& world, const ReplicationSchema& schema,
	        const std::vector<reflect::ComponentType>& catalog, NetSession& session, std::span<const std::byte> packet)
	{
		// The set of (componentIndex, fieldIndex) pairs actually marked AE_FIELD_REP.
		// Catalog bounds-checking alone lets a peer name ANY reflected field in the whole
		// engine; only fields in the schema may be written.
		std::unordered_set<std::uint32_t> replicatedFields;
		replicatedFields.reserve(schema.fields.size());
		for (const ReplicatedField& field: schema.fields)
		{
			replicatedFields.insert(PackFieldKey(field.componentIndex, field.fieldIndex));
		}

		ByteReader r{packet};
		const std::uint16_t count = r.U16();

		for (std::uint16_t i = 0; i < count; ++i)
		{
			const std::uint32_t netId = r.U32();
			const std::uint16_t componentIndex = r.U16();
			const std::uint16_t fieldIndex = r.U16();
			if (!r.Ok() || componentIndex >= catalog.size())
			{
				return; // malformed past this point; the rest of the packet is unparseable
			}

			const reflect::ComponentType& type = catalog[componentIndex];
			if (fieldIndex >= type.fields.size())
			{
				return;
			}
			const reflect::FieldDesc& field = type.fields[fieldIndex];

			// The value must be consumed even when the target is unknown or not
			// replicated, or the cursor desyncs and every remaining field in the packet
			// is garbage. The type to decode it with comes from the catalog field, which
			// is already known good at this point regardless of schema membership.
			const reflect::FieldValue value = ReadFieldValue(r, field.type);
			if (!r.Ok())
			{
				return;
			}

			if (!replicatedFields.contains(PackFieldKey(componentIndex, fieldIndex)))
			{
				continue; // in range, but not a field this schema allows a peer to set
			}

			const Entity entity = session.EntityFor(netId);
			if (!entity.IsValid())
			{
				continue;
			}
			void* component = type.tryGetRaw(world, entity);
			if (component == nullptr)
			{
				continue;
			}
			field.set(component, value);
		}
	}
} // namespace aether::net
