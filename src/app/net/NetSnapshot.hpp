#pragma once

#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

#include "net/NetSerialize.hpp"
#include "net/NetSession.hpp"
#include "net/ReplicationSchema.hpp"
#include "scene/Entity.hpp"

namespace aether
{
	class World;
}

namespace aether::net
{
	// Identifies one replicated field on one networked entity. Component fields leave
	// scriptTypeHash at 0; replicated C# script fields set it (and park componentIndex
	// on a sentinel) so both kinds share one change-detection cache with no aliasing.
	struct FieldKey
	{
		std::uint32_t netId = 0;
		std::uint16_t componentIndex = 0;
		std::uint16_t fieldIndex = 0;
		std::uint32_t scriptTypeHash = 0;

		friend bool operator==(const FieldKey& a, const FieldKey& b)
		{
			return a.netId == b.netId && a.componentIndex == b.componentIndex && a.fieldIndex == b.fieldIndex
			       && a.scriptTypeHash == b.scriptTypeHash;
		}
	};
} // namespace aether::net

template<>
struct std::hash<aether::net::FieldKey>
{
	std::size_t operator()(const aether::net::FieldKey& k) const noexcept
	{
		return (static_cast<std::size_t>(k.netId) * 1315423911u) ^ (static_cast<std::size_t>(k.componentIndex) << 16)
		       ^ static_cast<std::size_t>(k.fieldIndex) ^ (static_cast<std::size_t>(k.scriptTypeHash) * 2654435761u);
	}
};

namespace aether::net
{
	// Last value sent per field, so a snapshot carries only what actually changed.
	// This is per-field change detection, NOT delta-against-acknowledged: a dropped
	// snapshot is not resent, the next change simply includes the field again.
	class SnapshotCache
	{
	public:
		// Records `value` and returns true if it differs from the last recorded one
		// (or if this field has never been seen).
		bool Changed(const FieldKey& key, const reflect::FieldValue& value);
		void Forget(std::uint32_t netId);
		void Clear();

	private:
		std::unordered_map<FieldKey, reflect::FieldValue> m_last;
	};

	// Host side. Returns an empty vector when nothing changed - callers must not
	// send an empty packet.
	[[nodiscard]] std::vector<std::byte> BuildSnapshot(World& world, const ReplicationSchema& schema,
	        const std::vector<reflect::ComponentType>& catalog, NetSession& session, SnapshotCache& cache,
	        const std::vector<Entity>& relevant);

	// Client side. Ignores unknown net ids, out-of-range indices, fields not present in
	// the replication schema, entities missing the component, and truncated packets - a
	// peer can send anything.
	void ApplySnapshot(World& world, const ReplicationSchema& schema,
	        const std::vector<reflect::ComponentType>& catalog, NetSession& session, std::span<const std::byte> packet);
} // namespace aether::net
