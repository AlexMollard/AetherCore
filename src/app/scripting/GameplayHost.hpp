#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "scene/Components.hpp"
#include "scripting/GameplayAbi.h"
#include "utils/Expected.hpp"

namespace aether::scripting
{
	class GameplayAccess;
}

namespace aether::app::scripting
{
	struct ScriptPropertyInfo
	{
		std::string name;
		ScriptPropertyValue::Type type = ScriptPropertyValue::Type::None;
		std::string enumNames;
		std::string componentType;
	};

	// Owns descriptor copies. Calls and teardown run synchronously on the gameplay owner thread.
	// Checked native access is optional; the legacy managed adapter uses world zero.
	class GameplayHost
	{
	public:
		void SetAccess(aether::scripting::GameplayAccess* access)
		{
			m_access = access;
		}

		[[nodiscard]] AetherGameplayEntity EntityHandle(std::uint32_t id) const;
		// `flags` is AE_GAMEPLAY_PHYSICS_FLAG_3D for Jolt events, 0 for Box2D events.
		Expected<void> DispatchPhysics(std::uint64_t handle, std::uint32_t other, std::uint32_t kind, std::uint32_t flags = 0) const;
		using ErrorHandler = std::function<void(const std::string&)>;
		Expected<void> Reset(const AetherGameplayModuleApi& module, std::string language, std::uint32_t generation);
		void Clear(std::uint32_t generation);

		void SetErrorHandler(ErrorHandler handler)
		{
			m_errorHandler = std::move(handler);
		}

		[[nodiscard]] bool IsAvailable() const
		{
			return m_available;
		}

		[[nodiscard]] const std::string& Language() const
		{
			return m_language;
		}

		[[nodiscard]] std::uint64_t Capabilities() const
		{
			return m_module.capabilities;
		}

		[[nodiscard]] std::uint32_t ScriptReloadGeneration() const
		{
			return m_generation;
		}

		[[nodiscard]] const std::vector<std::string>& GetScriptTypeNames() const
		{
			return m_typeNames;
		}

		[[nodiscard]] std::vector<ScriptPropertyInfo> GetScriptProperties(const std::string& typeName) const;
		[[nodiscard]] int FindPropertyIndex(const std::string& typeName, const std::string& name) const;
		Expected<std::uint64_t> CreateInstance(const std::string& typeName, std::uint32_t entityId) const;
		Expected<void> Attach(std::uint64_t handle) const;
		Expected<void> Update(std::uint64_t handle, float deltaSeconds) const;
		Expected<void> Detach(std::uint64_t handle) const;
		Expected<void> Destroy(std::uint64_t handle) const;
		Expected<void> DispatchOwnershipChanged(std::uint64_t handle, std::uint32_t owner, bool isOwner) const;
		[[nodiscard]] bool GetPropertyValue(std::uint64_t handle, int index, ScriptPropertyValue& out) const;
		Expected<void> TrySetPropertyValue(std::uint64_t handle, int index, const ScriptPropertyValue& value) const;

		void SetPropertyValue(std::uint64_t handle, int index, const ScriptPropertyValue& value) const
		{
			(void) TrySetPropertyValue(handle, index, value);
		}

		[[nodiscard]] bool GetDefaultPropertyValue(const std::string& typeName, int index, ScriptPropertyValue& out) const;
		Expected<void> TryApplyProperties(std::uint64_t handle, const std::string& typeName, const std::map<std::string, ScriptPropertyValue>& props) const;

		void ApplyProperties(std::uint64_t handle, const std::string& typeName, const std::map<std::string, ScriptPropertyValue>& props) const
		{
			(void) TryApplyProperties(handle, typeName, props);
		}

	private:
		std::uint64_t ProviderHandle(std::uint64_t handle) const;
		bool ValidInstance(std::uint64_t handle) const;
		AetherGameplayStatus Call(const std::function<AetherGameplayStatus()>& callback) const;
		int FindType(const std::string& name) const;
		Expected<void> Result(AetherGameplayStatus status, const char* operation) const;
		bool ReadProperty(std::uint64_t handle, int typeIndex, int index, ScriptPropertyValue& out) const;
		AetherGameplayModuleApi m_module{};
		bool m_available = false;
		std::uint32_t m_generation = 0;
		std::string m_language;
		std::vector<std::string> m_typeNames;
		std::vector<std::vector<ScriptPropertyInfo>> m_properties;
		ErrorHandler m_errorHandler;
		aether::scripting::GameplayAccess* m_access = nullptr;
		mutable std::map<std::uint64_t, bool> m_ownedInstances;

		struct NativeInstance
		{
			std::uint64_t provider;
			int typeIndex;
		};

		mutable std::map<std::uint64_t, NativeInstance> m_nativeHandles;
	};
} // namespace aether::app::scripting
