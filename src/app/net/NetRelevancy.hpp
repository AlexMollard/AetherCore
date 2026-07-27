#pragma once

#include <vector>

#include <glm/glm.hpp>

#include "net/NetComponents.hpp"
#include "scene/Entity.hpp"

namespace aether
{
	class World;
}

namespace aether::net
{
	struct RelevancySettings
	{
		float radius = 60.f;
		bool enabled = true;
	};

	// Entities the given connection should receive this tick. A connection always
	// receives what it owns regardless of distance - otherwise a player who falls
	// out of the world can never be told where they are.
	[[nodiscard]] std::vector<Entity> RelevantFor(World& world, ConnectionId viewer, glm::vec3 viewerPos,
	        const RelevancySettings& settings);
} // namespace aether::net
