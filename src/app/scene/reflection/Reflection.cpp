#include "scene/reflection/Reflection.hpp"

#include <algorithm>

namespace aether::reflect
{
	namespace
	{
		// Function-local so AE_COMPONENT static initializers (which run before main,
		// single-threaded) can append in any translation-unit order without a
		// static-init-order problem.
		std::vector<ComponentType>& Registry()
		{
			static std::vector<ComponentType> registry;
			return registry;
		}
	} // namespace

	std::string EnumTable::NameOf(int value) const
	{
		for (const auto& [name, v]: values)
		{
			if (v == value)
			{
				return name;
			}
		}
		return {};
	}

	int EnumTable::ValueOf(std::string_view name, int fallback) const
	{
		for (const auto& [n, v]: values)
		{
			if (n == name)
			{
				return v;
			}
		}
		return fallback;
	}

	const FieldDesc* ComponentType::FindField(std::string_view fieldName) const
	{
		const auto it = std::find_if(fields.begin(), fields.end(), [&](const FieldDesc& f) { return f.name == fieldName; });
		return it != fields.end() ? &*it : nullptr;
	}

	const std::vector<ComponentType>& ComponentTypes()
	{
		return Registry();
	}

	const ComponentType* FindComponentType(std::string_view name)
	{
		const auto& types = Registry();
		const auto it = std::find_if(types.begin(), types.end(), [&](const ComponentType& t) { return t.name == name; });
		return it != types.end() ? &*it : nullptr;
	}

	void RegisterComponent(ComponentType type)
	{
		auto& registry = Registry();
		// Last declaration wins if a name is registered twice (keeps the table unique).
		const auto it = std::find_if(registry.begin(), registry.end(), [&](const ComponentType& t) { return t.name == type.name; });
		if (it != registry.end())
		{
			*it = std::move(type);
		}
		else
		{
			registry.push_back(std::move(type));
		}
	}
} // namespace aether::reflect
