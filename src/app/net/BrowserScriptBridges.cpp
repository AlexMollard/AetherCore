#include "net/BrowserScriptBridges.hpp"

#include <algorithm>
#include <utility>

#include "scripting/GameplayHost.hpp"
#include "scripting/ManagedInterop.hpp"
#include "scripting/SceneContext.hpp"
#include "utils/ServiceContainer.hpp"

namespace aether::net
{
	BrowserScriptFieldBridge::BrowserScriptFieldBridge(const aether::scripting::ManagedScriptApi& api, const aether::app::scripting::GameplayHost& host, BrowserInstanceLookup lookup)
	      : m_api(api), m_host(host), m_lookup(std::move(lookup))
	{
	}

	std::vector<ScriptPropertyDesc> BrowserScriptFieldBridge::ReplicatedProperties(const std::string& typeName) const
	{
		const auto generation = m_host.ScriptReloadGeneration();
		if (!m_seeded || m_generation != generation)
		{
			m_cache.clear();
			m_generation = generation;
			m_seeded = true;
		}
		if (const auto found = m_cache.find(typeName); found != m_cache.end())
		{
			return found->second;
		}
		std::vector<ScriptPropertyDesc> result;
		if (m_api.GetReplicatedPropertyIndices != nullptr)
		{
			const int count = m_api.GetReplicatedPropertyIndices(typeName.c_str(), nullptr, 0);
			if (count > 0)
			{
				std::vector<int> indices(static_cast<std::size_t>(count));
				const int total = m_api.GetReplicatedPropertyIndices(typeName.c_str(), indices.data(), count);
				const auto properties = m_host.GetScriptProperties(typeName);
				result.reserve(static_cast<std::size_t>(std::clamp(total, 0, count)));
				for (int i = 0; i < std::min(total, count); ++i)
				{
					const int index = indices[static_cast<std::size_t>(i)];
					if (index >= 0 && index <= UINT16_MAX && static_cast<std::size_t>(index) < properties.size())
					{
						result.push_back({.index = static_cast<std::uint16_t>(index), .type = properties[static_cast<std::size_t>(index)].type});
					}
				}
			}
		}
		return m_cache.emplace(typeName, std::move(result)).first->second;
	}

	bool BrowserScriptFieldBridge::GetProperty(Entity entity, std::uint32_t scriptIndex, std::uint16_t propertyIndex, ScriptPropertyValue& out) const
	{
		const auto handle = m_lookup(entity, scriptIndex);
		return handle != 0 && m_host.GetPropertyValue(handle, propertyIndex, out);
	}

	void BrowserScriptFieldBridge::SetProperty(Entity entity, std::uint32_t scriptIndex, std::uint16_t propertyIndex, const ScriptPropertyValue& value) const
	{
		const auto handle = m_lookup(entity, scriptIndex);
		if (handle != 0)
		{
			m_host.SetPropertyValue(handle, propertyIndex, value);
		}
	}

	BrowserRpcBridge::BrowserRpcBridge(const aether::scripting::ManagedScriptApi& api, const aether::app::scripting::GameplayHost& host, BrowserInstanceLookup lookup, const ServiceContainer& services)
	      : m_api(api), m_host(host), m_lookup(std::move(lookup)), m_services(services)
	{
	}

	RpcMethod BrowserRpcBridge::FindMethod(const std::string& typeName, const std::string& methodName) const
	{
		if (m_api.GetNetRpcMethod == nullptr)
		{
			return {};
		}
		int target = 0;
		const int index = m_api.GetNetRpcMethod(typeName.c_str(), methodName.c_str(), &target);
		if (index < 0 || target < 0 || target > static_cast<int>(kNetRpcTargetMax))
		{
			return {};
		}
		return {.index = index, .target = static_cast<NetRpcTarget>(target)};
	}

	void BrowserRpcBridge::Invoke(Entity entity, std::uint32_t scriptIndex, std::uint16_t methodIndex, std::span<const std::byte> args) const
	{
		const auto handle = m_lookup(entity, scriptIndex);
		const auto provider = handle != 0 ? m_host.ProviderHandle(handle) : 0;
		auto* context = m_services.TryGet<aether::app::scripting::SceneContext>();
		if (provider == 0 || context == nullptr || m_api.InvokeNetRpc == nullptr)
		{
			return;
		}
		const aether::app::scripting::ActiveContextScope scope(*context);
		const aether::app::scripting::ActiveScriptEntityScope actor(*context, entity);
		m_api.InvokeNetRpc(provider, methodIndex, reinterpret_cast<const std::uint8_t*>(args.data()), static_cast<std::int32_t>(args.size()));
	}
} // namespace aether::net
