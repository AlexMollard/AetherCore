#pragma once

// Shared fixtures for the two integration-layer test TUs (NetworkContextTests.cpp
// and NetworkSystemsTests.cpp). Kept out of either one so both drive the same
// notion of "a scene-placed entity" and "an entity this session spawned" - the
// distinction Stop()'s predicate turns on, and the single easiest thing for two
// copies of a helper to drift apart on.

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <entt/entt.hpp>

#include "net/NetComponents.hpp"
#include "net/NetRpc.hpp"
#include "net/NetScriptFields.hpp"
#include "net/NetworkContext.hpp"
#include "scene/Components.hpp"
#include "scene/World.hpp"

namespace aether::net::test
{
	// A scene entity: it carries a SceneNodeComponent, which is what makes
	// AssignScenePlacedNetIds pick it up and mark it scenePlaced. Its net id starts at
	// 0 exactly as a freshly loaded scene's does.
	inline Entity MakeScenePlaced(World& world, std::uint64_t nodeId)
	{
		const Entity entity = world.Create();
		world.Emplace<TransformComponent>(entity);
		world.Emplace<SceneNodeComponent>(entity).id = nodeId;
		world.Emplace<NetworkIdentity>(entity);
		return entity;
	}

	// What SpawnPrefab/ApplySpawn leave behind, minus the prefab asset read: a net id
	// from the live session, a binding, scenePlaced == false, and no scene node id.
	// The asset is the only part omitted, and nothing under test reads it.
	inline Entity MakeSessionSpawned(World& world, NetworkContext& context, ConnectionId owner)
	{
		const Entity entity = world.Create();
		world.Emplace<TransformComponent>(entity);
		NetworkIdentity identity{};
		identity.netId = context.Session().AllocateNetId();
		identity.owner = owner;
		identity.spawnPrefab = "test_prefab";
		identity.scenePlaced = false;
		world.EmplaceOrReplace<NetworkIdentity>(entity, identity);
		context.Session().Bind(identity.netId, entity);
		return entity;
	}

	inline std::size_t CountIdentities(World& world)
	{
		std::size_t count = 0;
		world.View<NetworkIdentity>().each([&](NetworkIdentity&) { ++count; });
		return count;
	}

	inline std::vector<std::uint32_t> ScenePlacedNetIds(World& world)
	{
		std::vector<std::uint32_t> ids;
		world.View<NetworkIdentity>().each(
		        [&](NetworkIdentity& identity)
		        {
			        if (identity.scenePlaced)
			        {
				        ids.push_back(identity.netId);
			        }
		        });
		return ids;
	}

	// A ScriptFieldBridge with no CLR behind it, and a write counter. The counter is
	// the point: a role gate that drops a packet is proved by the bridge never being
	// asked to write, not by the absence of a crash.
	class FakeFieldBridge final : public ScriptFieldBridge
	{
	public:
		void Declare(const std::string& typeName, std::vector<ScriptPropertyDesc> props)
		{
			m_props[typeName] = std::move(props);
		}

		void Seed(std::uint32_t entityId, std::uint32_t scriptIndex, std::uint16_t propertyIndex, ScriptPropertyValue v)
		{
			m_values[Key(entityId, scriptIndex, propertyIndex)] = std::move(v);
		}

		[[nodiscard]] const ScriptPropertyValue* Peek(std::uint32_t entityId, std::uint32_t scriptIndex,
		        std::uint16_t propertyIndex) const
		{
			const auto it = m_values.find(Key(entityId, scriptIndex, propertyIndex));
			return it == m_values.end() ? nullptr : &it->second;
		}

		[[nodiscard]] int Writes() const
		{
			return m_writes;
		}

		[[nodiscard]] std::vector<ScriptPropertyDesc> ReplicatedProperties(const std::string& typeName) const override
		{
			const auto it = m_props.find(typeName);
			return it == m_props.end() ? std::vector<ScriptPropertyDesc>{} : it->second;
		}

		[[nodiscard]] bool GetProperty(Entity entity, std::uint32_t scriptIndex, std::uint16_t propertyIndex,
		        ScriptPropertyValue& out) const override
		{
			const auto it = m_values.find(Key(entity.id, scriptIndex, propertyIndex));
			if (it == m_values.end())
			{
				return false;
			}
			out = it->second;
			return true;
		}

		void SetProperty(Entity entity, std::uint32_t scriptIndex, std::uint16_t propertyIndex,
		        const ScriptPropertyValue& value) const override
		{
			++m_writes;
			m_values[Key(entity.id, scriptIndex, propertyIndex)] = value;
		}

	private:
		static std::string Key(std::uint32_t entityId, std::uint32_t scriptIndex, std::uint16_t propertyIndex)
		{
			return std::to_string(entityId) + "/" + std::to_string(scriptIndex) + "/" + std::to_string(propertyIndex);
		}

		std::map<std::string, std::vector<ScriptPropertyDesc>> m_props;
		mutable std::map<std::string, ScriptPropertyValue> m_values;
		mutable int m_writes = 0;
	};

	// Counts invocations. Same reasoning as above: an RPC that was gated out is one
	// the bridge was never asked to dispatch.
	class FakeRpcCounter final : public RpcBridge
	{
	public:
		[[nodiscard]] int Invocations() const
		{
			return m_invocations;
		}

		[[nodiscard]] RpcMethod FindMethod(const std::string&, const std::string&) const override
		{
			return RpcMethod{.index = 0, .target = NetRpcTarget::Server};
		}

		void Invoke(Entity, std::uint32_t, std::uint16_t, std::span<const std::byte>) const override
		{
			++m_invocations;
		}

	private:
		mutable int m_invocations = 0;
	};
} // namespace aether::net::test
