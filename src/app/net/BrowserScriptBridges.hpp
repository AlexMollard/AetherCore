#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "net/NetRpc.hpp"
#include "net/NetScriptFields.hpp"

namespace aether
{
	class ServiceContainer;
}

namespace aether::scripting
{
	struct ManagedScriptApi;
}

namespace aether::app::scripting
{
	class GameplayHost;
}

namespace aether::net
{
	using BrowserInstanceLookup = std::function<std::uint64_t(Entity, std::uint32_t)>;

	class BrowserScriptFieldBridge final : public ScriptFieldBridge
	{
	public:
		BrowserScriptFieldBridge(const aether::scripting::ManagedScriptApi& api, const aether::app::scripting::GameplayHost& host, BrowserInstanceLookup lookup);
		[[nodiscard]] std::vector<ScriptPropertyDesc> ReplicatedProperties(const std::string& typeName) const override;
		[[nodiscard]] bool GetProperty(Entity entity, std::uint32_t scriptIndex, std::uint16_t propertyIndex, ScriptPropertyValue& out) const override;
		void SetProperty(Entity entity, std::uint32_t scriptIndex, std::uint16_t propertyIndex, const ScriptPropertyValue& value) const override;

	private:
		const aether::scripting::ManagedScriptApi& m_api;
		const aether::app::scripting::GameplayHost& m_host;
		BrowserInstanceLookup m_lookup;
		mutable std::uint32_t m_generation = 0;
		mutable bool m_seeded = false;
		mutable std::unordered_map<std::string, std::vector<ScriptPropertyDesc>> m_cache;
	};

	class BrowserRpcBridge final : public RpcBridge
	{
	public:
		BrowserRpcBridge(const aether::scripting::ManagedScriptApi& api, const aether::app::scripting::GameplayHost& host, BrowserInstanceLookup lookup, const ServiceContainer& services);
		[[nodiscard]] RpcMethod FindMethod(const std::string& typeName, const std::string& methodName) const override;
		void Invoke(Entity entity, std::uint32_t scriptIndex, std::uint16_t methodIndex, std::span<const std::byte> args) const override;

	private:
		const aether::scripting::ManagedScriptApi& m_api;
		const aether::app::scripting::GameplayHost& m_host;
		BrowserInstanceLookup m_lookup;
		const ServiceContainer& m_services;
	};
} // namespace aether::net
