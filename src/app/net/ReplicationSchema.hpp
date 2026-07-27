#pragma once

#include <cstdint>
#include <vector>

#include "net/NetSerialize.hpp"
#include "scene/reflection/Reflection.hpp"

namespace aether::net
{
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
	};

	[[nodiscard]] ReplicationSchema BuildReplicationSchema(const std::vector<reflect::ComponentType>& catalog);
} // namespace aether::net
