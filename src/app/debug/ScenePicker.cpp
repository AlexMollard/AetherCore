#include "debug/ScenePicker.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

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
		std::uint64_t bestSpriteOrder = 0;
		bool haveSprite = false;
		for (const auto& [enttE, sprite, transform]: world.GetRegistry().view<SpriteRendererComponent, TransformComponent>(entt::exclude<DisabledComponent>).each())
		{
			if (!sprite.visible)
			{
				continue;
			}
			const glm::mat4 worldToLocal = glm::inverse(transform.localToWorld);
			const glm::vec3 localOrigin = glm::vec3(worldToLocal * glm::vec4(ray.origin, 1.0f));
			const glm::vec3 localDir = glm::vec3(worldToLocal * glm::vec4(ray.dir, 0.0f));
			if (std::abs(localDir.z) < 1e-6f)
			{
				continue;
			}
			const float localT = -localOrigin.z / localDir.z;
			if (localT < 0.0f)
			{
				continue;
			}
			const glm::vec2 point = glm::vec2(localOrigin + localDir * localT);
			const glm::vec2 size = sprite.pixelSize / std::max(sprite.pixelsPerUnit, 0.001f);
			const glm::vec2 minPoint = -sprite.pivot * size;
			const glm::vec2 maxPoint = (glm::vec2(1.0f) - sprite.pivot) * size;
			if (point.x < minPoint.x || point.y < minPoint.y || point.x > maxPoint.x || point.y > maxPoint.y)
			{
				continue;
			}
			const Entity entity = World::FromEntt(enttE);
			const auto biased = [](std::int32_t value) { return static_cast<std::uint64_t>(std::clamp(value, -32768, 32767) + 32768); };
			const std::uint64_t order = (biased(sprite.sortingLayer) << 48u) | (biased(sprite.orderInLayer) << 32u) | entity.id;
			if (!haveSprite || order > bestSpriteOrder)
			{
				const glm::vec3 localHit = localOrigin + localDir * localT;
				const glm::vec3 worldHit = glm::vec3(transform.localToWorld * glm::vec4(localHit, 1.0f));
				best = PickHit{.entity = entity, .t = glm::dot(worldHit - ray.origin, ray.dir)};
				bestSpriteOrder = order;
				haveSprite = true;
			}
		}
		// Sprites do not short-circuit: the sprite pass records its winning hit's
		// world-space t (chosen among sprites by sorting order), then meshes and physics
		// below compete on t, so a closer 3D hit wins in mixed 2D/3D scenes. Pure-2D
		// scenes have no meshes, so the top sprite still wins.
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
