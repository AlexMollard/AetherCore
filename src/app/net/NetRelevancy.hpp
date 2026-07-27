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
	//
	// Only entities that HAVE a TransformComponent are considered here; see the
	// comment in the implementation and use RelevantWithTransformless below if you
	// are driving replication rather than asking a purely spatial question.
	[[nodiscard]] std::vector<Entity> RelevantFor(World& world, ConnectionId viewer, glm::vec3 viewerPos,
	        const RelevancySettings& settings);

	// What a send system must actually use: RelevantFor() plus every networked
	// entity that carries no TransformComponent. A transformless entity has no
	// position to measure, so RelevantFor can never admit it and it would be
	// replicated to nobody - which silently breaks the transformless game-state
	// entity that exists only to replicate script fields. Those bypass relevancy
	// entirely: there is no distance at which they stop mattering.
	[[nodiscard]] std::vector<Entity> RelevantWithTransformless(World& world, ConnectionId viewer, glm::vec3 viewerPos,
	        const RelevancySettings& settings);

	// Where `viewer` sees from: the first entity it owns that has a transform. A
	// connection that owns nothing positioned yet views from the origin, which only
	// affects what it receives, never what it may own.
	//
	// Lives here rather than on either network system because BOTH must agree on it:
	// the host's join replay filters by relevancy from this position and every send
	// tick after it does the same, and two copies of "where does this connection look
	// from" drifting apart is precisely how an entity ends up sent once and then never
	// spoken about again.
	[[nodiscard]] glm::vec3 ViewerPosition(World& world, ConnectionId viewer);
} // namespace aether::net
