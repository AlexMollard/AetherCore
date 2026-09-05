#include "net/NetRelevancy.hpp"

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <limits>

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

	std::vector<Entity> RelevantWithTransformless(World& world, ConnectionId viewer, glm::vec3 viewerPos,
	        const RelevancySettings& settings)
	{
		std::vector<Entity> out = RelevantFor(world, viewer, viewerPos, settings);

		// The union half of the contract documented on RelevantFor's view: a
		// NetworkIdentity with no TransformComponent is never spatial, so it is
		// always relevant to every connection. Appending rather than re-filtering
		// keeps RelevantFor's result untouched, and the exclude_t on the view means
		// nothing here can duplicate an entity RelevantFor already returned.
		world.GetRegistry()
		        .view<NetworkIdentity>(entt::exclude<TransformComponent>)
		        .each([&](entt::entity ent, NetworkIdentity&) { out.push_back(World::FromEntt(ent)); });
		return out;
	}

	glm::vec3 ViewerPosition(World& world, ConnectionId viewer)
	{
		// Deterministic on purpose: "the first owned entity the view happens to
		// yield" is a different anchor on consecutive frames - and between the join
		// replay and the send tick - whenever a connection owns more than one
		// positioned entity, because ECS iteration order is unspecified. A jumping
		// anchor is relevancy churn: entities near either anchor's radius boundary
		// leave and re-enter, and every crossing destroys the client's copy and re
		// -Spawns it with a reliable full-state send. The lowest entity id is stable
		// whatever order the pool is walked in.
		glm::vec3 position{0.f};
		std::uint32_t anchorId = std::numeric_limits<std::uint32_t>::max();
		world.View<NetworkIdentity, TransformComponent>().each(
		        [&](entt::entity ent, NetworkIdentity& identity, TransformComponent& transform)
		        {
			        if (identity.owner != viewer)
			        {
				        return;
			        }
			        const Entity entity = World::FromEntt(ent);
			        if (entity.id >= anchorId)
			        {
				        return;
			        }
			        anchorId = entity.id;
			        position = glm::vec3(transform.localToWorld[3]);
		        });
		return position;
	}
} // namespace aether::net
