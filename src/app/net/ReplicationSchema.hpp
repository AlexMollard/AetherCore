#pragma once

#include <cstdint>
#include <unordered_set>
#include <vector>

#include "net/NetSerialize.hpp"
#include "scene/reflection/Reflection.hpp"

namespace aether::net
{
	// Packs a (componentIndex, fieldIndex) pair into one lookup key. Both indices are
	// std::uint16_t, so they sit side by side in 32 bits with no collisions.
	[[nodiscard]] constexpr std::uint32_t PackFieldKey(std::uint16_t componentIndex, std::uint16_t fieldIndex)
	{
		return (static_cast<std::uint32_t>(componentIndex) << 16) | fieldIndex;
	}

	// One replicated field, addressed by its position in the component catalog.
	// Both ends run the same binary's static registration, so the indices agree
	// without a handshake.
	struct ReplicatedField
	{
		std::uint16_t componentIndex = 0;
		std::uint16_t fieldIndex = 0;
		reflect::FieldType type = reflect::FieldType::Float;
	};

	// A flat table, deliberately: this is the structure a later optimisation swaps
	// for cached member offsets, and flat keeps that change contained.
	struct ReplicationSchema
	{
		std::vector<ReplicatedField> fields;
		// The same membership as `fields`, keyed for O(1) lookup. ApplySnapshot has to
		// answer "may a peer write this (component, field)?" for every entry in every
		// inbound packet - catalog bounds-checking alone would let a peer name ANY
		// reflected field in the engine - so the set is built once here rather than
		// rebuilt per packet. Kept in sync by BuildReplicationSchema, the only
		// producer.
		std::unordered_set<std::uint32_t> fieldKeys;

		[[nodiscard]] bool IsReplicated(std::uint16_t componentIndex, std::uint16_t fieldIndex) const
		{
			return fieldKeys.contains(PackFieldKey(componentIndex, fieldIndex));
		}
	};

	[[nodiscard]] ReplicationSchema BuildReplicationSchema(const std::vector<reflect::ComponentType>& catalog);
} // namespace aether::net
