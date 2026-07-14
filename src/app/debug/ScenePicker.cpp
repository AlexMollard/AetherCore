#include "debug/ScenePicker.hpp"

#include <glm/glm.hpp>

#include "mesh/Mesh.hpp"
#include "physics/PhysicsComponents.hpp"
#include "physics/PhysicsSystem.hpp"
#include "scene/Components.hpp"
#include "scene/World.hpp"

namespace aether::editor
{
	PickHit PickEntity(World& world, PhysicsSystem* physics, const Ray& ray, float maxDist)
	{
		PickHit best{};
		best.t = maxDist;

		for (const auto& [enttE, meshComp, tc]: world.View<MeshComponent, TransformComponent>().each())
		{
			const Entity e = World::FromEntt(enttE);
			if (!e.IsValid() || meshComp.mesh == nullptr)
			{
				continue;
			}
			const glm::vec3 mn = meshComp.mesh->GetAABBMin();
			const glm::vec3 mx = meshComp.mesh->GetAABBMax();
			if (mn == mx)
			{
				continue;
			}

			const glm::mat4 worldToLocal = glm::inverse(tc.localToWorld);
			Ray local{};
			local.origin = glm::vec3(worldToLocal * glm::vec4(ray.origin, 1.0f));
			local.dir = glm::vec3(worldToLocal * glm::vec4(ray.dir, 0.0f));

			float tLocal = 0.0f;
			if (!RayVsAabb(local, mn, mx, tLocal))
			{
				continue;
			}
			const glm::vec3 worldHit = glm::vec3(tc.localToWorld * glm::vec4(local.origin + local.dir * tLocal, 1.0f));
			const float tWorld = glm::dot(worldHit - ray.origin, ray.dir);
			if (tWorld >= 0.0f && tWorld < best.t)
			{
				best.entity = e;
				best.t = tWorld;
			}
		}

		if (physics != nullptr)
		{
			const auto hit = physics->CastRay(ray.origin, ray.dir, maxDist);
			if (hit.hit)
			{
				const float tWorld = hit.fraction * maxDist;
				if (tWorld < best.t)
				{
					for (const auto& [enttE, rb]: world.View<RigidBodyComponent>().each())
					{
						if (rb.body.value == hit.body.value)
						{
							best.entity = World::FromEntt(enttE);
							best.t = tWorld;
							break;
						}
					}
				}
			}
		}

		return best.entity.IsValid() ? best : PickHit{};
	}
} // namespace aether::editor
