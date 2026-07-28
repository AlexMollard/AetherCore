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

	// ── The ownership gate ──────────────────────────────────────────────────────
	//
	// WHO IS ALLOWED TO WRITE WHAT, on the receiving end of a state packet. This is
	// the security boundary of the whole client-authoritative design, and it is a
	// PARAMETER of the apply rather than a check at the call site precisely because a
	// call site can forget one.
	//
	// State no longer travels in a single trusted direction. The peer that owns an
	// entity replicates it and the host relays, so one decoder now serves two very
	// different streams:
	//   - the host's snapshot, applied by a client. The host is authoritative for the
	//     session, so this is trusted wholesale (TrustAll).
	//   - a client's snapshot, applied by the host. A client is authoritative for
	//     exactly the entities it owns and for nothing else, so every field is checked
	//     against the sending connection (OwnedBy). Without this check any client
	//     could drive any other player's character by writing one net id.
	//
	// Deliberately NOT expressible as "an owner id, or none": kInvalidConnection is a
	// real owner value - it is how the host owns its own entities - so "no gate" needs
	// a flag of its own rather than a sentinel that also means "the host owns it".
	struct StateWriteGate
	{
		bool enforceOwnership = false;
		ConnectionId sender = kInvalidConnection;

		// The host's own state, landing on a client.
		[[nodiscard]] static StateWriteGate TrustAll()
		{
			return {};
		}

		// A client's state, landing on the host: only entities `sender` owns.
		[[nodiscard]] static StateWriteGate OwnedBy(ConnectionId sender)
		{
			return StateWriteGate{.enforceOwnership = true, .sender = sender};
		}

		// An entity with no NetworkIdentity is not a replicated entity at all, so a
		// gated sender addresses nothing by naming it and is refused.
		[[nodiscard]] bool Allows(World& world, Entity entity) const;
	};

	// Sender side. Returns an empty vector when nothing changed - callers must not
	// send an empty packet. `replicated` is whatever THIS peer is authoritative for
	// on this link; the two send paths (a host relaying to a connection, a client
	// uploading what it owns) differ only in that list.
	[[nodiscard]] std::vector<std::byte> BuildSnapshot(World& world, const ReplicationSchema& schema,
	        const std::vector<reflect::ComponentType>& catalog, NetSession& session, SnapshotCache& cache,
	        const std::vector<Entity>& replicated);

	// Receive side. Ignores unknown net ids, out-of-range indices, fields not present in
	// the replication schema, entities missing the component, entities `gate` refuses,
	// and truncated packets - a peer can send anything.
	void ApplySnapshot(World& world, const ReplicationSchema& schema,
	        const std::vector<reflect::ComponentType>& catalog, NetSession& session, std::span<const std::byte> packet,
	        const StateWriteGate& gate);
} // namespace aether::net
