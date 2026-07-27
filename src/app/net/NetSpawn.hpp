#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>

#include "net/NetSerialize.hpp"
#include "net/NetSession.hpp"

namespace aether
{
	class World;
}

namespace aether::net
{
	// Every reliable-channel packet starts with one of these so the receive system
	// can dispatch without a second framing layer.
	enum class NetMessage : std::uint8_t
	{
		Snapshot = 1,
		Spawn = 2,
		Despawn = 3,
		Rpc = 4,
		Welcome = 5,
		ScriptFields = 6,
	};

	struct SpawnMessage
	{
		std::uint32_t netId = 0;
		ConnectionId owner = kInvalidConnection;
		std::string prefab;
		glm::vec3 position{0.f};
	};

	[[nodiscard]] std::vector<std::byte> EncodeSpawn(std::uint32_t netId, ConnectionId owner, std::string_view prefab,
	        glm::vec3 position);
	[[nodiscard]] std::optional<SpawnMessage> DecodeSpawn(ByteReader& r);

	[[nodiscard]] std::vector<std::byte> EncodeDespawn(std::uint32_t netId);
	[[nodiscard]] std::optional<std::uint32_t> DecodeDespawn(ByteReader& r);

	// Gives every scene-placed NetworkIdentity a deterministic id. Iterates in
	// order of SceneNodeComponent::id - the stable, persisted scene-node id that
	// round-trips through the scene file - which is identical on every machine
	// loading the same scene, so host and client agree with no handshake. Entities
	// with nodeId == 0 (prefab-instantiated, not top-level scene entities) are
	// skipped; they get their id through spawn replication instead. ECS iteration
	// order is NOT deterministic and must never be used here.
	void AssignScenePlacedNetIds(World& world, NetSession& session);
} // namespace aether::net
