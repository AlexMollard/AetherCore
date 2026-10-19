#include "scripting/GameplayHost.hpp"

#include <algorithm>
#include <array>
#include <limits>

#include "scripting/GameplayAbiValidation.hpp"

namespace aether::app::scripting
{
	namespace
	{
		template<typename F>
		AetherGameplayStatus Call(F&& callback)
		{
			try
			{
				return callback();
			}
			catch (...)
			{
				return AE_GAMEPLAY_SCRIPT_ERROR;
			}
		}

		std::string Own(AetherGameplayUtf8 text)
		{
			return text.length == 0 ? std::string{} : std::string(text.data, text.length);
		}
	} // namespace

	Expected<void> GameplayHost::Result(AetherGameplayStatus status, const char* operation) const
	{
		if (status == AE_GAMEPLAY_OK)
		{
			return {};
		}
		AetherError error{.category = LogCategory::App, .message = std::string(operation) + " failed (gameplay status " + std::to_string(status) + ")", .code = status};
		if (m_errorHandler)
		{
			m_errorHandler(error.message);
		}
		return std::unexpected(std::move(error));
	}

	void GameplayHost::Clear(std::uint32_t generation)
	{
		m_available = false;
		m_module = {};
		m_typeNames.clear();
		m_properties.clear();
		m_generation = generation;
	}

	Expected<void> GameplayHost::Reset(const AetherGameplayModuleApi& module, std::string language, std::uint32_t generation)
	{
		Clear(generation);
		m_language = std::move(language);
		AE_TRY_VOID(Result(aether::scripting::ValidateGameplayModule(module), "module validation"));
		std::vector<std::string> names;
		std::vector<std::vector<ScriptPropertyInfo>> properties;
		std::uint32_t count = 0;
		AE_TRY_VOID(Result(Call([&] { return module.getTypeCount(module.context, &count); }), "type count"));
		for (std::uint32_t i = 0; i < count; ++i)
		{
			AetherGameplayTypeInfo info{.structSize = sizeof(info)};
			AE_TRY_VOID(Result(Call([&] { return module.getTypeInfo(module.context, i, &info); }), "type metadata"));
			if (info.structSize != sizeof(info) || !aether::scripting::IsGameplayUtf8(info.name) || info.name.length == 0)
			{
				return Result(AE_GAMEPLAY_INVALID_ARGUMENT, "type metadata");
			}
			names.push_back(Own(info.name));
			properties.emplace_back();
			for (std::uint32_t j = 0; j < info.propertyCount; ++j)
			{
				AetherGameplayPropertyInfo property{.structSize = sizeof(property)};
				AE_TRY_VOID(Result(Call([&] { return module.getPropertyInfo(module.context, i, j, &property); }), "property metadata"));
				if (property.structSize != sizeof(property) || property.type > AE_GAMEPLAY_PROPERTY_COMPONENT || !aether::scripting::IsGameplayUtf8(property.name) || !aether::scripting::IsGameplayUtf8(property.enumNames)
				        || !aether::scripting::IsGameplayUtf8(property.componentType))
				{
					return Result(AE_GAMEPLAY_INVALID_ARGUMENT, "property metadata");
				}
				properties.back().push_back({Own(property.name), static_cast<ScriptPropertyValue::Type>(property.type), Own(property.enumNames), Own(property.componentType)});
			}
		}
		m_module = module;
		m_typeNames = std::move(names);
		m_properties = std::move(properties);
		m_available = true;
		return {};
	}

	int GameplayHost::FindType(const std::string& name) const
	{
		const auto it = std::find(m_typeNames.begin(), m_typeNames.end(), name);
		return it == m_typeNames.end() ? -1 : static_cast<int>(it - m_typeNames.begin());
	}

	std::vector<ScriptPropertyInfo> GameplayHost::GetScriptProperties(const std::string& name) const
	{
		const int index = FindType(name);
		return index < 0 ? std::vector<ScriptPropertyInfo>{} : m_properties[index];
	}

	int GameplayHost::FindPropertyIndex(const std::string& typeName, const std::string& name) const
	{
		const int type = FindType(typeName);
		if (type >= 0)
		{
			for (std::size_t i = 0; i < m_properties[type].size(); ++i)
			{
				if (m_properties[type][i].name == name)
				{
					return static_cast<int>(i);
				}
			}
		}
		return -1;
	}

	Expected<std::uint64_t> GameplayHost::CreateInstance(const std::string& typeName, std::uint32_t entityId) const
	{
		const int type = FindType(typeName);
		if (!m_available || type < 0)
		{
			const std::string message = "Unknown gameplay script type: " + typeName;
			if (m_errorHandler)
			{
				m_errorHandler(message);
			}
			return std::unexpected(AetherError::Engine(message));
		}
		AetherGameplayInstance instance = 0;
		AE_TRY_VOID(Result(Call([&] { return m_module.create(m_module.context, type, {0, entityId, 0}, &instance); }), "create"));
		if (instance == 0)
		{
			return std::unexpected(AetherError::Engine("Gameplay module returned an empty instance"));
		}
		return instance;
	}

	Expected<void> GameplayHost::Attach(std::uint64_t handle) const
	{
		return Result(m_available ? Call([&] { return m_module.attach(m_module.context, handle); }) : AE_GAMEPLAY_UNSUPPORTED, "attach");
	}

	Expected<void> GameplayHost::Update(std::uint64_t handle, float dt) const
	{
		return Result(m_available ? Call([&] { return m_module.update(m_module.context, handle, dt); }) : AE_GAMEPLAY_UNSUPPORTED, "update");
	}

	Expected<void> GameplayHost::Detach(std::uint64_t handle) const
	{
		return Result(m_available ? Call([&] { return m_module.detach(m_module.context, handle); }) : AE_GAMEPLAY_UNSUPPORTED, "detach");
	}

	Expected<void> GameplayHost::Destroy(std::uint64_t handle) const
	{
		return Result(m_available ? Call([&] { return m_module.destroy(m_module.context, handle); }) : AE_GAMEPLAY_UNSUPPORTED, "destroy");
	}

	Expected<void> GameplayHost::DispatchOwnershipChanged(std::uint64_t handle, std::uint32_t owner, bool isOwner) const
	{
		AetherGameplayEvent event{.structSize = sizeof(event), .kind = AE_GAMEPLAY_EVENT_OWNERSHIP, .code = owner, .flags = isOwner ? 1u : 0u};
		return Result(m_available ? aether::scripting::DispatchGameplayEvent(m_module, handle, event) : AE_GAMEPLAY_UNSUPPORTED, "ownership event");
	}

	bool GameplayHost::ReadProperty(std::uint64_t handle, int typeIndex, int index, ScriptPropertyValue& out) const
	{
		if (!m_available || index < 0)
		{
			return false;
		}
		AetherGameplayPropertyValue value{.structSize = sizeof(value)};
		std::uint32_t required = 0;
		std::array<char, 256> local{};
		AetherGameplayUtf8Buffer buffer{local.data(), static_cast<std::uint32_t>(local.size()), &required};
		auto read = [&]
		{
			return typeIndex < 0 ? m_module.getProperty(m_module.context, handle, index, &value, buffer) : m_module.getDefaultProperty(m_module.context, typeIndex, index, &value, buffer);
		};
		auto status = Call(read);
		std::vector<char> storage;
		if (status == AE_GAMEPLAY_BUFFER_TOO_SMALL && required > buffer.capacity)
		{
			storage.resize(required);
			buffer.data = storage.data();
			buffer.capacity = required;
			status = Call(read);
		}
		if (status != AE_GAMEPLAY_OK || value.structSize != sizeof(value) || value.type > AE_GAMEPLAY_PROPERTY_COMPONENT || required > buffer.capacity)
		{
			return false;
		}
		ScriptPropertyValue owned{};
		owned.type = static_cast<ScriptPropertyValue::Type>(value.type);
		std::copy(std::begin(value.vector), std::end(value.vector), std::begin(owned.f4));
		owned.i64 = value.type == AE_GAMEPLAY_PROPERTY_ENTITY || value.type == AE_GAMEPLAY_PROPERTY_COMPONENT ? value.entity.id : value.integer;
		if (required > 0)
		{
			owned.str.assign(buffer.data, required - 1);
		}
		out = std::move(owned);
		return true;
	}

	bool GameplayHost::GetPropertyValue(std::uint64_t handle, int index, ScriptPropertyValue& out) const
	{
		return handle != 0 && ReadProperty(handle, -1, index, out);
	}

	bool GameplayHost::GetDefaultPropertyValue(const std::string& typeName, int index, ScriptPropertyValue& out) const
	{
		const int type = FindType(typeName);
		return type >= 0 && ReadProperty(0, type, index, out);
	}

	void GameplayHost::SetPropertyValue(std::uint64_t handle, int index, const ScriptPropertyValue& value) const
	{
		if (!m_available || handle == 0 || index < 0 || value.str.size() > std::numeric_limits<std::uint32_t>::max())
		{
			return;
		}
		AetherGameplayPropertyValue abi{.structSize = sizeof(abi), .type = static_cast<std::uint32_t>(value.type)};
		if (abi.type == AE_GAMEPLAY_PROPERTY_FLOAT || abi.type == AE_GAMEPLAY_PROPERTY_VECTOR3)
		{
			std::copy(std::begin(value.f4), std::end(value.f4), std::begin(abi.vector));
		}
		else if (abi.type == AE_GAMEPLAY_PROPERTY_ENTITY || abi.type == AE_GAMEPLAY_PROPERTY_COMPONENT)
		{
			abi.entity.id = static_cast<std::uint32_t>(value.i64);
		}
		else
		{
			abi.integer = value.i64;
		}
		(void) Result(Call([&] { return m_module.setProperty(m_module.context, handle, index, &abi, {value.str.data(), static_cast<std::uint32_t>(value.str.size())}); }), "set property");
	}

	void GameplayHost::ApplyProperties(std::uint64_t handle, const std::string& typeName, const std::map<std::string, ScriptPropertyValue>& props) const
	{
		for (const auto& [name, value]: props)
		{
			const int index = FindPropertyIndex(typeName, name);
			if (index >= 0)
			{
				SetPropertyValue(handle, index, value);
			}
		}
	}
} // namespace aether::app::scripting
