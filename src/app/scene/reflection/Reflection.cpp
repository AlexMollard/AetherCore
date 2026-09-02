#include <cctype>
#include "scene/reflection/Reflection.hpp"

#include <algorithm>

namespace aether::reflect
{
	namespace
	{
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
	std::string PrettyComponentName(std::string_view cppTypeName)
	{
		std::string_view name = cppTypeName;
		// entt hands back whatever the compiler calls the type: MSVC prefixes "struct ", and
		// every compiler keeps the namespace. Neither belongs in an API a person reads.
		for (const std::string_view prefix: {"struct ", "class "})
		{
			if (name.starts_with(prefix))
			{
				name.remove_prefix(prefix.size());
			}
		}
		if (const std::size_t sep = name.rfind("::"); sep != std::string_view::npos)
		{
			name.remove_prefix(sep + 2);
		}
		if (name.ends_with("Component"))
		{
			name.remove_suffix(std::string_view{"Component"}.size());
		}
		if (name.empty())
		{
			return std::string(cppTypeName);
		}

		// "PhysicsState" reads as "Physics State", matching the reflected names it sits
		// beside in the same list.
		std::string out;
		out.reserve(name.size() + 4);
		for (std::size_t i = 0; i < name.size(); ++i)
		{
			const bool boundary = i > 0 && std::isupper(static_cast<unsigned char>(name[i])) != 0 && std::islower(static_cast<unsigned char>(name[i - 1])) != 0;
			if (boundary)
			{
				out += ' ';
			}
			out += name[i];
		}
		return out;
	}
} // namespace aether::reflect
