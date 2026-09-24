#pragma once

#include <string>

#include <nlohmann/json.hpp>

#include "scene/reflection/Reflection.hpp"

namespace aether::editor
{
	// `field` is only needed for FieldType::List (to name each element's sub-fields);
	// scalar values ignore it, so element recursion passes nullptr.
	inline nlohmann::json FieldValueToJson(const reflect::FieldValue& v, const reflect::FieldDesc* field = nullptr)
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
			case FT::List:
			{
				nlohmann::json arr = nlohmann::json::array();
				const std::vector<reflect::ListElementDesc>* elems = field != nullptr ? &field->elementFields : nullptr;
				// Flat list (one unnamed element field) -> array of bare values; otherwise
				// an array of {sub-field: value} objects.
				const bool flat = elems != nullptr && elems->size() == 1 && (*elems)[0].name.empty();
				for (const std::vector<reflect::FieldValue>& row: v.list)
				{
					if (flat && !row.empty())
					{
						arr.push_back(FieldValueToJson(row[0]));
						continue;
					}
					nlohmann::json obj = nlohmann::json::object();
					for (std::size_t i = 0; i < row.size(); ++i)
					{
						const std::string name = (elems != nullptr && i < elems->size()) ? (*elems)[i].name : std::to_string(i);
						obj[name] = FieldValueToJson(row[i]);
					}
					arr.push_back(std::move(obj));
				}
				return arr;
			}
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
			case FT::List:
				if (j.is_array())
				{
					const bool flat = field.elementFields.size() == 1 && field.elementFields[0].name.empty();
					for (const nlohmann::json& elemJson: j)
					{
						if (flat)
						{
							reflect::FieldDesc ef;
							ef.type = field.elementFields[0].type;
							v.list.push_back({JsonToFieldValue(elemJson, ef)});
							continue;
						}
						std::vector<reflect::FieldValue> row;
						row.reserve(field.elementFields.size());
						for (const reflect::ListElementDesc& ed: field.elementFields)
						{
							reflect::FieldDesc ef;
							ef.name = ed.name;
							ef.type = ed.type;
							const auto it = elemJson.is_object() ? elemJson.find(ed.name) : elemJson.end();
							if (it != elemJson.end())
							{
								row.push_back(JsonToFieldValue(*it, ef));
							}
							else
							{
								reflect::FieldValue miss;
								miss.type = ed.type;
								row.push_back(std::move(miss));
							}
						}
						v.list.push_back(std::move(row));
					}
				}
				break;
		}
		return v;
	}

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
			case FT::List:
				return "list";
		}
		return "?";
	}
} // namespace aether::editor
