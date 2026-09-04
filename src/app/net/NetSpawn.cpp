#include "net/NetSpawn.hpp"

#include <algorithm>

#include <entt/entt.hpp>

#include "net/NetComponents.hpp"
#include "scene/Components.hpp"
#include "scene/World.hpp"
#include "utils/Logger.hpp"

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

	std::vector<std::byte> EncodeRelevancyLeave(std::uint32_t netId)
	{
		ByteWriter w;
		w.U8(static_cast<std::uint8_t>(NetMessage::Relevancy));
		w.U32(netId);
		return w.Take();
	}

	std::optional<std::uint32_t> DecodeRelevancyLeave(ByteReader& r)
	{
		const std::uint32_t netId = r.U32();
		if (!r.Ok() || netId == 0)
		{
			return std::nullopt;
		}
		return netId;
	}

	std::vector<std::byte> EncodeClientReady()
	{
		// Self-framing and empty: one kind byte is the whole message. There is no
		// decoder to match, because there is nothing to decode - the receiver learns
		// everything from the kind and the peer it arrived on.
		ByteWriter w;
		w.U8(static_cast<std::uint8_t>(NetMessage::ClientReady));
		return w.Take();
	}

	std::vector<std::byte> EncodeDisconnect(std::string_view reason)
	{
		ByteWriter w;
		w.U8(static_cast<std::uint8_t>(NetMessage::Disconnect));
		w.Str(SanitizeReason(reason));
		return w.Take();
	}

	std::optional<std::string> DecodeDisconnect(ByteReader& r)
	{
		std::string reason = r.Str();
		if (!r.Ok())
		{
			return std::nullopt;
		}
		// Sanitised HERE rather than at the call site, so there is no route by which an
		// unsanitised reason reaches a caller: every decode is a decode of remote bytes.
		return SanitizeReason(reason);
	}

	std::string SanitizeReason(std::string_view raw)
	{
		std::string clean;
		clean.reserve(std::min(raw.size(), kMaxDisconnectReasonLength));
		for (const char c: raw)
		{
			if (c < ' ' || c > '~')
			{
				continue; // ASCII-only: the font pipeline bakes no other glyphs
			}
			clean.push_back(c);
			if (clean.size() >= kMaxDisconnectReasonLength)
			{
				break;
			}
		}
		return clean;
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
			const std::uint32_t netId = session.AllocateSceneNetId();
			if (netId == 0)
			{
				AE_WARN(LogCategory::App,
				        "Net: scene-placed net-id space exhausted at {} ids - entity with node id {} goes unreplicated",
				        kSpawnNetIdBase, nodeId);
				continue;
			}
			identity->netId = netId;
			identity->scenePlaced = true;
			session.Bind(identity->netId, entity);
		}
	}
} // namespace aether::net
