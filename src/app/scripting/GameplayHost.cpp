#include "scripting/GameplayHost.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <limits>

#include "scripting/GameplayAbiValidation.hpp"
#include "scripting/GameplayAccess.hpp"

namespace aether::app::scripting
{
	namespace
	{

		std::string Own(AetherGameplayUtf8 text)
		{
			return text.length == 0 ? std::string{} : std::string(text.data, text.length);
		}
	} // namespace

	bool GameplayHost::ValidInstance(std::uint64_t handle) const
	{
		return m_available && (m_access == nullptr || m_nativeHandles.contains(handle));
	}

	std::uint64_t GameplayHost::ProviderHandle(std::uint64_t handle) const
	{
		if (m_access == nullptr)
		{
			return handle;
		}
		const auto it = m_nativeHandles.find(handle);
		return it == m_nativeHandles.end() ? 0 : it->second.provider;
	}

	AetherGameplayStatus GameplayHost::Call(const std::function<AetherGameplayStatus()>& callback, bool predictedAttachOperation) const
	{
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
		try
#endif
		{
			if (m_access != nullptr)
			{
				aether::scripting::GameplayAccess::Scope scope(*m_access, predictedAttachOperation
				        ? aether::scripting::GameplayAccess::ScopeKind::PredictedAttachCallback
				        : aether::scripting::GameplayAccess::ScopeKind::Callback);
				return scope.Status() == AE_GAMEPLAY_OK ? callback() : scope.Status();
			}
			return callback();
		}
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
		catch (...)
		{
			return AE_GAMEPLAY_SCRIPT_ERROR;
		}
#endif
	}

	AetherGameplayEntity GameplayHost::EntityHandle(std::uint32_t id) const
	{
		return id == 0 ? AetherGameplayEntity{} : (m_access != nullptr ? m_access->Handle(Entity{id}) : AetherGameplayEntity{0, id, 0});
	}

	Expected<void> GameplayHost::DispatchPhysics(std::uint64_t handle, std::uint32_t other, std::uint32_t kind, std::uint32_t flags) const
	{
		AetherGameplayEvent event{.structSize = sizeof(event), .kind = AE_GAMEPLAY_EVENT_PHYSICS, .other = EntityHandle(other), .code = kind, .flags = flags};
		return Result(ValidInstance(handle) ? Call([&] { return aether::scripting::DispatchGameplayEvent(m_module, ProviderHandle(handle), event); }) : AE_GAMEPLAY_UNSUPPORTED, "physics event");
	}

	Expected<void> GameplayHost::DispatchNetwork(std::uint64_t handle, std::uint32_t kind, std::uint32_t peer) const
	{
		AetherGameplayEvent event{.structSize = sizeof(event), .kind = AE_GAMEPLAY_EVENT_NETWORK, .code = kind, .flags = peer};
		return Result(ValidInstance(handle) ? Call([&] { return aether::scripting::DispatchGameplayEvent(m_module, ProviderHandle(handle), event); }) : AE_GAMEPLAY_UNSUPPORTED, "network event");
	}

	Expected<std::string> GameplayHost::CaptureStreamingState(std::uint64_t handle) const
	{
		if (!ValidInstance(handle) || !m_module.captureStreamingState)
			return std::unexpected(AetherError::Engine("streaming capture is unavailable"));
		std::string buffer(1024 * 1024, '\0');
		std::uint32_t required = 0;
		const auto status = Call([&] { return m_module.captureStreamingState(m_module.context, ProviderHandle(handle), {buffer.data(), static_cast<std::uint32_t>(buffer.size()), &required}); });
		if (auto result = Result(status, "streaming capture"); !result) return std::unexpected(result.error());
		if (required == 0 || required > buffer.size())
			return std::unexpected(AetherError::Engine("invalid streaming capture size"));
		buffer.resize(required - 1);
		return buffer;
	}

	Expected<void> GameplayHost::RestoreStreamingState(std::uint64_t handle, const std::string& state) const
	{
		if (!ValidInstance(handle) || !m_module.restoreStreamingState || state.size() > std::numeric_limits<std::uint32_t>::max())
			return std::unexpected(AetherError::Engine("streaming restore is unavailable"));
		return Result(Call([&] { return m_module.restoreStreamingState(m_module.context, ProviderHandle(handle), {state.data(), static_cast<std::uint32_t>(state.size())}); }), "streaming restore");
	}

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
		// Native allocations must die while their provider is still loaded. Managed
		// adapters retain their own established reset/cleanup behavior.
		if (m_access != nullptr)
		{
			const auto owned = m_ownedInstances;
			for (const auto& [handle, attached]: owned)
			{
				if (attached)
				{
					(void) Detach(handle);
				}
				(void) Destroy(handle);
			}
		}
		m_ownedInstances.clear();
		m_nativeHandles.clear();
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

	std::span<const ScriptPropertyInfo> GameplayHost::BorrowScriptProperties(const std::string& name) const
	{
		const int index = FindType(name);
		return index < 0 ? std::span<const ScriptPropertyInfo>{} : std::span<const ScriptPropertyInfo>{m_properties[index]};
	}

	Expected<std::uint32_t> GameplayHost::GetPropertyRequiredBytes(std::uint64_t handle, int index) const
	{
		if (!m_available || !ValidInstance(handle) || index < 0 || !m_module.getProperty)
			return std::unexpected(AetherError::Engine("Invalid gameplay property sizing request"));
		AetherGameplayPropertyValue value{.structSize = sizeof(value)};
		std::uint32_t required = 0;
		AetherGameplayUtf8Buffer buffer{nullptr, 0, &required};
		const auto status = Call([&] { return m_module.getProperty(m_module.context, ProviderHandle(handle), index, &value, buffer); });
		if ((status != AE_GAMEPLAY_OK && status != AE_GAMEPLAY_BUFFER_TOO_SMALL) || value.structSize != sizeof(value) ||
		    value.type > AE_GAMEPLAY_PROPERTY_COMPONENT || (status == AE_GAMEPLAY_BUFFER_TOO_SMALL && required == 0))
			return std::unexpected(AetherError::Engine("Gameplay property sizing failed"));
		if (m_access != nullptr && (value.type == AE_GAMEPLAY_PROPERTY_ENTITY || value.type == AE_GAMEPLAY_PROPERTY_COMPONENT) &&
		    value.entity.id != 0 && (value.entity.world != EntityHandle(value.entity.id).world || value.entity.reserved != 0))
			return std::unexpected(AetherError::Engine("Invalid gameplay property entity"));
		if (value.type == AE_GAMEPLAY_PROPERTY_COMPONENT)
		{
			const auto instance = m_nativeHandles.find(handle);
			if (instance != m_nativeHandles.end() && instance->second.typeIndex >= 0 &&
			    static_cast<std::size_t>(instance->second.typeIndex) < m_properties.size())
			{
				const auto& properties = m_properties[instance->second.typeIndex];
				if (static_cast<std::size_t>(index) < properties.size())
				{
					const auto fallback = properties[index].componentType.size();
					if (fallback >= std::numeric_limits<std::uint32_t>::max())
						return std::unexpected(AetherError::Engine("Gameplay property descriptor too large"));
					required = std::max(required, static_cast<std::uint32_t>(fallback + 1));
				}
			}
		}
		return required;
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
		AE_TRY_VOID(Result(Call([&] { return m_module.create(m_module.context, type, EntityHandle(entityId), &instance); }, true), "create"));
		if (instance == 0)
		{
			return std::unexpected(AetherError::Engine("Gameplay module returned an empty instance"));
		}
		if (m_access != nullptr)
		{
			static std::atomic<std::uint64_t> nextHandle{1};
			const auto publicHandle = nextHandle.fetch_add(1);
			m_nativeHandles.emplace(publicHandle, NativeInstance{instance, type});
			instance = publicHandle;
		}
		m_ownedInstances.emplace(instance, false);
		return instance;
	}

	Expected<void> GameplayHost::Attach(std::uint64_t handle) const
	{
		auto result = Result(ValidInstance(handle) ? Call([&] { return m_module.attach(m_module.context, ProviderHandle(handle)); }, true) : AE_GAMEPLAY_UNSUPPORTED, "attach");
		if (result)
		{
			m_ownedInstances[handle] = true;
		}
		return result;
	}

	Expected<void> GameplayHost::Update(std::uint64_t handle, float dt) const
	{
		return Result(ValidInstance(handle) ? Call([&] { return m_module.update(m_module.context, ProviderHandle(handle), dt); }) : AE_GAMEPLAY_UNSUPPORTED, "update");
	}

	Expected<void> GameplayHost::Detach(std::uint64_t handle) const
	{
		auto result = Result(ValidInstance(handle) ? Call([&] { return m_module.detach(m_module.context, ProviderHandle(handle)); }) : AE_GAMEPLAY_UNSUPPORTED, "detach");
		if (auto it = m_ownedInstances.find(handle); it != m_ownedInstances.end())
		{
			it->second = false;
		}
		return result;
	}

	Expected<void> GameplayHost::Destroy(std::uint64_t handle) const
	{
		auto result = Result(ValidInstance(handle) ? Call([&] { return m_module.destroy(m_module.context, ProviderHandle(handle)); }) : AE_GAMEPLAY_UNSUPPORTED, "destroy");
		m_ownedInstances.erase(handle);
		m_nativeHandles.erase(handle);
		return result;
	}

	Expected<void> GameplayHost::DispatchOwnershipChanged(std::uint64_t handle, std::uint32_t owner, bool isOwner) const
	{
		AetherGameplayEvent event{.structSize = sizeof(event), .kind = AE_GAMEPLAY_EVENT_OWNERSHIP, .code = owner, .flags = isOwner ? 1u : 0u};
		return Result(ValidInstance(handle) ? Call([&] { return aether::scripting::DispatchGameplayEvent(m_module, ProviderHandle(handle), event); }) : AE_GAMEPLAY_UNSUPPORTED, "ownership event");
	}

	bool GameplayHost::ReadProperty(std::uint64_t handle, int typeIndex, int index, ScriptPropertyValue& out, std::uint32_t maxBytes) const
	{
		if (!m_available || (typeIndex < 0 && !ValidInstance(handle)) || index < 0)
		{
			return false;
		}
		AetherGameplayPropertyValue value{.structSize = sizeof(value)};
		std::uint32_t required = 0;
		std::array<char, 256> local{};
		AetherGameplayUtf8Buffer buffer{local.data(), static_cast<std::uint32_t>(local.size()), &required};
		auto read = [&]
		{
			return typeIndex < 0 ? m_module.getProperty(m_module.context, ProviderHandle(handle), index, &value, buffer) : m_module.getDefaultProperty(m_module.context, typeIndex, index, &value, buffer);
		};
		auto status = Call(read);
		if (required > maxBytes) return false;
		std::vector<char> storage;
		if (status == AE_GAMEPLAY_BUFFER_TOO_SMALL && required > buffer.capacity)
		{
			storage.resize(required);
			buffer.data = storage.data();
			buffer.capacity = required;
			status = Call(read);
		}
		if (status != AE_GAMEPLAY_OK || value.structSize != sizeof(value) || value.type > AE_GAMEPLAY_PROPERTY_COMPONENT || required > buffer.capacity || required > maxBytes)
		{
			return false;
		}
		if (m_access != nullptr && (value.type == AE_GAMEPLAY_PROPERTY_ENTITY || value.type == AE_GAMEPLAY_PROPERTY_COMPONENT) && value.entity.id != 0 && (value.entity.world != EntityHandle(value.entity.id).world || value.entity.reserved != 0))
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
		if (owned.type == ScriptPropertyValue::Type::Component && owned.str.empty())
		{
			int descriptorType = typeIndex;
			if (descriptorType < 0)
			{
				if (const auto instance = m_nativeHandles.find(handle); instance != m_nativeHandles.end())
				{
					descriptorType = instance->second.typeIndex;
				}
			}
			if (descriptorType >= 0 && static_cast<std::size_t>(descriptorType) < m_properties.size() && static_cast<std::size_t>(index) < m_properties[descriptorType].size())
			{
				if (m_properties[descriptorType][index].componentType.size() >= maxBytes) return false;
				owned.str = m_properties[descriptorType][index].componentType;
			}
		}
		out = std::move(owned);
		return true;
	}

	bool GameplayHost::GetPropertyValue(std::uint64_t handle, int index, ScriptPropertyValue& out) const
	{
		return handle != 0 && ReadProperty(handle, -1, index, out);
	}

	bool GameplayHost::GetPropertyValueBounded(std::uint64_t handle, int index, std::uint32_t maxBytes, ScriptPropertyValue& out) const
	{
		return handle != 0 && ReadProperty(handle, -1, index, out, maxBytes);
	}

	bool GameplayHost::GetDefaultPropertyValue(const std::string& typeName, int index, ScriptPropertyValue& out) const
	{
		const int type = FindType(typeName);
		return type >= 0 && ReadProperty(0, type, index, out);
	}

	Expected<void> GameplayHost::TrySetPropertyValue(std::uint64_t handle, int index, const ScriptPropertyValue& value) const
	{
		if (!ValidInstance(handle) || handle == 0 || index < 0 || value.str.size() > std::numeric_limits<std::uint32_t>::max())
		{
			return Result(AE_GAMEPLAY_INVALID_ARGUMENT, "set property");
		}
		AetherGameplayPropertyValue abi{.structSize = sizeof(abi), .type = static_cast<std::uint32_t>(value.type)};
		if (abi.type == AE_GAMEPLAY_PROPERTY_FLOAT || abi.type == AE_GAMEPLAY_PROPERTY_VECTOR3)
		{
			std::copy(std::begin(value.f4), std::end(value.f4), std::begin(abi.vector));
		}
		else if (abi.type == AE_GAMEPLAY_PROPERTY_ENTITY || abi.type == AE_GAMEPLAY_PROPERTY_COMPONENT)
		{
			abi.entity = EntityHandle(static_cast<std::uint32_t>(value.i64));
		}
		else
		{
			abi.integer = value.i64;
		}
		return Result(Call([&] { return m_module.setProperty(m_module.context, ProviderHandle(handle), index, &abi, {value.str.data(), static_cast<std::uint32_t>(value.str.size())}); }, true), "set property");
	}

	Expected<void> GameplayHost::TryApplyProperties(std::uint64_t handle, const std::string& typeName, const std::map<std::string, ScriptPropertyValue>& props) const
	{
		for (const auto& [name, value]: props)
		{
			const int index = FindPropertyIndex(typeName, name);
			if (index >= 0)
			{
				auto result = TrySetPropertyValue(handle, index, value);
				if (!result && m_access != nullptr)
				{
					return result;
				}
			}
		}
		return {};
	}
} // namespace aether::app::scripting
