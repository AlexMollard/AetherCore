#pragma once

#include <atomic>
#include <unordered_set>

#include "scripting/GameplayHost.hpp"
#include "scripting/ManagedInterop.hpp"

namespace aether::app::scripting
{
	// Adapts CoreCLR's existing ABI without changing its public C# SDK or exports.
	// Reset is called only after old instances have been invalidated by script unload.
	class CSharpGameplayHost
	{
	public:
		CSharpGameplayHost() = default;
		CSharpGameplayHost(const CSharpGameplayHost&) = delete;
		CSharpGameplayHost& operator=(const CSharpGameplayHost&) = delete;
		Expected<void> Reset(const aether::scripting::ManagedScriptApi* api, std::uint32_t generation);

		[[nodiscard]] GameplayHost& Gameplay()
		{
			return m_gameplay;
		}

		[[nodiscard]] const GameplayHost& Gameplay() const
		{
			return m_gameplay;
		}

		void ReportManagedError()
		{
			m_errorSerial.fetch_add(1, std::memory_order_relaxed);
		}

	private:
		struct TypeDescriptor
		{
			std::string name;
			std::vector<ScriptPropertyInfo> properties;
		};

		AetherGameplayModuleApi MakeModule();
		AetherGameplayStatus Read(std::uint64_t handle, std::uint32_t type, std::uint32_t index, AetherGameplayPropertyValue* out, AetherGameplayUtf8Buffer buffer, bool defaults);
		AetherGameplayStatus Invoke(std::uint64_t handle, void (*callback)(std::uint64_t));

		bool Has(std::uint64_t handle) const
		{
			return handle != 0 && m_instances.contains(handle);
		}

		const aether::scripting::ManagedScriptApi* m_api = nullptr;
		std::vector<TypeDescriptor> m_types;
		std::unordered_set<std::uint64_t> m_instances;
		std::atomic<std::uint64_t> m_errorSerial{0};
		GameplayHost m_gameplay;
	};
} // namespace aether::app::scripting
