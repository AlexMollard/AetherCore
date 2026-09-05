#include "net/NetScriptFields.hpp"

#include <unordered_set>

#include "net/NetComponents.hpp"
#include "scene/World.hpp"

namespace aether::net
{
	namespace
	{
		// Decodes the on-wire field-type tag. Anything that is not a replicable type
		// has no defined byte length, so it cannot be skipped - the caller must give
		// up on the whole packet rather than guess.
		std::optional<reflect::FieldType> DecodeFieldType(std::uint8_t tag)
		{
			const auto type = static_cast<reflect::FieldType>(tag);
			if (tag > static_cast<std::uint8_t>(reflect::FieldType::List) || !IsReplicableFieldType(type))
			{
				return std::nullopt;
			}
			return type;
		}

		// The first script entry on `entity` whose type name hashes to `typeHash`.
		// Two entries of the same type on one entity are indistinguishable on the wire,
		// so the first wins here and BuildScriptFieldPacket only ever emits the first.
		std::optional<std::uint32_t> FindScriptIndex(const ScriptComponent& scripts, std::uint32_t typeHash)
		{
			for (std::size_t i = 0; i < scripts.scripts.size(); ++i)
			{
				if (ScriptTypeHash(scripts.scripts[i].path) == typeHash)
				{
					return static_cast<std::uint32_t>(i);
				}
			}
			return std::nullopt;
		}
	} // namespace

	std::optional<reflect::FieldType> ScriptFieldType(ScriptPropertyValue::Type type)
	{
		using PT = ScriptPropertyValue::Type;
		switch (type)
		{
		case PT::Float:
			return reflect::FieldType::Float;
		case PT::Int:
			return reflect::FieldType::Int;
		case PT::Bool:
			return reflect::FieldType::Bool;
		case PT::Vector3:
			return reflect::FieldType::Vec3;
		case PT::String:
			return reflect::FieldType::String;
		case PT::Enum:
			return reflect::FieldType::Enum;
		case PT::None:
		case PT::Entity:
		case PT::Component:
			return std::nullopt; // an entity id is meaningless on another machine
		}
		return std::nullopt;
	}

	reflect::FieldValue ToFieldValue(const ScriptPropertyValue& value, reflect::FieldType type)
	{
		reflect::FieldValue out;
		out.type = type;
		switch (type)
		{
		case reflect::FieldType::Float:
			out.num = value.f4[0];
			break;
		case reflect::FieldType::Int:
			out.num = static_cast<double>(value.i64);
			break;
		case reflect::FieldType::Bool:
			out.boolean = value.i64 != 0;
			break;
		case reflect::FieldType::Vec3:
			out.vec = glm::vec4(value.f4[0], value.f4[1], value.f4[2], 0.f);
			break;
		case reflect::FieldType::String:
			out.str = value.str;
			break;
		case reflect::FieldType::Enum:
			out.enumValue = static_cast<int>(value.i64);
			break;
		// ScriptFieldType never maps a script property onto any of these, so there is
		// no managed value shape to convert from.
		case reflect::FieldType::UInt:
		case reflect::FieldType::Vec2:
		case reflect::FieldType::Vec4:
		case reflect::FieldType::Color3:
		case reflect::FieldType::Color4:
		case reflect::FieldType::EntityRef:
		case reflect::FieldType::List:
			break;
		}
		return out;
	}

	ScriptPropertyValue ToScriptValue(const reflect::FieldValue& value, ScriptPropertyValue::Type type)
	{
		ScriptPropertyValue out;
		out.type = type;
		switch (type)
		{
		case ScriptPropertyValue::Type::Float:
			out.f4[0] = static_cast<float>(value.num);
			break;
		case ScriptPropertyValue::Type::Int:
			out.i64 = static_cast<std::int64_t>(value.num);
			break;
		case ScriptPropertyValue::Type::Bool:
			out.i64 = value.boolean ? 1 : 0;
			break;
		case ScriptPropertyValue::Type::Vector3:
			out.f4[0] = value.vec.x;
			out.f4[1] = value.vec.y;
			out.f4[2] = value.vec.z;
			break;
		case ScriptPropertyValue::Type::String:
			out.str = value.str;
			break;
		case ScriptPropertyValue::Type::Enum:
			out.i64 = value.enumValue;
			break;
		// Never replicated (see ScriptFieldType); callers filter these out first.
		case ScriptPropertyValue::Type::None:
		case ScriptPropertyValue::Type::Entity:
		case ScriptPropertyValue::Type::Component:
			break;
		}
		return out;
	}

	std::vector<std::byte> BuildScriptFieldPacket(World& world, NetSession& session, SnapshotCache& cache,
	        const ScriptFieldBridge& bridge, const std::vector<Entity>& replicated)
	{
		ByteWriter body;
		std::uint32_t count = 0;

		for (const Entity entity: replicated)
		{
			const std::uint32_t netId = session.NetIdFor(entity);
			if (netId == 0)
			{
				continue;
			}
			const auto* scripts = world.TryGet<ScriptComponent>(entity);
			if (scripts == nullptr)
			{
				continue;
			}

			// A second script of the same type on one entity would share every wire
			// key with the first: the same cache slot (so the two values would fight
			// each other every frame) and the same receiver-side target. Replicate the
			// first and leave the rest local.
			std::unordered_set<std::uint32_t> seenTypes;

			for (std::size_t scriptIndex = 0; scriptIndex < scripts->scripts.size(); ++scriptIndex)
			{
				const std::string& typeName = scripts->scripts[scriptIndex].path;
				const std::uint32_t typeHash = ScriptTypeHash(typeName);
				if (!seenTypes.insert(typeHash).second)
				{
					continue;
				}

				for (const ScriptPropertyDesc& prop: bridge.ReplicatedProperties(typeName))
				{
					const std::optional<reflect::FieldType> fieldType = ScriptFieldType(prop.type);
					if (!fieldType.has_value())
					{
						continue;
					}
					ScriptPropertyValue raw;
					if (!bridge.GetProperty(entity, static_cast<std::uint32_t>(scriptIndex), prop.index, raw))
					{
						continue;
					}
					const reflect::FieldValue value = ToFieldValue(raw, *fieldType);
					const FieldKey key{
					        .netId = netId,
					        .componentIndex = kScriptFieldComponentIndex,
					        .fieldIndex = prop.index,
					        .scriptTypeHash = typeHash,
					};
					// Refused BEFORE cache.Changed records it: a value the reader
					// would reject (non-finite float, string past its cap) must not
					// be emitted at all, and must not be remembered as sent - the
					// cache is what would otherwise re-emit it in every later
					// packet, aborting each one at this field and losing every
					// field written after it. Recovery to a legal value still
					// registers as a change, because nothing was recorded.
					if (!IsSendableFieldValue(value))
					{
						continue;
					}
					if (!cache.Changed(key, value))
					{
						continue;
					}
					body.U32(netId);
					body.U32(typeHash);
					body.U16(prop.index);
					body.U8(static_cast<std::uint8_t>(*fieldType));
					WriteFieldValue(body, value);
					++count;
				}
			}
		}

		if (count == 0)
		{
			return {};
		}

		ByteWriter packet;
		packet.U32(count);
		packet.Bytes(body.View());
		return packet.Take();
	}

	void ApplyScriptFieldPacket(World& world, NetSession& session, const ScriptFieldBridge& bridge,
	        std::span<const std::byte> packet, const StateWriteGate& gate)
	{
		ByteReader r{packet};
		const std::uint32_t count = r.U32();

		for (std::uint32_t i = 0; i < count; ++i)
		{
			const std::uint32_t netId = r.U32();
			const std::uint32_t typeHash = r.U32();
			const std::uint16_t propertyIndex = r.U16();
			const std::uint8_t typeTag = r.U8();
			if (!r.Ok())
			{
				return; // truncated header; the rest of the packet is unparseable
			}
			const std::optional<reflect::FieldType> fieldType = DecodeFieldType(typeTag);
			if (!fieldType.has_value())
			{
				return; // unknown tag means unknown value length - cannot skip safely
			}

			// The value is consumed before every skip below. Skipping without reading
			// it desyncs the cursor and turns every later field in the packet into
			// garbage, so nothing past this point may `continue` before this line.
			const reflect::FieldValue value = ReadFieldValue(r, *fieldType);
			if (!r.Ok())
			{
				return;
			}

			const Entity entity = session.EntityFor(netId);
			if (!entity.IsValid())
			{
				continue;
			}
			// The same ownership gate the component snapshot runs - see its note in
			// NetSnapshot.hpp. A client owns its player's AnimState; it owns nobody
			// else's.
			if (!gate.Allows(world, entity))
			{
				continue;
			}
			const auto* scripts = world.TryGet<ScriptComponent>(entity);
			if (scripts == nullptr)
			{
				continue;
			}
			const std::optional<std::uint32_t> scriptIndex = FindScriptIndex(*scripts, typeHash);
			if (!scriptIndex.has_value())
			{
				continue;
			}

			// Only properties this peer also considers replicated may be written, and
			// only when the sender's type still matches ours. A script assembly that
			// reloaded with reordered or retyped fields therefore drops the field
			// rather than writing the wrong one.
			const std::string& typeName = scripts->scripts[*scriptIndex].path;
			ScriptPropertyValue::Type propType = ScriptPropertyValue::Type::None;
			for (const ScriptPropertyDesc& prop: bridge.ReplicatedProperties(typeName))
			{
				if (prop.index == propertyIndex)
				{
					propType = prop.type;
					break;
				}
			}
			const std::optional<reflect::FieldType> localType = ScriptFieldType(propType);
			if (!localType.has_value() || *localType != *fieldType)
			{
				continue;
			}

			bridge.SetProperty(entity, *scriptIndex, propertyIndex, ToScriptValue(value, propType));
		}
	}
} // namespace aether::net
