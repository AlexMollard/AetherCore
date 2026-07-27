#include "net/NetRelevancy.hpp"

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include "scene/Components.hpp"
#include "scene/World.hpp"

namespace aether::net
{
	std::vector<Entity> RelevantFor(World& world, ConnectionId viewer, glm::vec3 viewerPos,
	        const RelevancySettings& settings)
	{
		std::vector<Entity> out;
		const float radiusSq = settings.radius * settings.radius;

		// This view silently excludes a networked entity that has no TransformComponent -
		// there is no position to measure, so it can never pass a distance test and would
		// be replicated to nobody. That is correct for spatial entities but WRONG for a
		// transformless one (a game-state entity replicating only script fields), which
		// must bypass relevancy entirely rather than be dropped. The system that drives
		// this is responsible for treating those as always-relevant.
		world.View<NetworkIdentity, TransformComponent>().each(
		        [&](entt::entity ent, NetworkIdentity& id, TransformComponent& transform)
		        {
			        const Entity e = World::FromEntt(ent);
			        if (!settings.enabled || id.owner == viewer)
			        {
				        out.push_back(e);
				        return;
			        }
			        const glm::vec3 d = glm::vec3(transform.localToWorld[3]) - viewerPos;
			        if (glm::dot(d, d) <= radiusSq)
			        {
				        out.push_back(e);
			        }
		        });
		return out;
	}
} // namespace aether::net
