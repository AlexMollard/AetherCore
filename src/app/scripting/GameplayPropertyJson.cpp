#include "scripting/GameplayPropertyJson.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace aether::app::scripting
{
	namespace
	{
		bool ReadInteger(const nlohmann::json& value, std::int64_t& out)
		{
			if (!value.is_number_integer())
			{
				return false;
			}
			if (value.is_number_unsigned() && value.get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
			{
				return false;
			}
			out = value.get<std::int64_t>();
			return true;
		}

		bool ReadFloat(const nlohmann::json& value, float& out)
		{
			if (!value.is_number())
			{
				return false;
			}
			const double number = value.get<double>();
			if (!std::isfinite(number) || std::abs(number) > std::numeric_limits<float>::max())
			{
				return false;
			}
			out = static_cast<float>(number);
			return true;
		}

		bool ReadReference(const nlohmann::json& value, std::int64_t& out)
		{
			return ReadInteger(value, out) && out >= 0 && static_cast<std::uint64_t>(out) <= std::numeric_limits<std::uint32_t>::max();
		}
	} // namespace

	Expected<std::map<std::string, ScriptPropertyValue>> ParseGameplayScriptProperties(const nlohmann::json& input, std::span<const ScriptPropertyInfo> declarations, bool requireDeclared)
	{
		if (!input.is_object())
		{
			return std::unexpected(AetherError::Engine("Script properties must be an object."));
		}
		std::map<std::string, ScriptPropertyValue> properties;
		for (const auto& [name, value]: input.items())
		{
			ScriptPropertyValue property;
			const auto declaration = std::ranges::find(declarations, name, &ScriptPropertyInfo::name);
			if (requireDeclared || !declarations.empty())
			{
				if (declaration == declarations.end())
				{
					return std::unexpected(AetherError::Engine("Unknown script property '" + name + "'."));
				}
				property.type = declaration->type;
			}
			else if (value.is_boolean())
			{
				property.type = ScriptPropertyValue::Type::Bool;
			}
			else if (value.is_number_integer())
			{
				property.type = ScriptPropertyValue::Type::Int;
			}
			else if (value.is_number())
			{
				property.type = ScriptPropertyValue::Type::Float;
			}
			else if (value.is_string())
			{
				property.type = ScriptPropertyValue::Type::String;
			}
			else if (value.is_array() && value.size() == 3)
			{
				property.type = ScriptPropertyValue::Type::Vector3;
			}
			bool valid = false;
			switch (property.type)
			{
				case ScriptPropertyValue::Type::Float:
					valid = ReadFloat(value, property.f4[0]);
					break;
				case ScriptPropertyValue::Type::Int:
				case ScriptPropertyValue::Type::Enum:
					valid = ReadInteger(value, property.i64);
					break;
				case ScriptPropertyValue::Type::Entity:
					valid = ReadReference(value, property.i64);
					break;
				case ScriptPropertyValue::Type::Component:
					property.str = declaration->componentType;
					if (value.is_object())
					{
						valid = value.size() == 2 && value.contains("entity") && value.contains("component") && value["component"].is_string() && ReadReference(value["entity"], property.i64);
						if (valid)
						{
							const auto component = value["component"].get<std::string>();
							valid = !component.empty() && (property.str.empty() || property.str == component);
							property.str = component;
						}
					}
					else
					{
						valid = !property.str.empty() && ReadReference(value, property.i64);
					}
					break;
				case ScriptPropertyValue::Type::Bool:
					valid = value.is_boolean();
					if (valid)
					{
						property.i64 = value.get<bool>() ? 1 : 0;
					}
					break;
				case ScriptPropertyValue::Type::String:
					valid = value.is_string();
					if (valid)
					{
						property.str = value.get<std::string>();
					}
					break;
				case ScriptPropertyValue::Type::Vector3:
					valid = value.is_array() && value.size() == 3;
					if (valid)
					{
						for (std::size_t axis = 0; axis < 3; ++axis)
						{
							valid = ReadFloat(value[axis], property.f4[axis]) && valid;
						}
					}
					break;
				case ScriptPropertyValue::Type::None:
					break;
			}
			if (!valid)
			{
				return std::unexpected(AetherError::Engine("Property '" + name + "' does not match its declared type, shape, or numeric range."));
			}
			properties.emplace(name, std::move(property));
		}
		return properties;
	}
} // namespace aether::app::scripting
