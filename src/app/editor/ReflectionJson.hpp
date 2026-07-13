#pragma once

// Adapts the reflection core's neutral FieldValue to/from JSON for the MCP control
// server. Editor-only (JSON is a control-server dependency; the reflection core
// stays JSON-free so it can compile into GameRuntime).

#include <string>

#include <nlohmann/json.hpp>

#include "scene/reflection/Reflection.hpp"

namespace aether::editor
{
	inline nlohmann::json FieldValueToJson(const reflect::FieldValue& v)
	{
		using FT = reflect::FieldType;
		switch (v.type)
		{
			case FT::Float:
				return v.num;
			case FT::Int:
				return static_cast<std::int64_t>(v.num);
			case FT::UInt:
				return static_cast<std::uint64_t>(v.num < 0.0 ? 0.0 : v.num);
			case FT::Bool:
				return v.boolean;
			case FT::Vec2:
				return nlohmann::json::array({v.vec.x, v.vec.y});
			case FT::Vec3:
			case FT::Color3:
				return nlohmann::json::array({v.vec.x, v.vec.y, v.vec.z});
			case FT::Vec4:
			case FT::Color4:
				return nlohmann::json::array({v.vec.x, v.vec.y, v.vec.z, v.vec.w});
			case FT::Enum:
				return v.enumValue;
			case FT::String:
				return v.str;
			case FT::EntityRef:
				return v.entity;
		}
		return nullptr;
	}

	inline reflect::FieldValue JsonToFieldValue(const nlohmann::json& j, const reflect::FieldDesc& field)
	{
		using FT = reflect::FieldType;
		reflect::FieldValue v;
		v.type = field.type;
		switch (field.type)
		{
			case FT::Float:
			case FT::Int:
			case FT::UInt:
				if (j.is_number())
				{
					v.num = j.get<double>();
				}
				break;
			case FT::Bool:
				if (j.is_boolean())
				{
					v.boolean = j.get<bool>();
				}
				break;
			case FT::Vec2:
			case FT::Vec3:
			case FT::Vec4:
			case FT::Color3:
			case FT::Color4:
				if (j.is_array())
				{
					for (std::size_t i = 0; i < j.size() && i < 4; ++i)
					{
						if (j[i].is_number())
						{
							v.vec[static_cast<int>(i)] = j[i].get<float>();
						}
					}
				}
				break;
			case FT::Enum:
				if (j.is_number())
				{
					v.enumValue = j.get<int>();
				}
				else if (j.is_string() && field.meta.enumTable != nullptr)
				{
					v.enumValue = field.meta.enumTable->ValueOf(j.get<std::string>(), 0);
				}
				break;
			case FT::String:
				if (j.is_string())
				{
					v.str = j.get<std::string>();
				}
				break;
			case FT::EntityRef:
				if (j.is_number())
				{
					v.entity = j.get<std::uint64_t>();
				}
				break;
		}
		return v;
	}

	// "name:type" hint string for list_component_types / discovery.
	inline std::string FieldTypeName(reflect::FieldType t)
	{
		using FT = reflect::FieldType;
		switch (t)
		{
			case FT::Float:
				return "float";
			case FT::Int:
				return "int";
			case FT::UInt:
				return "uint";
			case FT::Bool:
				return "bool";
			case FT::Vec2:
				return "vec2";
			case FT::Vec3:
				return "vec3";
			case FT::Vec4:
				return "vec4";
			case FT::Color3:
				return "color3";
			case FT::Color4:
				return "color4";
			case FT::Enum:
				return "enum";
			case FT::String:
				return "string";
			case FT::EntityRef:
				return "entity";
		}
		return "?";
	}
} // namespace aether::editor
