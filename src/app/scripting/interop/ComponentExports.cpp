// Generic reflected-component access for gameplay scripts.
//
// Every component declared with AE_COMPONENT already carries a full field table -
// name, type, getter, setter - and that table is what drives the inspector, the scene
// serializer and the MCP get/set methods. This file makes the SAME table reachable from
// C#, so a script can read and write any reflected component's fields by name instead of
// waiting for someone to hand-write a bespoke export pair per component.
//
// Before this, a behaviour like Spin could be ADDED from script and never read, changed
// or removed again - a one-way door, and only for the four of seven that happened to have
// an Add helper at all.
//
// Runtime-safe by construction: reflection's core (Reflection.hpp) converts fields to a
// neutral FieldValue with no json/toml/imgui dependency, which is why the shipped runtime
// can link it. Nothing here touches ComponentCatalog or any other editor-only type.
#include "scripting/interop/InteropCommon.hpp"

#include <cstring>
#include <string>

#include "scene/Entity.hpp"
#include "scene/World.hpp"
#include "scene/reflection/Reflection.hpp"

using namespace aether::app::scripting::interop;

namespace
{
	using aether::reflect::ComponentType;
	using aether::reflect::FieldDesc;
	using aether::reflect::FieldType;
	using aether::reflect::FieldValue;

	const ComponentType* FindType(const char* type)
	{
		return type == nullptr ? nullptr : aether::reflect::FindComponentType(type);
	}

	// The component instance plus the field descriptor, resolved together because every
	// accessor needs both and either half can legitimately be absent (wrong entity, wrong
	// field name, component not present).
	struct Resolved
	{
		const ComponentType* type = nullptr;
		const FieldDesc* field = nullptr;
		void* instance = nullptr;

		[[nodiscard]] bool Ok() const
		{
			return type != nullptr && field != nullptr && instance != nullptr;
		}
	};

	Resolved Resolve(std::uint32_t id, const char* typeName, const char* fieldName)
	{
		Resolved r;
		r.type = FindType(typeName);
		if (r.type == nullptr || fieldName == nullptr || r.type->tryGetRaw == nullptr)
		{
			return r;
		}
		for (const FieldDesc& f: r.type->fields)
		{
			if (f.name == fieldName)
			{
				r.field = &f;
				break;
			}
		}
		if (r.field == nullptr)
		{
			return r;
		}
		r.instance = r.type->tryGetRaw(ActiveWorld(), aether::Entity{id});
		return r;
	}

	// Every numeric field collapses to a double across the ABI. Bool and Enum ride along
	// rather than getting their own export pair: the managed side already knows the field's
	// shape from the typed wrapper it is standing behind.
	bool ReadNumber(const FieldValue& v, double& out)
	{
		switch (v.type)
		{
			case FieldType::Float:
			case FieldType::Int:
			case FieldType::UInt:
				out = v.num;
				return true;
			case FieldType::Bool:
				out = v.boolean ? 1.0 : 0.0;
				return true;
			case FieldType::Enum:
				out = static_cast<double>(v.enumValue);
				return true;
			// Named rather than left to the default, because -Wswitch-enum is an error here on
			// purpose: it is what makes adding a field type a compile failure in every switch
			// that has to decide about it, instead of a silent fallthrough discovered later.
			case FieldType::Vec2:
			case FieldType::Vec3:
			case FieldType::Vec4:
			case FieldType::Color3:
			case FieldType::Color4:
			case FieldType::String:
			case FieldType::EntityRef:
			case FieldType::List:
				return false;
		}
		return false;
	}

	bool WriteNumber(FieldValue& v, double value)
	{
		switch (v.type)
		{
			case FieldType::Float:
			case FieldType::Int:
			case FieldType::UInt:
				v.num = value;
				return true;
			case FieldType::Bool:
				v.boolean = value != 0.0;
				return true;
			case FieldType::Enum:
				v.enumValue = static_cast<int>(value);
				return true;
			// Named for the same reason as in ReadNumber above.
			case FieldType::Vec2:
			case FieldType::Vec3:
			case FieldType::Vec4:
			case FieldType::Color3:
			case FieldType::Color4:
			case FieldType::String:
			case FieldType::EntityRef:
			case FieldType::List:
				return false;
		}
		return false;
	}

	bool IsVectorField(FieldType t)
	{
		return t == FieldType::Vec2 || t == FieldType::Vec3 || t == FieldType::Vec4 || t == FieldType::Color3 || t == FieldType::Color4;
	}
} // namespace

AE_SCRIPT_API int aether_component_has(std::uint32_t id, const char* type)
{
	const ComponentType* ct = FindType(type);
	if (ct == nullptr || ct->has == nullptr)
	{
		return 0;
	}
	return ct->has(ActiveWorld(), aether::Entity{id}) ? 1 : 0;
}

// Adds with the component's declared defaults, or does nothing if it is already there -
// so calling this from OnAttach is idempotent across a hot reload.
AE_SCRIPT_API int aether_component_add(std::uint32_t id, const char* type)
{
	const ComponentType* ct = FindType(type);
	if (ct == nullptr || ct->emplaceDefault == nullptr)
	{
		return 0;
	}
	aether::World& world = ActiveWorld();
	const aether::Entity entity{id};
	if (ct->has != nullptr && ct->has(world, entity))
	{
		return 1;
	}
	return ct->emplaceDefault(world, entity) != nullptr ? 1 : 0;
}

AE_SCRIPT_API int aether_component_remove(std::uint32_t id, const char* type)
{
	const ComponentType* ct = FindType(type);
	if (ct == nullptr || ct->remove == nullptr)
	{
		return 0;
	}
	ct->remove(ActiveWorld(), aether::Entity{id});
	return 1;
}

AE_SCRIPT_API int aether_component_get_number(std::uint32_t id, const char* type, const char* field, double* out)
{
	const Resolved r = Resolve(id, type, field);
	if (!r.Ok() || out == nullptr || r.field->get == nullptr)
	{
		return 0;
	}
	const FieldValue v = r.field->get(r.instance);
	return ReadNumber(v, *out) ? 1 : 0;
}

AE_SCRIPT_API int aether_component_set_number(std::uint32_t id, const char* type, const char* field, double value)
{
	const Resolved r = Resolve(id, type, field);
	if (!r.Ok() || r.field->get == nullptr || r.field->set == nullptr)
	{
		return 0;
	}
	// Round-trip through the current value so the FieldValue keeps its declared type -
	// the setter dispatches on it, and a default-constructed one would silently write a
	// Float into an Enum.
	FieldValue v = r.field->get(r.instance);
	if (!WriteNumber(v, value))
	{
		return 0;
	}
	r.field->set(r.instance, v);
	if (r.type->postSet != nullptr)
	{
		r.type->postSet(ActiveWorld(), aether::Entity{id});
	}
	return 1;
}

// Vec2/Vec3/Vec4 and both colour types share one export: they are all a glm::vec4 behind
// the FieldValue, and the managed wrapper knows how many components it asked for.
AE_SCRIPT_API int aether_component_get_vector(std::uint32_t id, const char* type, const char* field, Vec4* out)
{
	const Resolved r = Resolve(id, type, field);
	if (!r.Ok() || out == nullptr || r.field->get == nullptr)
	{
		return 0;
	}
	const FieldValue v = r.field->get(r.instance);
	if (!IsVectorField(v.type))
	{
		return 0;
	}
	*out = Vec4{v.vec.x, v.vec.y, v.vec.z, v.vec.w};
	return 1;
}

AE_SCRIPT_API int aether_component_set_vector(std::uint32_t id, const char* type, const char* field, Vec4 value)
{
	const Resolved r = Resolve(id, type, field);
	if (!r.Ok() || r.field->get == nullptr || r.field->set == nullptr)
	{
		return 0;
	}
	FieldValue v = r.field->get(r.instance);
	if (!IsVectorField(v.type))
	{
		return 0;
	}
	v.vec = glm::vec4(value.x, value.y, value.z, value.w);
	r.field->set(r.instance, v);
	if (r.type->postSet != nullptr)
	{
		r.type->postSet(ActiveWorld(), aether::Entity{id});
	}
	return 1;
}

// Returns bytes written, or -1 when the field is not a string. Truncates rather than
// failing, matching the other string-returning exports.
AE_SCRIPT_API int aether_component_get_string(std::uint32_t id, const char* type, const char* field, char* buffer, int capacity)
{
	const Resolved r = Resolve(id, type, field);
	if (!r.Ok() || buffer == nullptr || capacity <= 0 || r.field->get == nullptr)
	{
		return -1;
	}
	const FieldValue v = r.field->get(r.instance);
	if (v.type != FieldType::String)
	{
		return -1;
	}
	const int written = static_cast<int>(v.str.size() < static_cast<std::size_t>(capacity) ? v.str.size() : static_cast<std::size_t>(capacity));
	std::memcpy(buffer, v.str.data(), static_cast<std::size_t>(written));
	return written;
}

AE_SCRIPT_API int aether_component_set_string(std::uint32_t id, const char* type, const char* field, const char* value)
{
	const Resolved r = Resolve(id, type, field);
	if (!r.Ok() || value == nullptr || r.field->get == nullptr || r.field->set == nullptr)
	{
		return 0;
	}
	FieldValue v = r.field->get(r.instance);
	if (v.type != FieldType::String)
	{
		return 0;
	}
	v.str = value;
	r.field->set(r.instance, v);
	if (r.type->postSet != nullptr)
	{
		r.type->postSet(ActiveWorld(), aether::Entity{id});
	}
	return 1;
}
