#include "net/NetSpawn.hpp"

#include <algorithm>

#include <entt/entt.hpp>

#include "net/NetComponents.hpp"
#include "scene/Components.hpp"
#include "scene/World.hpp"

namespace aether::net
{
	bool IsSafePrefabName(std::string_view name)
	{
		return !name.empty() && name.find("..") == std::string_view::npos
		       && name.find('/') == std::string_view::npos && name.find('\\') == std::string_view::npos
		       && name.find(':') == std::string_view::npos;
	}

	std::vector<std::byte> FrameMessage(NetMessage kind, std::span<const std::byte> payload)
	{
		ByteWriter w;
		w.U8(static_cast<std::uint8_t>(kind));
		w.Bytes(payload);
		return w.Take();
	}

	std::vector<std::byte> EncodeSpawn(std::uint32_t netId, ConnectionId owner, std::string_view prefab, glm::vec3 position)
	{
		ByteWriter w;
		w.U8(static_cast<std::uint8_t>(NetMessage::Spawn));
		w.U32(netId);
		w.U32(owner);
		w.Str(prefab);
		w.F32(position.x);
		w.F32(position.y);
		w.F32(position.z);
		return w.Take();
	}

	std::optional<SpawnMessage> DecodeSpawn(ByteReader& r)
	{
		SpawnMessage msg;
		msg.netId = r.U32();
		msg.owner = r.U32();
		msg.prefab = r.Str();
		msg.position.x = r.F32();
		msg.position.y = r.F32();
		msg.position.z = r.F32();
		if (!r.Ok() || msg.netId == 0)
		{
			return std::nullopt;
		}
		return msg;
	}

	std::vector<std::byte> EncodeDespawn(std::uint32_t netId)
	{
		ByteWriter w;
		w.U8(static_cast<std::uint8_t>(NetMessage::Despawn));
		w.U32(netId);
		return w.Take();
	}

	std::optional<std::uint32_t> DecodeDespawn(ByteReader& r)
	{
		const std::uint32_t netId = r.U32();
		if (!r.Ok() || netId == 0)
		{
			return std::nullopt;
		}
		return netId;
	}

	void AssignScenePlacedNetIds(World& world, NetSession& session)
	{
		// SceneNodeComponent::id is the persisted, stable scene-node id - identical on
		// every machine loading the same scene file - which is what makes this
		// deterministic. Sorting by it, rather than walking the view in ECS order, is
		// the whole point. nodeId == 0 means "not a top-level scene entity" (e.g. a
		// prefab-instantiated child), so those are skipped here and get their id
		// through spawn replication instead.
		std::vector<std::pair<std::uint64_t, Entity>> ordered;
		world.View<NetworkIdentity, SceneNodeComponent>().each(
		        [&](entt::entity ent, NetworkIdentity& identity, SceneNodeComponent& node)
		        {
			        if (node.id == 0 || identity.netId != 0)
			        {
				        return;
			        }
			        ordered.emplace_back(node.id, World::FromEntt(ent));
		        });

		// Node ids are unique in a well-formed scene, but a hand-edited or corrupt TOML
		// could duplicate one - and an unstable sort would then order those two by
		// whatever the collection happened to produce, i.e. ECS order, which is the
		// single thing this function exists to avoid. Break ties on the entity id so the
		// result is total and deterministic even when the invariant is violated.
		std::sort(ordered.begin(), ordered.end(),
		        [](const auto& a, const auto& b) { return a.first != b.first ? a.first < b.first : a.second.id < b.second.id; });

		for (const auto& [nodeId, entity]: ordered)
		{
			auto* identity = world.TryGet<NetworkIdentity>(entity);
			if (identity == nullptr || identity->netId != 0)
			{
				continue;
			}
			identity->netId = session.AllocateNetId();
			identity->scenePlaced = true;
			session.Bind(identity->netId, entity);
		}
	}
} // namespace aether::net
