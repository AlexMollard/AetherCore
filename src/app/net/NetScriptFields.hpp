#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "net/NetSerialize.hpp"
#include "net/NetSession.hpp"
#include "net/NetSnapshot.hpp"
#include "scene/Components.hpp"
#include "scene/Entity.hpp"

namespace aether
{
	class World;
}

namespace aether::net
{
	// Script types are named on the wire by a 32-bit FNV-1a hash of the type name,
	// not by an index: a script assembly reloads independently of the C++ binary, so
	// an index would silently shift and start writing a different field. std::hash is
	// not stable across runs or implementations and must never be used here.
	[[nodiscard]] constexpr std::uint32_t ScriptTypeHash(std::string_view name)
	{
		std::uint32_t hash = 2166136261u; // FNV-1a 32-bit offset basis
		for (const char c: name)
		{
			hash ^= static_cast<std::uint32_t>(static_cast<unsigned char>(c));
			hash *= 16777619u; // FNV-1a 32-bit prime
		}
		return hash;
	}

	// Parks script fields on a componentIndex no catalog can reach, so a script
	// field and a component field can never collide in the shared SnapshotCache.
	inline constexpr std::uint16_t kScriptFieldComponentIndex = 0xFFFFu;

	// One replicated property of one script type, as the managed registry reports it.
	struct ScriptPropertyDesc
	{
		std::uint16_t index = 0; // position in the script type's property table
		ScriptPropertyValue::Type type = ScriptPropertyValue::Type::None;
	};

	// The managed scripting layer, reduced to what replication needs. Keeping it
	// abstract means the packet logic never links the CLR host, so it is testable
	// headless - and the concrete bridge (CSharpScriptFieldBridge) stays the only
	// place that knows about script instance handles.
	class ScriptFieldBridge
	{
	public:
		ScriptFieldBridge() = default;
		virtual ~ScriptFieldBridge() = default;
		ScriptFieldBridge(const ScriptFieldBridge&) = delete;
		ScriptFieldBridge& operator=(const ScriptFieldBridge&) = delete;
		ScriptFieldBridge(ScriptFieldBridge&&) = delete;
		ScriptFieldBridge& operator=(ScriptFieldBridge&&) = delete;

		// Properties of `typeName` marked [Replicated], in property-table order.
		// Empty when the type is unknown or the assembly is not loaded.
		[[nodiscard]] virtual std::vector<ScriptPropertyDesc> ReplicatedProperties(const std::string& typeName) const = 0;

		// Reads/writes one property of the live instance of ScriptComponent::scripts
		// [scriptIndex] on `entity`. Both go through the same by-index bridge the
		// inspector uses; replication adds no second value encoding.
		[[nodiscard]] virtual bool GetProperty(Entity entity, std::uint32_t scriptIndex, std::uint16_t propertyIndex,
		        ScriptPropertyValue& out) const = 0;
		virtual void SetProperty(Entity entity, std::uint32_t scriptIndex, std::uint16_t propertyIndex,
		        const ScriptPropertyValue& value) const = 0;
	};

	// Maps a managed property type onto the replication codec's field type. Returns
	// nothing for types replication cannot carry: None, and Entity/Component, whose
	// value is a local entity id that means nothing on another machine.
	[[nodiscard]] std::optional<reflect::FieldType> ScriptFieldType(ScriptPropertyValue::Type type);

	[[nodiscard]] reflect::FieldValue ToFieldValue(const ScriptPropertyValue& value, reflect::FieldType type);
	[[nodiscard]] ScriptPropertyValue ToScriptValue(const reflect::FieldValue& value, ScriptPropertyValue::Type type);

	// Sender: reads every replicated script field on every entity this peer
	// replicates and packs the changed ones. Returns empty when nothing changed -
	// callers must not send an empty packet. Layout mirrors the component snapshot:
	//
	//   u16 count, then per field: u32 netId, u32 scriptTypeHash, u16 propertyIndex,
	//   u8 fieldType, <value>
	//
	// Unlike the component snapshot the value's type IS on the wire. The component
	// schema is fixed by the C++ binary at both ends, so a receiver can always look a
	// field's type up; a script property table can differ between peers, and without
	// the tag a receiver that cannot resolve the target would not know how many bytes
	// to consume and would desync the rest of the packet.
	[[nodiscard]] std::vector<std::byte> BuildScriptFieldPacket(World& world, NetSession& session, SnapshotCache& cache,
	        const ScriptFieldBridge& bridge, const std::vector<Entity>& replicated);

	// Receiver. Ignores unknown net ids, entities `gate` refuses, entities with no
	// matching script, properties that are not replicated, and stale indices whose
	// type no longer matches - a peer can send anything, and a script assembly can
	// reload out from under it.
	//
	// The gate is the SAME one the component snapshot uses, and it must be: a
	// replicated script field is state exactly as much as a transform is, so a
	// client that could not move another player's body but could rewrite that
	// player's replicated script fields would be no better gated at all.
	void ApplyScriptFieldPacket(World& world, NetSession& session, const ScriptFieldBridge& bridge,
	        std::span<const std::byte> packet, const StateWriteGate& gate);
} // namespace aether::net
